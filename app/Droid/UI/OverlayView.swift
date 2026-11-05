import SwiftUI

struct OverlayActions {
    var back: () -> Void
    var home: () -> Void
    var recents: () -> Void
    var power: () -> Void
    var volumeUp: () -> Void
    var volumeDown: () -> Void
    var rotate: () -> Void
    var reboot: () -> Void
    var enableJIT: () -> Void
}

struct OverlayView: View {
    @ObservedObject var state: AppState
    let actions: OverlayActions

    var body: some View {
        ZStack {
            switch state.phase {
            case .running:
                EmptyView()
            case .booting where state.guestVisible:
                compactBar.transition(.move(edge: .bottom).combined(with: .opacity))
            default:
                fullScreen.transition(.opacity)
            }
            if let toast = state.toast {
                VStack {
                    Spacer()
                    Text(toast)
                        .font(.callout.weight(.medium))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 16).padding(.vertical, 10)
                        .background(.black.opacity(0.75), in: Capsule())
                        .padding(.bottom, 60)
                }
                .allowsHitTesting(false)
            }
        }
        .animation(.easeInOut(duration: 0.35), value: state.phase)
        .animation(.easeInOut(duration: 0.35), value: state.guestVisible)
        .sheet(isPresented: $state.showMenu) {
            ControlsSheet(state: state, actions: actions)
                .presentationDetents([.medium])
        }
        .sheet(isPresented: $state.showSettings) {
            SettingsView(state: state)
        }
    }

    private var fraction: Double? {
        switch state.phase {
        case .preparing: return state.progress
        case .booting: return state.bootFraction
        default: return nil
        }
    }

    /// Opaque screen used during setup and early boot (before Android draws anything).
    private var fullScreen: some View {
        let failed: String? = { if case .failed(let s) = state.phase { return s }; return nil }()
        return ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 0) {
                Image("BootLogo")
                    .resizable()
                    .frame(width: 88, height: 88)
                    .clipShape(RoundedRectangle(cornerRadius: 20, style: .continuous))
                    .padding(.bottom, 22)
                Text(title)
                    .font(.title2.weight(.semibold))
                    .foregroundStyle(.white)
                    .padding(.bottom, 28)
                if let failed {
                    Text(failed)
                        .font(.callout)
                        .foregroundStyle(.white.opacity(0.75))
                        .multilineTextAlignment(.center)
                        .padding(.bottom, 20)
                    Button("Settings") { state.showSettings = true }
                        .buttonStyle(.borderedProminent)
                        .tint(.white.opacity(0.2))
                } else if state.phase == .stopped {
                    Text("Close and reopen the app to start it again.")
                        .font(.callout)
                        .foregroundStyle(.white.opacity(0.7))
                } else {
                    progressBlock(width: 260)
                    if state.phase == .booting && !state.jitEnabled {
                        Text("Running without JIT, so the first start can take several minutes.")
                            .font(.footnote)
                            .foregroundStyle(.white.opacity(0.45))
                            .multilineTextAlignment(.center)
                            .frame(width: 260)
                            .padding(.top, 18)
                    }
                }
            }
            .padding(.horizontal, 32)
        }
    }

    /// Slim bar shown over Android's own boot animation.
    private var compactBar: some View {
        VStack {
            Spacer()
            progressBlock(width: 220)
                .padding(.horizontal, 20)
                .padding(.vertical, 14)
                .background(.ultraThinMaterial.opacity(0.9), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                .environment(\.colorScheme, .dark)
                .padding(.bottom, 48)
        }
        .allowsHitTesting(false)
    }

    private func progressBlock(width: CGFloat) -> some View {
        VStack(spacing: 10) {
            ProgressView(value: min(max(fraction ?? 0, 0), 1))
                .progressViewStyle(.linear)
                .tint(.white)
                .frame(width: width)
                .animation(.linear(duration: 0.25), value: fraction ?? 0)
            HStack {
                Text(state.detail.isEmpty ? "Starting" : state.detail)
                    .lineLimit(1)
                    .truncationMode(.tail)
                Spacer(minLength: 8)
                Text("\(Int(((fraction ?? 0) * 100).rounded()))%")
                    .monospacedDigit()
            }
            .font(.footnote)
            .foregroundStyle(.white.opacity(0.7))
            .frame(width: width)
        }
    }

    private var title: String {
        switch state.phase {
        case .preparing: return "Setting up Android"
        case .booting: return "Starting Android"
        case .running: return ""
        case .stopped: return "Android is off"
        case .failed: return "Android couldn\u{2019}t start"
        }
    }
}

struct ControlsSheet: View {
    @ObservedObject var state: AppState
    let actions: OverlayActions
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section("Navigation") {
                    HStack(spacing: 12) {
                        control("arrow.uturn.backward", "Back", actions.back)
                        control("circle", "Home", actions.home)
                        control("square.on.square", "Recents", actions.recents)
                    }
                }
                Section("Device") {
                    HStack(spacing: 12) {
                        control("power", "Power", actions.power)
                        control("speaker.minus", "Vol −", actions.volumeDown)
                        control("speaker.plus", "Vol +", actions.volumeUp)
                        control("rotate.right", "Rotate", actions.rotate)
                    }
                }
                Section {
                    Button("Restart Android", role: .destructive) { actions.reboot(); dismiss() }
                    if !state.jitEnabled {
                        Button("Enable JIT with StikDebug") { actions.enableJIT(); dismiss() }
                    }
                    Button("Settings") { dismiss(); DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { state.showSettings = true } }
                }
            }
            .navigationTitle("Android")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    private func control(_ icon: String, _ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: icon).font(.title3)
                Text(label).font(.caption2)
            }
            .frame(maxWidth: .infinity, minHeight: 54)
        }
        .buttonStyle(.bordered)
    }
}

struct SettingsView: View {
    @ObservedObject var state: AppState
    @State private var s = VMSettings.current
    @State private var confirmReset = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section(footer: Text("Changes apply the next time Android starts.")) {
                    Stepper("Memory: \(s.memoryMB) MB", value: $s.memoryMB, in: 2048...8192, step: 512)
                    Stepper("CPU cores: \(s.cpus)", value: $s.cpus, in: 1...ProcessInfo.processInfo.activeProcessorCount)
                    Stepper("Screen width: \(s.width) px", value: $s.width, in: 540...1440, step: 36)
                    Stepper("Density: \(s.density) dpi", value: $s.density, in: 240...560, step: 20)
                    Toggle("Use JIT when available", isOn: $s.preferJIT)
                }
                Section("Status") {
                    LabeledContent("Acceleration", value: state.jitEnabled ? "JIT (TCG)" : "Interpreter (TCTI)")
                    LabeledContent("ADB", value: ADBClient.shared.isConnected ? "Connected" : "Waiting")
                    LabeledContent("Shared folder", value: "Files › On My iPhone › Android › Android Storage")
                }
                Section {
                    Button("Factory reset Android", role: .destructive) { confirmReset = true }
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        let screen = UIScreen.main.nativeBounds.size
                        s.height = Int((Double(s.width) * Double(screen.height) / Double(screen.width)).rounded() / 2) * 2
                        s.save()
                        dismiss()
                    }
                }
            }
            .confirmationDialog("Erase all Android data?", isPresented: $confirmReset, titleVisibility: .visible) {
                Button("Erase and restart", role: .destructive) {
                    UserDefaults.standard.set(true, forKey: "pendingFactoryReset")
                    exit(0)
                }
            }
        }
    }
}
