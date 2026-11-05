#!/bin/bash
# Builds the gfxstream host renderer (static) for <ios|macos>.
. "$(dirname "$0")/env.sh" "$1"
D="$SRC/gfxstream"
[ -d "$D" ] || git clone -q https://github.com/google/gfxstream.git "$D"
for p in "$ROOT"/patches/gfxstream/*.patch; do
    [ -f "$p" ] || continue
    stamp="$D/.applied-$(basename "$p")"
    [ -f "$stamp" ] || { echo "applying $(basename "$p")"; patch -d "$D" -p1 < "$p" && touch "$stamp"; }
done
# QEMU_NEXT: compose/post on gfxstream's own post thread instead of an embedder UI thread.
write_cross "-DQEMU_NEXT=1" gfxstream
# gfxstream keys all Apple code paths off system == 'darwin'.
sed -i '' "s/^system = .*/system = 'darwin'/" "$MESON_CROSS"
BDIR="$D/build-$PLATFORM"
if [ -z "$REBUILD" ] || [ ! -f "$BDIR/build.ninja" ]; then
    rm -rf "$BDIR"
    meson setup "$BDIR" "$D" --prefix="$PREFIX" --libdir=lib --buildtype=release \
        --default-library=static --cross-file "$MESON_CROSS" \
        -Ddecoders=$( [ "$PLATFORM" = ios ] && echo gles,vulkan,composer || echo gles,vulkan,composer )
fi
meson compile -C "$BDIR" -j "$NCPU"
meson install -C "$BDIR" --quiet
echo "=== [$PLATFORM] gfxstream done"
