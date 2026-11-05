import Foundation

/// File locations used by the app.
///
/// - `Library/Android`  : the Android disk images (hidden from Files/Finder).
/// - `Documents`        : user-visible storage, mirrored into the Android device's shared storage.
/// - `tmp/s`            : short paths for unix sockets (sun_path is limited to 104 bytes).
enum Paths {
    static let fm = FileManager.default

    static var documents: URL { fm.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    static var library: URL { fm.urls(for: .libraryDirectory, in: .userDomainMask)[0] }

    static var android: URL { ensure(library.appendingPathComponent("Android", isDirectory: true)) }
    static var images: URL { ensure(android.appendingPathComponent("images", isDirectory: true)) }
    static var vm: URL { ensure(android.appendingPathComponent("vm", isDirectory: true)) }
    static var logs: URL { ensure(android.appendingPathComponent("logs", isDirectory: true)) }

    /// Shared with Finder / Files ("On My iPhone > Android").
    static var sharedStorage: URL { ensure(documents.appendingPathComponent("Android Storage", isDirectory: true)) }

    static var appGroup: URL? {
        fm.containerURL(forSecurityApplicationGroupIdentifier: "group.com.tomer.androidonios")
    }
    /// APKs and files handed over by the share extension.
    static var inbox: URL {
        ensure((appGroup ?? library).appendingPathComponent("Inbox", isDirectory: true))
    }

    static var sockets: URL {
        ensure(URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("s", isDirectory: true))
    }
    static var adbSocket: String { sockets.appendingPathComponent("adb").path }
    static var servicesSocket: String { sockets.appendingPathComponent("svc").path }
    static var modemSocket: String { sockets.appendingPathComponent("modem").path }

    @discardableResult
    static func ensure(_ url: URL) -> URL {
        if !fm.fileExists(atPath: url.path) {
            try? fm.createDirectory(at: url, withIntermediateDirectories: true)
        }
        return url
    }
}

enum Log {
    private static let queue = DispatchQueue(label: "log")
    private static var handle: FileHandle? = {
        let url = Paths.logs.appendingPathComponent("app.log")
        if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
           let size = attrs[.size] as? Int, size > 4 << 20 {
            try? FileManager.default.removeItem(at: url)
        }
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        let h = try? FileHandle(forWritingTo: url)
        h?.seekToEndOfFile()
        return h
    }()

    static func info(_ msg: String) {
        let line = "\(Date()) \(msg)\n"
        NSLog("[Droid] %@", msg)
        queue.async { handle?.write(line.data(using: .utf8)!) }
    }
}
