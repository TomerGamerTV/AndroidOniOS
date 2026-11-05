import Foundation

/// Starts the AT-command modem (modem.c) that QEMU's "modem" serial port connects to.
final class Modem {
    static let shared = Modem()
    private var started = false

    func start() {
        guard !started else { return }
        started = true
        let log = Paths.logs.appendingPathComponent("modem.log").path
        if droid_modem_start(Paths.modemSocket, log) != 0 {
            Log.info("modem: failed to start (\(errno))")
        }
    }
}
