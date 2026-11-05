#!/bin/bash
# Builds QEMU's host-side dependencies as static libraries into build/sysroot-<platform>.
# Usage: scripts/build-deps.sh <ios|macos> [dep...]
. "$(dirname "$0")/env.sh" "$1"
shift || true
DEPS="${*:-ffi glib pixman zstd slirp ucontext epoxy}"

build_ffi() {
    local d; d=$(fetch https://github.com/libffi/libffi/releases/download/v3.5.0/libffi-3.5.0.tar.gz)
    (cd "$d" && rm -rf build-$PLATFORM && mkdir build-$PLATFORM && cd build-$PLATFORM &&
        ../configure --host=aarch64-apple-darwin --prefix="$PREFIX" --libdir="$PREFIX/lib" \
            --disable-shared --enable-static --disable-docs --disable-multi-os-directory >/dev/null &&
        make -j"$NCPU" >/dev/null && make install >/dev/null)
}

build_glib() {
    local d; d=$(fetch https://download.gnome.org/sources/glib/2.83/glib-2.83.0.tar.xz)
    meson_dep "$d" -Dtests=false -Dintrospection=disabled -Dnls=disabled -Dlibmount=disabled \
        -Dselinux=disabled -Dxattr=false -Dman-pages=disabled -Ddtrace=disabled -Dsystemtap=disabled \
        -Dsysprof=disabled -Dglib_debug=disabled -Dlibelf=disabled -Dbsymbolic_functions=false \
        -Doss_fuzz=disabled -Dglib_assert=false -Dglib_checks=true
}

build_pixman() {
    local d; d=$(fetch https://www.cairographics.org/releases/pixman-0.44.2.tar.gz)
    meson_dep "$d" -Da64-neon=disabled -Darm-simd=disabled -Dneon=disabled -Dtests=disabled \
        -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled
}

build_zstd() {
    local d; d=$(fetch https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz)
    meson_dep "$d/build/meson" -Dbin_programs=false -Dbin_contrib=false -Dbin_tests=false \
        -Dzlib=disabled -Dlzma=disabled -Dlz4=disabled
}

build_slirp() {
    local d; d=$(fetch https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.1/libslirp-v4.9.1.tar.gz)
    local p="$ROOT/build/src/UTM/patches/libslirp-v4.9.1.patch"
    if [ -f "$p" ] && [ ! -f "$d/.patched" ]; then patch -d "$d" -p1 < "$p" >/dev/null && touch "$d/.patched"; fi
    meson_dep "$d"
}

build_ucontext() {
    local d="$SRC/libucontext"
    [ -d "$d" ] || git clone -q https://github.com/utmapp/libucontext.git "$d"
    (cd "$d" && git checkout -q 9b1d8f01a6e99166f9808c79966abe10786de8b6)
    meson_dep "$d" -Dfreestanding=true -Dexport_unprefixed=false
}

build_epoxy() {
    local d="$SRC/libepoxy"
    [ -d "$d" ] || git clone -q https://github.com/utmapp/libepoxy.git "$d"
    (cd "$d" && git checkout -q bf98587477fe68d07b93319ece7b40a7d0e2eabe)
    meson_dep "$d" -Dglx=no -Degl=yes -Dx11=false -Dtests=false
}

for dep in $DEPS; do
    echo "=== [$PLATFORM] building $dep"
    "build_$dep"
done
echo "=== [$PLATFORM] deps done: $DEPS"
