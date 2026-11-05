FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    lz4 cpio e2fsprogs erofs-utils android-sdk-libsparse-utils python3 python3-pip gdisk fdisk xxd file \
    zstd xz-utils unzip zip qemu-utils dosfstools mtools fsverity binutils device-tree-compiler ca-certificates curl git openssl \
    && rm -rf /var/lib/apt/lists/*
