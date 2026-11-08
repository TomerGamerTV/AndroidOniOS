import Combine
import os
import SwiftUI
import UIKit

/// Observable state shown by the SwiftUI overlay.
final class AppState: ObservableObject {
    enum Phase: Equatable { case preparing, booting, running, failed(String), stopped }
    @Published var phase: Phase = .preparing
    @Published var detail = ""
    @Published var progress: Double? = nil
    @Published var toast: String? = nil
    @Published var showMenu = false
    @Published var showSettings = false
    @Published var jitEnabled = false
    @Published var bootFraction: Double = 0
    @Published var guestVisible = false

    func flash(_ message: String) {
        toast = message
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            if self?.toast == message { self?.toast = nil }
        }
    }
}

final class MainViewController: UIViewController {
    private let display = DisplayView(frame: .zero)
    private let state = AppState()
    private var overlay: UIHostingController<OverlayView>!
    private let bootProgress = BootProgress()
    private var cancellables = Set<AnyCancellable>()

    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        // Android rotates its own content from the accelerometer; keep the framebuffer upright.
        UIDevice.current.userInterfaceIdiom == .pad ? .all : .portrait
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        display.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(display)
        NSLayoutConstraint.activate([
            display.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            display.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            // Inside the safe area: the Dynamic Island and corners would hide Android's status bar.
            display.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            display.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),
        ])

        overlay = UIHostingController(rootView: OverlayView(state: state, actions: actions))
        overlay.view.backgroundColor = .clear
        overlay.view.translatesAutoresizingMaskIntoConstraints = false
        addChild(overlay)
        view.addSubview(overlay.view)
        NSLayoutConstraint.activate([
            overlay.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            overlay.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            overlay.view.topAnchor.constraint(equalTo: view.topAnchor),
            overlay.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        overlay.didMove(toParent: self)
        // The SwiftUI overlay covers the whole screen; once Android is visible it must not
        // swallow touches meant for the guest (sheets are presented separately and still work).
        state.$phase.combineLatest(state.$guestVisible)
            .receive(on: RunLoop.main)
            .sink { [weak self] phase, visible in
                let passThrough = phase == .running || (phase == .booting && visible)
                self?.overlay.view.isUserInteractionEnabled = !passThrough
            }
            .store(in: &cancellables)

        // Three-finger long press opens the device controls.
        let lp = UILongPressGestureRecognizer(target: self, action: #selector(showMenu(_:)))
        lp.numberOfTouchesRequired = 3
        lp.minimumPressDuration = 0.6
        lp.cancelsTouchesInView = false
        display.addGestureRecognizer(lp)

        // Hidden until Android draws its first frame (avoids a black/garbage flash).
        display.alpha = 0
        display.onFirstFrame = { [weak self] in
            guard let self else { return }
            self.bootProgress.firstFrame()
            UIView.animate(withDuration: 0.35) { self.display.alpha = 1 }
            self.state.guestVisible = true
        }
        bootProgress.onFailure = { [weak self] reason in
            VirtualMachine.shared.setRunning(false)
            self?.state.phase = .failed(reason)
        }
        bootProgress.onChange = { [weak self] f, label in
            guard let self, self.state.phase == .booting else { return }
            self.state.bootFraction = f
            self.state.detail = label
        }
        Inbox.shared.onMessage = { [weak self] msg in self?.state.flash(msg) }
        NotificationCenter.default.addObserver(forName: .androidBootCompleted, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            VirtualMachine.shared.ready = true
            self.state.bootFraction = 1
            GuestSetup.apply(VMSettings.current)
            Log.info("boot completed; memory headroom \(os_proc_available_memory() >> 20) MiB, thermal state \(ProcessInfo.processInfo.thermalState.rawValue)")
            Timer.scheduledTimer(withTimeInterval: 120, repeats: true) { _ in
                Log.info("memory headroom \(os_proc_available_memory() >> 20) MiB, thermal state \(ProcessInfo.processInfo.thermalState.rawValue)")
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { self.state.phase = .running }
        }
        Task { await boot() }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        display.becomeFirstResponder()
    }

    @objc private func showMenu(_ g: UILongPressGestureRecognizer) {
        if g.state == .began { state.showMenu = true }
    }

    private lazy var actions = OverlayActions(
        back: { VirtualMachine.shared.tapKey(.acBack) },
        home: { VirtualMachine.shared.tapKey(.acHome) },
        recents: {
            if VMSettings.current.native { VirtualMachine.shared.tapCode(1000) }   // KEY_APPSELECT in Onirun
            else { _ = try? ADBClient.shared.shell("input keyevent KEYCODE_APP_SWITCH") }
        },
        power: { VirtualMachine.shared.tapKey(.power) },
        volumeUp: { VirtualMachine.shared.tapKey(.volumeUp) },
        volumeDown: { VirtualMachine.shared.tapKey(.volumeDown) },
        rotate: { DispatchQueue.global().async { _ = try? ADBClient.shared.shell("cmd window user-rotation lock $(( ($(settings get system user_rotation) + 1) % 4 ))") } },
        reboot: { VirtualMachine.shared.reset() },
        saveAndClose: { [weak self] in
            self?.state.flash("Saving Android…")
            VirtualMachine.shared.saveAndQuit { ok in if !ok { self?.state.flash("Couldn't save Android") } }
        },
        enableJIT: { [weak self] in self?.requestJIT() }
    )

    // MARK: - Boot sequence

    private func boot() async {
        if VMSettings.current.native { await prepareNative(); return }
        let store = ImageStore.shared
        store.onStage = { [weak self] stage in self?.render(stage) }
        do {
            state.phase = .preparing
            state.detail = "Checking Android system image…"
            try await store.prepare()
            if UserDefaults.standard.bool(forKey: "pendingFactoryReset") {
                SaveState.discard()
                try store.resetUserData()
                UserDefaults.standard.set(false, forKey: "pendingFactoryReset")
            }
        } catch {
            state.phase = .failed(error.localizedDescription)
            return
        }
        start()
    }

    /// The native engine's Android: installed from Library/Android/onirun-rootfs.tar the first time.
    private func prepareNative() async {
        state.phase = .preparing
        let fm = FileManager.default
        if !fm.fileExists(atPath: VirtualMachine.nativeRoot.path) {
            guard fm.fileExists(atPath: VirtualMachine.nativeArchive.path) else {
                state.phase = .failed("The native engine's Android isn't installed. Copy onirun-rootfs.tar (tools/mkrootfs-tar.py) into the app's Library/Android folder.")
                return
            }
            state.detail = "Installing Android"
            state.progress = 0
            do {
                try await Task.detached(priority: .userInitiated) {
                    try VirtualMachine.shared.installNative { f in DispatchQueue.main.async { self.state.progress = f } }
                }.value
            } catch {
                state.phase = .failed(error.localizedDescription)
                return
            }
            state.progress = nil
        }
        start()
    }

    private func render(_ stage: ImageStore.Stage) {
        switch stage {
        case .downloading(let f): state.detail = "Downloading Android 17 · 2.4 GB"; state.progress = f
        case .verifying(let f): state.detail = "Verifying download"; state.progress = f
        case .extracting(_, let f): state.detail = "Unpacking system image"; state.progress = f
        case .finishing: state.detail = "Finishing setup"; state.progress = 1
        case .ready: state.progress = nil
        case .failed(let s): state.phase = .failed(s)
        case .idle: break
        }
    }

    private func start() {
        let settings = VMSettings.current
        DebugShell.shared.startIfEnabled()
        ADBRelay.shared.startIfEnabled()
        Log.info("jit reservation: \(ProcessInfo.processInfo.environment["DROID_JIT_RESERVE"] ?? "none")")
        if let diag = ProcessInfo.processInfo.environment["DROID_JIT_RESERVE_DIAG"] {
            Log.info("jit reservation map: \(diag)")
        }
        // If the last JIT start never got past QEMU init (it exits the process), use the
        // interpreter this time so the app can't get stuck in a crash loop.
        let jitMarker = Paths.android.appendingPathComponent("jit-attempt")
        let lastJITFailed = FileManager.default.fileExists(atPath: jitMarker.path)
        if lastJITFailed {
            try? FileManager.default.removeItem(at: jitMarker)
            Log.info("previous JIT start failed; using the interpreter this launch")
        }
        // Try to get JIT from the bundled StikJIT helper before QEMU starts (needs a pairing
        // file and LocalDevVPN). Without it we fall back to the slower interpreter.
        let wantJIT = settings.native || (settings.preferJIT && !lastJITFailed)   // native has no interpreter
        if wantJIT && !droid_bridge_jit_available() && JITEnabler.shared.pairingFileURL != nil {
            state.phase = .booting
            state.detail = "Enabling JIT acceleration"
            // Bring up the bundled loopback tunnel StikJIT needs to reach 10.7.0.1, then ask the
            // helper to attach. QEMU starts once the debugger is attached, or when the helper fails.
            let failed = DispatchSemaphore(value: 0)
            LoopbackVPN.shared.start { vpnOK, vpnMsg in
                Log.info("loopback vpn: ok=\(vpnOK) \(vpnMsg)")
                JITEnabler.shared.start { [weak self] ok, msg in
                    if !ok {
                        failed.signal()
                        DispatchQueue.main.async { self?.state.flash("JIT: \(msg.split(separator: "\n").first ?? "")") }
                    }
                    LoopbackVPN.shared.stop()
                }
                DispatchQueue.global(qos: .userInitiated).async {
                    let attached = JITEnabler.shared.waitForAttach(timeout: 45, cancel: failed)
                    DispatchQueue.main.async {
                        if !attached { self.state.flash("JIT unavailable — using the interpreter") }
                        self.launchVM(settings)
                    }
                }
            }
            return
        }
        launchVM(settings)
    }

    private func launchVM(_ settings: VMSettings) {
        state.phase = .booting
        state.progress = nil
        state.bootFraction = 0
        state.detail = "Starting Android"
        if settings.native { launchNative(settings); return }
        GuestServices.shared.density = settings.density
        GuestServices.shared.start()
        Modem.shared.start()
        do {
            try VirtualMachine.shared.start(settings: settings, adbPublicKey: ADBKey.shared.androidPublicKey, display: display)
        } catch {
            state.phase = .failed(error.localizedDescription)
            return
        }
        state.jitEnabled = VirtualMachine.shared.usingJIT
        if VirtualMachine.shared.usingJIT {
            let marker = Paths.android.appendingPathComponent("jit-attempt")
            FileManager.default.createFile(atPath: marker.path, contents: nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 30) { try? FileManager.default.removeItem(at: marker) }
        }
        VirtualMachine.shared.onExit = { [weak self] code in
            self?.state.phase = .stopped
            self?.state.detail = VirtualMachine.shared.restoring
                ? "Android could not resume from its saved state. Open the app again to start it normally."
                : "Android stopped (\(code))."
        }
        if VirtualMachine.shared.restoring {
            // Resuming from a save state: no boot to follow.
            state.detail = "Resuming Android"
            VirtualMachine.shared.onRestored = { [weak self] in
                guard let self else { return }
                self.bootProgress.finish()
                self.state.bootFraction = 1
                self.state.phase = .running
            }
        } else {
            bootProgress.start(kernelLog: VirtualMachine.shared.kernelLog)
        }
        DispatchQueue.global(qos: .utility).async {
            if ADBClient.shared.connect() {
                DispatchQueue.main.async {
                    FileBridge.shared.start()
                    Inbox.shared.process()
                }
            }
        }
    }

    /// onirun: no ADB, modem or boot log; Android is up when it draws.
    private func launchNative(_ settings: VMSettings) {
        do {
            try VirtualMachine.shared.startNative(settings: settings, display: display)
        } catch {
            state.phase = .failed(error.localizedDescription)
            return
        }
        state.jitEnabled = true
        state.detail = "Starting Android (native)"
        let first = display.onFirstFrame
        display.onFirstFrame = { [weak self] in
            first?()
            VirtualMachine.shared.ready = true
            self?.state.bootFraction = 1
            self?.state.phase = .running
        }
        VirtualMachine.shared.onExit = { [weak self] code in
            self?.state.phase = .stopped
            self?.state.detail = "Android stopped (\(code))."
        }
    }

    private func requestJIT() {
        let pid = getpid()
        let id = Bundle.main.bundleIdentifier ?? "com.tomergamertv.oni"
        UserDefaults.standard.set(true, forKey: "preferJIT")
        if let url = URL(string: "stikdebug://enable-jit?bundle-id=\(id)&pid=\(pid)&script-name=universal.js"),
           UIApplication.shared.canOpenURL(url) {
            UIApplication.shared.open(url)
            state.flash("After StikDebug attaches, relaunch Android to use JIT.")
        } else {
            state.flash("Install StikDebug (with a pairing file) to enable JIT.")
        }
    }
}

extension VirtualMachine {
    func tapCode(_ code: Int32) {
        key(code, down: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { self.key(code, down: false) }
    }
    func tapKey(_ k: KeyMap.Q) {
        key(k.rawValue, down: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { self.key(k.rawValue, down: false) }
    }
}
