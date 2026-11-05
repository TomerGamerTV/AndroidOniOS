# Shared build environment. Source with: . scripts/env.sh <ios|macos>
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
PLATFORM="${1:-ios}"
NCPU="${NCPU:-6}"
BUILD="$ROOT/build"
SRC="$BUILD/src"
TOOLS="$BUILD/tools"
export PATH="$TOOLS/bin:$HOME/Library/Python/3.9/bin:$HOME/.cargo/bin:/usr/local/bin:$PATH"

case "$PLATFORM" in
ios)
    SDK=iphoneos
    MINVER_FLAG="-miphoneos-version-min=26.0"
    MESON_SYSTEM=darwin
    RUST_TARGET=aarch64-apple-ios
    ;;
macos)
    SDK=macosx
    MINVER_FLAG="-mmacosx-version-min=13.0"
    MESON_SYSTEM=darwin
    RUST_TARGET=aarch64-apple-darwin
    ;;
*) echo "unknown platform $PLATFORM"; exit 1 ;;
esac

PREFIX="$BUILD/sysroot-$PLATFORM"
SDKROOT="$(xcrun --sdk $SDK --show-sdk-path)"
export SDKROOT
CC_BIN="$(xcrun --sdk $SDK --find clang)"
CXX_BIN="$(xcrun --sdk $SDK --find clang++)"
export CC="$CC_BIN"
export CXX="$CXX_BIN"
export OBJC="$CC_BIN"
export AR="$(xcrun --sdk $SDK --find ar)"
export RANLIB="$(xcrun --sdk $SDK --find ranlib)"
export NM="$(xcrun --sdk $SDK --find nm)"
export STRIP="$(xcrun --sdk $SDK --find strip)"
ARCH_FLAGS="-arch arm64 -isysroot $SDKROOT $MINVER_FLAG"
export CFLAGS="$ARCH_FLAGS -O2 -I$PREFIX/include"
export CXXFLAGS="$ARCH_FLAGS -O2 -I$PREFIX/include"
export OBJCFLAGS="$ARCH_FLAGS -O2 -I$PREFIX/include"
export LDFLAGS="$ARCH_FLAGS -L$PREFIX/lib"
export PKG_CONFIG="$TOOLS/bin/pkg-config"
export PKG_CONFIG_PATH=""
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
mkdir -p "$PREFIX" "$SRC"

meson_quote() { local out=""; for a in "$@"; do out="$out'$a', "; done; echo "${out%, }"; }

MESON_CROSS="$BUILD/meson-$PLATFORM.cross"
cat > "$MESON_CROSS" <<EOF
[properties]
needs_exe_wrapper = $( [ "$PLATFORM" = ios ] && echo true || echo false )
has_function_pipe2 = false
has_function_dup3 = false
[built-in options]
c_args = [$(meson_quote $CFLAGS)]
cpp_args = [$(meson_quote $CXXFLAGS)]
objc_args = [$(meson_quote $OBJCFLAGS)]
objcpp_args = [$(meson_quote $OBJCFLAGS)]
c_link_args = [$(meson_quote $LDFLAGS)]
cpp_link_args = [$(meson_quote $LDFLAGS)]
objc_link_args = [$(meson_quote $LDFLAGS)]
objcpp_link_args = [$(meson_quote $LDFLAGS)]
[binaries]
c = ['$CC']
cpp = ['$CXX']
objc = ['$CC']
objcpp = ['$CXX']
ar = ['$AR']
nm = ['$NM']
ranlib = ['$RANLIB']
strip = ['$STRIP', '-x']
pkg-config = ['$PKG_CONFIG']
python = ['/usr/bin/python3']
[host_machine]
system = '$( [ "$PLATFORM" = ios ] && echo ios || echo darwin )'
subsystem = '$( [ "$PLATFORM" = ios ] && echo ios || echo macos )'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

fetch() { # url -> extracts into $SRC, echoes dir
    local url="$1" file dir
    file="$SRC/$(basename "$url")"
    [ -f "$file" ] || curl -sfL -o "$file" "$url"
    dir="$SRC/$(basename "$url" | sed -E 's/\.tar\.(gz|xz|bz2)$//')"
    [ -d "$dir" ] || tar -xf "$file" -C "$SRC"
    echo "$dir"
}

meson_dep() { # srcdir [args...]
    local dir="$1"; shift
    local bdir="$dir/build-$PLATFORM"
    rm -rf "$bdir"
    meson setup "$bdir" "$dir" --prefix="$PREFIX" --libdir=lib --buildtype=release \
        --default-library=static --cross-file "$MESON_CROSS" "$@"
    meson compile -C "$bdir" -j "$NCPU"
    meson install -C "$bdir" --quiet
}
