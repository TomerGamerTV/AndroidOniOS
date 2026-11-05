#!/bin/bash
# Dev harness: boots the Android image with our macOS QEMU build (HVF) to validate the VM config.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${IMG:-$ROOT/build/images/a17/arm64-v8a}"
VM="$ROOT/build/vm-macos"
Q="${QBIN:-$ROOT/build/src/qemu-10.0.12-utm/build-macos/qemu-system-aarch64-unsigned}"
mkdir -p "$VM"
[ -f "$VM/metadata.img" ] || cp "$IMG/encryptionkey.img" "$VM/metadata.img"
[ -f "$VM/userdata.img" ] || truncate -s 8G "$VM/userdata.img"
[ -f "$VM/cache.img" ] || truncate -s 64M "$VM/cache.img"
ADBKEY_PUB="$(cat "$HOME/.android/adbkey.pub" 2>/dev/null || true)"
cp "$ROOT/config/bootconfig.txt" "$VM/bootconfig.txt"
[ "${GLES:-off}" = off ] && echo "androidboot.hardwareegl=angle" >> "$VM/bootconfig.txt"
[ -n "$ADBKEY_PUB" ] && echo "androidboot.qemu.adb.pubkey=$ADBKEY_PUB" >> "$VM/bootconfig.txt"
python3 "$ROOT/tools/mkinitrd.py" "$IMG/ramdisk.img" "$VM/initrd" --file "$VM/bootconfig.txt"

ENT="$VM/ent.plist"
cat > "$ENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
EOF
cp "$Q" "$VM/qemu"
codesign -f -s - --entitlements "$ENT" "$VM/qemu" 2>/dev/null

FW="$ROOT/build/utm/mac-fw"
mkdir -p "$VM/lib"; ln -sf "$FW/EGL.framework/EGL" "$VM/lib/libEGL.dylib"; ln -sf "$FW/GLESv2.framework/GLESv2" "$VM/lib/libGLESv2.dylib"
export ANDROID_EMU_VK_ICD=moltenvk ANDROID_EMU_VK_LOADER_PATH="$FW/MoltenVK.framework/MoltenVK"
export DYLD_LIBRARY_PATH="$VM/lib" DYLD_FRAMEWORK_PATH="$FW" ANGLE_DEFAULT_PLATFORM=metal
export RUTABAGA_DISPLAY_WIDTH="${WIDTH:-1080}" RUTABAGA_DISPLAY_HEIGHT="${HEIGHT:-2400}"
pkill -f "droidsvc.py $VM/svc.sock" 2>/dev/null || true
python3 "$ROOT/tools/droidsvc.py" "$VM/svc.sock" > "$VM/droidsvc.log" 2>&1 &
pkill -f "modemsim $VM/modem.sock" 2>/dev/null || true
rm -f "$VM/modem.log"; "$ROOT/build/tools/bin/modemsim" "$VM/modem.sock" "$VM/modem.log" &
sleep 0.5
ACCEL="${ACCEL:-hvf}"
CPU="host"; [ "$ACCEL" != hvf ] && CPU="${TCGCPU:-max,sve=off,pauth-impdef=on}"
exec "$VM/qemu" \
  -machine virt,highmem=on -accel "$ACCEL" -cpu "$CPU" -smp "${SMP:-4}" -m "${MEM:-3072}" \
  -nodefaults -no-user-config \
  -kernel "$IMG/kernel-ranchu" -initrd "$VM/initrd" \
  -append "8250.nr_uarts=1 no_timer_check console=ttyAMA0,38400 keep_bootcon earlyprintk=ttyAMA0 ndns=2 loop.max_part=7 printk.devkmsg=on bootconfig" \
  -serial "file:$VM/kernel.log" \
  -drive if=none,id=vendor,file="$IMG/vendor.img",format=raw,read-only=on -device virtio-blk-device,drive=vendor \
  -drive if=none,id=metadata,file="$VM/metadata.img",format=raw,cache=unsafe -device virtio-blk-device,drive=metadata \
  -drive if=none,id=userdata,file="$VM/userdata.img",format=raw,cache=unsafe,discard=unmap -device virtio-blk-device,drive=userdata \
  -drive if=none,id=cache,file="$VM/cache.img",format=raw,cache=unsafe -device virtio-blk-device,drive=cache \
  -drive if=none,id=system,file="$VM/system.img",format=raw,read-only=on -device virtio-blk-device,drive=system \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
  -device virtio-rng-device \
  -chardev null,id=hvc0 -chardev "file,id=hvc1,path=$VM/logcat.log" \
  -chardev "socket,id=modem,path=$VM/modem.sock,reconnect-ms=1000" \
  -device virtio-serial-pci,ioeventfd=off -device virtconsole,chardev=hvc0 -device virtconsole,chardev=hvc1 \
  -device virtserialport,chardev=modem,name=modem \
  -device virtio-gpu-rutabaga-pci,gfxstream-vulkan=on,x-gfxstream-gles=${GLES:-off},x-gfxstream-composer=on,cross-domain=off,hostmem=1G,xres=${WIDTH:-1080},yres=${HEIGHT:-2400} \
  -device virtio-multitouch-pci -device virtio-keyboard-pci \
  -device "virtio-vsock-host-pci,guest-cid=77,forward=tcp:6520:5555,guest-forward=5000:unix:$VM/svc.sock" \
  -monitor "unix:$VM/monitor.sock,server,nowait" \
  -qmp "unix:$VM/qmp.sock,server,nowait" \
  ${DISPLAY_OPTS:--display cocoa} "$@"
