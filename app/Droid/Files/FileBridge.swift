import Foundation

/// Two-way mirror between the app's Documents/"Android Storage" folder (visible in
/// Finder and the Files app) and the Android device's shared storage (/sdcard).
final class FileBridge {
    static let shared = FileBridge()
    static let folders = ["Download", "Documents", "DCIM", "Pictures", "Movies", "Music"]
    private let remoteRoot = "/sdcard"
    private let queue = DispatchQueue(label: "file-bridge")
    private var timer: DispatchSourceTimer?
    private var running = false

    private struct Meta: Codable, Equatable { var mtime: Int64; var size: UInt64 }
    private struct Record: Codable { var local: Meta; var remote: Meta }
    private var state: [String: Record] = [:]
    private var stateURL: URL { Paths.android.appendingPathComponent("sync-state.json") }

    func start() {
        for f in FileBridge.folders { Paths.ensure(Paths.sharedStorage.appendingPathComponent(f, isDirectory: true)) }
        if let d = try? Data(contentsOf: stateURL), let s = try? JSONDecoder().decode([String: Record].self, from: d) { state = s }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 5, repeating: 5)
        t.setEventHandler { [weak self] in self?.syncOnce() }
        t.resume()
        timer = t
    }

    func syncNow() { queue.async { self.syncOnce() } }

    private func localFiles() -> [String: Meta] {
        var out: [String: Meta] = [:]
        let root = Paths.sharedStorage
        for folder in FileBridge.folders {
            let dir = root.appendingPathComponent(folder)
            guard let e = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: [.isRegularFileKey, .contentModificationDateKey, .fileSizeKey], options: [.skipsHiddenFiles]) else { continue }
            for case let u as URL in e {
                guard let v = try? u.resourceValues(forKeys: [.isRegularFileKey, .contentModificationDateKey, .fileSizeKey]), v.isRegularFile == true else { continue }
                if u.pathExtension == "part" { continue }
                let rel = String(u.path.dropFirst(root.path.count + 1))
                out[rel] = Meta(mtime: Int64(v.contentModificationDate?.timeIntervalSince1970 ?? 0), size: UInt64(v.fileSize ?? 0))
            }
        }
        return out
    }

    private func remoteFiles() throws -> [String: Meta] {
        var out: [String: Meta] = [:]
        func walk(_ rel: String, depth: Int) throws {
            guard depth < 12 else { return }
            for e in try ADBClient.shared.list("\(remoteRoot)/\(rel)") where !e.name.hasPrefix(".") {
                let r = "\(rel)/\(e.name)"
                if e.isDirectory { try walk(r, depth: depth + 1) } else if e.isFile { out[r] = Meta(mtime: Int64(e.mtime), size: e.size) }
            }
        }
        for f in FileBridge.folders { try walk(f, depth: 0) }
        return out
    }

    private func shellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    private func syncOnce() {
        guard ADBClient.shared.isConnected, !running else { return }
        running = true
        defer { running = false }
        do {
            let local = localFiles()
            let remote = try remoteFiles()
            var changed = false
            let keys = Set(local.keys).union(remote.keys).union(state.keys)
            for rel in keys {
                let l = local[rel], r = remote[rel], s = state[rel]
                let localURL = Paths.sharedStorage.appendingPathComponent(rel)
                let remotePath = "\(remoteRoot)/\(rel)"
                switch (l, r, s) {
                case let (l?, r?, s?):
                    let lc = l != s.local, rc = r != s.remote
                    if lc && (!rc || l.mtime >= r.mtime) {
                        try ADBClient.shared.push(localURL, to: remotePath)
                    } else if rc {
                        try pull(remotePath, localURL, r)
                    } else { continue }
                    state[rel] = try refreshed(rel)
                case let (l?, r?, nil):
                    if l.size != r.size {
                        if l.mtime >= r.mtime { try ADBClient.shared.push(localURL, to: remotePath) } else { try pull(remotePath, localURL, r) }
                    }
                    state[rel] = try refreshed(rel)
                case (_?, nil, let s):
                    if let s, local[rel] == s.local {
                        try? FileManager.default.removeItem(at: localURL)      // deleted on Android
                        state[rel] = nil
                    } else {
                        try ADBClient.shared.push(localURL, to: remotePath)
                        state[rel] = try refreshed(rel)
                    }
                case (nil, let r?, let s):
                    if let s, r == s.remote {
                        _ = try? ADBClient.shared.shell("rm -f \(shellQuote(remotePath))")  // deleted on iOS
                        state[rel] = nil
                    } else {
                        try pull(remotePath, localURL, r)
                        state[rel] = try refreshed(rel)
                    }
                case (nil, nil, _?):
                    state[rel] = nil
                default:
                    continue
                }
                changed = true
            }
            if changed, let d = try? JSONEncoder().encode(state) { try? d.write(to: stateURL) }
        } catch {
            Log.info("file sync: \(error.localizedDescription)")
        }
    }

    private func pull(_ remote: String, _ local: URL, _ meta: Meta) throws {
        Paths.ensure(local.deletingLastPathComponent())
        try ADBClient.shared.pull(remote, to: local)
        try? FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: TimeInterval(meta.mtime))], ofItemAtPath: local.path)
    }

    private func refreshed(_ rel: String) throws -> Record {
        let u = Paths.sharedStorage.appendingPathComponent(rel)
        let v = try u.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
        let l = Meta(mtime: Int64(v.contentModificationDate?.timeIntervalSince1970 ?? 0), size: UInt64(v.fileSize ?? 0))
        let parent = (rel as NSString).deletingLastPathComponent
        let name = (rel as NSString).lastPathComponent
        let e = try ADBClient.shared.list("\(remoteRoot)/\(parent)").first { $0.name == name }
        let r = e.map { Meta(mtime: Int64($0.mtime), size: $0.size) } ?? l
        return Record(local: l, remote: r)
    }
}

