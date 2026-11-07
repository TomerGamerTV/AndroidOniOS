#!/bin/bash
# Boots Android's services under onirun with the host GPU stack (ANGLE + MoltenVK, as the QEMU
# app uses). Usage: userland/run-init.sh [extra onirun args]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$ROOT/build/utm/mac-fw"
LIB="$ROOT/build/vm-macos/lib"
mkdir -p "$LIB"
ln -sf "$FW/EGL.framework/EGL" "$LIB/libEGL.dylib"
ln -sf "$FW/GLESv2.framework/GLESv2" "$LIB/libGLESv2.dylib"
export ANDROID_EMU_VK_ICD=moltenvk ANDROID_EMU_VK_LOADER_PATH="$FW/MoltenVK.framework/MoltenVK"
export DYLD_LIBRARY_PATH="$LIB" DYLD_FRAMEWORK_PATH="$FW" ANGLE_DEFAULT_PLATFORM=metal
export RUTABAGA_DISPLAY_WIDTH="${WIDTH:-1080}" RUTABAGA_DISPLAY_HEIGHT="${HEIGHT:-2400}"
set -a; . "$ROOT/userland/data/android-env.sh"; set +a
RFS="$ROOT/build/userland/rootfs"
# The image ships no boot image; odrefresh compiles it (and system_server's jars) once into /data.
# Without it every boot builds an uncompiled one in memory and the framework runs ~4x slower.
[ -e "$RFS/data/misc/apexdata/com.android.art/dalvik-cache/arm64/boot.art" ] ||
    "$ROOT/build/userland/onirun" --root "$RFS" /apex/com.android.art/bin/odrefresh --compile
exec "$ROOT/build/userland/onirun" --root "$RFS" --init "$@"
