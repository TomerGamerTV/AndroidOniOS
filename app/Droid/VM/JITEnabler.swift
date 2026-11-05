import ExtensionFoundation
import Foundation

@available(iOS 26, *)
extension AppExtensionPoint {
    /// Hosts the helper that enables JIT for this app through StikJIT.
    @Definition
    static var jitHelper: AppExtensionPoint {
        Name("jit-helper")
        UserInterface(false)
    }
}

/// Enables JIT for the running app via the bundled StikJIT helper extension.
///
/// Requirements on the iPhone: Developer Mode, a pairing file for this device
/// (Files › Android › pairingFile.plist) and the loopback tunnel (bundled DroidTunnel or LocalDevVPN).
final class JITEnabler {
    static let shared = JITEnabler()
    private var process: Any?
    private var connection: NSXPCConnection?

    /// Candidate locations for the pairing file the user drops in via Finder/Files.
    var pairingFileURL: URL? {
        let candidates = [
            Paths.documents.appendingPathComponent("pairingFile.plist"),
            Paths.documents.appendingPathComponent("StikJIT/pairingFile.plist"),
            Paths.documents.appendingPathComponent("rp_pairing_file.plist"),
            Paths.android.appendingPathComponent("pairingFile.plist"),
        ]
        if let found = candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) }) { return found }
        let docs = (try? FileManager.default.contentsOfDirectory(at: Paths.documents, includingPropertiesForKeys: nil)) ?? []
        return docs.first { ["plist", "mobiledevicepairing"].contains($0.pathExtension.lowercased()) && $0.lastPathComponent.lowercased().contains("pair") }
    }

    /// Starts the helper and asks it to attach. Returns when the helper replies (after the
    /// app has prepared its JIT region and detached) or fails. Call `waitForAttach` to know
    /// when it's safe to start QEMU.
    func start(completion: @escaping (Bool, String) -> Void) {
        guard #available(iOS 26, *) else { completion(false, "Requires iOS 26"); return }
        guard let url = pairingFileURL, let data = try? Data(contentsOf: url) else {
            completion(false, "No pairing file. Put pairingFile.plist in Files › Android.")
            return
        }
        if let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
           plist["public_key"] == nil, plist["HostCertificate"] != nil {
            completion(false, "Old-style pairing file. JIT on iOS 26+ needs a RemotePairing file (scripts/make-pairing.sh).")
            return
        }
        Task {
            do {
                let monitor = try await AppExtensionPoint.Monitor(appExtensionPoint: .jitHelper)
                guard let identity = monitor.identities.first else {
                    completion(false, "JIT helper extension not found"); return
                }
                let config = AppExtensionProcess.Configuration(appExtensionIdentity: identity) {
                    Log.info("jit helper interrupted")
                }
                let proc = try await AppExtensionProcess(configuration: config)
                let conn = try proc.makeXPCConnection()
                conn.remoteObjectInterface = NSXPCInterface(with: DroidJITProtocol.self)
                conn.resume()
                self.process = proc
                self.connection = conn
                let proxy = conn.remoteObjectProxyWithErrorHandler { err in
                    completion(false, "JIT helper failed: \(err.localizedDescription)")
                } as! DroidJITProtocol
                proxy.enableJIT(pid: getpid(), pairingFile: data) { ok, msg in
                    Log.info("jit helper: ok=\(ok) \(msg)")
                    completion(ok, msg)
                }
            } catch {
                completion(false, "JIT helper: \(error.localizedDescription)")
            }
        }
    }

    /// Polls until a debugger is attached (StikJIT's script is ready for brk requests).
    func waitForAttach(timeout: TimeInterval, cancel: DispatchSemaphore? = nil) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if droid_bridge_debugger_attached() && droid_bridge_jit_available() { return true }
            if cancel?.wait(timeout: .now() + 0.2) == .success { return false }
            if cancel == nil { Thread.sleep(forTimeInterval: 0.2) }
        }
        return false
    }
}
