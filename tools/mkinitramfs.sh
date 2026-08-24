#!/bin/bash
# Build the InvariantFS M1 initramfs (busybox + invf-fuse + fuse.ko).
# Output: vm/initramfs.cpio.gz
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IR="$ROOT/vm/initramfs"
KVER="$(uname -r)"

cd "$IR"
rm -f "$ROOT/vm/initramfs.cpio.gz"

# busybox
cp "$ROOT/bin/busybox-static" bin/busybox
chmod 755 bin/busybox
ln -sf busybox bin/sh

# engine daemon + shared libs (host paths -> /usr/local)
cp "$ROOT/bin/invf-fuse" usr/local/bin/
chmod 755 usr/local/bin/invf-fuse
for lib in libzstd.so.1 libz.so.1 libfuse3.so.4 libc.so.6; do
    cp -L "/lib64/$lib" lib64/
done
cp -L /lib64/ld-linux-x86-64.so.2 lib64/

# fuse kernel module (decompress; busybox insmod can't do xz/zst)
find "/lib/modules/$KVER/kernel/fs/fuse" -name 'fuse.ko.*' \
    | head -1 | xargs -I{} cp {} /tmp/fuse.ko.packed
case "$(file -b /tmp/fuse.ko.packed)" in
    *XZ*)    xz  -dc /tmp/fuse.ko.packed > fuse.ko ;;
    *Zstd*)  zstd -dc /tmp/fuse.ko.packed > fuse.ko ;;
    *)       cp /tmp/fuse.ko.packed fuse.ko ;;
esac
rm -f /tmp/fuse.ko.packed

# virtio_net + net_failover (guest NIC for M4c ssh; decompress like fuse)
mkdir -p modules
for mod in virtio_net net_failover; do
    src=$(find "/lib/modules/$KVER/kernel/drivers/net" -name "$mod.ko.*" | head -1)
    [ -n "$src" ] || continue
    case "$(file -b "$src")" in
        *XZ*)   xz  -dc "$src" > "modules/$mod.ko" ;;
        *Zstd*) zstd -dc "$src" > "modules/$mod.ko" ;;
        *)      cp "$src" "modules/$mod.ko" ;;
    esac
done

chmod 755 init

find . | cpio -o -H newc --quiet | gzip -1 > "$ROOT/vm/initramfs.cpio.gz"
ls -l "$ROOT/vm/initramfs.cpio.gz"
