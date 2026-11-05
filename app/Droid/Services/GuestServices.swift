import CoreLocation
import CoreMotion
import Foundation
import UIKit

/// Host side of the guest's "qemu pipe" services (sensors HAL, GNSS, boot properties,
/// misc pipe). The guest connects over vsock port 5000; QEMU forwards each connection
/// to a unix socket served here. The first bytes are the NUL-terminated pipe name.
final class GuestServices: NSObject {
    static let shared = GuestServices()

    private var listenFD: Int32 = -1
    private var acceptSource: DispatchSourceRead?
    private let queue = DispatchQueue(label: "guest-services", attributes: .concurrent)
    var density = 420

    func start() {
        guard listenFD < 0 else { return }
        listenFD = droid_bridge_unix_listen(Paths.servicesSocket)
        guard listenFD >= 0 else { Log.info("services: listen failed \(errno)"); return }
        let src = DispatchSource.makeReadSource(fileDescriptor: listenFD, queue: queue)
        src.setEventHandler { [weak self] in
            guard let self else { return }
            let c = droid_bridge_accept(self.listenFD)
            if c >= 0 { Thread { self.handle(Conn(fd: c)) }.start() }
        }
        src.resume()
        acceptSource = src
        Sensors.shared.prepare()
        GPS.shared.prepare()
    }

    final class Conn {
        let fd: Int32
        private let wlock = NSLock()
        init(fd: Int32) { self.fd = fd }
        deinit { close(fd) }

        func readExact(_ n: Int) -> Data? {
            var buf = [UInt8](repeating: 0, count: n)
            var got = 0
            while got < n {
                let r = buf.withUnsafeMutableBytes { read(fd, $0.baseAddress! + got, n - got) }
                if r <= 0 { return nil }
                got += r
            }
            return Data(buf)
        }

        @discardableResult
        func write(_ d: Data) -> Bool {
            wlock.lock(); defer { wlock.unlock() }
            return d.withUnsafeBytes { raw -> Bool in
                var off = 0
                while off < d.count {
                    let r = Darwin.write(fd, raw.baseAddress! + off, d.count - off)
                    if r <= 0 { return false }
                    off += r
                }
                return true
            }
        }

        // qemud framing: 4 hex digits length + payload
        func qemudSend(_ s: String) -> Bool { qemudSend(Data(s.utf8)) }
        func qemudSend(_ d: Data) -> Bool {
            var out = Data(String(format: "%04x", d.count).utf8)
            out.append(d)
            return write(out)
        }
        func qemudRecv() -> String? {
            guard let h = readExact(4), let n = Int(String(decoding: h, as: UTF8.self), radix: 16),
                  let p = readExact(n) else { return nil }
            return String(decoding: p, as: UTF8.self)
        }
    }

    private func handle(_ c: Conn) {
        var name = Data()
        while true {
            guard let b = c.readExact(1) else { return }
            if b[0] == 0 { break }
            name.append(b)
            if name.count > 256 { return }
        }
        let service = String(decoding: name, as: UTF8.self)
        Log.info("guest pipe open: \(service)")
        switch service {
        case "pipe:qemud:sensors": Sensors.shared.serve(c)
        case "pipe:qemud:boot-properties": bootProperties(c)
        case "pipe:QemuMiscPipe": miscPipe(c)
        case "pipe:qemud:gps": GPS.shared.serve(c)
        default:
            // Unknown service: keep it open so the guest doesn't spin reconnecting.
            var buf = [UInt8](repeating: 0, count: 4096)
            while read(c.fd, &buf, buf.count) > 0 {}
        }
    }

    private func bootProperties(_ c: Conn) {
        guard c.qemudRecv() == "list" else { return }
        let props = [
            "qemu.sf.lcd_density=\(density)",
            "qemu.hw.mainkeys=0",
            "qemu.timezone=\(TimeZone.current.identifier)",
        ]
        for p in props { _ = c.qemudSend(p) }
        _ = c.qemudSend(Data([0]))
        var buf = [UInt8](repeating: 0, count: 64)
        while read(c.fd, &buf, buf.count) > 0 {}
    }

