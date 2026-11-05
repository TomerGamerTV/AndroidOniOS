#!/usr/bin/env python3
"""Minimal Android dynamic-partition (super / liblp) reader and in-place writer.

Usage:
  lp.py dump <super.img> [--offset N]
  lp.py extract <super.img> <partition> <out.img> [--offset N]
  lp.py replace <super.img> <partition> <in.img> [--offset N]   (in-place; must fit in existing extents)

<super.img> may be a whole GPT disk if --offset points at the super partition start (bytes).
"""
import struct, sys, os

LP_GEOMETRY_MAGIC = 0x616C4467
LP_HEADER_MAGIC = 0x414C5030
SECTOR = 512


def read_at(f, off, n):
    f.seek(off)
    return f.read(n)


def parse(path, base):
    f = open(path, 'rb')
    geo = read_at(f, base + 4096, 4096)
    magic, struct_size = struct.unpack_from('<II', geo, 0)
    assert magic == LP_GEOMETRY_MAGIC, hex(magic)
    meta_max_size, slot_count, logical_block_size = struct.unpack_from('<III', geo, 8 + 32)
    # metadata primary copy starts at base + 4096 + 2*4096 (geometry + backup)
    meta_off = base + 4096 * 3
    hdr = read_at(f, meta_off, 256)
    (hmagic, major, minor, header_size) = struct.unpack_from('<IHHI', hdr, 0)
    assert hmagic == LP_HEADER_MAGIC, hex(hmagic)
    # header layout (v10.2): magic, major, minor, header_size, header_checksum[32], tables_size, tables_checksum[32],
    # partitions desc (offset,num_entries,entry_size), extents desc, groups desc, block_devices desc, flags, reserved
    p = 4 + 2 + 2 + 4 + 32
    tables_size, = struct.unpack_from('<I', hdr, p); p += 4 + 32
    descs = []
    for _ in range(4):
        descs.append(struct.unpack_from('<III', hdr, p)); p += 12
    tables = read_at(f, meta_off + header_size, tables_size)
    parts, extents, groups, bdevs = [], [], [], []
    off, n, sz = descs[0]
    for i in range(n):
        e = tables[off + i * sz: off + (i + 1) * sz]
        name = e[:36].split(b'\0')[0].decode()
        attrs, first_ext, num_ext, group = struct.unpack_from('<IIII', e, 36)
        parts.append(dict(name=name, attrs=attrs, first=first_ext, num=num_ext, group=group))
    off, n, sz = descs[1]
    for i in range(n):
        nsec, ttype, tdata, tsrc = struct.unpack_from('<QIQI', tables, off + i * sz)
        extents.append(dict(sectors=nsec, type=ttype, data=tdata, source=tsrc))
    off, n, sz = descs[3]
    for i in range(n):
        e = tables[off + i * sz: off + (i + 1) * sz]
        first_sector, alignment, align_off, size = struct.unpack_from('<QIIQ', e, 0)
        name = e[24:24 + 36].split(b'\0')[0].decode()
        bdevs.append(dict(name=name, first_sector=first_sector, size=size))
    return dict(parts=parts, extents=extents, bdevs=bdevs, block_size=logical_block_size)


def part_extents(md, name):
    for p in md['parts']:
        if p['name'] == name:
            return md['extents'][p['first']:p['first'] + p['num']]
    raise SystemExit(f'partition {name} not found')


def main():
    args = sys.argv[1:]
    base = 0
    if '--offset' in args:
        i = args.index('--offset'); base = int(args[i + 1], 0); del args[i:i + 2]
    cmd, path = args[0], args[1]
    md = parse(path, base)
    if cmd == 'dump':
        print('block devices:', md['bdevs'])
        for p in md['parts']:
            exts = md['extents'][p['first']:p['first'] + p['num']]
            size = sum(e['sectors'] for e in exts) * SECTOR
            print(f"{p['name']:24s} size={size:>12d} ({size/1048576:.1f} MiB) extents=" +
                  ','.join(f"[{e['data']}+{e['sectors']}@dev{e['source']}]" for e in exts))
    elif cmd == 'extract':
        name, out = args[2], args[3]
        with open(path, 'rb') as f, open(out, 'wb') as o:
            for e in part_extents(md, name):
                assert e['type'] == 0 and e['source'] == 0
                f.seek(base + e['data'] * SECTOR)
                left = e['sectors'] * SECTOR
                while left:
                    chunk = f.read(min(left, 1 << 24)); o.write(chunk); left -= len(chunk)
    elif cmd == 'replace':
        name, inp = args[2], args[3]
        exts = part_extents(md, name)
        cap = sum(e['sectors'] for e in exts) * SECTOR
        size = os.path.getsize(inp)
        if size > cap:
            raise SystemExit(f'{inp} is {size} bytes, exceeds partition capacity {cap}')
        with open(path, 'r+b') as f, open(inp, 'rb') as i:
            for e in exts:
                f.seek(base + e['data'] * SECTOR)
                left = e['sectors'] * SECTOR
                while left:
                    chunk = i.read(min(left, 1 << 24))
                    if not chunk:
                        f.write(b'\0' * left); left = 0; break
                    f.write(chunk); left -= len(chunk)
        print(f'replaced {name}: {size} bytes into {cap} capacity')


if __name__ == '__main__':
    main()
