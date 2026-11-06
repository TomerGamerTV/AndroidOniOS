#!/usr/bin/env python3
"""Builds onirun's root filesystem from an Android emulator system image.

  mkrootfs.py SYSTEM_IMG ROOTFS [--props GETPROP_DUMP] [--linkerconfig DIR] [--properties-dir DIR]

SYSTEM_IMG is the emulator's system.img (a GPT disk with a 'super' partition). The script copies the
system, system_ext, product and vendor partitions, extracts every APEX, lays out the directories
init and vold would create, generates the static SELinux tree, device-node placeholders, the APEX
info list and the linker-config tweaks onirun needs, and installs userland/data/{init.txt,
android-properties.txt, android-env.sh}.

Two things only a booted Android can provide and must be copied from a device (adb):
  --properties-dir   /dev/__properties__ (the property areas and property_info)
  --linkerconfig     /linkerconfig (ld.config.txt and the per-APEX configs)
Without them onirun still starts, but Android's libc finds no properties.

Everything written is derived from the image; nothing here is executed.
"""
import argparse, os, shutil, struct, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
EXT4PULL = os.path.join(HERE, 'ext4pull.py')


def run(*args):
    r = subprocess.run([sys.executable, '-I', *args], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'{" ".join(args)}: {r.stderr.strip()[-400:]}')
    return r.stdout


def pb_name_version(data):
    i, name, ver = 0, None, 0
    while i < len(data):
        tag = data[i]; i += 1
        if tag == 0x0a:
            n = shift = 0
            while True:
                b = data[i]; i += 1; n |= (b & 0x7f) << shift; shift += 7
                if not b & 0x80: break
            name = data[i:i + n].decode(); i += n
        elif tag == 0x10:
            v = shift = 0
            while True:
                b = data[i]; i += 1; v |= (b & 0x7f) << shift; shift += 7
                if not b & 0x80: break
            ver = v
        else:
            break
    return name, ver


