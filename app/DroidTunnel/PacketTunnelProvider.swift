import NetworkExtension

/// Loopback tunnel that lets the app reach this device's own developer services
/// (RemotePairing/RSD) at 10.7.0.1, which StikJIT needs to enable JIT.
///
/// Packets leaving through the tunnel get their addresses swapped (device 10.7.0.0 <-> 10.7.0.1)
/// and are written straight back, so they arrive at the local network stack. Swapping keeps the
/// IP/TCP checksums valid because the sum of the two addresses is unchanged.
/// Based on the approach used by StosVPN / LocalDevVPN (SideStore Team).
final class PacketTunnelProvider: NEPacketTunnelProvider {
    private let deviceIP = "10.7.0.0"
    private let fakeIP = "10.7.0.1"
    private var device: UInt32 = 0
    private var fake: UInt32 = 0

    override func startTunnel(options: [String: NSObject]?, completionHandler: @escaping (Error?) -> Void) {
        device = Self.ip(deviceIP)
        fake = Self.ip(fakeIP)
        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: deviceIP)
        let v4 = NEIPv4Settings(addresses: [deviceIP], subnetMasks: ["255.255.255.0"])
        v4.includedRoutes = [NEIPv4Route(destinationAddress: deviceIP, subnetMask: "255.255.255.0")]
        v4.excludedRoutes = [.default()]
        settings.ipv4Settings = v4
        setTunnelNetworkSettings(settings) { error in
            if let error { completionHandler(error); return }
            self.pump()
            completionHandler(nil)
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        completionHandler()
    }

    private func pump() {
        packetFlow.readPackets { [weak self] packets, protocols in
            guard let self else { return }
            var out = packets
            for i in out.indices where protocols[i].int32Value == AF_INET && out[i].count >= 20 {
                out[i].withUnsafeMutableBytes { raw in
                    guard let p = raw.baseAddress?.assumingMemoryBound(to: UInt32.self) else { return }
                    if UInt32(bigEndian: p[3]) == self.device { p[3] = self.fake.bigEndian }
                    if UInt32(bigEndian: p[4]) == self.fake { p[4] = self.device.bigEndian }
                }
            }
            self.packetFlow.writePackets(out, withProtocols: protocols)
            self.pump()
        }
    }

    private static func ip(_ s: String) -> UInt32 {
        s.split(separator: ".").compactMap { UInt32($0) }.reduce(0) { ($0 << 8) | $1 }
    }
}
