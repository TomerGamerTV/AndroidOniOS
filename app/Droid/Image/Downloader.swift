import Foundation
import UIKit

/// The system-image download, split into ranged parts fetched in parallel (like a download
/// manager) on a background URLSession. iOS keeps it going while the app is in the background or
/// suspended, and relaunches the app for it if needed. Finished parts and resume data are kept in
/// Library/Android/download, so a failed or interrupted download continues instead of restarting.
final class Downloader: NSObject, URLSessionDownloadDelegate {
    static let shared = Downloader()
    static let parts = 8

    /// From AppDelegate's handleEventsForBackgroundURLSession; called once iOS has delivered them.
    var backgroundCompletion: (() -> Void)?

    private let queue: OperationQueue = { let q = OperationQueue(); q.maxConcurrentOperationCount = 1; return q }()
    private lazy var session: URLSession = {
        let c = URLSessionConfiguration.background(withIdentifier: "com.tomergamertv.oni.image-download")
        c.isDiscretionary = false
        c.sessionSendsLaunchEvents = true
        c.httpMaximumConnectionsPerHost = Self.parts
        c.timeoutIntervalForResource = 7 * 24 * 3600
        return URLSession(configuration: c, delegate: self, delegateQueue: queue)
    }()
    private let dir = Paths.android.appendingPathComponent("download")

    // Everything below is only touched on `queue`.
    private var url: URL?
    private var size: Int64 = 0
    private var dest: URL?
    private var continuation: CheckedContinuation<URL, Error>?
    private var onProgress: ((_ fraction: Double, _ bytesPerSecond: Double) -> Void)?
    private var inFlight: [Int: Int64] = [:]
    private var failures: [Int: Int] = [:]
    private var speedMark: (time: Date, bytes: Int64)?
    private var speed = 0.0
    private var lastReport = Date.distantPast

    /// Makes sure the session exists so iOS can deliver events for tasks from an earlier launch.
    func attach() { _ = session }

    func download(_ url: URL, size: Int64, to dest: URL,
                  progress: @escaping (_ fraction: Double, _ bytesPerSecond: Double) -> Void) async throws -> URL {
        try await withCheckedThrowingContinuation { c in
            queue.addOperation {
                self.continuation?.resume(throwing: CancellationError())
                (self.url, self.size, self.dest, self.continuation, self.onProgress) = (url, size, dest, c, progress)
                self.failures = [:]
                self.prepareDirectory(for: url)
                self.session.getAllTasks { tasks in self.queue.addOperation { self.schedule(running: tasks) } }
            }
        }
    }

    // MARK: - Parts

    private func range(_ i: Int) -> ClosedRange<Int64> {
        let per = (size + Int64(Self.parts) - 1) / Int64(Self.parts)
        return Int64(i) * per ... min(size, Int64(i + 1) * per) - 1
    }
    private func partFile(_ i: Int) -> URL { dir.appendingPathComponent("part-\(i)") }
    private func resumeFile(_ i: Int) -> URL { dir.appendingPathComponent("part-\(i).resume") }
    private func fileSize(_ u: URL) -> Int64 {
        ((try? FileManager.default.attributesOfItem(atPath: u.path)[.size]) as? NSNumber)?.int64Value ?? -1
    }
    private func isDone(_ i: Int) -> Bool { fileSize(partFile(i)) == Int64(range(i).count) }

    /// Parts belong to one URL; anything left from another image is thrown away.
    private func prepareDirectory(for url: URL) {
        let fm = FileManager.default
        let stamp = dir.appendingPathComponent("source")
        let id = "2 " + url.absoluteString   // bump when the request format changes
        if (try? String(contentsOf: stamp, encoding: .utf8)) != id {
            try? fm.removeItem(at: dir)
        }
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        try? id.write(to: stamp, atomically: true, encoding: .utf8)
        try? fm.removeItem(at: Paths.android.appendingPathComponent("download.partial"))   // the old single-file download
    }

    private func schedule(running tasks: [URLSessionTask]) {
        var active = Set<Int>()
        for t in tasks {
            guard let i = Self.part(of: t), i < Self.parts, !isDone(i), t.state == .running || t.state == .suspended,
                  t.originalRequest?.url == url, t.originalRequest?.value(forHTTPHeaderField: "Accept-Encoding") == "identity"
            else { t.cancel(); continue }
            if active.insert(i).inserted { t.resume() } else { t.cancel() }
        }
        for i in 0..<Self.parts where !isDone(i) && !active.contains(i) { start(i) }
        Log.info("download: \((0..<Self.parts).filter(isDone).count)/\(Self.parts) parts done, \(active.count) resumed in flight")
        finishIfComplete()
    }

    private func start(_ i: Int) {
        guard let url else { return }
        let task: URLSessionDownloadTask
        if let data = try? Data(contentsOf: resumeFile(i)) {
            task = session.downloadTask(withResumeData: data)
        } else {
            var req = URLRequest(url: url)
            req.setValue("bytes=\(range(i).lowerBound)-\(range(i).upperBound)", forHTTPHeaderField: "Range")
            // Otherwise Google serves a gzipped copy and the ranges would slice the compressed stream.
            req.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
            task = session.downloadTask(with: req)
        }
        try? FileManager.default.removeItem(at: resumeFile(i))
        task.taskDescription = "part \(i)"
        inFlight[i] = 0
        task.resume()
    }

