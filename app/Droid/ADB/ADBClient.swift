import Foundation

/// Minimal ADB host implementation talking to the guest adbd over the vsock bridge.
final class ADBClient {
    static let shared = ADBClient()

    enum ADBError: Error, LocalizedError {
        case notConnected, closed, failed(String)
        var errorDescription: String? {
            switch self {
            case .notConnected: return "Android is not reachable over ADB yet"
            case .closed: return "ADB stream closed"
            case .failed(let s): return s
            }
        }
    }

    fileprivate enum Cmd: UInt32 {
        case cnxn = 0x4e584e43, auth = 0x48545541, open = 0x4e45504f
        case okay = 0x59414b4f, clse = 0x45534c43, wrte = 0x45545257
    }

    final class Stream {
        let localId: UInt32
        var remoteId: UInt32 = 0
        fileprivate var inbox = Data()
        fileprivate var closed = false
        fileprivate var opened = false
        fileprivate var canWrite = false
        fileprivate let cond = NSCondition()
        fileprivate weak var client: ADBClient?
        init(localId: UInt32) { self.localId = localId }

        /// Writes data, honouring adb's one-in-flight WRTE flow control.
        func write(_ data: Data) throws {
            guard let client else { throw ADBError.closed }
            var off = 0
            while off < data.count {
                cond.lock()
                while !canWrite && !closed { cond.wait() }
                if closed { cond.unlock(); throw ADBError.closed }
                canWrite = false
                cond.unlock()
                let n = min(data.count - off, client.maxPayload)
                try client.send(.wrte, localId, remoteId, data.subdata(in: off..<(off + n)))
                off += n
            }
        }

        /// Reads exactly n bytes (or returns nil on close).
        func read(exactly n: Int) -> Data? {
            cond.lock(); defer { cond.unlock() }
            while inbox.count < n && !closed { cond.wait() }
            guard inbox.count >= n else { return nil }
            let d = inbox.prefix(n)
            inbox.removeFirst(n)
            return Data(d)
        }

        /// Reads everything until the remote closes.
        func readAll() -> Data {
            cond.lock(); defer { cond.unlock() }
            while !closed { cond.wait() }
            let d = inbox
            inbox.removeAll()
            return d
        }

        func close() {
            guard let client else { return }
            cond.lock()
            let was = closed
            closed = true
            cond.broadcast()
            cond.unlock()
            if !was { try? client.send(.clse, localId, remoteId, Data()) }
            client.forget(self)
        }
    }

    private var fd: Int32 = -1
    private let wlock = NSLock()
    private let state = NSLock()
    private var streams: [UInt32: Stream] = [:]
    private var nextId: UInt32 = 1
    private(set) var maxPayload = 256 * 1024
    private(set) var connected = false
    private var connectCond = NSCondition()

    var isConnected: Bool { connected }

    // MARK: - Connection

