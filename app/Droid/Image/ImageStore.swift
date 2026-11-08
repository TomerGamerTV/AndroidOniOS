import CryptoKit
import Foundation
import Compression

/// Owns the Android disk images: first-run download/import, extraction, patching,
/// user data creation and per-boot initrd generation.
final class ImageStore {
    struct Manifest: Decodable {
        struct Patch: Decodable {
            let file: String
            let offset: UInt64
            let expect_sha256: String?
            let data_b64: String
        }
        let name: String
        let url: String
        let size: UInt64
        let sha1: String
        let files: [String]
        let patches: [Patch]
    }

    enum Stage: Equatable {
        case idle
        case downloading(Double, Double)   // fraction, bytes per second
        case verifying(Double)
        case extracting(String, Double)
        case finishing
        case ready
        case failed(String)
    }

    static let shared = ImageStore()
    let manifest: Manifest
    var onStage: ((Stage) -> Void)?


    init() {
        let url = Bundle.main.url(forResource: "android", withExtension: "json")!
        manifest = try! JSONDecoder().decode(Manifest.self, from: Data(contentsOf: url))
    }

    private var readyMarker: URL { Paths.images.appendingPathComponent(".ready") }
    func path(_ name: String) -> URL { Paths.images.appendingPathComponent(name) }
    var userdata: URL { Paths.vm.appendingPathComponent("userdata.img") }
    var metadata: URL { Paths.vm.appendingPathComponent("metadata.img") }
    var cache: URL { Paths.vm.appendingPathComponent("cache.img") }
    var initrd: URL { Paths.vm.appendingPathComponent("initrd.img") }

    var isReady: Bool {
        guard let s = try? String(contentsOf: readyMarker, encoding: .utf8) else { return false }
        return s == manifest.sha1 && manifest.files.allSatisfy { FileManager.default.fileExists(atPath: path($0).path) }
    }

    private func report(_ s: Stage) {
        DispatchQueue.main.async { self.onStage?(s) }
    }

    // MARK: - Preparation

    func prepare() async throws {
        if isReady {
            try ensureUserImages()
            removeImportedArchive()
            return
        }
        let zip = try await obtainArchive()
        try await Task.detached(priority: .userInitiated) { try self.verify(zip) }.value
        try await Task.detached(priority: .userInitiated) { try self.extract(zip) }.value
        report(.finishing)
        try applyPatches()
        try ensureUserImages()
        try manifest.sha1.write(to: readyMarker, atomically: true, encoding: .utf8)
        if zip.path.hasPrefix(Paths.android.path) {
            try? FileManager.default.removeItem(at: zip)
        }
        removeImportedArchive()
        report(.ready)
    }