def symlink(target, path):
    if os.path.lexists(path):
        if os.path.islink(path):
            return
        shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    os.symlink(target, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image'); ap.add_argument('rootfs')
    ap.add_argument('--properties-dir'); ap.add_argument('--linkerconfig')
    a = ap.parse_args()
    R = os.path.abspath(a.rootfs)
    os.makedirs(R, exist_ok=True)

    # 1. Partitions, laid out like Android: /system, /system_ext, /product, /vendor, /odm
    for part, dest in (('system', 'system'), ('system_ext', 'system_ext'), ('product', 'product'), ('vendor', 'vendor')):
        if not os.path.isdir(os.path.join(R, dest)):
            print('extracting', part)
            src = '/system' if part == 'system' else '/'
            run(EXT4PULL, a.image, '-p', part, 'pull', src, os.path.join(R, dest))
    os.makedirs(os.path.join(R, 'odm'), exist_ok=True)
    symlink('/system_ext', os.path.join(R, 'system/system_ext'))
    symlink('/product', os.path.join(R, 'system/product'))
    for name, target in (('etc', '/system/etc'), ('bin', '/system/bin')):
        symlink(target, os.path.join(R, name))

    # 2. APEXes: /system/apex/*.apex and /vendor/apex/*.apex, each payload at /apex/<name>
    entries = []
    for src, part in (('system/apex', 'SYSTEM'), ('vendor/apex', 'VENDOR'), ('system_ext/apex', 'SYSTEM_EXT'), ('product/apex', 'PRODUCT')):
        d = os.path.join(R, src)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith('.apex'):
                continue
            path = os.path.join(d, f)
            name, ver = pb_name_version(zipfile.ZipFile(path).read('apex_manifest.pb'))
            dest = os.path.join(R, 'apex', name)
            if not os.path.isdir(dest):
                print('extracting apex', name)
                run(EXT4PULL, path, 'pull', '/', dest)
            entries.append(f'  <apex-info moduleName="{name}" modulePath="/{src}/{f}" preinstalledModulePath="/{src}/{f}" '
                           f'versionCode="{ver}" versionName="" isFactory="true" isActive="true" provideSharedApexLibs="false" partition="{part}"/>')
    with open(os.path.join(R, 'apex/apex-info-list.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<apex-info-list>\n' + '\n'.join(entries) + '\n</apex-info-list>\n')

    # 2b. Pre-compiled boot image and odex files: built for a collector that needs userfaultfd, so
    # ART can't use them; left in place they only make its stack dumper abort.
    for rel in ('system/framework/arm64', 'system/framework/oat'):
        shutil.rmtree(os.path.join(R, rel), ignore_errors=True)
    for f in os.listdir(os.path.join(R, 'system/framework')):
        if f.startswith('boot') and f.endswith('.vdex'):
            os.remove(os.path.join(R, 'system/framework', f))

    # 3. Directories init, vold and the framework expect
    dirs = '''mnt/user/0 mnt/expand mnt/runtime/default/emulated mnt/runtime/read/emulated mnt/runtime/write/emulated
    mnt/runtime/full/emulated mnt/sdcard mnt/media_rw mnt/pass_through/0/emulated mnt/installer/0/emulated
    mnt/androidwritable/0/emulated storage/emulated/0 storage/self cache config metadata data/system data/misc
    data/misc_ce/0 data/misc_de/0 data/system_ce/0 data/system_de/0 data/user/0 data/user_de/0 data/app
    data/app-private data/app-ephemeral data/app-lib data/app-staging data/apex data/dalvik-cache/arm64 data/data
    data/media/0 data/local/tmp data/vendor data/vendor_ce/0 data/vendor_de/0 data/property data/anr data/tombstones
    data/backup data/cache data/drm data/mediadrm data/nfc data/resource-cache data/ss data/misc/wifi
    data/misc/bluetooth data/misc/keystore data/misc/profiles/cur/0 data/misc/profiles/ref data/misc/user/0
    data/misc/zoneinfo data/misc/apexdata data/misc/perfetto-traces data/misc/stats-data data/misc/stats-service
    data/misc/textclassifier data/misc/vold data/misc/adb data/misc/audio data/misc/systemkeys data/misc/installd
    data/misc/gatekeeper data/misc/recovery data/misc/trace data/misc/update_engine data/misc/shared_relro
    data/misc/sms data/misc/carrierid data/misc/radio data/misc/net data/misc/ethernet data/misc/dhcp
    data/misc/display data/misc/incidents data/misc/logd data/misc/odrefresh data/misc/wmtrace data/misc/a11ytrace
    data/misc/credstore data/misc/appcompat data/misc/apns data/misc/emergencynumberdb data/misc/sdksandbox
    data/misc/threadnetwork data/misc/uprobestats data/misc/aconfig_flags data/misc/connectivityblobdb
    data/misc/odsign data/misc/virtualizationservice data/misc/oem_lock data/misc/bootstat data/misc/keychain
    data/misc/ota data/misc/adbd data/misc/system_server data/misc/securitylog data/misc/surfaceflinger data/misc/gpu dev/dri dev/socket dev/__properties__
    dev/binderfs oni sys/fs/cgroup/cgroup.controllers.d'''.split()
    for d in dirs:
        os.makedirs(os.path.join(R, d), exist_ok=True)
    # cgroup trees (controllers from /system/etc/cgroups.json; the groups from task_profiles.json)
    groups = ['', 'application', 'application/background', 'application/performance', 'background', 'camera-daemon',
              'dex2oat', 'foreground', 'foreground_window', 'nnapi-hal', 'restricted', 'rt', 'system', 'system-background',
              'system/background', 'system/performance', 'top-app', 'apps', 'uid_0', 'uid_1000']
    for root in ('sys/fs/cgroup', 'dev/cpuctl', 'dev/cpuset', 'dev/blkio', 'dev/memcg', 'dev/stune', 'acct'):
        for g in groups:
            os.makedirs(os.path.join(R, root, g), exist_ok=True)
    with open(os.path.join(R, 'sys/fs/cgroup/cgroup.controllers'), 'w') as f:
        f.write('cpu memory freezer pids')
    for d in os.listdir(os.path.join(R, 'dev/cpuset')):
        p = os.path.join(R, 'dev/cpuset', d)
        if os.path.isdir(p):
            open(os.path.join(p, 'cpuset.cpus'), 'w').write('0-7'); open(os.path.join(p, 'cpuset.mems'), 'w').write('0')

    # 4. Device-node placeholders (onirun intercepts opens; stat() sees character devices)
    boot_id = '0f3a6d9e-7c1b-4b2a-9d6e-0a1b2c3d4e5f'   # must match procfs.c
    for n in ('binder', 'hwbinder', 'vndbinder', 'ashmem', 'ashmem' + boot_id, 'kmsg', 'null', 'zero', 'urandom', 'random',
              'binderfs/binder', 'binderfs/hwbinder', 'binderfs/vndbinder'):
        open(os.path.join(R, 'dev', n), 'a').close()

    # 5. SELinux tree, properties defaults, service table, environment
    run(os.path.join(HERE, 'mkselinuxfs.py'), os.path.join(REPO, 'userland/data/selinux-classes.txt'), R)
    shutil.copy(os.path.join(REPO, 'userland/data/android-properties.txt'), os.path.join(R, 'oni/properties.txt'))
    shutil.copy(os.path.join(REPO, 'userland/data/init.txt'), os.path.join(R, 'oni/init.txt'))
    shutil.copy(os.path.join(REPO, 'userland/data/android-env.sh'), os.path.join(R, 'data/local/tmp/android-env.sh'))

    # 6. From a booted device: property areas and linker configs
    if a.properties_dir:
        dest = os.path.join(R, 'dev/__properties__')
        for f in os.listdir(a.properties_dir):
            src = os.path.join(a.properties_dir, f)
            if f == 'property_info' or os.path.getsize(src) == 131072:    # skip anything that isn't a real area
                shutil.copy(src, os.path.join(dest, f)); os.chmod(os.path.join(dest, f), 0o644)
    if a.linkerconfig:
        shutil.copytree(a.linkerconfig, os.path.join(R, 'linkerconfig'), dirs_exist_ok=True)
    ldc = os.path.join(R, 'linkerconfig/ld.config.txt')
    if os.path.exists(ldc):
        s = open(ldc).read()
        if 'onirun' not in s:
            # vendor APEX HALs (com.android.hardware.*) have no per-APEX config we can read; let the
            # vendor section serve them and find their own libraries
            import re
            apexes = [n for n in sorted(os.listdir(os.path.join(R, 'apex')))
                      if (n.startswith('com.android.hardware.') or n == 'com.google.android.widevine') and os.path.isdir(os.path.join(R, 'apex', n, 'bin'))]
            s = s.replace('dir.vendor = /odm/bin/\n', '# onirun: vendor APEX HALs resolve like vendor binaries\n' +
                          ''.join(f'dir.vendor = /apex/{n}/bin\n' for n in apexes) + 'dir.vendor = /odm/bin/\n', 1)
            i = s.index('[vendor]'); j = s.index('\n[', i + 1); sec = s[i:j]
            m = re.search(r'namespace\.default\.search\.paths = .*\n', sec)
            add = ''.join(f'namespace.default.search.paths += /apex/{n}/${{LIB}}\nnamespace.default.permitted.paths += /apex/{n}/${{LIB}}\n' for n in apexes)
            s = s[:i] + sec[:m.end()] + add + sec[m.end():] + s[j:]
            open(ldc, 'w').write(s)
    print('rootfs ready at', R)


if __name__ == '__main__':
    main()
