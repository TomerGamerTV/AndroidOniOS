import AVFoundation
import os
import Foundation
import UIKit

/// User-tunable VM settings (persisted in UserDefaults).
struct VMSettings {
    var memoryMB: Int
    var cpus: Int
    var width: Int
    var height: Int
    var density: Int
    var preferJIT: Bool
    var navButtons: Bool
    var saveState: Bool
    /// "qemu": the virtual machine; "native": onirun, Android's own programs translated in-process
    /// (no VM; needs JIT and the onirun rootfs).
    var engine: String
    var native: Bool { engine == "native" }

    /// The part of the screen Android gets, in portrait points: everything inside iOS's safe area,
    /// so the Dynamic Island, rounded corners and home indicator never cover Android's status and
    /// navigation bars. Works out per device.
    static var screenArea: CGSize {
        let b = UIScreen.main.bounds.size
        let w = min(b.width, b.height), h = max(b.width, b.height)
        let window = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }.first { $0.isKeyWindow }
        var inset = window.map { $0.safeAreaInsets.top + $0.safeAreaInsets.bottom } ?? 0
        if inset == 0 && h / w > 2 { inset = 59 + 34 }   // before the window exists: Face ID iPhones
        return CGSize(width: w, height: h - inset)
    }

    /// Android pixels tall for a given width, matching the screen area's shape.
    static func height(forWidth width: Int) -> Int {
        let a = screenArea
        return Int((Double(width) * Double(a.height) / Double(a.width) / 2).rounded()) * 2
    }

    /// Density that makes Android's sizes match iOS points (160 dpi = 1 dp per point).
    static func density(forWidth width: Int) -> Int {
        Int((160 * Double(width) / Double(screenArea.width) / 10).rounded()) * 10
    }

    static var current: VMSettings {
        let d = UserDefaults.standard
        if !d.bool(forKey: "layoutV2") {          // sizes are derived now; drop ones saved by older versions
            d.removeObject(forKey: "height")
            d.removeObject(forKey: "density")
            d.set(true, forKey: "layoutV2")
        }
        let defW = 1080
        let mem = ProcessInfo.processInfo.physicalMemory
        let defMem = mem >= 11 << 30 ? 4096 : (mem >= 7 << 30 ? 3072 : 2048)
        return VMSettings(
            memoryMB: d.object(forKey: "memoryMB") as? Int ?? defMem,
            cpus: d.object(forKey: "cpus") as? Int ?? min(ProcessInfo.processInfo.activeProcessorCount, 4),
            width: d.object(forKey: "width") as? Int ?? defW,
            height: height(forWidth: d.object(forKey: "width") as? Int ?? defW),
            density: d.object(forKey: "density") as? Int ?? density(forWidth: d.object(forKey: "width") as? Int ?? defW),
            preferJIT: d.object(forKey: "preferJIT") as? Bool ?? true,
            navButtons: d.object(forKey: "navButtons") as? Bool ?? true,
            saveState: SaveState.enabled,
            engine: VMSettings.debugEngine ?? d.string(forKey: "engine") ?? "qemu")
    }

    /// debug.json {"engine": "native"} overrides the setting (developer use).
    static var debugEngine: String? {
        (try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json")))
            .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }?["engine"] as? String
    }

    func save() {
        let d = UserDefaults.standard
        d.set(memoryMB, forKey: "memoryMB")
        d.set(cpus, forKey: "cpus")
        d.set(width, forKey: "width")
        d.set(density, forKey: "density")
        d.set(navButtons, forKey: "navButtons")
        d.set(preferJIT, forKey: "preferJIT")
        d.set(saveState, forKey: "saveState")
        d.set(engine, forKey: "engine")
    }
}

/// Runs QEMU in-process on a dedicated thread.
final class VirtualMachine {
    static let shared = VirtualMachine()

    private(set) var qemu = DroidQEMU()
    private(set) var started = false
    private(set) var usingJIT = false
    var onExit: ((Int32) -> Void)?

    /// Launched from a save state; true until the saved machine is loaded.
    private(set) var restoring = false
    /// Android is up (booted, or resumed from a save): a save state may be taken.
    var ready = false
    private(set) var saving = false
    private var fingerprint = ""
    private var writableDisks: [URL] = []
    /// Called on the main thread when a restore has loaded (true) or the VM was not restored.
    var onRestored: (() -> Void)?

