import UIKit

/// Maps iOS HID usages to QEMU QKeyCode values (qapi/ui.json order).
enum KeyMap {
    enum Q: Int32 {
        case shift = 1, shiftR = 2, alt = 3, altR = 4, ctrl = 5, ctrlR = 6, menu = 7, esc = 8
        case k1 = 9, k2, k3, k4, k5, k6, k7, k8, k9, k0, minus, equal, backspace, tab
        case q = 23, w, e, r, t, y, u, i, o, p, bracketLeft, bracketRight, ret
        case a = 36, s, d, f, g, h, j, k, l, semicolon, apostrophe, grave, backslash
        case z = 49, x, c, v, b, n, m, comma, dot, slash, asterisk, space, capsLock
        case f1 = 62, f2, f3, f4, f5, f6, f7, f8, f9, f10
        case f11 = 92, f12 = 93
        case home = 95, pgUp = 96, pgDn = 97, end = 98, left = 99, up = 100, down = 101, right = 102
        case insert = 103, delete = 104
        case metaL = 117, metaR = 118
        case power = 129
        case audioNext = 132, audioPrev = 133, audioStop = 134, audioPlay = 135, mute = 136
        case volumeUp = 137, volumeDown = 138
        case acHome = 143, acBack = 144
    }

    static func qcode(for usage: UIKeyboardHIDUsage) -> Int32? {
        table[usage.rawValue].map { $0.rawValue }
    }

    private static let table: [Int: Q] = {
        var t: [Int: Q] = [:]
        let letters: [Q] = [.a, .b, .c, .d, .e, .f, .g, .h, .i, .j, .k, .l, .m, .n, .o, .p, .q, .r, .s, .t, .u, .v, .w, .x, .y, .z]
        for (i, q) in letters.enumerated() { t[0x04 + i] = q }
        let digits: [Q] = [.k1, .k2, .k3, .k4, .k5, .k6, .k7, .k8, .k9, .k0]
        for (i, q) in digits.enumerated() { t[0x1E + i] = q }
        t[0x28] = .ret; t[0x29] = .esc; t[0x2A] = .backspace; t[0x2B] = .tab; t[0x2C] = .space
        t[0x2D] = .minus; t[0x2E] = .equal; t[0x2F] = .bracketLeft; t[0x30] = .bracketRight
        t[0x31] = .backslash; t[0x33] = .semicolon; t[0x34] = .apostrophe; t[0x35] = .grave
        t[0x36] = .comma; t[0x37] = .dot; t[0x38] = .slash; t[0x39] = .capsLock
        let fkeys: [Q] = [.f1, .f2, .f3, .f4, .f5, .f6, .f7, .f8, .f9, .f10, .f11, .f12]
        for (i, q) in fkeys.enumerated() { t[0x3A + i] = q }
        t[0x49] = .insert; t[0x4A] = .home; t[0x4B] = .pgUp; t[0x4C] = .delete; t[0x4D] = .end
        t[0x4E] = .pgDn; t[0x4F] = .right; t[0x50] = .left; t[0x51] = .down; t[0x52] = .up
        t[0xE0] = .ctrl; t[0xE1] = .shift; t[0xE2] = .alt; t[0xE3] = .metaL
        t[0xE4] = .ctrlR; t[0xE5] = .shiftR; t[0xE6] = .altR; t[0xE7] = .metaR
        t[0x7F] = .mute; t[0x80] = .volumeUp; t[0x81] = .volumeDown
        return t
    }()
}
