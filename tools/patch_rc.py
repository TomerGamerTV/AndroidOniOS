#!/usr/bin/env python3
"""Rewrite a small file's single data block inside an ext4 image embedded in a super partition.

The replacement is padded with a trailing comment to the original size so the inode size
stays valid (the filesystem uses shared_blocks and cannot be opened read-write by e2fsprogs).

usage: patch_rc.py <disk.img> <byte offset of ext4 fs> <block number> <orig size> <new content file>
"""
import sys

disk, fs_off, block, orig_size, content_path = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
data = open(content_path, 'rb').read()
if len(data) > orig_size:
    raise SystemExit(f'new content is {len(data)} bytes, must be <= {orig_size}')
pad = orig_size - len(data)
if pad:
    filler = b'#' + b' ' * (pad - 2) + b'\n' if pad >= 2 else b'\n'
    data += filler
assert len(data) == orig_size
with open(disk, 'r+b') as f:
    f.seek(fs_off + block * 4096)
    f.write(data)
print(f'patched {orig_size} bytes at block {block}')
