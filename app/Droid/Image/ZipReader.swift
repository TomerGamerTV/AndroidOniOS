import Compression
import Foundation

/// Minimal streaming ZIP (incl. ZIP64) extractor for stored and deflated entries.
/// Used to unpack the Android system-image archive (multi-GB entries) on device.
struct ZipReader {
    struct Entry {
        let name: String
        let method: UInt16
        let compressedSize: UInt64
        let uncompressedSize: UInt64
        let localHeaderOffset: UInt64
    }

    enum ZipError: Error, LocalizedError {
        case notZip, corrupt(String), unsupported(UInt16)
        var errorDescription: String? {
            switch self {
            case .notZip: return "Not a ZIP archive"
            case .corrupt(let s): return "Corrupt ZIP: \(s)"
            case .unsupported(let m): return "Unsupported ZIP compression method \(m)"
            }
        }
    }

    let url: URL
    private(set) var entries: [Entry] = []

    init(url: URL) throws {
        self.url = url
        entries = try ZipReader.readCentralDirectory(url)
    }

    private static func u16(_ d: Data, _ o: Int) -> UInt16 { d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt16.self) }.littleEndian }
    private static func u32(_ d: Data, _ o: Int) -> UInt32 { d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt32.self) }.littleEndian }
    private static func u64(_ d: Data, _ o: Int) -> UInt64 { d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt64.self) }.littleEndian }

    private static func readCentralDirectory(_ url: URL) throws -> [Entry] {
        let fh = try FileHandle(forReadingFrom: url)
        defer { try? fh.close() }
        let fileSize = try fh.seekToEnd()
        let tailLen = UInt64(min(fileSize, 70_000))
        try fh.seek(toOffset: fileSize - tailLen)
        let tail = try fh.read(upToCount: Int(tailLen)) ?? Data()
        // End of central directory record.
        guard let eocd = tail.lastRange(of: Data([0x50, 0x4b, 0x05, 0x06]))?.lowerBound else { throw ZipError.notZip }
        var cdOffset = UInt64(u32(tail, eocd + 16))
        var cdSize = UInt64(u32(tail, eocd + 12))
        var count = UInt64(u16(tail, eocd + 10))
        // ZIP64 end of central directory locator precedes EOCD.
        if let loc = tail.lastRange(of: Data([0x50, 0x4b, 0x06, 0x07]))?.lowerBound, loc < eocd {
            let z64Offset = u64(tail, loc + 8)
            try fh.seek(toOffset: z64Offset)
            let z = try fh.read(upToCount: 56) ?? Data()
            guard u32(z, 0) == 0x06064b50 else { throw ZipError.corrupt("zip64 eocd") }
            count = u64(z, 32)
            cdSize = u64(z, 40)
            cdOffset = u64(z, 48)
        }
        try fh.seek(toOffset: cdOffset)
        let cd = try fh.read(upToCount: Int(cdSize)) ?? Data()
        var entries: [Entry] = []
        var p = 0
        for _ in 0..<count {
            guard p + 46 <= cd.count, u32(cd, p) == 0x02014b50 else { throw ZipError.corrupt("central dir") }
            let method = u16(cd, p + 10)
            var csize = UInt64(u32(cd, p + 20))
            var usize = UInt64(u32(cd, p + 24))
            let nameLen = Int(u16(cd, p + 28))
            let extraLen = Int(u16(cd, p + 30))
            let commentLen = Int(u16(cd, p + 32))
            var lho = UInt64(u32(cd, p + 42))
            let name = String(decoding: cd.subdata(in: (p + 46)..<(p + 46 + nameLen)), as: UTF8.self)
            // ZIP64 extra field.
            var e = p + 46 + nameLen
            let eEnd = e + extraLen
            while e + 4 <= eEnd {
                let id = u16(cd, e), len = Int(u16(cd, e + 2))
                if id == 0x0001 {
                    var q = e + 4
                    if usize == 0xFFFF_FFFF { usize = u64(cd, q); q += 8 }
                    if csize == 0xFFFF_FFFF { csize = u64(cd, q); q += 8 }
                    if lho == 0xFFFF_FFFF { lho = u64(cd, q); q += 8 }
                }
                e += 4 + len
            }
            entries.append(Entry(name: name, method: method, compressedSize: csize, uncompressedSize: usize, localHeaderOffset: lho))
            p = eEnd + commentLen
        }
        return entries
    }

    /// Extracts `entry` to `dest`. Zero-filled 1 MiB runs are skipped (sparse output).
    func extract(_ entry: Entry, to dest: URL, progress: (UInt64) -> Void) throws {
        let fh = try FileHandle(forReadingFrom: url)
        defer { try? fh.close() }
        try fh.seek(toOffset: entry.localHeaderOffset)
        let lh = try fh.read(upToCount: 30) ?? Data()
        guard ZipReader.u32(lh, 0) == 0x04034b50 else { throw ZipError.corrupt("local header") }
        let skip = UInt64(ZipReader.u16(lh, 26)) + UInt64(ZipReader.u16(lh, 28))
        try fh.seek(toOffset: entry.localHeaderOffset + 30 + skip)

        FileManager.default.createFile(atPath: dest.path, contents: nil)
        let out = try FileHandle(forWritingTo: dest)
        defer { try? out.close() }
        var written: UInt64 = 0
        let writer = SparseWriter(out: out)

        switch entry.method {
        case 0:
            var left = entry.compressedSize
            while left > 0 {
                try autoreleasepool {
                    let chunk = try fh.read(upToCount: Int(min(left, 4 << 20))) ?? Data()
                    if chunk.isEmpty { throw ZipError.corrupt("truncated") }
                    try writer.write(chunk)
                    left -= UInt64(chunk.count)
                    written += UInt64(chunk.count)
                    progress(written)
                }
            }
        case 8:
            let dstSize = 4 << 20
            let srcSize = 1 << 20
            let dstBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: dstSize)
            let srcBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: srcSize)
            defer { dstBuf.deallocate(); srcBuf.deallocate() }
            var stream = compression_stream(dst_ptr: dstBuf, dst_size: 0, src_ptr: UnsafePointer(srcBuf), src_size: 0, state: nil)
            guard compression_stream_init(&stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
                throw ZipError.corrupt("inflate init")
            }
            defer { compression_stream_destroy(&stream) }
            var left = entry.compressedSize
            var done = false
            while !done { try autoreleasepool {
                if stream.src_size == 0 && left > 0 {
                    let chunk = try fh.read(upToCount: Int(min(left, UInt64(srcSize)))) ?? Data()
                    if chunk.isEmpty { throw ZipError.corrupt("truncated") }
                    chunk.copyBytes(to: srcBuf, count: chunk.count)
                    stream.src_ptr = UnsafePointer(srcBuf)
                    stream.src_size = chunk.count
                    left -= UInt64(chunk.count)
                }
                stream.dst_ptr = dstBuf
                stream.dst_size = dstSize
                let flags = left == 0 ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
                let status = compression_stream_process(&stream, flags)
                let produced = dstSize - stream.dst_size
                if produced > 0 {
                    try writer.write(Data(bytesNoCopy: dstBuf, count: produced, deallocator: .none))
                    written += UInt64(produced)
                    progress(written)
                }
                switch status {
                case COMPRESSION_STATUS_END: done = true
                case COMPRESSION_STATUS_OK: break
                default: throw ZipError.corrupt("inflate")
                }
            } }
        default:
            throw ZipError.unsupported(entry.method)
        }
        try writer.finish(size: entry.uncompressedSize)
    }
}

/// Writes data, seeking over all-zero 1 MiB blocks so large images stay sparse on APFS.
final class SparseWriter {
    private let out: FileHandle
    private var offset: UInt64 = 0
    private var pending = Data()
    private let block = 1 << 20

    init(out: FileHandle) { self.out = out }

    func write(_ data: Data) throws {
        pending.append(data)
        if pending.count < block { return }
        var start = 0
        while pending.count - start >= block {
            try emit(pending.subdata(in: start..<(start + block)))
            start += block
        }
        pending = start < pending.count ? pending.subdata(in: start..<pending.count) : Data()
    }

    private func emit(_ b: Data) throws {
        let isZero = b.withUnsafeBytes { raw -> Bool in
            let words = raw.bindMemory(to: UInt64.self)
            for w in words where w != 0 { return false }
            return true
        }
        if !isZero {
            try out.seek(toOffset: offset)
            try out.write(contentsOf: b)
        }
        offset += UInt64(b.count)
    }

    func finish(size: UInt64) throws {
        if !pending.isEmpty { try emit(pending); pending.removeAll() }
        try out.truncate(atOffset: size)
    }
}
