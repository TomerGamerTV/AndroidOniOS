#!/usr/bin/env python3
"""Builds a static /sys/fs/selinux tree for onirun's rootfs from a class dump taken on a device
(userland/data/selinux-classes.txt). SELinux is reported as present and permissive.

  mkselinuxfs.py DUMP ROOTFS
"""
import os, struct, sys

dump, root = sys.argv[1], sys.argv[2]
base = os.path.join(root, 'sys/fs/selinux')


def put(rel, data):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w' if isinstance(data, str) else 'wb') as f:
        f.write(data)


for line in open(dump):
    parts = line.rstrip('\n').split(' ')
    if parts[0] == 'C' and len(parts) >= 3:
        put(f'class/{parts[1]}/index', parts[2])
    elif parts[0] == 'P' and len(parts) >= 4:
        put(f'class/{parts[1]}/perms/{parts[2]}', parts[3])
    elif parts[0] == 'I' and len(parts) >= 3:
        put(f'initial_contexts/{parts[1]}', ' '.join(parts[2:]))
    elif parts[0] == 'K' and len(parts) >= 3:
        put(f'policy_capabilities/{parts[1]}', parts[2])
    elif parts[0] == 'V' and len(parts) >= 3:
        put(parts[1], parts[2])
put('enforce', '0')
put('checkreqprot', '0')
# struct selinux_status_t: version, sequence (even = stable), enforcing, policyload, deny_unknown
status = struct.pack('<IIIII', 1, 2, 0, 1, 1)
put('status', status + b'\0' * (16384 - len(status)))
print('wrote', base)
