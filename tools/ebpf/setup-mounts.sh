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
        sudo losetup -d "$(cat "$S/loopdev")" 2>/dev/null || true
    fi
    echo "down $S"
    exit 0
fi

S=$1
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
B=$REPO/bin
mkdir -p "$S/mnt-ext4" "$S/mnt-tmpfs" "$S/mnt-invfs"

# ext4 on a loop file (backs leg3 block-layer attribution on the loop dev)
dd if=/dev/zero of="$S/ext4.img" bs=1M count=48 status=none
mkfs.ext4 -q -F "$S/ext4.img"
/sbin/losetup -f 2>/dev/null || sudo losetup -f >/dev/null
LOOP=$(sudo losetup --show -f "$S/ext4.img")
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
