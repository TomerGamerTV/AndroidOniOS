import Foundation
import Security

/// RSA-2048 identity used to authenticate to the guest's adbd. The public half is
/// handed to Android through bootconfig (androidboot.qemu.adb.pubkey) so no prompt
/// is ever shown.
final class ADBKey {
    static let shared = ADBKey()
    let privateKey: SecKey
    /// Android's custom RSA public key encoding, base64 + " user@host".
    let androidPublicKey: String

    private init() {
        let url = Paths.android.appendingPathComponent("adbkey.der")
        var key: SecKey?
        if let der = try? Data(contentsOf: url) {
            key = SecKeyCreateWithData(der as CFData, [
                kSecAttrKeyType: kSecAttrKeyTypeRSA,
                kSecAttrKeyClass: kSecAttrKeyClassPrivate,
            ] as CFDictionary, nil)
        }
        if key == nil {
            key = SecKeyCreateRandomKey([kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits: 2048] as CFDictionary, nil)
            if let k = key, let der = SecKeyCopyExternalRepresentation(k, nil) as Data? {
                try? der.write(to: url, options: .completeFileProtectionUntilFirstUserAuthentication)
            }
        }
        privateKey = key!
        let pub = SecKeyCopyPublicKey(privateKey)!
        let pkcs1 = SecKeyCopyExternalRepresentation(pub, nil)! as Data
        androidPublicKey = ADBKey.encodeAndroid(pkcs1: pkcs1) + " droid@ios"
    }

    func sign(token: Data) -> Data? {
        SecKeyCreateSignature(privateKey, .rsaSignatureDigestPKCS1v15SHA1, token as CFData, nil) as Data?
    }

    // MARK: - Android public key format

    /// struct { u32 len; u32 n0inv; u32 n[len]; u32 rr[len]; u32 exponent; } (little endian)
    private static func encodeAndroid(pkcs1: Data) -> String {
        let (modulus, exponent) = parseRSAPublicKey(pkcs1)
        let words = 64
        // modulus as little-endian 32-bit words
        var n = [UInt32](repeating: 0, count: words)
        let mBytes = Array(modulus.drop { $0 == 0 })
        for (i, b) in mBytes.reversed().enumerated() where i < words * 4 {
            n[i / 4] |= UInt32(b) << (8 * UInt32(i % 4))
        }
        // n0inv = -1 / n[0] mod 2^32
        var inv: UInt32 = 1
        for _ in 0..<5 { inv = inv &* (2 &- n[0] &* inv) }
        let n0inv = 0 &- inv
        // rr = 2^(2*2048) mod n
        var r = [UInt32](repeating: 0, count: words + 1)
        r[0] = 1
        for _ in 0..<(2 * words * 32) {
            var carry: UInt32 = 0
            for i in 0..<r.count {
                let v = r[i]
                r[i] = (v << 1) | carry
                carry = v >> 31
            }
            if geq(r, n) { sub(&r, n) }
        }
        var out = Data()
        func put(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { out.append(contentsOf: $0) } }
        put(UInt32(words))
        put(n0inv)
        for w in n { put(w) }
        for i in 0..<words { put(r[i]) }
        var e: UInt32 = 0
        for b in exponent { e = (e << 8) | UInt32(b) }
        put(e)
        return out.base64EncodedString()
    }

    private static func geq(_ a: [UInt32], _ n: [UInt32]) -> Bool {
        if a[n.count] != 0 { return true }
        for i in stride(from: n.count - 1, through: 0, by: -1) where a[i] != n[i] {
            return a[i] > n[i]
        }
        return true
    }

    private static func sub(_ a: inout [UInt32], _ n: [UInt32]) {
        var borrow: UInt64 = 0
        for i in 0..<a.count {
            let rhs = UInt64(i < n.count ? n[i] : 0) + borrow
            let lhs = UInt64(a[i])
            if lhs >= rhs { a[i] = UInt32(lhs - rhs); borrow = 0 } else { a[i] = UInt32(lhs + (1 << 32) - rhs); borrow = 1 }
        }
    }

    /// Parses PKCS#1 RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
    private static func parseRSAPublicKey(_ der: Data) -> (Data, Data) {
        var p = der.startIndex
        func readLen() -> Int {
            let b = der[p]; p += 1
            if b & 0x80 == 0 { return Int(b) }
            var n = 0
            for _ in 0..<Int(b & 0x7f) { n = (n << 8) | Int(der[p]); p += 1 }
            return n
        }
        p += 1; _ = readLen()            // SEQUENCE
        p += 1; let ml = readLen()       // INTEGER modulus
        let m = der[p..<(p + ml)]; p += ml
        p += 1; let el = readLen()       // INTEGER exponent
        let e = der[p..<(p + el)]
        return (Data(m), Data(e))
    }
}
