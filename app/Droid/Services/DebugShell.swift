import Foundation
import Network

/// Developer-only shell relay: when Library/Android/debug.json has `"debugShell": true`, the app
/// listens on TCP 4445 and runs each received line as an Android shell command (through the
/// app's own authenticated ADB connection), replying with its output. Off by default.
final class DebugShell {
    static let shared = DebugShell()
    private var listener: NWListener?

    func startIfEnabled() {
        guard listener == nil,
              let d = try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json")),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              j["debugShell"] as? Bool == true else { return }
        do {
            let l = try NWListener(using: .tcp, on: 4445)
            l.newConnectionHandler = { [weak self] in self?.handle($0) }
            l.start(queue: .global(qos: .utility))
            listener = l
            Log.info("debug shell listening on :4445")
            startFileChannel()
        } catch {
            Log.info("debug shell: \(error)")
        }
    }

    private func handle(_ c: NWConnection) {
        c.start(queue: .global(qos: .utility))
        var buf = Data()
        func read() {
            c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, done, err in
                if let data { buf.append(data) }
                if let nl = buf.firstIndex(of: 10) {
                    let cmd = String(decoding: buf[..<nl], as: UTF8.self)
                    let out: String
                    do { out = try ADBClient.shared.shell(cmd) } catch { out = "error: \(error)\n" }
                    c.send(content: Data(out.utf8), completion: .contentProcessed { _ in c.cancel() })
                } else if done || err != nil {
                    c.cancel()
                } else {
                    read()
                }
            }
        }
        read()
    }

    /// The same, through files (works whenever `devicectl device copy` does, no networking):
    /// write Library/Android/debug-cmd, read debug-cmd.out once it appears. A line starting with
    /// "monitor " goes to QEMU's monitor (when debug.json enables it on port 4444) instead.
    private func startFileChannel() {
        let cmdURL = Paths.android.appendingPathComponent("debug-cmd")
        let outURL = Paths.android.appendingPathComponent("debug-cmd.out")
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler {
            guard let cmd = try? String(contentsOf: cmdURL, encoding: .utf8) else { return }
            try? FileManager.default.removeItem(at: cmdURL)
            var out = ""
            for line in cmd.split(separator: "\n").map(String.init) where !line.isEmpty {
                if line.hasPrefix("monitor ") { out += Self.monitor(String(line.dropFirst(8))) }
                else if line == "app save-and-quit" {   // the menu's Save & close
                    DispatchQueue.main.async { VirtualMachine.shared.saveAndQuit { ok in Log.info("save-and-quit: \(ok)") } }
                    out += "saving\n"
                }
                else { do { out += try ADBClient.shared.shell(line) } catch { out += "error: \(error)\n" } }
            }
            try? out.write(to: outURL, atomically: true, encoding: .utf8)
        }
        timer.resume()
        fileTimer = timer
    }
    private var fileTimer: DispatchSourceTimer?

    /// One QEMU monitor command over 127.0.0.1:4444; returns what QEMU printed.
    static func monitor(_ cmd: String) -> String {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return "monitor: socket failed\n" }
        defer { close(fd) }
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(4444).bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let ok = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard ok == 0 else { return "monitor: not listening (debug.json extraArgs -monitor tcp:...:4444)\n" }
        var tv = timeval(tv_sec: 2, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        let line = cmd + "\n"
        _ = line.withCString { write(fd, $0, strlen($0)) }
        var data = Data()
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = read(fd, &buf, buf.count)
            if n <= 0 { break }
            data.append(buf, count: n)
        }
        return String(decoding: data, as: UTF8.self)
    }
}
