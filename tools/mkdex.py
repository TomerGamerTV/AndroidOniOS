#!/usr/bin/env python3
"""Writes a small DEX file by hand, for testing ART under onirun without a Java toolchain.

  mkdex.py OUT.dex [hello|bench]

The class `Hello` has one method, `public static void main(String[] args)`, which prints a few
lines through System.out.println, catches a NullPointerException it provokes on purpose, and
exits. The file follows the DEX 035 format: sorted id tables, a map list, Adler-32 checksum and
SHA-1 signature.
"""
import hashlib, struct, sys, zlib


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def mutf8(s):
    return uleb(len(s)) + s.encode('utf-8') + b'\0'


class Dex:
    def __init__(self):
        self.strings, self.types, self.protos, self.fields, self.methods = [], [], [], [], []

    def s(self, v):
        if v not in self.strings:
            self.strings.append(v)
        return v

    def t(self, desc):
        self.s(desc)
        if desc not in self.types:
            self.types.append(desc)
        return desc

    def p(self, ret, params):
        shorty = ''.join('L' if d.startswith(('L', '[')) else d for d in [ret] + params)
        self.s(shorty)
        self.t(ret)
        for d in params:
            self.t(d)
        key = (ret, tuple(params), shorty)
        if key not in self.protos:
            self.protos.append(key)
        return key

    def f(self, cls, typ, name):
        self.t(cls); self.t(typ); self.s(name)
        key = (cls, typ, name)
        if key not in self.fields:
            self.fields.append(key)
        return key

    def m(self, cls, ret, params, name):
        self.t(cls); self.s(name)
        proto = self.p(ret, params)
        key = (cls, proto, name)
        if key not in self.methods:
            self.methods.append(key)
        return key

    def finish(self):
        # Sort the way the format requires (string order is by UTF-16 code units; ASCII here).
        self.strings.sort()
        self.types.sort(key=lambda d: self.strings.index(d))
        self.protos.sort(key=lambda k: (self.types.index(k[0]), [self.types.index(x) for x in k[1]]))
        self.fields.sort(key=lambda k: (self.types.index(k[0]), self.strings.index(k[2]), self.types.index(k[1])))
        self.methods.sort(key=lambda k: (self.types.index(k[0]), self.strings.index(k[2]), self.protos.index(k[1])))

    def sid(self, v): return self.strings.index(v)
    def tid(self, v): return self.types.index(v)
    def pid(self, v): return self.protos.index(v)
    def fid(self, v): return self.fields.index(v)
    def mid(self, v): return self.methods.index(v)


