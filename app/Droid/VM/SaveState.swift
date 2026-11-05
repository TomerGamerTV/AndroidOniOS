import Foundation

/// Save states ("quick resume"): the whole Android machine (memory, devices and the GPU
/// renderer's state) is written to disk when the app leaves the foreground, so the next launch
/// continues where it left off in seconds instead of booting Android again.
///
/// A save is only valid with the exact VM configuration and disk contents it was taken with.
/// The fingerprint covers the QEMU configuration and the app build; the disk images' sizes and
/// modification times are recorded after the save and must be unchanged when restoring. Once
/// Android runs again its disks change, so the save is deleted before the VM continues.
enum SaveState {
    static var enabled: Bool { UserDefaults.standard.object(forKey: "saveState") as? Bool ?? true }

    static var dir: URL {
        let url = Paths.ensure(Paths.android.appendingPathComponent("savestate", isDirectory: true))
        var u = url
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? u.setResourceValues(values)
        return url
    }
    static var file: URL { dir.appendingPathComponent("state.bin") }
    static var partial: URL { dir.appendingPathComponent("state.partial") }
    private static var metaURL: URL { dir.appendingPathComponent("state.json") }
    /// Present while a restore runs. Finding it at launch means that restore crashed.
    static var attemptMarker: URL { dir.appendingPathComponent("restore-attempt") }

    private struct Meta: Codable {
        var fingerprint: String
        var disks: [String: String]
        var saved: Date
    }

    /// Identifies the virtual machine a save belongs to.
    static func fingerprint(args: [String], features: String) -> String {
        // App container paths change when the app is updated and don't affect the machine; the
        // accelerator (JIT or interpreter) doesn't either.
        let home = NSHomeDirectory()
        var parts: [String] = []
        var skip = false
        for a in args {
            if skip { skip = false; continue }
            if a == "-accel" { skip = true; continue }
            parts.append(a.replacingOccurrences(of: home, with: "~"))
        }
        // Any new build may change QEMU or gfxstream's saved formats.
        if let exe = Bundle.main.executableURL { parts.append("app:" + stamp(exe)) }
        parts.append("features:" + features)
        return parts.joined(separator: "\u{1f}")
    }

    /// "size:mtime" with nanosecond precision.
    private static func stamp(_ url: URL) -> String {
        var st = stat()
        guard stat(url.path, &st) == 0 else { return "missing" }
        return "\(st.st_size):\(st.st_mtimespec.tv_sec).\(st.st_mtimespec.tv_nsec)"
    }

    private static func diskStamps(_ disks: [URL]) -> [String: String] {
        Dictionary(uniqueKeysWithValues: disks.map { ($0.lastPathComponent, stamp($0)) })
    }

    /// True if a save exists that matches this machine and its disks.
    static func restorable(fingerprint: String, disks: [URL]) -> Bool {
        guard enabled else { discard(); return false }
        let fm = FileManager.default
        if fm.fileExists(atPath: attemptMarker.path) {
            Log.info("save state: the last restore did not finish; discarding it")
            discard()
            return false
        }
        guard fm.fileExists(atPath: file.path) else { return false }
        guard let data = try? Data(contentsOf: metaURL),
              let meta = try? JSONDecoder().decode(Meta.self, from: data) else {
            discard()
            return false
        }
        if meta.fingerprint != fingerprint {
            Log.info("save state: VM configuration or app build changed; discarding it")
            discard()
            return false
        }
        if meta.disks != diskStamps(disks) {
            Log.info("save state: disks changed since the save; discarding it")
            discard()
            return false
        }
        return true
    }

    /// Makes a finished save the current one.
    static func commit(fingerprint: String, disks: [URL]) {
        let fm = FileManager.default
        try? fm.removeItem(at: file)
        do {
            try fm.moveItem(at: partial, to: file)
            let meta = Meta(fingerprint: fingerprint, disks: diskStamps(disks), saved: Date())
            try JSONEncoder().encode(meta).write(to: metaURL, options: .atomic)
        } catch {
            Log.info("save state: commit failed: \(error)")
            discard()
        }
    }

    static func discard() {
        let fm = FileManager.default
        for u in [metaURL, file, partial, attemptMarker] { try? fm.removeItem(at: u) }
    }

    static var exists: Bool { FileManager.default.fileExists(atPath: file.path) }
}
