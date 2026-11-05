import Foundation

/// XPC interface between the app and its JIT helper extension (which hosts StikJIT).
@objc(DroidJITProtocol) protocol DroidJITProtocol {
    /// Attaches the device's debugserver to `pid` and runs StikJIT's universal script, which
    /// services the app's `brk #0xf00d` JIT requests. Replies once the script detaches.
    func enableJIT(pid: Int32, pairingFile: Data, reply: @escaping (Bool, String) -> Void)
    /// Readiness check without attaching (DDI mount, tunnel reachability).
    func prepare(pairingFile: Data, reply: @escaping (Bool, String) -> Void)
}
