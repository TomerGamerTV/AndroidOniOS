#!/usr/bin/env python3
"""Lays out /sys/fs/bpf for onirun from the image's BPF object files.

  mkbpffs.py ROOTFS

netbpfload would load the programs in /apex/com.android.tethering/etc/bpf/mainline/*.o into the
kernel and pin them; onirun has no kernel BPF, but the network stack verifies the pinned files
(mode, owner, SELinux label) and uses the maps through bpf(2). This script reads each object's
.android_maps / .android_progs definitions (pin path, type, owner, mode) and the BTF map
descriptions (key/value sizes, max_entries), creates the pinned files with the expected
attributes, and writes /oni/bpf.txt, which onirun's bpf emulation reads at startup:

  map  PATH TYPE KEY_SIZE VALUE_SIZE MAX_ENTRIES UID GID MODE
  prog PATH TYPE

Nothing here is executed; the objects are only parsed.
"""
import os, struct, subprocess, sys

OBJ_DIRS = ['apex/com.android.tethering/etc/bpf/mainline']


class Elf:
    def __init__(self, data):
        self.f = data
        shoff = struct.unpack_from('<Q', data, 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
        self.secs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
        strtab = self.secs[shstrndx]
        self.names = [self.cstr(strtab[4], s[0]) for s in self.secs]

    def cstr(self, base, off):
        s = self.f[base + off:]
        return s[:s.index(b'\0')].decode()

    def section(self, name):
        if name not in self.names:
            return None
        s = self.secs[self.names.index(name)]
        return self.f[s[4]:s[4] + s[5]]

    def symbols_in(self, name):
        if name not in self.names:
            return []
        idx = self.names.index(name)
        sym = self.secs[self.names.index('.symtab')]
        symstr = self.secs[sym[6]]
        out = []
        for k in range(sym[5] // 24):
            st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', self.f, sym[4] + k * 24)
            if st_shndx == idx and st_name:
                out.append((st_value, self.cstr(symstr[4], st_name), st_size))
        return sorted(out)


class Btf:
    """Enough of the BTF type graph to size BTF-defined maps."""
    def __init__(self, data):
        magic, version, flags, hdr_len, type_off, type_len, str_off, str_len = struct.unpack_from('<HBBIIIII', data, 0)
        if magic != 0xeB9F:
            raise ValueError('not BTF')
        self.strs = data[hdr_len + str_off:hdr_len + str_off + str_len]
        self.types = [None]                      # type ids start at 1
        pos, end = hdr_len + type_off, hdr_len + type_off + type_len
        while pos < end:
            name_off, info, size_or_type = struct.unpack_from('<III', data, pos)
            pos += 12
            kind, vlen = (info >> 24) & 0x1f, info & 0xffff
            t = {'kind': kind, 'name': self.s(name_off), 'size': size_or_type, 'vlen': vlen, 'members': []}
            if kind == 1:                        # INT
                pos += 4
            elif kind == 3:                      # ARRAY
                t['elem'], t['index'], t['nelems'] = struct.unpack_from('<III', data, pos); pos += 12
            elif kind in (4, 5):                 # STRUCT, UNION
                for _ in range(vlen):
                    n, ty, off = struct.unpack_from('<III', data, pos); pos += 12
                    t['members'].append((self.s(n), ty))
            elif kind == 6:                      # ENUM
                pos += 8 * vlen
            elif kind == 13:                     # FUNC_PROTO
                pos += 8 * vlen
            elif kind == 14:                     # VAR
                pos += 4
            elif kind == 15:                     # DATASEC
                for _ in range(vlen):
                    ty, off, sz = struct.unpack_from('<III', data, pos); pos += 12
                    t['members'].append((ty, off))
            elif kind == 17:                     # DECL_TAG
                pos += 4
            elif kind == 19:                     # ENUM64
                pos += 12 * vlen
            self.types.append(t)

    def s(self, off):
        e = self.strs.index(b'\0', off)
        return self.strs[off:e].decode()

    def sizeof(self, tid):
        t = self.types[tid]
        k = t['kind']
        if k in (1, 4, 5, 6, 16, 19):
            return t['size']
        if k == 2:
            return 8
        if k == 3:
            return t['nelems'] * self.sizeof(t['elem'])
        if k in (8, 9, 10, 11, 18):              # TYPEDEF, VOLATILE, CONST, RESTRICT, TYPE_TAG
            return self.sizeof(t['size'])
        raise ValueError(f'cannot size BTF kind {k}')

    def map_desc(self, var_name):
        """key_size, value_size, max_entries, type of the BTF-defined map variable (DATASEC .maps)."""
        for t in self.types[1:]:
            if t['kind'] != 15 or t['name'] != '.maps':
                continue
            for vt, _ in t['members']:
                var = self.types[vt]
                if var['name'] != var_name:
                    continue
                st = self.types[var['size']]            # the VAR's type: struct of pointers
                d = {}
                for name, ty in st['members']:
                    p = self.types[ty]
                    if p['kind'] != 2:
                        continue
                    target = self.types[p['size']]
                    if name in ('key', 'value'):
                        d[name + '_size'] = self.sizeof(p['size'])
                    elif target['kind'] == 3:            # pointer to array: the array length is the value
                        d[name] = target['nelems']
                return d
        return None

def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    R = os.path.abspath(sys.argv[1])
    lines, seen = [], set()
    for d in OBJ_DIRS:
        full = os.path.join(R, d)
        if not os.path.isdir(full):
            continue
        for fn in sorted(os.listdir(full)):
            if not fn.endswith('.o') or '@' in fn:
                continue
            elf = Elf(open(os.path.join(full, fn), 'rb').read())
            btf_data = elf.section('.BTF')
            btf = Btf(btf_data) if btf_data else None
            maps = elf.section('.android_maps')
            for off, sym, size in elf.symbols_in('.android_maps'):
                e = maps[off:off + size]
                ints = struct.unpack_from('<9I', e, 0)
                pin = e[106:e.index(b'\0', 106)].decode()
                if not pin or pin in seen:
                    continue
                desc = (btf.map_desc(sym[:-4] if sym.endswith('_def') else sym) if btf else None) or {}
                if not desc:
                    print('no BTF description for map', sym, 'in', fn, file=sys.stderr)
                seen.add(pin)
                lines.append(('map', pin, ints[0], desc.get('key_size', 0), desc.get('value_size', 0), desc.get('max_entries', 0),
                              ints[1], ints[3], ints[4] or 0o660))
            progs = elf.section('.android_progs')
            for off, sym, size in elf.symbols_in('.android_progs'):
                e = progs[off:off + size]
                pin = e[106:e.index(b'\0', 106)].decode()
                if not pin or pin in seen:
                    continue
                seen.add(pin)
                lines.append(('prog', pin, struct.unpack_from('<I', e, 0)[0], 0, 0, 0, 0, 1000, 0o440))
    with open(os.path.join(R, 'oni/bpf.txt'), 'w') as f:
        for kind, pin, typ, ks, vs, me, uid, gid, mode in lines:
            f.write(f'{kind} {pin} {typ} {ks} {vs} {me} {uid} {gid} {mode:o}\n' if kind == 'map' else f'{kind} {pin} {typ}\n')
            host = os.path.join(R, pin.lstrip('/'))
            os.makedirs(os.path.dirname(host), exist_ok=True)
            open(host, 'a').close()
            os.chmod(host, mode | 0o600)        # keep it writable for the xattrs below; stat() reports the real mode
            sub = pin[len('/sys/fs/bpf/'):].split('/')[0] if pin.count('/') > 3 else ''
            ctx = f'u:object_r:fs_bpf_{sub}:s0' if sub else 'u:object_r:fs_bpf:s0'
            for k, v in (('security.selinux', ctx), ('user.oni.uid', str(uid)), ('user.oni.gid', str(gid)), ('user.oni.mode', f'{mode:o}')):
                subprocess.run(['xattr', '-w', k, v, host], check=True)
    print(f'{len(lines)} BPF objects laid out under {R}/sys/fs/bpf')


if __name__ == '__main__':
    main()
