#!/bin/bash
# Builds QEMU (utm 10.0.12 fork + our patches).
# Usage: scripts/build-qemu.sh <macos|ios-jit|ios-tcti>
VARIANT="$1"
case "$VARIANT" in
macos) PLAT=macos ;;
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
ios-jit)
    EXTRA=(--cross-prefix="" --enable-shared-lib --disable-hvf --disable-cocoa --disable-coreaudio
        --disable-slirp-smbd --enable-ucontext --with-coroutine=libucontext --disable-vnc
        --extra-ldflags=-Wl,-no_deduplicate)
    ;;
ios-tcti)
    EXTRA=(--cross-prefix="" --enable-shared-lib --disable-hvf --disable-cocoa --disable-coreaudio
        --disable-slirp-smbd --enable-ucontext --with-coroutine=libucontext --disable-vnc
        --enable-tcg-threaded-interpreter --extra-cflags=-Wno-unused-command-line-argument
        --extra-ldflags=-Wl,-no_deduplicate --extra-ldflags=-Wl,-random_uuid --extra-ldflags=-Wl,-no_compact_unwind)
    ;;
esac

if [ -z "$REBUILD" ] || [ ! -f "$BDIR/build.ninja" ]; then
    rm -rf "$BDIR"; mkdir -p "$BDIR"
    (cd "$BDIR" && ../configure "${COMMON[@]}" "${EXTRA[@]}" \
        --extra-cflags="$CFLAGS" --extra-cxxflags="$CXXFLAGS" --extra-ldflags="$LDFLAGS" \
        --cc="$CC" --cxx="$CXX" --objcc="$CC" )
fi
(cd "$BDIR" && ninja -j "$NCPU")
echo "=== qemu $VARIANT built in $BDIR"