    private func miscPipe(_ c: Conn) {
        while let lenData = c.readExact(4) {
            let len = Int(lenData.withUnsafeBytes { $0.load(as: Int32.self) })
            guard len >= 0, len < 1 << 20, let msg = c.readExact(len) else { return }
            let text = String(decoding: msg.prefix { $0 != 0 }, as: UTF8.self)
            if text == "bootcomplete" {
                NotificationCenter.default.post(name: .androidBootCompleted, object: nil)
            }
            var zero = Int32(0)
            c.write(Data(bytes: &zero, count: 4))
        }
    }
}

extension Notification.Name {
    static let androidBootCompleted = Notification.Name("androidBootCompleted")
}

/// CoreMotion -> Android sensor events. Android and iOS share the device axes
/// (x right, y up, z out of the screen); iOS reports acceleration in g with gravity
/// negated relative to Android's convention.
final class Sensors {
    static let shared = Sensors()
    private let motion = CMMotionManager()
    private let altimeter = CMAltimeter()
    private var pressureKPa: Double = 101.325
    private let g = 9.80665

    // Bits follow the guest HAL's sensor handles: accel, gyro, mag, orientation, proximity,
    // pressure, mag-uncal, gyro-uncal, accel-uncal.
    static let mask: UInt32 = [0, 1, 2, 3, 5, 7, 9, 10, 17].reduce(0) { $0 | (1 << $1) }

    func prepare() {
        if CMAltimeter.isRelativeAltitudeAvailable() {
            altimeter.startRelativeAltitudeUpdates(to: OperationQueue()) { [weak self] d, _ in
                if let d { self?.pressureKPa = d.pressure.doubleValue }
            }
        }
    }

    func serve(_ c: GuestServices.Conn) {
        var started = false
        var interval = 0.05
        var running = true
        defer { running = false }
        while let msg = c.qemudRecv() {
            if msg == "list-sensors" {
                _ = c.qemudSend(String(Sensors.mask))
            } else if msg.hasPrefix("set-delay:"), let ms = Double(msg.dropFirst(10)) {
                interval = max(0.01, ms / 1000)
            } else if msg.hasPrefix("set:"), msg.hasSuffix(":1"), !started {
                started = true
                startMotion(interval: interval)
                let iv = interval
                Thread {
                    while running {
                        if !self.pump(c) { break }
                        Thread.sleep(forTimeInterval: iv)
                    }
                }.start()
            }
        }
    }

    private func startMotion(interval: Double) {
        DispatchQueue.main.async {
            let m = self.motion
            m.deviceMotionUpdateInterval = interval
            m.accelerometerUpdateInterval = interval
            m.gyroUpdateInterval = interval
            m.magnetometerUpdateInterval = interval
            if m.isDeviceMotionAvailable && !m.isDeviceMotionActive {
                m.startDeviceMotionUpdates(using: .xMagneticNorthZVertical)
            }
            if m.isAccelerometerAvailable && !m.isAccelerometerActive { m.startAccelerometerUpdates() }
            if m.isGyroAvailable && !m.isGyroActive { m.startGyroUpdates() }
            if m.isMagnetometerAvailable && !m.isMagnetometerActive { m.startMagnetometerUpdates() }
        }
    }

