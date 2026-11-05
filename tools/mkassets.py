#!/usr/bin/env python3
"""Build-time assets for the iOS app.

1. userdata.tpl  - sparse ext4 template: raw-deflate( "DRUD" u64 size, {u64 off, u32 len, data}* )
2. android.json - image source + byte patches applied to system.img after download

usage: mkassets.py <out dir> <ext4 image (mke2fs output)> <rc file> <sys-img xml>
"""
import hashlib
import json
import os
import re
import struct
import sys
import zlib

out, ext4, rc_path, xml_path = sys.argv[1:5]
os.makedirs(out, exist_ok=True)

# --- userdata template ---
size = os.path.getsize(ext4)
chunks = []
with open(ext4, 'rb') as f:
    off = 0
    blk = 4096
    run_start, run = None, bytearray()
    while off < size:
        b = f.read(blk)
        if not b:
            break
        if b.strip(b'\0'):
            if run_start is None:
                run_start = off
            run += b
        elif run_start is not None:
            chunks.append((run_start, bytes(run)))
            run_start, run = None, bytearray()
        off += len(b)
    if run_start is not None:
        chunks.append((run_start, bytes(run)))
blob = bytearray(b'DRUD' + struct.pack('<Q', size))
for o, d in chunks:
    blob += struct.pack('<QI', o, len(d)) + d
c = zlib.compressobj(9, zlib.DEFLATED, -15)
packed = c.compress(bytes(blob)) + c.flush()
open(os.path.join(out, 'userdata.tpl'), 'wb').write(packed)
print(f'userdata.tpl: {len(chunks)} chunks, {len(blob)} raw -> {len(packed)} packed, fs size {size}')

# --- image manifest ---
xml = open(xml_path).read()
m = re.search(r'<remotePackage path="system-images;android-37.2;google_apis_playstore_ps16k;arm64-v8a">(.*?)</remotePackage>', xml, re.S)
body = m.group(1)
url = re.findall(r'<url>(.*?)</url>', body)[-1]
sz = int(re.findall(r'<size>(.*?)</size>', body)[-1])
sha1 = re.findall(r'<checksum[^>]*>(.*?)</checksum>', body)[-1]

rc = open(rc_path, 'rb').read()
RC_SIZE = 384
pad = RC_SIZE - len(rc)
rc_padded = rc + (b'#' + b' ' * (pad - 2) + b'\n' if pad >= 2 else b'\n' * pad)
assert len(rc_padded) == RC_SIZE
SUPER = 4096 * 512
SYSTEM_EXT_START = 2695168 * 512
manifest = {
    'name': 'Android 17 (API 37.2) Google Play',
    'url': 'https://dl.google.com/android/repository/sys-img/google_apis_playstore/' + url,
    'size': sz,
    'sha1': sha1,
    'files': ['kernel-ranchu', 'ramdisk.img', 'system.img', 'vendor.img', 'encryptionkey.img',
              'VerifiedBootParams.textproto', 'build.prop'],
    'patches': [{
        'file': 'system.img',
        'offset': SUPER + SYSTEM_EXT_START + 2070 * 4096,
        'expect_sha256': None,
        'data_b64': __import__('base64').b64encode(rc_padded).decode(),
        'why': 'system_ext init.system_ext.rc: write adb key and start adbd before boot completes',
    }],
}
open(os.path.join(out, 'android.json'), 'w').write(json.dumps(manifest, indent=2))
print('android.json written:', manifest['url'], sz, sha1)
