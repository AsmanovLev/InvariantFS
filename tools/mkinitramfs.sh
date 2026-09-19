#!/bin/bash
# Build the InvariantFS M1 initramfs (busybox + invf-fuse + invf-sweep +
# sweepboot branch + fuse.ko).
# Output: vm/initramfs.cpio.gz
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IR="$ROOT/vm/initramfs"
KVER="$(uname -r)"

mkdir -p "$IR"
cd "$IR"
rm -f "$ROOT/vm/initramfs.cpio.gz"

# vm/ is gitignored, so a fresh checkout has no vm/initramfs tree at all;
# create the directory skeleton the recipe writes into (WP64: reproducible
# builds, WP69: the stale tracked /init was retired).
mkdir -p bin sbin usr/local/bin usr/local/lib lib64 modules \
         proc sys dev tmp mnt/invfs run

# busybox
cp "$ROOT/bin/busybox-static" bin/busybox
chmod 755 bin/busybox
ln -sf busybox bin/sh
# Applet symlinks: /init calls mount/grep/cat/insmod/chroot/... by name, so
# install the full applet set (the tracked tree has no skeleton symlinks).
# `busybox --install -s` records the build-time absolute path as the link
# target, which does not exist in the guest; create relative links instead.
for a in $(./bin/busybox --list); do ln -sf busybox "bin/$a"; done

# engine daemon + shared libs (host paths -> /usr/local)
cp "$ROOT/bin/invf-fuse" usr/local/bin/
chmod 755 usr/local/bin/invf-fuse
# WP23 sweepboot: the offline sweep + the /init branch script. A static
# invf-sweep would spare the shared libs, but this distro ships no
# libzstd.a/libz.a (checked /usr/lib64), so the dynamic deps ride along
# -- invf-sweep needs exactly the set invf-fuse already pulls in, minus
# libfuse (ldd bin/invf-sweep: libzstd.so.1, libz.so.1, libc.so.6).
cp "$ROOT/bin/invf-sweep" usr/local/bin/
chmod 755 usr/local/bin/invf-sweep
cp "$ROOT/tools/sweepboot-init.sh" sweepboot-init.sh
chmod 755 sweepboot-init.sh
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
# Paths must match initramfs-init.sh's insmod /lib/modules/<mod>.ko.
mkdir -p lib/modules
for mod in failover net_failover virtio_net; do
    src=$(find "/lib/modules/$KVER/kernel" -name "$mod.ko.*" | head -1)
    [ -n "$src" ] || continue
    case "$(file -b "$src")" in
        *XZ*)   xz  -dc "$src" > "lib/modules/$mod.ko" ;;
        *Zstd*) zstd -dc "$src" > "lib/modules/$mod.ko" ;;
        *)      cp "$src" "lib/modules/$mod.ko" ;;
    esac
done

# init: UUID-aware device discovery (probes each block device for the
# InvariantFS superblock via `invf-fuse --probe-uuid`; kernel names like
# sda/sdb are not stable and a stale mknod can make a name look present).
cp "$ROOT/tools/initramfs-init.sh" init
chmod 755 init

find . | cpio -o -H newc --quiet | gzip -1 > "$ROOT/vm/initramfs.cpio.gz"
ls -l "$ROOT/vm/initramfs.cpio.gz"
