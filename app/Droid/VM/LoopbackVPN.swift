import NetworkExtension

/// Installs and starts the bundled loopback tunnel (DroidTunnel) that StikJIT needs to reach
/// 10.7.0.1. iOS asks once to "Add VPN Configurations".
final class LoopbackVPN {
    static let shared = LoopbackVPN()
    private let providerID = "com.tomer.androidonios.tunnel"

    func start(timeout: TimeInterval = 15, completion: @escaping (Bool, String) -> Void) {
        NETunnelProviderManager.loadAllFromPreferences { managers, error in
            let manager = managers?.first {
                ($0.protocolConfiguration as? NETunnelProviderProtocol)?.providerBundleIdentifier == self.providerID
            } ?? NETunnelProviderManager()
            let proto = NETunnelProviderProtocol()
            proto.providerBundleIdentifier = self.providerID
            proto.serverAddress = "On-device JIT tunnel"
            manager.protocolConfiguration = proto
            manager.localizedDescription = "Android JIT"
            manager.isEnabled = true
            manager.saveToPreferences { error in
                if let error {
                    completion(false, "VPN configuration not allowed: \(error.localizedDescription)")
                    return
                }
                manager.loadFromPreferences { _ in
                    do {
                        if manager.connection.status != .connected {
                            try manager.connection.startVPNTunnel()
                        }
                    } catch {
                        completion(false, "Could not start tunnel: \(error.localizedDescription)")
                        return
                    }
                    let deadline = Date().addingTimeInterval(timeout)
                    func poll() {
                        if manager.connection.status == .connected { completion(true, "connected"); return }
                        if Date() > deadline { completion(false, "Tunnel did not connect"); return }
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: poll)
                    }
                    poll()
                }
            }
        }
    }

    func stop() {
        NETunnelProviderManager.loadAllFromPreferences { managers, _ in
            for m in managers ?? [] where (m.protocolConfiguration as? NETunnelProviderProtocol)?.providerBundleIdentifier == self.providerID {
                m.connection.stopVPNTunnel()
            }
        }
    }
}
