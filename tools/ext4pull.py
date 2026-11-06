#!/usr/bin/env python3
"""Read files out of an ext4 image (or a GPT disk image holding ext4 partitions) without mounting.

  ext4pull.py IMAGE parts                      list GPT partitions
  ext4pull.py IMAGE [-p PART] ls PATH          list a directory
  ext4pull.py IMAGE [-p PART] pull SRC DEST    copy a file or tree (symlinks and modes kept)

Used to extract Android system partitions for onirun's rootfs. Read-only; the image is untrusted
input and only ever parsed, never executed.
"""
import argparse, os, struct, sys


class Ext4:
    def __init__(self, f, base, size):
        self.f, self.base, self.size = f, base, size
        sb = self.read_raw(1024, 1024)
        if struct.unpack_from('<H', sb, 0x38)[0] != 0xEF53:
            raise SystemExit('not an ext4 filesystem')
        self.block_size = 1024 << struct.unpack_from('<I', sb, 0x18)[0]
        self.inodes_per_group = struct.unpack_from('<I', sb, 0x28)[0]
        self.inode_size = struct.unpack_from('<H', sb, 0x58)[0]
        self.first_data_block = struct.unpack_from('<I', sb, 0x14)[0]
        incompat = struct.unpack_from('<I', sb, 0x60)[0]
        self.desc_size = struct.unpack_from('<H', sb, 0xFE)[0] if incompat & 0x80 else 32
        self.cache = {}

    def read_raw(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def block(self, b):
        return self.read_raw(b * self.block_size, self.block_size)

    def inode(self, ino):
        if ino in self.cache:
            return self.cache[ino]
        group, index = divmod(ino - 1, self.inodes_per_group)
        gd_off = (self.first_data_block + 1) * self.block_size + group * self.desc_size
        gd = self.read_raw(gd_off, self.desc_size)
        table = struct.unpack_from('<I', gd, 8)[0]
        if self.desc_size >= 64:
            table |= struct.unpack_from('<I', gd, 0x28)[0] << 32
        raw = self.read_raw(table * self.block_size + index * self.inode_size, self.inode_size)
        mode, = struct.unpack_from('<H', raw, 0)
        size = struct.unpack_from('<I', raw, 4)[0] | (struct.unpack_from('<I', raw, 0x6C)[0] << 32)
        flags, = struct.unpack_from('<I', raw, 0x20)
        node = (mode, size, flags, raw[0x28:0x28 + 60])
        self.cache[ino] = node
        return node

    def extents(self, iblock, out):
        magic, entries, _max, depth = struct.unpack_from('<HHHH', iblock, 0)
        if magic != 0xF30A:
            raise SystemExit('inode without extents (unsupported)')
        for i in range(entries):
            e = iblock[12 + 12 * i:24 + 12 * i]
            if depth == 0:
                lblk, length, hi, lo = struct.unpack('<IHHI', e)
                out.append((lblk, length & 0x7FFF, lo | (hi << 32)))
            else:
                lblk, lo, hi, _ = struct.unpack('<IIHH', e)
                self.extents(self.block(lo | (hi << 32)), out)
        return out

    def read_file(self, ino):
        mode, size, flags, iblock = self.inode(ino)
        if flags & 0x10000000:                       # inline data
            return iblock[:size]
        data = bytearray(size)
        for lblk, length, pblk in self.extents(iblock, []):
            off = lblk * self.block_size
            n = min(length * self.block_size, size - off)
            if n > 0:
                data[off:off + n] = self.read_raw(pblk * self.block_size, n)
        return bytes(data)

    def readlink(self, ino):
        mode, size, flags, iblock = self.inode(ino)
        if size < 60 and not (flags & 0x80000):
            return iblock[:size].decode()
        return self.read_file(ino).decode()

    def listdir(self, ino):
        out = []
        for lblk, length, pblk in self.extents(self.inode(ino)[3], []):
            for b in range(length):
                blk = self.read_raw((pblk + b) * self.block_size, self.block_size)
                pos = 0
                while pos + 8 <= len(blk):
                    cino, rec_len, name_len, ftype = struct.unpack_from('<IHBB', blk, pos)
                    if rec_len < 8:
                        break
                    if cino and name_len:
                        name = blk[pos + 8:pos + 8 + name_len].decode(errors='replace')
                        if name not in ('.', '..'):
                            out.append((name, cino, ftype))
                    pos += rec_len
        return out

    def lookup(self, path):
        ino = 2
        for comp in path.strip('/').split('/'):
            if not comp:
                continue
            for name, cino, ftype in self.listdir(ino):
                if name == comp:
                    ino = cino
                    break
            else:
                raise SystemExit(f'{path}: not found')
        return ino

    def pull(self, ino, dest, rel=''):
        mode, size, flags, _ = self.inode(ino)
        kind = mode & 0xF000
        if kind == 0x4000:
            os.makedirs(dest, exist_ok=True)
            for name, cino, ftype in self.listdir(ino):
                self.pull(cino, os.path.join(dest, name), rel + '/' + name)
        elif kind == 0xA000:
            if os.path.lexists(dest):
                os.remove(dest)
            os.symlink(self.readlink(ino), dest)
        elif kind == 0x8000:
            with open(dest, 'wb') as out:
                out.write(self.read_file(ino))
            os.chmod(dest, (mode & 0o777) | 0o600)
        # devices, sockets and fifos are skipped


def lp_parts(f, base):
    """Logical partitions inside an Android 'super' partition (liblp metadata)."""
    f.seek(base + 4096)
    geo = f.read(4096)
    if struct.unpack_from('<I', geo, 0)[0] != 0x616C4467:
        return None
    f.seek(base + 4096 + 2 * 4096)                     # primary metadata, slot 0
    hdr = f.read(256)
    if struct.unpack_from('<I', hdr, 0)[0] != 0x414C5030:
        return None
    header_size = struct.unpack_from('<I', hdr, 8)[0]
    tables = base + 4096 + 2 * 4096 + header_size
    def table(off):
        return struct.unpack_from('<III', hdr, off)     # offset, num_entries, entry_size
    p_off, p_n, p_sz = table(0x50)
    e_off, e_n, e_sz = table(0x5C)
    f.seek(tables + e_off)
    extents = [struct.unpack('<QIQI', f.read(e_sz)[:24]) for _ in range(e_n)]
    out = []
    for i in range(p_n):
        f.seek(tables + p_off + i * p_sz)
        e = f.read(p_sz)
        name = e[:36].split(b'\0')[0].decode()
        first, count = struct.unpack_from('<II', e, 40)
        if count == 1 and extents[first][1] == 0:       # one LINEAR extent
            sectors, _, start, _ = extents[first]
            out.append((name, base + start * 512, sectors * 512))
    return out


def gpt_parts(f):
    f.seek(512)
    hdr = f.read(512)
    if hdr[:8] != b'EFI PART':
        return None
    entries_lba, count, esize = struct.unpack_from('<QII', hdr, 0x48)
    f.seek(entries_lba * 512)
    parts = []
    for i in range(count):
        e = f.read(esize)
        first, last = struct.unpack_from('<QQ', e, 0x20)
        name = e[0x38:0x38 + 72].decode('utf-16le').rstrip('\0')
        if name:
            parts.append((name, first * 512, (last - first + 1) * 512))
    return parts


def apex_payload(f):
    """Offset and size of apex_payload.img inside an APEX (a zip that stores it uncompressed)."""
    import zipfile
    z = zipfile.ZipFile(f)
    info = z.getinfo('apex_payload.img')
    if info.compress_type != zipfile.ZIP_STORED:
        raise SystemExit('apex payload is compressed')
    f.seek(info.header_offset)
    hdr = f.read(30)
    name_len, extra_len = struct.unpack_from('<HH', hdr, 26)
    return info.header_offset + 30 + name_len + extra_len, info.file_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('-p', '--part', help='GPT partition name (default: the first or only filesystem)')
    ap.add_argument('cmd', choices=['parts', 'ls', 'pull'])
    ap.add_argument('args', nargs='*')
    a = ap.parse_args()
    f = open(a.image, 'rb')
    if a.image.endswith('.apex'):
        off, size = apex_payload(f)
        parts = [('apex_payload', off, size)]
    else:
        parts = gpt_parts(f)
    if parts:                                           # expand a 'super' partition
        flat = []
        for name, off, size in parts:
            flat += lp_parts(f, off) or [(name, off, size)]
        parts = flat
    else:
        parts = lp_parts(f, 0)
    if a.cmd == 'parts':
        for name, off, size in parts or [('(whole image)', 0, os.path.getsize(a.image))]:
            print(f'{name:16} {size >> 20:8} MiB')
        return
    if parts:
        want = a.part or parts[0][0]
        match = [p for p in parts if p[0] == want]
        if not match:
            raise SystemExit(f'no partition {want}')
        base, size = match[0][1], match[0][2]
    else:
        base, size = 0, os.path.getsize(a.image)
    fs = Ext4(f, base, size)
    if a.cmd == 'ls':
        ino = fs.lookup(a.args[0] if a.args else '/')
        for name, cino, ftype in sorted(fs.listdir(ino)):
            mode, sz, _, _ = fs.inode(cino)
            tag = {0x4000: 'd', 0xA000: 'l'}.get(mode & 0xF000, '-')
            extra = ' -> ' + fs.readlink(cino) if tag == 'l' else ''
            print(f'{tag}{mode & 0o7777:04o} {sz:10} {name}{extra}')
    else:
        src, dest = a.args
        fs.pull(fs.lookup(src), dest)


if __name__ == '__main__':
    main()
