#!/bin/bash
# Builds QEMU (utm 10.0.12 fork + our patches).
# Usage: scripts/build-qemu.sh <macos|ios-jit|ios-tcti>
VARIANT="$1"
case "$VARIANT" in
macos|macos-tcti) PLAT=macos ;;
ios-jit|ios-tcti) PLAT=ios ;;
*) echo "usage: $0 <macos|ios-jit|ios-tcti>"; exit 1 ;;
esac
. "$(dirname "$0")/env.sh" "$PLAT"

QSRC="$SRC/qemu-10.0.12-utm"
[ -d "$QSRC" ] || { curl -sfL -o "$SRC/qemu-10.0.12-utm.tar.xz" https://github.com/utmapp/qemu/releases/download/v10.0.12-utm/qemu-10.0.12-utm.tar.xz; tar -xf "$SRC/qemu-10.0.12-utm.tar.xz" -C "$SRC"; }

# Apply our patches once (tracked by stamp files).
for p in "$ROOT"/patches/qemu/*.patch; do
    [ -f "$p" ] || continue
    stamp="$QSRC/.applied-$(basename "$p")"
    if [ ! -f "$stamp" ]; then
        echo "applying $(basename "$p")"
        patch -d "$QSRC" -p1 < "$p" && touch "$stamp"
    fi
done
# Copy in our new source files (overwrite each build).
if [ -d "$ROOT/qemu-src" ]; then
    rsync -a "$ROOT/qemu-src/" "$QSRC/"
fi

BDIR="$QSRC/build-$VARIANT"
# iOS: SDKROOT points at the iPhone SDK, which breaks host tools built during configure.
UNSET_SDK=""; [ "$PLAT" = ios ] && UNSET_SDK="-u SDKROOT"
# TCTI's gadget generator needs Python >= 3.10.
PY="${PYTHON:-$HOME/miniconda3/bin/python3.13}"; [ -x "$PY" ] || PY=python3
# Some generators run via '#!/usr/bin/env python3'; make that resolve to $PY.
[ -x "$TOOLS/py3/python3" ] && export PATH="$TOOLS/py3:$PATH"
COMMON=(--prefix="$PREFIX/qemu-$VARIANT" --target-list=aarch64-softmmu --disable-docs --disable-werror
    --enable-slirp --enable-rutabaga-gfx --enable-pixman --disable-gnutls --disable-nettle --disable-gcrypt
    --disable-curl --disable-libssh --disable-vnc-sasl --disable-vnc-jpeg --disable-png --disable-spice
    --disable-opengl --disable-virglrenderer --disable-sdl --disable-gtk --disable-libusb --disable-usb-redir
    --disable-smartcard --disable-capstone --disable-tools --disable-guest-agent --disable-plugins
    --disable-debug-info --enable-zstd --disable-lzo --disable-snappy --disable-bzip2 --disable-dmg
    --disable-vdi --disable-vhdx --disable-vmdk --disable-vpc --disable-vvfat --disable-qed --disable-parallels
    --disable-cloop --disable-bochs --disable-libvduse --disable-vduse-blk-export --disable-keyring
    --disable-tpm --disable-libdw --disable-linux-io-uring --enable-virtfs --disable-curses --disable-brlapi)
case "$VARIANT" in
macos)
    EXTRA=(--enable-hvf --enable-cocoa --enable-coreaudio --enable-vnc)
    LDFLAGS="$LDFLAGS -framework ParavirtualizedGraphics"
    ;;
macos-tcti)
    EXTRA=(--disable-hvf --disable-cocoa --enable-coreaudio --disable-vnc --enable-tcg-threaded-interpreter
        --extra-cflags=-Wno-unused-command-line-argument)
    LDFLAGS="$LDFLAGS -framework ParavirtualizedGraphics"
    ;;
ios-jit)
    EXTRA=(--cross-prefix="" --enable-shared-lib --disable-hvf --disable-cocoa --enable-coreaudio
        --disable-slirp-smbd --enable-ucontext --with-coroutine=libucontext --disable-vnc
        --extra-ldflags=-Wl,-no_deduplicate)
    ;;
ios-tcti)
    EXTRA=(--cross-prefix="" --enable-shared-lib --disable-hvf --disable-cocoa --enable-coreaudio
        --disable-slirp-smbd --enable-ucontext --with-coroutine=libucontext --disable-vnc
        --enable-tcg-threaded-interpreter --extra-cflags=-Wno-unused-command-line-argument
        --extra-ldflags=-Wl,-no_deduplicate --extra-ldflags=-Wl,-random_uuid --extra-ldflags=-Wl,-no_compact_unwind)
    ;;
esac

if [ -z "$REBUILD" ] || [ ! -f "$BDIR/build.ninja" ]; then
    rm -rf "$BDIR"; mkdir -p "$BDIR"
    # Meson applies CFLAGS/LDFLAGS from the environment to the *build* machine compiler
    # when cross compiling; pass target flags only through --extra-* options.
    # iOS binaries must never be executed during configure.
    # (iOS build dirs keep referencing this file on regeneration; macOS configures hide it temporarily.)
    printf '[properties]\nneeds_exe_wrapper = true\n' > "$QSRC/configs/meson/darwin.txt"
    [ "$PLAT" = macos ] && mv "$QSRC/configs/meson/darwin.txt" "$QSRC/configs/meson/darwin.txt.ios"
    (cd "$BDIR" && env -u CFLAGS -u CXXFLAGS -u OBJCFLAGS -u LDFLAGS $UNSET_SDK ../configure "${COMMON[@]}" "${EXTRA[@]}" \
        --extra-cflags="$CFLAGS" --extra-cxxflags="$CXXFLAGS" --extra-ldflags="$LDFLAGS" \
        --cc="$CC" --cxx="$CXX" --objcc="$CC" --python="$PY" )
    [ -f "$QSRC/configs/meson/darwin.txt.ios" ] && mv "$QSRC/configs/meson/darwin.txt.ios" "$QSRC/configs/meson/darwin.txt"
fi
(cd "$BDIR" && env $UNSET_SDK ninja -j "$NCPU")
echo "=== qemu $VARIANT built in $BDIR"
