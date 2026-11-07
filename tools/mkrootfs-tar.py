#!/usr/bin/env python3
"""Packs an onirun rootfs into a pax tar that keeps its extended attributes.

  mkrootfs-tar.py ROOTFS OUT.tar

onirun keeps guest ownership, modes and SELinux labels in xattrs (user.oni.*, security.selinux),
which copying files to the iPhone would drop. Each entry carries them as SCHILY.xattr.* pax
records; onirun's installer (userland/src/rootfs_install.c) restores them on the device.
Runtime leftovers (ANR traces, tombstones, dropbox, debug-shell files) are left out.
"""
import ctypes, os, sys, tarfile

libc = ctypes.CDLL(None, use_errno=True)
libc.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
libc.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
libc.listxattr.restype = libc.getxattr.restype = ctypes.c_ssize_t
NOFOLLOW = 1

SKIP = ('data/anr/', 'data/tombstones/', 'data/system/dropbox/', 'data/local/tmp/oni-cmd', 'data/local/tmp/sf.txt')


def xattrs(path):
    p = path.encode()
    n = libc.listxattr(p, None, 0, NOFOLLOW)
    if n <= 0:
        return {}
    buf = ctypes.create_string_buffer(n)
    n = libc.listxattr(p, buf, n, NOFOLLOW)
    out = {}
    for name in buf.raw[:n].split(b'\0'):
        if not name or name.startswith(b'com.apple.'):
            continue
        m = libc.getxattr(p, name, None, 0, 0, NOFOLLOW)
        if m < 0:
            continue
        val = ctypes.create_string_buffer(max(m, 1))
        m = libc.getxattr(p, name, val, m, 0, NOFOLLOW)
        out['SCHILY.xattr.' + name.decode()] = val.raw[:m].decode('utf-8', 'surrogateescape')
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    root, out = os.path.abspath(sys.argv[1]), sys.argv[2]
    count = 0
    with tarfile.open(out, 'w', format=tarfile.PAX_FORMAT) as tar:
        for dirpath, dirs, files in os.walk(root):
            dirs.sort()
            for name in [''] + sorted(files) + [d for d in dirs if os.path.islink(os.path.join(dirpath, d))]:
                path = os.path.join(dirpath, name) if name else dirpath
                rel = os.path.relpath(path, root)
                if any(rel.startswith(s) for s in SKIP):   # the directories themselves stay
                    continue
                ti = tar.gettarinfo(path, arcname=rel)
                if ti is None or not (ti.isreg() or ti.isdir() or ti.issym() or ti.islnk()):
                    continue                       # sockets, fifos
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = ''
                ti.pax_headers = xattrs(path)
                if ti.isreg():
                    with open(path, 'rb') as f:
                        tar.addfile(ti, f)
                else:
                    tar.addfile(ti)
                count += 1
    print(f'{count} entries -> {out}')


if __name__ == '__main__':
    main()