    /// Connects (retrying) until adbd answers and authenticates, or the timeout passes.
    func connect(timeout: TimeInterval = 600) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if connected { return true }
            if tryConnect() { return true }
            Thread.sleep(forTimeInterval: 2)
        }
        return false
    }

    private func tryConnect() -> Bool {
        let s = droid_bridge_unix_connect(Paths.adbSocket)
        guard s >= 0 else { return false }
        fd = s
        let banner = Data("host::features=shell_v2,cmd,stat_v2,ls_v2,fixed_push_mkdir,abb,abb_exec,sendrecv_v2\0".utf8)
        do {
            try send(.cnxn, 0x01000001, UInt32(maxPayload), banner)
            var sentSig = false
            while true {
                guard let (cmd, a0, a1, payload) = try readPacket(timeout: 15) else { throw ADBError.closed }
                if cmd == Cmd.auth.rawValue && a0 == 1 {
                    if !sentSig, let sig = ADBKey.shared.sign(token: payload) {
                        sentSig = true
                        try send(.auth, 2, 0, sig)
                    } else {
                        // Key not trusted: offer the public key (shows the RSA prompt in Android).
                        try send(.auth, 3, 0, Data((ADBKey.shared.androidPublicKey + "\0").utf8))
                    }
                } else if cmd == Cmd.cnxn.rawValue {
                    maxPayload = min(Int(a1), 256 * 1024)
                    _ = a0
                    break
                }
            }
        } catch {
            close(fd); fd = -1
            return false
        }
        connected = true
        Log.info("adb connected")
        let t = Thread { self.readLoop() }
        t.name = "adb-reader"
        t.start()
        return true
    }

    private func readLoop() {
        while true {
            guard let pkt = try? readPacket(timeout: nil) else { break }
            let (cmd, a0, a1, payload) = pkt
            state.lock()
            let s = streams[a1]
            state.unlock()
            switch Cmd(rawValue: cmd) {
            case .okay:
                guard let s else { continue }
                s.cond.lock()
                s.remoteId = a0
                s.opened = true
                s.canWrite = true
                s.cond.broadcast()
                s.cond.unlock()
            case .wrte:
                guard let s else { continue }
                s.cond.lock()
                s.inbox.append(payload)
                s.cond.broadcast()
                s.cond.unlock()
                try? send(.okay, s.localId, a0, Data())
            case .clse:
                guard let s else { continue }
                s.cond.lock()
                s.closed = true
                s.cond.broadcast()
                s.cond.unlock()
                forget(s)
            default:
                break
            }
        }
        connected = false
        close(fd); fd = -1
        state.lock()
        let all = streams.values
        streams.removeAll()
        state.unlock()
        for s in all { s.cond.lock(); s.closed = true; s.cond.broadcast(); s.cond.unlock() }
        Log.info("adb disconnected")
    }

    fileprivate func forget(_ s: Stream) {
        state.lock(); streams[s.localId] = nil; state.unlock()
    }

    fileprivate func send(_ cmd: Cmd, _ a0: UInt32, _ a1: UInt32, _ payload: Data) throws {
        var hdr = Data(capacity: 24)
        for v in [cmd.rawValue, a0, a1, UInt32(payload.count), 0, cmd.rawValue ^ 0xffffffff] {
            withUnsafeBytes(of: v.littleEndian) { hdr.append(contentsOf: $0) }
        }
        wlock.lock(); defer { wlock.unlock() }
        guard fd >= 0 else { throw ADBError.notConnected }
        try writeAll(hdr)
        if !payload.isEmpty { try writeAll(payload) }
    }

    private func writeAll(_ d: Data) throws {
        try d.withUnsafeBytes { raw in
            var off = 0
            while off < d.count {
                let r = Darwin.write(fd, raw.baseAddress! + off, d.count - off)
                if r <= 0 { throw ADBError.closed }
                off += r
            }
        }
    }

    private func readExact(_ n: Int, timeout: TimeInterval?) throws -> Data {
        var buf = Data(count: n)
        var got = 0
        while got < n {
            if let timeout {
                var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                if poll(&pfd, 1, Int32(timeout * 1000)) <= 0 { throw ADBError.closed }
            }
            let r = buf.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress! + got, n - got) }
            if r <= 0 { throw ADBError.closed }
            got += r
        }
        return buf
    }

    private func readPacket(timeout: TimeInterval?) throws -> (UInt32, UInt32, UInt32, Data)? {
        let h = try readExact(24, timeout: timeout)
        let w = h.withUnsafeBytes { raw in (0..<6).map { raw.loadUnaligned(fromByteOffset: $0 * 4, as: UInt32.self).littleEndian } }
        let len = Int(w[3])
        let payload = len > 0 ? try readExact(len, timeout: timeout) : Data()
        return (w[0], w[1], w[2], payload)
    }

    // MARK: - Streams

    func open(_ service: String) throws -> Stream {
        guard connected else { throw ADBError.notConnected }
        state.lock()
        let s = Stream(localId: nextId)
        nextId += 1
        s.client = self
        streams[s.localId] = s
        state.unlock()
        try send(.open, s.localId, 0, Data((service + "\0").utf8))
        s.cond.lock()
        let deadline = Date().addingTimeInterval(20)
        while !s.opened && !s.closed && Date() < deadline { s.cond.wait(until: deadline) }
        let ok = s.opened
        s.cond.unlock()
        guard ok else { forget(s); throw ADBError.failed("could not open \(service)") }
        return s
    }

    @discardableResult
    func shell(_ command: String) throws -> String {
        let s = try open("exec:" + command)
        let out = s.readAll()
        return String(decoding: out, as: UTF8.self)
    }

    /// Installs an APK by streaming it to `cmd package install -S`.
    func install(apk url: URL) throws -> String {
        let size = (try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.intValue ?? 0
        let s = try open("exec:cmd package install -r -t -S \(size)")
        let fh = try FileHandle(forReadingFrom: url)
        defer { try? fh.close() }
        while let chunk = try fh.read(upToCount: 1 << 20), !chunk.isEmpty {
            try s.write(chunk)
        }
        let result = String(decoding: s.readAll(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard result.contains("Success") else { throw ADBError.failed(result.isEmpty ? "Install failed" : result) }
        return result
    }

    // MARK: - Sync protocol

    struct RemoteEntry { let name: String; let mode: UInt32; let size: UInt64; let mtime: UInt32
        var isDirectory: Bool { mode & 0o170000 == 0o040000 }
        var isFile: Bool { mode & 0o170000 == 0o100000 }
    }

    private func syncRequest(_ s: Stream, _ id: String, _ payload: Data) throws {
        var d = Data(id.utf8)
        withUnsafeBytes(of: UInt32(payload.count).littleEndian) { d.append(contentsOf: $0) }
        d.append(payload)
        try s.write(d)
    }

    private func u32(_ d: Data, _ o: Int) -> UInt32 { d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt32.self).littleEndian } }

    func list(_ path: String) throws -> [RemoteEntry] {
        let s = try open("sync:")
        defer { try? syncRequest(s, "QUIT", Data()); s.close() }
        try syncRequest(s, "LIST", Data(path.utf8))
        var out: [RemoteEntry] = []
        while let h = s.read(exactly: 20) {
            let id = String(decoding: h.prefix(4), as: UTF8.self)
            if id == "DONE" { break }
            guard id == "DENT" else { throw ADBError.failed("bad LIST reply") }
            let nameLen = Int(u32(h, 16))
            guard let name = s.read(exactly: nameLen) else { break }
            let n = String(decoding: name, as: UTF8.self)
            if n == "." || n == ".." { continue }
            out.append(RemoteEntry(name: n, mode: u32(h, 4), size: UInt64(u32(h, 8)), mtime: u32(h, 12)))
        }
        return out
    }

    func push(_ local: URL, to remote: String, mode: UInt32 = 0o644) throws {
        let s = try open("sync:")
        defer { try? syncRequest(s, "QUIT", Data()); s.close() }
        try syncRequest(s, "SEND", Data("\(remote),\(mode | 0o100000)".utf8))
        let fh = try FileHandle(forReadingFrom: local)
        defer { try? fh.close() }
        while let chunk = try fh.read(upToCount: 64 * 1024), !chunk.isEmpty {
            try syncRequest(s, "DATA", chunk)
        }
        let mtime = (try? local.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date()
        var done = Data("DONE".utf8)
        withUnsafeBytes(of: UInt32(mtime.timeIntervalSince1970).littleEndian) { done.append(contentsOf: $0) }
        try s.write(done)
        guard let r = s.read(exactly: 8) else { throw ADBError.closed }
        if String(decoding: r.prefix(4), as: UTF8.self) != "OKAY" {
            let msg = s.read(exactly: Int(u32(r, 4))).map { String(decoding: $0, as: UTF8.self) } ?? "push failed"
            throw ADBError.failed(msg)
        }
    }

    func pull(_ remote: String, to local: URL) throws {
        let s = try open("sync:")
        defer { try? syncRequest(s, "QUIT", Data()); s.close() }
        try syncRequest(s, "RECV", Data(remote.utf8))
        let tmp = local.appendingPathExtension("part")
        FileManager.default.createFile(atPath: tmp.path, contents: nil)
        let fh = try FileHandle(forWritingTo: tmp)
        defer { try? fh.close() }
        while let h = s.read(exactly: 8) {
            let id = String(decoding: h.prefix(4), as: UTF8.self)
            let n = Int(u32(h, 4))
            if id == "DONE" { break }
            if id == "FAIL" {
                let msg = s.read(exactly: n).map { String(decoding: $0, as: UTF8.self) } ?? "pull failed"
                try? FileManager.default.removeItem(at: tmp)
                throw ADBError.failed(msg)
            }
            guard id == "DATA", let chunk = s.read(exactly: n) else { throw ADBError.closed }
            try fh.write(contentsOf: chunk)
        }
        try fh.close()
        try? FileManager.default.removeItem(at: local)
        try FileManager.default.moveItem(at: tmp, to: local)
    }
}