/// Installs APKs handed over via the share sheet / "Open in", and files dropped for Android.
final class Inbox {
    static let shared = Inbox()
    private let queue = DispatchQueue(label: "inbox")
    var onMessage: ((String) -> Void)?

    func process() {
        queue.async { self.processSync() }
    }

    private func processSync() {
        guard ADBClient.shared.isConnected else { return }
        let dirs = [Paths.inbox, Paths.documents.appendingPathComponent("Inbox")]
        for dir in dirs {
            let items = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
            for u in items where !u.lastPathComponent.hasPrefix(".") {
                let ext = u.pathExtension.lowercased()
                if ["apk"].contains(ext) {
                    report("Installing \(u.lastPathComponent)…")
                    do {
                        _ = try ADBClient.shared.install(apk: u)
                        report("Installed \(u.deletingPathExtension().lastPathComponent)")
                        try? FileManager.default.removeItem(at: u)
                    } catch {
                        report("Install failed: \(error.localizedDescription)")
                        try? FileManager.default.removeItem(at: u)
                    }
                } else if ["apks", "xapk", "apkm"].contains(ext) {
                    report("Installing \(u.lastPathComponent)…")
                    do {
                        try installSplits(u)
                        report("Installed \(u.deletingPathExtension().lastPathComponent)")
                    } catch {
                        report("Install failed: \(error.localizedDescription)")
                    }
                    try? FileManager.default.removeItem(at: u)
                } else {
                    // Any other shared file lands in Android's Download folder.
                    let dest = Paths.sharedStorage.appendingPathComponent("Download").appendingPathComponent(u.lastPathComponent)
                    try? FileManager.default.removeItem(at: dest)
                    try? FileManager.default.moveItem(at: u, to: dest)
                    report("Copied \(u.lastPathComponent) to Downloads")
                    FileBridge.shared.syncNow()
                }
            }
        }
    }

    /// Split APK bundles (.apks/.xapk): install every contained .apk in one session.
    private func installSplits(_ bundle: URL) throws {
        let tmp = Paths.android.appendingPathComponent("splits-\(UUID().uuidString)")
        Paths.ensure(tmp)
        defer { try? FileManager.default.removeItem(at: tmp) }
        let zip = try ZipReader(url: bundle)
        var apks: [URL] = []
        for e in zip.entries where e.name.lowercased().hasSuffix(".apk") && !e.name.contains("/") {
            let dest = tmp.appendingPathComponent((e.name as NSString).lastPathComponent)
            try zip.extract(e, to: dest) { _ in }
            apks.append(dest)
        }
        guard !apks.isEmpty else { throw ADBClient.ADBError.failed("No APKs inside bundle") }
        let adb = ADBClient.shared
        let total = apks.reduce(0) { $0 + ((try? FileManager.default.attributesOfItem(atPath: $1.path)[.size] as? Int) ?? 0) }
        let created = try adb.shell("cmd package install-create -r -t -S \(total)")
        guard let open = created.firstIndex(of: "["), let close = created.firstIndex(of: "]") else {
            throw ADBClient.ADBError.failed(created)
        }
        let session = String(created[created.index(after: open)..<close])
        for (i, apk) in apks.enumerated() {
            let size = (try FileManager.default.attributesOfItem(atPath: apk.path)[.size] as? Int) ?? 0
            let s = try adb.open("exec:cmd package install-write -S \(size) \(session) \(i)_\(apk.lastPathComponent) -")
            let fh = try FileHandle(forReadingFrom: apk)
            while let c = try fh.read(upToCount: 1 << 20), !c.isEmpty { try s.write(c) }
            try fh.close()
            _ = s.readAll()
        }
        let result = try adb.shell("cmd package install-commit \(session)")
        guard result.contains("Success") else { throw ADBClient.ADBError.failed(result) }
    }

    private func report(_ s: String) {
        Log.info("inbox: \(s)")
        DispatchQueue.main.async { self.onMessage?(s) }
    }
}