def build_bench():
    """class Bench { static void main(String[] a) { int s=0; for (int i=0;i<N;i++) s += (i*i) % 7; System.out.println(Integer.toString(s)); } }"""
    d = Dex()
    BENCH, OBJ, STR, STRARR, SYS, PS, INT = 'LBench;', 'Ljava/lang/Object;', 'Ljava/lang/String;', '[Ljava/lang/String;', 'Ljava/lang/System;', 'Ljava/io/PrintStream;', 'Ljava/lang/Integer;'
    for ty in (BENCH, OBJ, STR, STRARR, SYS, PS, INT):
        d.t(ty)
    out = d.f(SYS, PS, 'out')
    println = d.m(PS, 'V', [STR], 'println')
    tostr = d.m(INT, STR, ['I'], 'toString')
    main = d.m(BENCH, 'V', [STRARR], 'main')
    d.finish()
    code = bytearray()
    def op(*words):
        for w in words:
            code.extend(struct.pack('<H', w))
    N = 30000000
    # v0 = s, v1 = i, v2 = tmp, v3 = N, v4 = System.out (arg in v5)
    op(0x0012)                                    # const/4 v0, 0
    op(0x0112)                                    # const/4 v1, 0
    op(0x0314, N & 0xffff, N >> 16)               # const v3, N
    loop = len(code) // 2
    op(0x0292, 0x0101)                            # mul-int v2, v1, v1
    op(0x0213, 0x0007)                            # const/16 v2... (placeholder replaced below)
    code[-4:] = struct.pack('<HH', 0x0613, 0x0007)   # const/16 v6, 7
    op(0x0294, 0x0602)                            # rem-int v2, v2, v6
    op(0x20b0)                                    # add-int/2addr v0, v2
    op(0x0001 << 8 | 0xd8, 0x0101)                # add-int/lit8 v1, v1, 1
    end_branch = len(code) // 2
    op(0x3134 | 0, 0)                             # if-lt v1, v3, loop (offset patched)
    off = loop - end_branch
    code[-4:] = struct.pack('<HH', 0x3134, off & 0xffff)
    op(0x0462, d.fid(out))                        # sget-object v4, System.out
    op(0x1071, d.mid(tostr), 0x0000)              # invoke-static {v0}, Integer.toString
    op(0x020c)                                    # move-result-object v2
    op(0x206e, d.mid(println), 0x0024)            # invoke-virtual {v4, v2}, println
    op(0x000e)                                    # return-void
    insns = bytes(code)
    if len(insns) % 4:
        insns += b'\0\0'
    code_item = struct.pack('<HHHHII', 7, 1, 2, 0, 0, len(insns) // 2) + insns
    return d, BENCH, OBJ, main, code_item


def build(which='hello'):
    if which == 'bench':
        d, CLS, OBJ, main, code_item = build_bench()
        return layout(d, CLS, OBJ, main, code_item)
    d = Dex()
    HELLO, OBJ, STR, STRARR, SYS, PS, NPE, THROWABLE = 'LHello;', 'Ljava/lang/Object;', 'Ljava/lang/String;', '[Ljava/lang/String;', 'Ljava/lang/System;', 'Ljava/io/PrintStream;', 'Ljava/lang/NullPointerException;', 'Ljava/lang/Throwable;'
    for ty in (HELLO, OBJ, STR, STRARR, SYS, PS, NPE, THROWABLE):
        d.t(ty)
    out = d.f(SYS, PS, 'out')
    println = d.m(PS, 'V', [STR], 'println')
    hashcode = d.m(OBJ, 'I', [], 'hashCode')
    getmsg = d.m(THROWABLE, STR, [], 'getMessage')
    main = d.m(HELLO, 'V', [STRARR], 'main')
    msgs = [d.s('Hello from ART under onirun'), d.s('strings and classes resolve'), d.s('caught the NullPointerException')]
    d.finish()

    # main(String[] args): v0 = System.out; println x2; v1 = null; v1.hashCode() -> NPE handler; println; return
    code = bytearray()
    def op(*words):
        for w in words:
            code.extend(struct.pack('<H', w))
    # sget-object v0, System.out
    op(0x0062 | 0, d.fid(out))
    # const-string v1, msg0 ; invoke-virtual {v0, v1}, println
    op(0x011a, d.sid(msgs[0])); op(0x206e, d.mid(println), 0x0010)
    op(0x011a, d.sid(msgs[1])); op(0x206e, d.mid(println), 0x0010)
    # const/4 v1, #0 ; invoke-virtual {v1}, Object.hashCode  (throws NPE)
    try_start = len(code) // 2
    op(0x0112)                                    # const/4 v1, #0
    op(0x106e, d.mid(hashcode), 0x0001)           # invoke-virtual {v1}, hashCode
    try_end = len(code) // 2
    op(0x000e)                                    # return-void
    # handler: move-exception v1; const-string v1, msg2; println; return-void
    handler = len(code) // 2
    op(0x010d)                                    # move-exception v1
    op(0x011a, d.sid(msgs[2])); op(0x206e, d.mid(println), 0x0010)
    op(0x000e)
    insns = bytes(code)
    if len(insns) % 4:
        insns += b'\0\0'
    tries = struct.pack('<IHH', try_start, try_end - try_start, 0)   # start_addr, insn_count, handler_off (filled below)
    # encoded_catch_handler_list: size 1; handler: size 1 (one typed catch), type NPE, addr
    handlers = uleb(1) + uleb(1) + uleb(d.tid(NPE)) + uleb(handler)
    tries = struct.pack('<IHH', try_start, try_end - try_start, 1)   # handler_off = offset of the handler within the list (after the size uleb)
    code_item = struct.pack('<HHHHII', 2, 1, 2, 1, 0, len(insns) // 2) + insns + tries + handlers
    while len(code_item) % 4:
        code_item += b'\0'
    return layout(d, HELLO, OBJ, main, code_item)


def layout(d, CLS, OBJ, main, code_item):
    # ---- lay out the file ----
    off = 0x70
    string_ids_off = off; off += 4 * len(d.strings)
    type_ids_off = off; off += 4 * len(d.types)
    proto_ids_off = off; off += 12 * len(d.protos)
    field_ids_off = off; off += 8 * len(d.fields)
    method_ids_off = off; off += 8 * len(d.methods)
    class_defs_off = off; off += 32
    data_off = off
    data = bytearray()
    def align(n):
        while (data_off + len(data)) % n:
            data.append(0)
    # type lists (for protos with params)
    type_list_off = {}
    for k in d.protos:
        if k[1]:
            align(4)
            type_list_off[k] = data_off + len(data)
            data += struct.pack('<I', len(k[1])) + b''.join(struct.pack('<H', d.tid(x)) for x in k[1])
    align(4)
    code_off = data_off + len(data)
    data += code_item
    string_off = {}
    for s in d.strings:
        string_off[s] = data_off + len(data)
        data += mutf8(s)
    class_data_off = data_off + len(data)
    data += uleb(0) + uleb(0) + uleb(1) + uleb(0)                       # static/instance fields, direct/virtual methods
    data += uleb(d.mid(main)) + uleb(0x0009) + uleb(code_off)         # public static main
    align(4)
    map_off = data_off + len(data)
    items = [(0x0000, 1, 0), (0x0001, len(d.strings), string_ids_off), (0x0002, len(d.types), type_ids_off),
             (0x0003, len(d.protos), proto_ids_off), (0x0004, len(d.fields), field_ids_off),
             (0x0005, len(d.methods), method_ids_off), (0x0006, 1, class_defs_off)]
    if type_list_off:
        items.append((0x1001, len(type_list_off), min(type_list_off.values())))
    items += [(0x2001, 1, code_off), (0x2002, len(d.strings), string_off[d.strings[0]]),
              (0x2000, 1, class_data_off), (0x1000, 1, map_off)]
    items.sort(key=lambda x: x[2])
    data += struct.pack('<I', len(items)) + b''.join(struct.pack('<HHII', t, 0, n, o) for t, n, o in items)

    body = bytearray()
    body += b''.join(struct.pack('<I', string_off[s]) for s in d.strings)
    body += b''.join(struct.pack('<I', d.sid(t)) for t in d.types)
    for k in d.protos:
        body += struct.pack('<III', d.sid(k[2]), d.tid(k[0]), type_list_off.get(k, 0))
    for k in d.fields:
        body += struct.pack('<HHI', d.tid(k[0]), d.tid(k[1]), d.sid(k[2]))
    for k in d.methods:
        body += struct.pack('<HHI', d.tid(k[0]), d.pid(k[1]), d.sid(k[2]))
    body += struct.pack('<8I', d.tid(CLS), 0x0001, d.tid(OBJ), 0, 0xFFFFFFFF, 0, class_data_off, 0)
    body += data
    file_size = 0x70 + len(body)
    header = bytearray(b'dex\n035\0' + b'\0' * 24)                     # magic, checksum, signature filled below
    header += struct.pack('<IIIII', file_size, 0x70, 0x12345678, 0, 0)   # size, header size, endian, link size/off
    header += struct.pack('<I', map_off)
    header += struct.pack('<II', len(d.strings), string_ids_off)
    header += struct.pack('<II', len(d.types), type_ids_off)
    header += struct.pack('<II', len(d.protos), proto_ids_off)
    header += struct.pack('<II', len(d.fields), field_ids_off)
    header += struct.pack('<II', len(d.methods), method_ids_off)
    header += struct.pack('<II', 1, class_defs_off)
    header += struct.pack('<II', len(data), data_off)
    assert len(header) == 0x70
    out = header + body
    out[12:32] = hashlib.sha1(out[32:]).digest()
    out[8:12] = struct.pack('<I', zlib.adler32(bytes(out[12:])))
    return bytes(out)


if __name__ == '__main__':
    open(sys.argv[1], 'wb').write(build(sys.argv[2] if len(sys.argv) > 2 else 'hello'))
