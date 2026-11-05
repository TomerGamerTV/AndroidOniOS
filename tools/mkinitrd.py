#!/usr/bin/env python3
"""Append an Android bootconfig block to a ramdisk.

usage: mkinitrd.py <ramdisk.img> <out initrd> key=value [key=value ...]
       mkinitrd.py <ramdisk.img> <out initrd> --file props.txt

Format (Linux Documentation/admin-guide/bootconfig.rst):
  [ramdisk][bootconfig text, NUL padded to 4][u32 size][u32 checksum]["#BOOTCONFIG\n"]
"""
import struct
import sys


def build(props):
    text = ''.join(f'{k}="{v}"\n' for k, v in props)
    data = text.encode() + b'\0'
    data += b'\0' * ((-len(data)) % 4)
    csum = sum(data) & 0xFFFFFFFF
    return data + struct.pack('<II', len(data), csum) + b'#BOOTCONFIG\n'


def main():
    ramdisk, out = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    props = []
    if args and args[0] == '--file':
        lines = open(args[1]).read().splitlines()
    else:
        lines = args
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        k, v = line.split('=', 1)
        props.append((k.strip(), v.strip().strip('"')))
    with open(ramdisk, 'rb') as f:
        rd = f.read()
    with open(out, 'wb') as f:
        f.write(rd)
        f.write(build(props))


if __name__ == '__main__':
    main()
