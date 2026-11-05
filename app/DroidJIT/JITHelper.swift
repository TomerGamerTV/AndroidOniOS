import ExtensionFoundation
import Foundation

/// ExtensionKit helper that enables JIT for the main app with StikJIT. A process cannot
/// debug itself, so the debugger session has to live in a separate process.
@main
final class JITHelper: AppExtension {
    required init() {}

    @AppExtensionPoint.Bind
    var boundExtensionPoint: AppExtensionPoint {
        AppExtensionPoint.Identifier(host: "com.tomer.androidonios", name: "jit-helper")
    }

    var configuration: some AppExtensionConfiguration {
        ConnectionHandler { connection in
            connection.exportedInterface = NSXPCInterface(with: DroidJITProtocol.self)
            connection.exportedObject = JITService()
            connection.invalidationHandler = { exit(0) }
            connection.resume()
            return true
        }
    }
}

final class JITService: NSObject, DroidJITProtocol {
    // StikJIT calls must run synchronously on one serial queue (see its INTEGRATION.md).
    private let queue = DispatchQueue(label: "stikjit")

    private var ddiPaths: DDIPaths {
        let lib = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
        try? FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        return DDIPaths.default(in: lib)
    }

    private func withPairingFile<T>(_ data: Data, _ body: (URL) throws -> T) throws -> T {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("pairing-\(UUID().uuidString).plist")
        try data.write(to: url, options: .completeFileProtection)
        defer { try? FileManager.default.removeItem(at: url) }
        return try body(url)
    }

    func prepare(pairingFile: Data, reply: @escaping (Bool, String) -> Void) {
        queue.async {
            do {
                let readiness = try self.withPairingFile(pairingFile) { url in
                    StikJIT.prepareDevice(pairingFile: url, paths: self.ddiPaths)
                }
                switch readiness {
                case .ready: reply(true, "ready")
                case .unreachable(let r): reply(false, "Device tunnel unreachable: \(r). Is LocalDevVPN connected?")
                case .preparationFailed(let r): reply(false, r)
                }
            } catch {
                reply(false, error.localizedDescription)
            }
        }
    }

    func enableJIT(pid: Int32, pairingFile: Data, reply: @escaping (Bool, String) -> Void) {
        queue.async {
            JITLog.reset()
            var log: [String] = []
            do {
                try self.withPairingFile(pairingFile) { url in
                    try StikJIT.enableJIT(targetPID: pid, pairingFile: url, ddiPaths: self.ddiPaths,
                                          script: .universal,
                                          progress: { line in
                                              log.append(line); if log.count > 50 { log.removeFirst() }
                                              JITLog.write(line)
                                          })
                }
                reply(true, log.suffix(5).joined(separator: "\n"))
            } catch {
                reply(false, "\(error.localizedDescription)\n" + log.suffix(5).joined(separator: "\n"))
            }
        }
    }
}

/// Progress log shared with the app (App Group: jit.log) so JIT failures can be diagnosed.
enum JITLog {
    private static let url = FileManager.default
        .containerURL(forSecurityApplicationGroupIdentifier: "group.com.tomer.androidonios")?
        .appendingPathComponent("jit.log")

    static func reset() {
        guard let url else { return }
        try? Data().write(to: url)
    }

    static func write(_ line: String) {
        guard let url, let h = try? FileHandle(forWritingTo: url) else { return }
        defer { try? h.close() }
        h.seekToEndOfFile()
        h.write(Data("\(Date()) \(line)\n".utf8))
    }
}
