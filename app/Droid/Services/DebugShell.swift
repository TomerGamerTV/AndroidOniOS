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
}
