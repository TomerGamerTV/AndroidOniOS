import Foundation
import Network

/// ADB for a computer on the same network: listens on TCP 5555 and connects each client to
/// Android's adbd (the same unix socket the app's own ADB uses, which QEMU forwards into the
/// guest). On the computer: `adb connect <iPhone IP>:5555`; Android asks once to allow its key.
/// Settings › "ADB over Wi-Fi", or debug.json `"adb": true`.
final class ADBRelay {
    static let shared = ADBRelay()
    static let port: UInt16 = 5555
    private var listener: NWListener?

    var enabled: Bool {
        get {
            if let d = try? Data(contentsOf: Paths.android.appendingPathComponent("debug.json")),
               let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any], j["adb"] as? Bool == true { return true }
            return UserDefaults.standard.bool(forKey: "adbRelay")
        }
        set {
            UserDefaults.standard.set(newValue, forKey: "adbRelay")
            newValue ? start() : stop()
        }
    }

    func startIfEnabled() { if enabled { start() } }

    func start() {
        guard listener == nil else { return }
        do {
            let l = try NWListener(using: .tcp, on: NWEndpoint.Port(rawValue: Self.port)!)
            l.newConnectionHandler = { [weak self] in self?.relay($0) }
            l.start(queue: .global(qos: .userInitiated))
            listener = l
            Log.info("adb relay listening on :\(Self.port) (\(Self.addresses().joined(separator: ", ")))")
        } catch {
            Log.info("adb relay: \(error)")
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
    }

    private func relay(_ c: NWConnection) {
        let fd = droid_bridge_unix_connect(Paths.adbSocket)
        guard fd >= 0 else { c.cancel(); return }
        Log.info("adb relay: client \(c.endpoint)")
        c.start(queue: .global(qos: .userInitiated))
        // guest -> client
        Thread.detachNewThread {
            var buf = [UInt8](repeating: 0, count: 65536)
            while true {
                let n = read(fd, &buf, buf.count)
                if n <= 0 { break }
                let sem = DispatchSemaphore(value: 0)
                c.send(content: Data(buf[0..<n]), completion: .contentProcessed { _ in sem.signal() })
                sem.wait()
            }
            c.cancel()
        }
        // client -> guest
        func pump() {
            c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, done, err in
                if let data, !data.isEmpty {
                    let ok = data.withUnsafeBytes { p -> Bool in
                        var off = 0
                        while off < p.count {
                            let n = write(fd, p.baseAddress! + off, p.count - off)
                            if n <= 0 { return false }
                            off += n
                        }
                        return true
                    }
                    if !ok { shutdown(fd, SHUT_RDWR); close(fd); c.cancel(); return }
                }
                if done || err != nil { shutdown(fd, SHUT_RDWR); close(fd); c.cancel(); return }
                pump()
            }
        }
        pump()
    }

    /// The phone's IPv4 addresses (Wi-Fi first), for showing in Settings.
    static func addresses() -> [String] {
        var out: [String] = []
        var ifs: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifs) == 0, let first = ifs else { return out }
        defer { freeifaddrs(ifs) }
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let sa = p.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString: p.pointee.ifa_name)
            guard name.hasPrefix("en") || name.hasPrefix("bridge") else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                out.append(String(cString: host))
            }
        }
        return out
    }
}