    private func pump(_ c: GuestServices.Conn) -> Bool {
        var lines: [String] = []
        if let a = motion.accelerometerData?.acceleration {
            let s = String(format: "%.5f:%.5f:%.5f", -a.x * g, -a.y * g, -a.z * g)
            lines.append("acceleration:" + s)
            lines.append("acceleration-uncalibrated:" + s + ":0:0:0")
        } else {
            lines.append("acceleration:0:\(g):0")
        }
        if let r = motion.gyroData?.rotationRate {
            let s = String(format: "%.5f:%.5f:%.5f", r.x, r.y, r.z)
            lines.append("gyroscope:" + s)
            lines.append("gyroscope-uncalibrated:" + s + ":0:0:0")
        }
        if let dm = motion.deviceMotion {
            let f = dm.magneticField.field
            if dm.magneticField.accuracy != .uncalibrated {
                lines.append(String(format: "magnetic-field:%.3f:%.3f:%.3f", f.x, f.y, f.z))
            }
            // Android orientation: azimuth, pitch, roll in degrees.
            let att = dm.attitude
            var az = -att.yaw * 180 / .pi
            if az < 0 { az += 360 }
            lines.append(String(format: "orientation:%.3f:%.3f:%.3f", az, -att.pitch * 180 / .pi, att.roll * 180 / .pi))
        }
        if let m = motion.magnetometerData?.magneticField {
            lines.append(String(format: "magnetic-field-uncalibrated:%.3f:%.3f:%.3f:0:0:0", m.x, m.y, m.z))
        }
        lines.append("proximity:5")
        lines.append(String(format: "pressure:%.3f", pressureKPa * 10))
        lines.append("sync:\(Int64(Date().timeIntervalSince1970 * 1_000_000))")
        for l in lines where !c.qemudSend(l) { return false }
        return true
    }
}

/// CoreLocation -> NMEA sentences for the guest GNSS HAL.
final class GPS: NSObject, CLLocationManagerDelegate {
    static let shared = GPS()
    private let manager = CLLocationManager()
    private var last: CLLocation?

    func prepare() {
        DispatchQueue.main.async {
            self.manager.delegate = self
            self.manager.desiredAccuracy = kCLLocationAccuracyBest
            if self.manager.authorizationStatus == .notDetermined {
                self.manager.requestWhenInUseAuthorization()
            }
            self.manager.startUpdatingLocation()
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        last = locations.last
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        manager.startUpdatingLocation()
    }

    func serve(_ c: GuestServices.Conn) {
        while true {
            if let loc = last {
                for s in nmea(loc) {
                    if !c.write(Data(s.utf8)) { return }
                }
            }
            Thread.sleep(forTimeInterval: 1)
        }
    }

    private func nmea(_ loc: CLLocation) -> [String] {
        let t = Date()
        let fmt = DateFormatter()
        fmt.timeZone = TimeZone(identifier: "UTC")
        fmt.dateFormat = "HHmmss.SS"
        let time = fmt.string(from: t)
        fmt.dateFormat = "ddMMyy"
        let date = fmt.string(from: t)
        func coord(_ v: Double, _ digits: Int) -> String {
            let d = Int(abs(v)); let m = (abs(v) - Double(d)) * 60
            return String(format: "%0\(digits)d%07.4f", d, m)
        }
        let lat = coord(loc.coordinate.latitude, 2), ns = loc.coordinate.latitude >= 0 ? "N" : "S"
        let lon = coord(loc.coordinate.longitude, 3), ew = loc.coordinate.longitude >= 0 ? "E" : "W"
        let speedKn = max(0, loc.speed) * 1.943844
        let course = max(0, loc.course)
        let gga = "GPGGA,\(time),\(lat),\(ns),\(lon),\(ew),1,10,\(String(format: "%.1f", max(0.5, loc.horizontalAccuracy / 5))),\(String(format: "%.1f", loc.altitude)),M,0.0,M,,"
        let rmc = "GPRMC,\(time),A,\(lat),\(ns),\(lon),\(ew),\(String(format: "%.1f", speedKn)),\(String(format: "%.1f", course)),\(date),,"
        return [gga, rmc].map { s in
            let cs = s.utf8.reduce(UInt8(0)) { $0 ^ $1 }
            return "$\(s)*\(String(format: "%02X", cs))\r\n"
        }
    }
}
