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
            display.topAnchor.constraint(equalTo: view.topAnchor),
            display.bottomAnchor.constraint(equalTo: view.bottomAnchor),
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
            self.state.bootFraction = 1
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
        recents: { _ = try? ADBClient.shared.shell("input keyevent KEYCODE_APP_SWITCH") },
        power: { VirtualMachine.shared.tapKey(.power) },
        volumeUp: { VirtualMachine.shared.tapKey(.volumeUp) },
        volumeDown: { VirtualMachine.shared.tapKey(.volumeDown) },
        rotate: { DispatchQueue.global().async { _ = try? ADBClient.shared.shell("cmd window user-rotation lock $(( ($(settings get system user_rotation) + 1) % 4 ))") } },
        reboot: { VirtualMachine.shared.reset() },
        enableJIT: { [weak self] in self?.requestJIT() }
    )

    // MARK: - Boot sequence

    private func boot() async {
        let store = ImageStore.shared
        store.onStage = { [weak self] stage in self?.render(stage) }
        do {
            state.phase = .preparing
            state.detail = "Checking Android system image…"
            try await store.prepare()
            if UserDefaults.standard.bool(forKey: "pendingFactoryReset") {
                try store.resetUserData()
                UserDefaults.standard.set(false, forKey: "pendingFactoryReset")
            }
        } catch {
            state.phase = .failed(error.localizedDescription)
            return
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
        state.phase = .booting
        state.progress = nil
        state.bootFraction = 0
        state.detail = "Starting Android"
        let settings = VMSettings.current
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
        VirtualMachine.shared.onExit = { [weak self] code in
            self?.state.phase = .stopped
            self?.state.detail = "Android stopped (\(code))."
        }
        bootProgress.start(kernelLog: VirtualMachine.shared.kernelLog)
        DispatchQueue.global(qos: .utility).async {
            if ADBClient.shared.connect() {
                DispatchQueue.main.async {
                    FileBridge.shared.start()
                    Inbox.shared.process()
                }
            }
        }
    }

    private func requestJIT() {
        let pid = getpid()
        let id = Bundle.main.bundleIdentifier ?? "com.tomer.androidonios"
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
    func tapKey(_ k: KeyMap.Q) {
        key(k.rawValue, down: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { self.key(k.rawValue, down: false) }
    }
}
