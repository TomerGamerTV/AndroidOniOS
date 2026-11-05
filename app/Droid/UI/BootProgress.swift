import Foundation

/// Estimates boot progress from real milestones: kernel/init markers in the serial log,
/// then Android's own `boot_progress_*` events (read over ADB), then boot completion.
/// Between milestones the bar creeps toward the next one so it never looks frozen.
final class BootProgress {
    struct Milestone { let match: String; let fraction: Double; let label: String }

    private static let kernelMilestones: [Milestone] = [
        .init(match: "virtio-pci", fraction: 0.03, label: "Starting kernel"),
        .init(match: "init: Loaded kernel module", fraction: 0.07, label: "Starting init"),
        .init(match: "init: Switching root", fraction: 0.11, label: "Mounting system"),
        .init(match: "init second stage started", fraction: 0.15, label: "Starting system services"),
        .init(match: "processing action (post-fs-data)", fraction: 0.20, label: "Unlocking storage"),
        .init(match: "starting service 'zygote", fraction: 0.26, label: "Starting Android runtime"),
    ]

    private static let androidMilestones: [Milestone] = [
        .init(match: "boot_progress_start", fraction: 0.32, label: "Starting Android runtime"),
        .init(match: "boot_progress_preload_start", fraction: 0.36, label: "Loading framework"),
        .init(match: "boot_progress_preload_end", fraction: 0.50, label: "Loading framework"),
        .init(match: "boot_progress_system_run", fraction: 0.54, label: "Starting system"),
        .init(match: "boot_progress_pms_start", fraction: 0.57, label: "Scanning apps"),
        .init(match: "boot_progress_pms_scan_end", fraction: 0.68, label: "Scanning apps"),
        .init(match: "boot_progress_pms_ready", fraction: 0.72, label: "Starting services"),
        .init(match: "boot_progress_ams_ready", fraction: 0.84, label: "Starting home screen"),
        .init(match: "boot_progress_enable_screen", fraction: 0.94, label: "Almost ready"),
    ]

    private(set) var reached: Double = 0      // last milestone reached
    private(set) var displayed: Double = 0    // smoothed value shown in the UI
    private(set) var label = "Starting Android"
    private var kernelOffset: UInt64 = 0
    private var lastAdbPoll = Date.distantPast
    private var done = false
    var onChange: ((Double, String) -> Void)?
    /// Called once if the guest crash-loops during boot (kernel panic / init failure).
    var onFailure: ((String) -> Void)?
    private var panics = 0
    private var timer: Timer?

    func start(kernelLog: URL) {
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.tick(kernelLog: kernelLog)
        }
        NotificationCenter.default.addObserver(forName: .androidBootCompleted, object: nil, queue: .main) { [weak self] _ in
            self?.finish()
        }
    }

    func finish() {
        guard !done else { return }
        done = true
        reached = 1; displayed = 1; label = "Ready"
        onChange?(1, label)
        timer?.invalidate()
    }

    private func advance(to m: Milestone) {
        if m.fraction > reached { reached = m.fraction; label = m.label }
    }

    /// The guest framebuffer showing up means the boot animation is on screen.
    func firstFrame() {
        advance(to: .init(match: "", fraction: 0.30, label: "Starting Android runtime"))
    }

    private func tick(kernelLog: URL) {
        guard !done else { return }
        scanKernelLog(kernelLog)
        if ADBClient.shared.isConnected, Date().timeIntervalSince(lastAdbPoll) > 2 {
            lastAdbPoll = Date()
            DispatchQueue.global(qos: .utility).async { self.pollAndroid() }
        }
        // Creep toward (but not past) the next milestone.
        let next = (Self.kernelMilestones + Self.androidMilestones).map(\.fraction).first { $0 > reached } ?? 0.99
        let ceiling = max(reached, next - 0.01)
        if displayed < reached { displayed += (reached - displayed) * 0.3 }
        else if displayed < ceiling { displayed += (ceiling - displayed) * 0.004 }
        onChange?(displayed, label)
    }

    private func scanKernelLog(_ url: URL) {
        guard let fh = try? FileHandle(forReadingFrom: url) else { return }
        defer { try? fh.close() }
        let end = (try? fh.seekToEnd()) ?? 0
        if end < kernelOffset { kernelOffset = 0 }   // guest rebooted, log restarted
        guard end > kernelOffset else { return }
        try? fh.seek(toOffset: kernelOffset)
        guard let data = try? fh.read(upToCount: Int(min(end - kernelOffset, 4 << 20))) else { return }
        kernelOffset += UInt64(data.count)
        let text = String(decoding: data, as: UTF8.self)
        for m in Self.kernelMilestones where text.contains(m.match) { advance(to: m) }
        if text.contains("Kernel panic") {
            panics += 1
            let reason: String
            if let r = text.range(of: "CANNOT LINK EXECUTABLE") {
                let line = text[r.lowerBound...].prefix { $0 != "\n" }
                reason = "Android's init could not start (\(line.prefix(80)))."
            } else {
                reason = "The Android kernel crashed while starting."
            }
            Log.info("boot failure #\(panics): \(reason)")
            if panics >= 2 {
                done = true
                timer?.invalidate()
                onFailure?(reason + " This build's no-JIT CPU interpreter is the likely cause; enabling JIT avoids it.")
            }
        }
    }

    private func pollAndroid() {
        guard let events = try? ADBClient.shared.shell("logcat -b events -d -s boot_progress_start boot_progress_preload_start boot_progress_preload_end boot_progress_system_run boot_progress_pms_start boot_progress_pms_scan_end boot_progress_pms_ready boot_progress_ams_ready boot_progress_enable_screen; getprop sys.boot_completed") else { return }
        DispatchQueue.main.async {
            for m in Self.androidMilestones where events.contains(m.match + ":") { self.advance(to: m) }
            if events.split(separator: "\n").last?.trimmingCharacters(in: .whitespaces) == "1" { self.finish() }
        }
    }
}