    private static func part(of t: URLSessionTask) -> Int? {
        t.taskDescription.flatMap { $0.hasPrefix("part ") ? Int($0.dropFirst(5)) : nil }
    }

    private func reportProgress() {
        guard let onProgress, size > 0, -lastReport.timeIntervalSinceNow > 0.25 else { return }
        lastReport = Date()
        var bytes: Int64 = 0
        for i in 0..<Self.parts { bytes += isDone(i) ? Int64(range(i).count) : min(inFlight[i] ?? 0, Int64(range(i).count)) }
        let now = Date()
        if let m = speedMark {
            let dt = now.timeIntervalSince(m.time)
            if dt >= 1 { speed = speed == 0 ? Double(bytes - m.bytes) / dt : speed * 0.7 + Double(bytes - m.bytes) / dt * 0.3; speedMark = (now, bytes) }
        } else { speedMark = (now, bytes) }
        onProgress(Double(bytes) / Double(size), max(speed, 0))
    }

    /// Joins the parts into `dest` once all are on disk: part 0 becomes the file and the others
    /// are appended and deleted one by one, so it never needs much more than the archive's size.
    private func finishIfComplete() {
        guard let dest, let c = continuation, (0..<Self.parts).allSatisfy(isDone) else { return }
        let bg = UIApplication.shared.beginBackgroundTask(withName: "join download", expirationHandler: nil)
        defer { UIApplication.shared.endBackgroundTask(bg) }
        let fm = FileManager.default
        do {
            try? fm.removeItem(at: dest)
            try fm.moveItem(at: partFile(0), to: dest)
            let out = try FileHandle(forWritingTo: dest)
            defer { try? out.close() }
            try out.seekToEnd()
            for i in 1..<Self.parts {
                let input = try FileHandle(forReadingFrom: partFile(i))
                while let chunk = try autoreleasepool(invoking: { try input.read(upToCount: 16 << 20) }), !chunk.isEmpty {
                    try out.write(contentsOf: chunk)
                }
                try input.close()
                try fm.removeItem(at: partFile(i))
            }
            try out.synchronize()
            try? fm.removeItem(at: dir)
            Log.info("download: complete")
            continuation = nil
            c.resume(returning: dest)
        } catch {
            try? fm.removeItem(at: dest)   // parts already joined are gone; those parts download again
            continuation = nil
            c.resume(throwing: error)
        }
    }

    // MARK: - URLSessionDownloadDelegate (on `queue`)

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let i = Self.part(of: downloadTask) else { return }
        inFlight[i] = totalBytesWritten
        reportProgress()
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didResumeAtOffset fileOffset: Int64,
                    expectedTotalBytes: Int64) {
        guard let i = Self.part(of: downloadTask) else { return }
        inFlight[i] = fileOffset
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file at `location` is deleted when this returns. A size check covers a server that
        // ignored the range (200 with the whole file) and a background relaunch before `size` is known.
        guard let i = Self.part(of: downloadTask), let http = downloadTask.response as? HTTPURLResponse,
              http.statusCode == 206 else { return }
        if size > 0 && fileSize(location) != Int64(range(i).count) { return }
        try? FileManager.default.removeItem(at: partFile(i))
        try? FileManager.default.moveItem(at: location, to: partFile(i))
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let i = Self.part(of: task) else { return }
        inFlight[i] = nil
        if let data = (error as NSError?)?.userInfo[NSURLSessionDownloadTaskResumeData] as? Data {
            try? data.write(to: resumeFile(i))
        }
        if (error as NSError?)?.code == NSURLErrorCancelled && (error as NSError?)?.domain == NSURLErrorDomain { return }
        guard continuation != nil, size > 0 else { return }   // relaunched in the background: the next download() picks up
        if isDone(i) { finishIfComplete(); return }
        failures[i, default: 0] += 1
        let why = error?.localizedDescription ?? "HTTP \((task.response as? HTTPURLResponse)?.statusCode ?? 0)"
        Log.info("download: part \(i) failed (\(failures[i]!)): \(why)")
        if failures[i]! <= 5 {
            if error == nil { try? FileManager.default.removeItem(at: resumeFile(i)) }   // bad response: start the part over
            DispatchQueue.global().asyncAfter(deadline: .now() + Double(failures[i]!) * 2) {
                self.queue.addOperation { if self.continuation != nil, !self.isDone(i) { self.start(i) } }
            }
        } else {
            // The other parts keep going in the background; trying again picks them up.
            let c = continuation
            continuation = nil
            c?.resume(throwing: NSError(domain: "Droid", code: 2, userInfo: [NSLocalizedDescriptionKey: "The download stopped: \(why)"]))
        }
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        DispatchQueue.main.async {
            self.backgroundCompletion?()
            self.backgroundCompletion = nil
        }
    }
}
