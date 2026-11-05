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

    static var current: VMSettings {
        let d = UserDefaults.standard
        let screen = UIScreen.main.nativeBounds.size // portrait pixels
        let defW = 1080
        let defH = Int((Double(defW) * Double(screen.height) / Double(screen.width)).rounded() / 2) * 2
        let mem = ProcessInfo.processInfo.physicalMemory
        let defMem = mem >= 11 << 30 ? 4096 : (mem >= 7 << 30 ? 3072 : 2048)
        return VMSettings(
            memoryMB: d.object(forKey: "memoryMB") as? Int ?? defMem,
            cpus: d.object(forKey: "cpus") as? Int ?? min(ProcessInfo.processInfo.activeProcessorCount, 4),
            width: d.object(forKey: "width") as? Int ?? defW,
            height: d.object(forKey: "height") as? Int ?? defH,
            density: d.object(forKey: "density") as? Int ?? 420,
            preferJIT: d.object(forKey: "preferJIT") as? Bool ?? true)
    }

    func save() {
        let d = UserDefaults.standard
        d.set(memoryMB, forKey: "memoryMB")
        d.set(cpus, forKey: "cpus")
        d.set(width, forKey: "width")
        d.set(height, forKey: "height")
        d.set(density, forKey: "density")
        d.set(preferJIT, forKey: "preferJIT")
    }
}

/// Runs QEMU in-process on a dedicated thread.
final class VirtualMachine {
    static let shared = VirtualMachine()

    private(set) var qemu = DroidQEMU()
    private(set) var started = false
    private(set) var usingJIT = false
    var onExit: ((Int32) -> Void)?

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
            ("androidboot.debug.hwui.renderer", "skiagl"),
            ("androidboot.logcat", "*:I"),
            ("androidboot.hardwareegl", "angle"),
            ("androidboot.hardware.gltransport", "virtio-gpu-asg"),
            ("androidboot.qemu.gltransport.name", "virtio-gpu-asg"),
            ("androidboot.qemu.gltransport.drawFlushInterval", "800"),
            ("androidboot.hardware.vulkan", "ranchu"),
            ("androidboot.hardware.gralloc", "minigbm"),
            ("androidboot.opengles.version", "196609"),
            ("androidboot.qemu.vsync", "60"),
            ("androidboot.qemu.virtiowifi", "0"),
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
        setenv("TMPDIR", NSTemporaryDirectory(), 1)

        // Optional developer overrides: Library/Android/debug.json
        struct Debug: Decodable { var accel: String?; var smp: Int?; var cpu: String?; var extraArgs: [String]?; var env: [String: String]?; var jit: Bool? }
        let dbg = (try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json"))).flatMap { try? JSONDecoder().decode(Debug.self, from: $0) }
        for (k, v) in dbg?.env ?? [:] { setenv(k, v, 1) }
        let accel = dbg?.accel ?? (usingJIT ? "tcg,thread=multi,tb-size=1024" : "tcg,thread=multi,tb-size=512")
        var args: [String] = [
            "qemu-system-aarch64",
            "-machine", "virt,highmem=on",
            "-accel", accel,
            "-cpu", dbg?.cpu ?? "max,sve=off,pauth-impdef=on",
            "-smp", String(dbg?.smp ?? settings.cpus),
            "-m", String(settings.memoryMB),
            "-nodefaults", "-no-user-config",
            "-display", "none",
            "-kernel", store.path("kernel-ranchu").path,
            "-initrd", store.initrd.path,
            "-append", "8250.nr_uarts=1 no_timer_check console=ttyAMA0,38400 keep_bootcon earlyprintk=ttyAMA0 ndns=2 loop.max_part=7 printk.devkmsg=on bootconfig mac80211_hwsim.radios=0",
            "-serial", "file:\(kernelLog.path)",
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
            "-chardev", "null,id=hvc0",
            "-chardev", "file,id=hvc1,path=\(logcatLog.path)",
            "-chardev", "socket,id=modem,path=\(Paths.modemSocket),reconnect-ms=1000",
            "-device", "virtio-serial-pci,ioeventfd=off",
            "-device", "virtconsole,chardev=hvc0",
            "-device", "virtconsole,chardev=hvc1",
            "-device", "virtserialport,chardev=modem,name=modem",
            "-device", "virtio-gpu-rutabaga-pci,gfxstream-vulkan=on,x-gfxstream-gles=off,x-gfxstream-composer=on,cross-domain=off,hostmem=1G,xres=\(settings.width),yres=\(settings.height)",
            "-device", "virtio-multitouch-pci",
            "-device", "virtio-keyboard-pci",
            "-device", "virtio-vsock-host-pci,guest-cid=77,forward=unix:\(Paths.adbSocket):5555,guest-forward=5000:unix:\(Paths.servicesSocket)",
        ]
        args += dbg?.extraArgs ?? []
        Log.info("qemu " + args.joined(separator: " "))

        display.attach(to: &qemu)
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