    /// Once the image is installed, the system-image zip copied into Documents is no longer
    /// needed; delete it so it doesn't keep using ~2 GB of storage.
    private func removeImportedArchive() {
        let docs = (try? FileManager.default.contentsOfDirectory(at: Paths.documents, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        for u in docs where u.pathExtension.lowercased() == "zip" &&
            (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize).map({ UInt64($0) }) == manifest.size {
            Log.info("removing installed archive \(u.lastPathComponent)")
            try? FileManager.default.removeItem(at: u)
        }
    }

    /// Prefers an archive the user copied into Documents (Finder/Files), else downloads it.
    private func obtainArchive() async throws -> URL {
        let docs = (try? FileManager.default.contentsOfDirectory(at: Paths.documents, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        if let local = docs.first(where: { u in
            u.pathExtension.lowercased() == "zip" &&
            ((try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize).map { UInt64($0) } == manifest.size)
        }) {
            Log.info("using local archive \(local.lastPathComponent)")
            return local
        }
        let dest = Paths.android.appendingPathComponent("system-image.zip")
        if let s = try? FileManager.default.attributesOfItem(atPath: dest.path)[.size] as? UInt64, s == manifest.size {
            return dest
        }
        return try await download(to: dest)
    }

    private func download(to dest: URL) async throws -> URL {
        report(.downloading(0, 0))
        return try await Downloader.shared.download(URL(string: manifest.url)!, size: Int64(manifest.size), to: dest) { f, speed in
            self.report(.downloading(f, speed))
        }
    }

    private func verify(_ zip: URL) throws {
        let fh = try FileHandle(forReadingFrom: zip)
        defer { try? fh.close() }
        var sha = Insecure.SHA1()
        var done: UInt64 = 0
        var lastReport = 0.0
        var finished = false
        while !finished {
            try autoreleasepool {
                guard let chunk = try fh.read(upToCount: 8 << 20), !chunk.isEmpty else { finished = true; return }
                sha.update(data: chunk)
                done += UInt64(chunk.count)
                let f = Double(done) / Double(manifest.size)
                if f - lastReport > 0.01 { lastReport = f; report(.verifying(f)) }
            }
        }
        let hex = sha.finalize().map { String(format: "%02x", $0) }.joined()
        guard hex == manifest.sha1 else {
            throw NSError(domain: "Droid", code: 1, userInfo: [NSLocalizedDescriptionKey: "System image checksum mismatch (\(hex))"])
        }
    }

    private func extract(_ zip: URL) throws {
        let reader = try ZipReader(url: zip)
        let wanted = manifest.files.compactMap { name in reader.entries.first { $0.name.hasSuffix("/" + name) || $0.name == name } }
        let total = wanted.reduce(0) { $0 + $1.uncompressedSize }
        var base: UInt64 = 0
        for entry in wanted {
            let name = (entry.name as NSString).lastPathComponent
            var last = 0.0
            try reader.extract(entry, to: path(name)) { n in
                let f = Double(base + n) / Double(max(total, 1))
                if f - last > 0.005 { last = f; self.report(.extracting(name, f)) }
            }
            base += entry.uncompressedSize
        }
    }

    private func applyPatches() throws {
        for p in manifest.patches {
            let fh = try FileHandle(forUpdating: path(p.file))
            defer { try? fh.close() }
            let data = Data(base64Encoded: p.data_b64)!
            try fh.seek(toOffset: p.offset)
            let orig = try fh.read(upToCount: data.count) ?? Data()
            let origHash = SHA256.hash(data: orig).map { String(format: "%02x", $0) }.joined()
            if orig == data { continue }
            if let expect = p.expect_sha256, expect != origHash {
                Log.info("patch at \(p.offset) skipped: unexpected original contents")
                continue
            }
            try fh.seek(toOffset: p.offset)
            try fh.write(contentsOf: data)
        }
    }

    /// Creates writable per-device images if missing.
    func ensureUserImages() throws {
        let fm = FileManager.default
        if !fm.fileExists(atPath: metadata.path) {
            try fm.copyItem(at: path("encryptionkey.img"), to: metadata)
        }
        if !fm.fileExists(atPath: cache.path) {
            fm.createFile(atPath: cache.path, contents: nil)
            let h = try FileHandle(forWritingTo: cache)
            try h.truncate(atOffset: 64 << 20)
            try h.close()
        }
        if !fm.fileExists(atPath: userdata.path) {
            try writeUserdataTemplate(to: userdata)
        }
    }

    /// Expands the bundled sparse ext4 template (format: "DRUD" u64 size {u64 off, u32 len, bytes}*).
    private func writeUserdataTemplate(to dest: URL) throws {
        let packed = try Data(contentsOf: Bundle.main.url(forResource: "userdata", withExtension: "tpl")!)
        let raw = try (packed as NSData).decompressed(using: .zlib) as Data
        guard raw.prefix(4) == Data("DRUD".utf8) else { throw NSError(domain: "Droid", code: 2) }
        func u64(_ o: Int) -> UInt64 { raw.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt64.self) } }
        func u32(_ o: Int) -> UInt32 { raw.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt32.self) } }
        let size = u64(4)
        let tmp = dest.appendingPathExtension("tmp")
        FileManager.default.createFile(atPath: tmp.path, contents: nil)
        let h = try FileHandle(forWritingTo: tmp)
        try h.truncate(atOffset: size)
        var p = 12
        while p + 12 <= raw.count {
            let off = u64(p), len = Int(u32(p + 8))
            try h.seek(toOffset: off)
            try h.write(contentsOf: raw.subdata(in: (p + 12)..<(p + 12 + len)))
            p += 12 + len
        }
        try h.close()
        try FileManager.default.moveItem(at: tmp, to: dest)
    }

    /// Wipes user data (factory reset).
    func resetUserData() throws {
        for u in [userdata, metadata] { try? FileManager.default.removeItem(at: u) }
        try ensureUserImages()
    }

    // MARK: - Boot config

    /// vbmeta parameters shipped alongside the image.
    var verifiedBootParams: [String: String] {
        guard let text = try? String(contentsOf: path("VerifiedBootParams.textproto"), encoding: .utf8) else { return [:] }
        var out: [String: String] = [:]
        for line in text.split(separator: "\n") where line.contains("param:") {
            let v = line.replacingOccurrences(of: "param:", with: "").trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\""))
            let kv = v.split(separator: "=", maxSplits: 1)
            if kv.count == 2 { out[String(kv[0])] = String(kv[1]) }
        }
        return out
    }

    /// ramdisk + Android bootconfig trailer.
    func writeInitrd(props: [(String, String)]) throws {
        var data = try Data(contentsOf: path("ramdisk.img"))
        var text = Data(props.map { "\($0.0)=\"\($0.1)\"\n" }.joined().utf8)
        text.append(0)
        while text.count % 4 != 0 { text.append(0) }
        let csum = text.reduce(UInt32(0)) { $0 &+ UInt32($1) }
        data.append(text)
        withUnsafeBytes(of: UInt32(text.count).littleEndian) { data.append(contentsOf: $0) }
        withUnsafeBytes(of: csum.littleEndian) { data.append(contentsOf: $0) }
        data.append(Data("#BOOTCONFIG\n".utf8))
        try data.write(to: initrd)
    }
}