    var kernelLog: URL { Paths.logs.appendingPathComponent("kernel.log") }
    var logcatLog: URL { Paths.logs.appendingPathComponent("logcat.log") }

    func start(settings: VMSettings, adbPublicKey: String, display: DisplayView) throws {
        guard !started else { return }
        let store = ImageStore.shared
        try store.ensureUserImages()

        let dbgJIT = (try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json"))).flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }?["jit"] as? Bool
        usingJIT = (dbgJIT ?? settings.preferJIT) && droid_bridge_jit_available()
        let fwName = usingJIT ? "QEMUJIT" : "QEMUTCTI"
        let fw = Bundle.main.privateFrameworksURL!.appendingPathComponent("\(fwName).framework/\(fwName)")
        var err = [CChar](repeating: 0, count: 512)
        guard droid_qemu_load(fw.path, &qemu, &err, err.count) == 0 else {
            throw NSError(domain: "Droid", code: 10, userInfo: [NSLocalizedDescriptionKey: "Could not load \(fwName): \(String(cString: err))"])
        }
        Log.info("loaded \(fwName) (JIT \(usingJIT ? "on" : "off"))")

        // Boot properties (androidboot.* -> ro.boot.*).
        var props: [(String, String)] = [
            ("androidboot.hardware", "ranchu"),
            ("androidboot.boot_devices", "a003600.virtio_mmio"),
            ("androidboot.qemu", "1"),
            ("androidboot.serialno", serialNumber()),
            ("androidboot.dalvik.vm.heapsize", settings.memoryMB >= 3072 ? "512m" : "384m"),
            // App UI renders with Skia on Vulkan (gfxstream), skipping the in-guest ANGLE
            // GL-on-Vulkan layer; needs gfxstream's VulkanVirtualQueue (HWUI wants 2 queues).
            ("androidboot.debug.hwui.renderer", "skiavk"),
            ("androidboot.logcat", "*:I"),
            ("androidboot.hardwareegl", "angle"),
            ("androidboot.hardware.gltransport", "virtio-gpu-asg"),
            ("androidboot.qemu.gltransport.name", "virtio-gpu-asg"),
            ("androidboot.qemu.gltransport.drawFlushInterval", "800"),
            ("androidboot.hardware.vulkan", "ranchu"),
            ("androidboot.hardware.gralloc", "minigbm"),
            ("androidboot.opengles.version", "196609"),
            ("androidboot.qemu.vsync", "60"),
            ("androidboot.qemu.virtiowifi", "1"),
            ("androidboot.qemu.settings.system.screen_off_timeout", "2147483647"),
            ("androidboot.qemu.hwcodec.avcdec", "2"),
            ("androidboot.qemu.hwcodec.hevcdec", "2"),
            ("androidboot.qemu.hwcodec.vpxdec", "2"),
            ("androidboot.qemu.camera_protocol_ver", "1"),
            ("androidboot.qemu.adb.pubkey", adbPublicKey),
            ("androidboot.lcd_density", String(settings.density)),
        ]
        if settings.memoryMB <= 2048 { props.append(("androidboot.config.low_ram", "true")) }
        for (k, v) in store.verifiedBootParams.sorted(by: { $0.key < $1.key }) { props.append((k, v)) }
        props.append(("androidboot.veritymode", "enforcing"))
        try store.writeInitrd(props: props)

        // gfxstream renders through MoltenVK; display size must match the virtio-gpu EDID.
        let mvk = Bundle.main.privateFrameworksURL!.appendingPathComponent("MoltenVK.framework/MoltenVK").path
        setenv("ANDROID_EMU_VK_ICD", "moltenvk", 1)
        setenv("ANDROID_EMU_VK_LOADER_PATH", mvk, 1)
        setenv("RUTABAGA_DISPLAY_WIDTH", String(settings.width), 1)
        setenv("RUTABAGA_DISPLAY_HEIGHT", String(settings.height), 1)
        setenv("MVK_CONFIG_LOG_LEVEL", "1", 1)
        // VulkanSnapshots makes gfxstream track what it needs to save and restore the GPU state.
        let features = settings.saveState && qemu.save_state != nil
            ? "VulkanVirtualQueue:enabled,VulkanSnapshots:enabled" : "VulkanVirtualQueue:enabled"
        setenv("RUTABAGA_RENDERER_FEATURES", features, 1)
        setenv("TMPDIR", NSTemporaryDirectory(), 1)

        // Optional developer overrides: Library/Android/debug.json
        struct Debug: Decodable { var accel: String?; var smp: Int?; var cpu: String?; var extraArgs: [String]?; var env: [String: String]?; var jit: Bool? }
        let dbg = (try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json"))).flatMap { try? JSONDecoder().decode(Debug.self, from: $0) }
        for (k, v) in dbg?.env ?? [:] { setenv(k, v, 1) }
        // JIT code cache: Android's translated working set is several hundred MiB, and every
        // time the cache fills QEMU throws all of it away. Size it to the low address range
        // reserved at launch (only the RX alias must be low; code run from high addresses hangs).
        let reserveMB = ProcessInfo.processInfo.environment["DROID_JIT_RESERVE"]
            .flatMap { $0.split(separator: ":").last }
            .flatMap { UInt64($0.dropFirst(2), radix: 16) }
            .map { Int($0 >> 20) } ?? 512
        let jitTB = min(1024, max(128, reserveMB))
        let accel = dbg?.accel ?? (usingJIT ? "tcg,thread=multi,split-wx=on,tb-size=\(jitTB)" : "tcg,thread=multi,tb-size=512")
        var args: [String] = [
            "qemu-system-aarch64",
            "-machine", "virt,highmem=on",
            "-accel", accel,
            "-cpu", dbg?.cpu ?? "max,sve=off,pauth=off",
            "-smp", String(dbg?.smp ?? settings.cpus),
            "-m", String(settings.memoryMB),
            "-nodefaults", "-no-user-config",
            "-display", "none",
            "-kernel", store.path("kernel-ranchu").path,
            "-initrd", store.initrd.path,
            // Kernel console on virtio-console (hvc0): whole lines per virtqueue kick instead of
            // one trapped MMIO write per character on the PL011 UART, which is costly under TCG.
            "-append", "8250.nr_uarts=1 no_timer_check console=hvc0 ndns=2 loop.max_part=7 printk.devkmsg=on bootconfig mac80211_hwsim.radios=0",
            "-serial", "null",
            // Disk order matches the emulator (vendor, metadata, userdata, cache, system) so
            // virtio-mmio slots and /dev/block/vdX names line up with the image's fstab.
            "-drive", "if=none,id=vendor,file=\(store.path("vendor.img").path),format=raw,read-only=on",
            "-device", "virtio-blk-device,drive=vendor",
            "-drive", "if=none,id=metadata,file=\(store.metadata.path),format=raw,cache=writeback",
            "-device", "virtio-blk-device,drive=metadata",
            "-drive", "if=none,id=userdata,file=\(store.userdata.path),format=raw,cache=writeback,discard=unmap,detect-zeroes=unmap",
            "-device", "virtio-blk-device,drive=userdata",
            "-drive", "if=none,id=cache,file=\(store.cache.path),format=raw,cache=writeback",
            "-device", "virtio-blk-device,drive=cache",
            "-drive", "if=none,id=system,file=\(store.path("system.img").path),format=raw,read-only=on",
            "-device", "virtio-blk-device,drive=system",
            "-netdev", "user,id=net0,dns=10.0.2.3",
            "-device", "virtio-net-device,netdev=net0",
            "-device", "virtio-rng-device",
            "-chardev", "file,id=hvc0,path=\(kernelLog.path)",
            "-chardev", "file,id=hvc1,path=\(logcatLog.path)",
            "-chardev", "socket,id=modem,path=\(Paths.modemSocket),reconnect-ms=1000",
            "-device", "virtio-serial-pci,ioeventfd=off",
            "-device", "virtconsole,chardev=hvc0",
            "-device", "virtconsole,chardev=hvc1",
            "-device", "virtserialport,chardev=modem,name=modem",
            // The image's Bluetooth/UWB/NFC HALs expect the emulator's virtual radio ports.
            // Without them their services retry forever; a null/file backend reads as EOF and makes
            // the HALs crash. Connect them to silent sockets (modem.c) so they start and idle.
            "-chardev", "socket,id=btport,path=\(Paths.radioSocket("bt")),reconnect-ms=1000", "-device", "virtserialport,chardev=btport,name=bluetooth",
            "-chardev", "socket,id=uwbport,path=\(Paths.radioSocket("uwb")),reconnect-ms=1000", "-device", "virtserialport,chardev=uwbport,name=uwb",
            "-chardev", "socket,id=nfcport,path=\(Paths.radioSocket("nfc")),reconnect-ms=1000", "-device", "virtserialport,chardev=nfcport,name=nfc",
            "-device", "virtio-gpu-rutabaga-pci,gfxstream-vulkan=on,x-gfxstream-gles=off,x-gfxstream-composer=on,cross-domain=off,hostmem=1G,xres=\(settings.width),yres=\(settings.height)",
            "-device", "virtio-multitouch-pci",
            "-device", "virtio-keyboard-pci",
            // Sound: virtio-snd output stream played through a RemoteIO audio unit.
            "-audiodev", "coreaudio,id=snd0",
            "-device", "virtio-sound-pci,audiodev=snd0,streams=1",
            "-device", "virtio-vsock-host-pci,guest-cid=77,forward=unix:\(Paths.adbSocket):5555,guest-forward=5000:unix:\(Paths.servicesSocket)",
        ]
        args += dbg?.extraArgs ?? []

        // Resume from a save state when one matches this exact machine and its disks.
        fingerprint = SaveState.fingerprint(args: args, features: features)
        writableDisks = [store.userdata, store.metadata, store.cache,
                         store.path("system.img"), store.path("vendor.img")]
        if settings.saveState, qemu.save_state != nil, qemu.restore_status != nil,
           SaveState.restorable(fingerprint: fingerprint, disks: writableDisks) {
            restoring = true
            FileManager.default.createFile(atPath: SaveState.attemptMarker.path, contents: nil)
            args += ["-incoming", "file:" + SaveState.file.path]
            Log.info("save state: restoring")
        } else {
            SaveState.discard()
        }
        Log.info("qemu " + args.joined(separator: " "))

        display.attach(to: &qemu)
        // Android's sound plays through a RemoteIO unit inside QEMU; mix with other apps' audio.
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setActive(true)
        // QEMU reports errors on stderr, which iOS discards; keep them in logs/qemu.log.
        if freopen(Paths.logs.appendingPathComponent("qemu.log").path, "w", stderr) != nil {
            setvbuf(stderr, nil, _IOLBF, 0)
        }
        started = true
        let q = qemu
        let thread = Thread {
            var cargs = args.map { UnsafePointer<CChar>?(strdup($0)) }
            var local = q
            let status = cargs.withUnsafeMutableBufferPointer { buf -> Int32 in
                droid_qemu_run(&local, Int32(args.count), buf.baseAddress)
            }
            for p in cargs { free(UnsafeMutablePointer(mutating: p)) }
            Log.info("qemu exited with \(status)")
            DispatchQueue.main.async { self.onExit?(status) }
        }
        thread.name = "qemu-main"
        thread.stackSize = 16 << 20
        thread.qualityOfService = QualityOfService.userInteractive
        thread.start()
        if restoring { waitForRestore() }
    }

    // MARK: - Native engine (onirun)

    /// Where the native engine's Android lives, and the archive it is installed from
    /// (tools/mkrootfs-tar.py on a Mac, copied into Library/Android).
    static var nativeRoot: URL { Paths.android.appendingPathComponent("onirun/rootfs", isDirectory: true) }
    static var nativeArchive: URL { Paths.android.appendingPathComponent("onirun-rootfs.tar") }

    private func loadNative() throws {
        guard qemu.handle == nil else { return }
        let fw = Bundle.main.privateFrameworksURL!.appendingPathComponent("Onirun.framework/Onirun")
        var err = [CChar](repeating: 0, count: 512)
        guard droid_qemu_load(fw.path, &qemu, &err, err.count) == 0 else {
            throw NSError(domain: "Droid", code: 20, userInfo: [NSLocalizedDescriptionKey: "Could not load Onirun: \(String(cString: err))"])
        }
    }

    /// Unpacks the rootfs archive (once). progress gets 0...1 on a background thread.
    func installNative(progress: @escaping (Double) -> Void) throws {
        try loadNative()
        guard let sym = dlsym(qemu.handle, "onirun_install_rootfs") else {
            throw NSError(domain: "Droid", code: 21, userInfo: [NSLocalizedDescriptionKey: "Onirun has no installer"])
        }
        typealias Install = @convention(c) (UnsafePointer<CChar>, UnsafePointer<CChar>, (@convention(c) (UInt64, UInt64, UnsafeMutableRawPointer?) -> Void)?, UnsafeMutableRawPointer?) -> Int32
        let install = unsafeBitCast(sym, to: Install.self)
        try? FileManager.default.createDirectory(at: Self.nativeRoot.deletingLastPathComponent(), withIntermediateDirectories: true)
        let box = Unmanaged.passRetained(ProgressBox(progress))
        defer { box.release() }
        let rc = install(Self.nativeArchive.path, Self.nativeRoot.path, { done, total, opaque in
            let b = Unmanaged<ProgressBox>.fromOpaque(opaque!).takeUnretainedValue()
            b.fn(total > 0 ? Double(done) / Double(total) : 0)
        }, box.toOpaque())
        guard rc == 0 else {
            throw NSError(domain: "Droid", code: 22, userInfo: [NSLocalizedDescriptionKey: "Installing Android failed (\(String(cString: strerror(-rc))))"])
        }
        try? FileManager.default.removeItem(at: Self.nativeArchive)
    }

    /// Starts Android with onirun: no virtual machine, its programs run translated in this process.
    func startNative(settings: VMSettings, display: DisplayView) throws {
        guard !started else { return }
        usingJIT = droid_bridge_jit_available()
        guard usingJIT else {
            throw NSError(domain: "Droid", code: 23, userInfo: [NSLocalizedDescriptionKey: "The native engine needs JIT (StikDebug with a pairing file)."])
        }
        try loadNative()
        let mvk = Bundle.main.privateFrameworksURL!.appendingPathComponent("MoltenVK.framework/MoltenVK").path
        setenv("ANDROID_EMU_VK_ICD", "moltenvk", 1)
        setenv("ANDROID_EMU_VK_LOADER_PATH", mvk, 1)
        setenv("RUTABAGA_DISPLAY_WIDTH", String(settings.width), 1)
        setenv("RUTABAGA_DISPLAY_HEIGHT", String(settings.height), 1)
        setenv("RUTABAGA_RENDERER_FEATURES", "VulkanVirtualQueue:enabled", 1)
        setenv("MVK_CONFIG_LOG_LEVEL", "1", 1)
        setenv("TMPDIR", NSTemporaryDirectory(), 1)
        struct Debug: Decodable { var env: [String: String]? }
        let dbg = (try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json"))).flatMap { try? JSONDecoder().decode(Debug.self, from: $0) }
        for (k, v) in dbg?.env ?? [:] { setenv(k, v, 1) }
        let args = ["onirun", "--root", Self.nativeRoot.path, "--init"]
        Log.info("onirun " + args.joined(separator: " "))
        display.attach(to: &qemu)
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setActive(true)
        if freopen(Paths.logs.appendingPathComponent("onirun.log").path, "w", stderr) != nil {
            setvbuf(stderr, nil, _IOLBF, 0)
        }
        started = true
        let q = qemu
        let thread = Thread {
            var cargs = args.map { UnsafePointer<CChar>?(strdup($0)) }
            var local = q
            let status = cargs.withUnsafeMutableBufferPointer { buf -> Int32 in
                droid_qemu_run(&local, Int32(args.count), buf.baseAddress)
            }
            Log.info("onirun exited with \(status)")
            DispatchQueue.main.async { self.onExit?(status) }
        }
        thread.name = "onirun-main"
        thread.stackSize = 16 << 20
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    // MARK: - Save states

    private func waitForRestore() {
        let began = Date()
        guard let status = qemu.restore_status else { return }
        DispatchQueue.global(qos: .userInitiated).async {
            while status() != 0 { usleep(20_000) }
            DispatchQueue.main.async {
                Log.info(String(format: "save state: restored in %.1fs", Date().timeIntervalSince(began)))
                // Loaded. The save stays valid until the VM runs again (resume() deletes it).
                try? FileManager.default.removeItem(at: SaveState.attemptMarker)
                self.restoring = false
                self.ready = true
                self.onRestored?()
                if UIApplication.shared.applicationState != .background { self.resume() }
            }
        }
    }

    /// In the background Android just pauses. It can't be saved on the way out: iOS lowers the app's
    /// memory limit as soon as it resigns (and refuses GPU work in the background), and a save
    /// started then gets the app killed (jetsam per-process-limit, seen on an iPhone 18 Pro Max).
    func enterBackground() { setRunning(false) }

    /// Menu › Save & close: saves Android while the app is in front (a few seconds), then quits,
    /// so the next launch continues where it left off. done(false) when it couldn't save.
    func saveAndQuit(done: @escaping (Bool) -> Void) {
        let started = startSave { ok in
            if ok { Log.info("save state: quitting after save"); exit(0) }
            done(false)
        }
        if !started { done(false) }
    }

    /// Pauses Android and writes its save state; false when there is nothing to do. completion runs
    /// on the main thread once the save is committed (true) or failed (false; Android resumes).
    private func startSave(completion: @escaping (Bool) -> Void) -> Bool {
        guard started, ready, !restoring, !saving, !SaveState.exists, VMSettings.current.saveState,
              let save = qemu.save_state else { return false }
        saving = true
        let began = Date()
        Log.info("save state: saving (memory headroom \(os_proc_available_memory() >> 20) MiB)")
        var task = UIBackgroundTaskIdentifier.invalid
        task = UIApplication.shared.beginBackgroundTask(withName: "save-android") {
            // Out of background time: iOS suspends us mid-save; the partial file is ignored.
            Log.info("save state: background time expired")
            UIApplication.shared.endBackgroundTask(task)
        }
        let done = SaveDone { [weak self] status in
            guard let self else { return }
            self.saving = false
            if status == 0 {
                SaveState.commit(fingerprint: self.fingerprint, disks: self.writableDisks)
                Log.info(String(format: "save state: saved in %.1fs", Date().timeIntervalSince(began)))
            } else {
                Log.info("save state: failed (\(status))")
                SaveState.discard()
                self.resume()
            }
            UIApplication.shared.endBackgroundTask(task)
            completion(status == 0)
        }
        save(SaveState.partial.path, { opaque, status in
            let done = Unmanaged<SaveDone>.fromOpaque(opaque!).takeRetainedValue()
            DispatchQueue.main.async { done.fn(status) }
        }, Unmanaged.passRetained(done).toOpaque())
        return true
    }

    /// App back in front: continue Android (dropping the save it can no longer match).
    func enterForeground() { resume() }

    private func resume() {
        guard started, !restoring else { return }
        if saving { return }                   // the save's completion quits or resumes
        if SaveState.exists { SaveState.discard() }
        setRunning(true)
        if ready { GuestSetup.syncClock() }
    }

    func touch(slot: Int, phase: Int32, x: Int, y: Int, width: Int, height: Int) {
        guard started else { return }
        qemu.touch(Int32(slot), phase, Int32(x), Int32(y), Int32(width), Int32(height))
    }

    func key(_ code: Int32, down: Bool) {
        guard started else { return }
        qemu.key(code, down)
    }

    func powerButton() { if started { qemu.powerdown() } }
    func reset() { if started { qemu.reset() } }
    func setRunning(_ running: Bool) { if started { qemu.set_running(running) } }

    private func serialNumber() -> String {
        if let s = UserDefaults.standard.string(forKey: "serial") { return s }
        let s = "DROID" + String(UUID().uuidString.replacingOccurrences(of: "-", with: "").prefix(11))
        UserDefaults.standard.set(s, forKey: "serial")
        return s
    }
}

/// Boxes an install-progress callback for the C callback.
private final class ProgressBox {
    let fn: (Double) -> Void
    init(_ fn: @escaping (Double) -> Void) { self.fn = fn }
}

/// Boxes a save-state completion for the C callback.
private final class SaveDone {
    let fn: (Int32) -> Void
    init(_ fn: @escaping (Int32) -> Void) { self.fn = fn }
}
