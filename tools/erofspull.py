#!/usr/bin/env python3
"""Reads uncompressed EROFS images (APEX payloads built with --payload_fs_type erofs).

  erofspull.py IMAGE ls PATH
  erofspull.py IMAGE pull SRC DEST        copy a file or directory tree out of the image
  erofspull.py IMAGE cat PATH

IMAGE may be an .apex (its apex_payload.img is read) or a raw image. Compact and extended inodes,
flat plain, flat inline and LZ4-compressed (full and compact index) data layouts.
"""
import os, struct, sys, zipfile

MAGIC = 0xE0F5E1E2
S_IFMT, S_IFDIR, S_IFLNK, S_IFREG = 0o170000, 0o040000, 0o120000, 0o100000
L_FULL = 4096


def lz4_block(src, want):
    """Decodes a raw LZ4 block until `want` bytes are produced (EROFS stores partial blocks)."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n and len(out) < want:
        tok = src[i]; i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[i]; i += 1; ll += b
                if b != 255: break
        out += src[i:i + ll]; i += ll
        if i >= n or len(out) >= want:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = (tok & 15) + 4
        if ml == 19:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        if off == 0 or off > len(out):
            raise SystemExit('corrupt lz4 block')
        start = len(out) - off
        if ml <= off:
            out += out[start:start + ml]
        else:
            chunk = out[start:]
            rep = bytes(chunk) * (ml // len(chunk) + 1)
            out += rep[:ml]
    return bytes(out[:want])


class Erofs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.off = 0
        if path.endswith('.apex') or self.f.read(4) == b'PK\x03\x04':
            z = zipfile.ZipFile(path)
            info = z.getinfo('apex_payload.img')
            if info.compress_type != 0:
                raise SystemExit('compressed payload inside the zip; unzip it first')
            self.f.seek(info.header_offset)
            lh = self.f.read(30)
            n, m = struct.unpack_from('<HH', lh, 26)
            self.off = info.header_offset + 30 + n + m
        sb = self.read(1024, 128)
        (magic, _, _, self.blkszbits, _, self.root_nid, _, _, _, self.blocks,
         self.meta_blkaddr, self.xattr_blkaddr) = struct.unpack_from('<IIIBBHQQIIII', sb, 0)
        if magic != MAGIC:
            raise SystemExit('not an EROFS image')
        self.blksz = 1 << self.blkszbits
        self.zero_padding = struct.unpack_from('<I', sb, 80)[0] & 1   # feature_incompat

    def read(self, pos, n):
        self.f.seek(self.off + pos)
        return self.f.read(n)

    def inode(self, nid):
        loc = self.meta_blkaddr * self.blksz + nid * 32
        d = self.read(loc, 64)
        fmt = struct.unpack_from('<H', d, 0)[0]
        ext, layout = fmt & 1, (fmt >> 1) & 7
        xcount = struct.unpack_from('<H', d, 2)[0]
        if ext:
            mode, _, size, u, _, uid, gid = struct.unpack_from('<HHQIIII', d, 4)
            isize = 64
        else:
            mode, _, size, _, u, _, uid, gid = struct.unpack_from('<HHIIIIHH', d, 4)
            isize = 32
        xsize = 12 + (xcount - 1) * 4 if xcount else 0
        return dict(nid=nid, loc=loc, mode=mode, size=size, u=u, uid=uid, gid=gid, layout=layout, hdr=isize + xsize)

    def data(self, ino):
        size, layout = ino['size'], ino['layout']
        if layout == 0:                                           # flat plain
            return self.read(ino['u'] * self.blksz, size)
        if layout == 2:                                           # flat inline: full blocks, then a tail after the inode
            tail = size % self.blksz
            full = size - tail
            out = self.read(ino['u'] * self.blksz, full) if full else b''
            if tail:
                out += self.read(ino['loc'] + ino['hdr'], tail)
            return out
        if layout in (1, 3):
            return self.zdata(ino)
        raise SystemExit(f'unsupported data layout {layout} (chunk-based) for inode {ino["nid"]}')

    # ---- compressed inodes (lz4, 4 KiB logical clusters, one block per physical cluster) ----
    def zdata(self, ino):
        size = ino['size']
        end = ino['loc'] + ino['hdr']
        mh = (end + 7) & ~7
        _, advise, algo, cbits = struct.unpack_from('<IHBB', self.read(mh, 8), 0)
        if (advise & 0x3e) or (algo & 0xf) or (cbits & 7):
            raise SystemExit(f'unsupported compression options (advise {advise:#x} algo {algo} clusterbits {cbits}) for inode {ino["nid"]}')
        L = self.blksz
        total = (size + L - 1) // L
        heads = []                                                 # (logical start, pblk, plain?)
        if ino['layout'] == 1:                                     # full 8-byte indexes
            idx = self.read(mh + 8 + 8, total * 8)
            for lcn in range(total):
                adv, cofs, blk = struct.unpack_from('<HHI', idx, lcn * 8)
                t = adv & 3
                if t != 2:
                    heads.append((lcn * L + cofs, blk, t == 0))
        else:                                                      # compacted 2B/4B packs
            ebase = mh + 8
            init4 = (32 - ebase % 32) // 4
            if init4 == 8:
                init4 = 0
            c2b = (total - init4) // 16 * 16 if (advise & 1) and init4 < total else 0
            for lcn in range(total):
                l = lcn
                pos = ebase
                if l < init4:
                    sh = 2
                else:
                    pos += init4 * 4; l -= init4
                    if l < c2b:
                        sh = 1
                    else:
                        pos += c2b * 2; l -= c2b; sh = 2
                pos += l << sh
                t, cofs, pblk = self.unpack_compact(sh, pos)
                if t != 2:
                    heads.append((lcn * L + cofs, pblk, t == 0))
        out = bytearray()
        for i, (start, pblk, plain) in enumerate(heads):
            nxt = heads[i + 1][0] if i + 1 < len(heads) else size
            need = nxt - start
            blk = self.read(pblk * L, L)
            if plain:
                out += blk[:need]
            else:
                if self.zero_padding:
                    blk = blk.lstrip(b'\0')
                out += lz4_block(blk, need)
        return bytes(out[:size])

    def unpack_compact(self, sh, pos):
        vcnt = 2 if sh == 2 else 16
        packsz = vcnt << sh
        base = pos - (pos % packsz)
        i = (pos - base) >> sh
        pack = self.read(base, packsz)
        encodebits = (packsz - 4) * 8 // vcnt
        lobits = 12

        def dec(k):
            nbits = encodebits * k
            v = struct.unpack_from('<I', pack + b'\0\0\0\0', nbits // 8)[0] >> (nbits & 7)
            return v & ((1 << lobits) - 1), (v >> lobits) & 3
        lo, t = dec(i)
        if t == 2:
            return t, L_FULL, 0
        nblk = 1
        j = i
        while j > 0:
            j -= 1
            lo2, t2 = dec(j)
            if t2 == 2:
                j -= lo2
            if j >= 0:
                nblk += 1
        return t, lo, struct.unpack_from('<I', pack, packsz - 4)[0] + nblk

    def readdir(self, ino):
        d = self.data(ino)
        out = []
        for b in range(0, len(d), self.blksz):
            blk = d[b:b + self.blksz]
            if len(blk) < 12:
                break
            first_nameoff = struct.unpack_from('<H', blk, 8)[0]
            n = first_nameoff // 12
            ents = [struct.unpack_from('<QHBB', blk, i * 12) for i in range(n)]
            for i, (nid, nameoff, ftype, _) in enumerate(ents):
                end = ents[i + 1][1] if i + 1 < n else len(blk)
                name = blk[nameoff:end].split(b'\0', 1)[0].decode()
                if name not in ('.', '..'):
                    out.append((name, nid, ftype))
        return out

    def lookup(self, path):
        ino = self.inode(self.root_nid)
        for part in [p for p in path.split('/') if p]:
            for name, nid, _ in self.readdir(ino):
                if name == part:
                    ino = self.inode(nid)
                    break
            else:
                raise SystemExit(f'{path}: not found')
        return ino

    def pull(self, ino, dest):
        t = ino['mode'] & S_IFMT
        if t == S_IFDIR:
            os.makedirs(dest, exist_ok=True)
            for name, nid, _ in self.readdir(ino):
                self.pull(self.inode(nid), os.path.join(dest, name))
            os.chmod(dest, (ino['mode'] & 0o7777) | 0o700)
        elif t == S_IFLNK:
            if os.path.lexists(dest):
                os.remove(dest)
            os.symlink(self.data(ino).decode(), dest)
        elif t == S_IFREG:
            with open(dest, 'wb') as f:
                f.write(self.data(ino))
            os.chmod(dest, (ino['mode'] & 0o7777) | 0o600)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    fs = Erofs(sys.argv[1])
    cmd, path = sys.argv[2], sys.argv[3]
    ino = fs.lookup(path)
    if cmd == 'ls':
        for name, nid, _ in fs.readdir(ino):
            i = fs.inode(nid)
            print(f'{"d" if i["mode"] & S_IFMT == S_IFDIR else "l" if i["mode"] & S_IFMT == S_IFLNK else "-"}{i["mode"] & 0o7777:04o} {i["size"]:10d} {name}')
    elif cmd == 'cat':
        sys.stdout.buffer.write(fs.data(ino))
    elif cmd == 'pull':
        fs.pull(ino, sys.argv[4])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
