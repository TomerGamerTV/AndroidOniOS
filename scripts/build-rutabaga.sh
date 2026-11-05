#!/bin/bash
# Builds rutabaga_gfx_ffi (static) with the gfxstream backend for <ios|macos>.
. "$(dirname "$0")/env.sh" "$1"
D="$SRC/rutabaga_gfx"
[ -d "$D" ] || { git clone -q https://github.com/magma-gpu/rutabaga_gfx.git "$D"; (cd "$D" && git checkout -q 9f22cc994019486a03086992609c0aa4769ae4b0); }
for p in "$ROOT"/patches/rutabaga/*.patch; do
    stamp="$D/.applied-$(basename "$p")"
    [ -f "$stamp" ] || { patch -d "$D" -p1 < "$p" && touch "$stamp"; }
done
TOOLCHAIN="${RUST_TOOLCHAIN:-1.92.0}"
rustup toolchain install "$TOOLCHAIN" --profile minimal >/dev/null 2>&1 || true
rustup target add --toolchain "$TOOLCHAIN" "$RUST_TARGET" >/dev/null

export PKG_CONFIG_ALLOW_CROSS=1
export CARGO_TARGET_DIR="$D/target-$PLATFORM"
if [ "$PLATFORM" = ios ]; then
    export IPHONEOS_DEPLOYMENT_TARGET=26.0
    export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER="$CC"
else
    export MACOSX_DEPLOYMENT_TARGET=13.0
fi
export USE_CLANG=1
# Xcode 27 strip leaves mis-aligned LINKEDIT in host proc-macro dylibs; dyld then refuses them.
export CARGO_PROFILE_RELEASE_STRIP=false CARGO_PROFILE_RELEASE_BUILD_OVERRIDE_STRIP=false CARGO_PROFILE_DEV_BUILD_OVERRIDE_STRIP=false
# Don't let our C flags leak into host build scripts.
unset CFLAGS CXXFLAGS LDFLAGS OBJCFLAGS
(cd "$D/ffi" && cargo +"$TOOLCHAIN" rustc --release --lib --crate-type staticlib --target "$RUST_TARGET" --features gfxstream)

install -m 644 "$CARGO_TARGET_DIR/$RUST_TARGET/release/librutabaga_gfx_ffi.a" "$PREFIX/lib/"
mkdir -p "$PREFIX/include/rutabaga_gfx"
cp "$D/ffi/src/include/rutabaga_gfx_ffi.h" "$PREFIX/include/rutabaga_gfx/"
cat > "$PREFIX/lib/pkgconfig/rutabaga_gfx_ffi.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: rutabaga_gfx_ffi
Description: C FFI bindings to Rutabaga VGI
Version: 0.1.85
Requires.private: gfxstream_backend
Libs: -L\${libdir} -lrutabaga_gfx_ffi -lgfxstream_backend -lc++ -framework Foundation -framework IOSurface -framework Metal -framework QuartzCore -framework IOKit$( [ "$PLATFORM" = macos ] && echo " -framework OpenGL -framework AppKit")
Cflags: -I\${includedir}/rutabaga_gfx
EOF
echo "=== [$PLATFORM] rutabaga done"
