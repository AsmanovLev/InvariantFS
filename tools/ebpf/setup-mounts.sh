#!/bin/bash
# setup-mounts.sh — WP622 scratch mounts: ext4-on-loop, tmpfs, invfs.
# Usage: bash tools/ebpf/setup-mounts.sh <scratch-dir>
# Prints KEY=value lines for run-leg.sh (sourced via eval).
# Teardown: bash tools/ebpf/setup-mounts.sh --down <scratch-dir>
#
# Disk discipline: two small images (48M ext4, 0.1G invfs sparse) +
# a 128M tmpfs; everything under the given scratch dir, itself under
# /tmp/invfs-wp622-*. Nothing on shared /dev/shm or /tmp paths.
set -e
set -o pipefail

if [ "$1" = "--down" ]; then
    S=$2
    for m in "$S/mnt-invfs" "$S/mnt-ext4" "$S/mnt-tmpfs"; do
        if grep -q " $m " /proc/mounts 2>/dev/null; then
            sudo umount "$m" 2>/dev/null || fusermount3 -u "$m" 2>/dev/null || true
        fi
    done
    sleep 0.5
    for m in "$S/mnt-invfs" "$S/mnt-ext4" "$S/mnt-tmpfs"; do
        if grep -q " $m " /proc/mounts 2>/dev/null; then
            fusermount3 -u "$m" 2>/dev/null || sudo umount -l "$m" 2>/dev/null || true
        fi
    done
    if [ -f "$S/loopdev" ]; then
        sudo "$(command -v losetup || echo /sbin/losetup)" -d "$(cat "$S/loopdev")" 2>/dev/null || true
    fi
    # Every loop device whose backing file is inside this scratch, not just
    # the one recorded in $S/loopdev: a setup that failed between attach and
    # the "echo $LOOP" would otherwise leak a device per attempt until the
    # box runs out of them (measured: ten leaked devices in one afternoon).
    sudo "$(command -v losetup || echo /sbin/losetup)" -a 2>/dev/null \
        | awk -v s="$S/" 'index($0, s) {sub(/:.*/, ""); print}' \
        | while read -r dev; do sudo "$(command -v losetup || echo /sbin/losetup)" -d "$dev" 2>/dev/null || true; done
    echo "down $S"
    exit 0
fi

S=$1
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
B=$REPO/bin
# Debian puts mkfs.ext4 / losetup under /sbin, which is not on a
# non-root user's PATH; resolving here is why the gate can run as the
# invoking user instead of demanding a root login shell.
MKFS=$(command -v mkfs.ext4 || echo /sbin/mkfs.ext4)
LOSETUP=$(command -v losetup || echo /sbin/losetup)
[ -x "$MKFS" ] || { echo "FAIL: mkfs.ext4 not found (e2fsprogs)"; exit 2; }
mkdir -p "$S/mnt-ext4" "$S/mnt-tmpfs" "$S/mnt-invfs"

# ext4 on a loop file (backs leg3 block-layer attribution on the loop dev)
dd if=/dev/zero of="$S/ext4.img" bs=1M count=48 status=none
"$MKFS" -q -F "$S/ext4.img"
LOOP=$(sudo "$LOSETUP" --show -f "$S/ext4.img")
echo "$LOOP" > "$S/loopdev"
sudo mount -o loop,noatime "$LOOP" "$S/mnt-ext4"
sudo chown "$(id -u):$(id -g)" "$S/mnt-ext4"

# tmpfs, small and explicit
sudo mount -t tmpfs -o size=128M,noatime tmpfs "$S/mnt-tmpfs"
sudo chown "$(id -u):$(id -g)" "$S/mnt-tmpfs"

# invfs volume + daemon mount
"$B/invf-mkfs" "$S/invfs.img" 0.1 >/dev/null
"$B/invf-fuse" "$S/invfs.img" "$S/mnt-invfs" 2>"$S/fuse.log"
for _ in $(seq 1 50); do
    grep -q " $S/mnt-invfs " /proc/mounts && break
    sleep 0.1
done
grep -q " $S/mnt-invfs " /proc/mounts || { echo "FAIL: invfs mount never appeared; tail:"; tail -n 20 "$S/fuse.log"; exit 1; }

echo "SCRATCH=$S"
echo "MNT_EXT4=$S/mnt-ext4"
echo "MNT_TMPFS=$S/mnt-tmpfs"
echo "MNT_INVFS=$S/mnt-invfs"
echo "LOOPDEV=$LOOP"
echo "up $S"
