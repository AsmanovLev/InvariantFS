#!/bin/bash
# bench-rootfs.sh -- density + read perf: InvariantFS vs squashfs/btrfs/
# xfs/ext4 on a rootfs-like corpus (/usr snapshot: ~72k entries, ~2.5G).
#
# NOT covered here (disclosed, not silently dropped): zfs/bcachefs/erofs --
# no userspace tools and no kernel modules for them on this box.
# No drop_caches between read runs (unprivileged container): runs are
# reported as run1/run2 in order, never labelled "cold".
#
# Layout: everything under $WORK (default /srv/flakey/invfs-bench).
# Nothing outside it is created or removed.
#
# Usage: bash tools/bench-rootfs.sh [workdir]
# Result: $WORK/RESULTS.txt (append) + summary table on stdout.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK="${1:-/srv/flakey/invfs-bench}"
CORPUS=$WORK/corpus/usr
RES=$WORK/RESULTS.txt
export PATH="$PATH:/sbin:/usr/sbin"

mkdir -p "$WORK"
echo "== bench-rootfs @ $(date -u +%FT%TZ) on $(uname -r) ==" | tee -a "$RES"

# ---- corpus snapshot (once) ---------------------------------------------
if [ ! -f "$WORK/corpus.ready" ]; then
    echo "-- snapshot /usr -> corpus (one time, ~2.6G)" | tee -a "$RES"
    mkdir -p "$WORK/corpus"
    cp -a /usr "$WORK/corpus/" 2>>"$RES" || { echo "FATAL: corpus copy failed" | tee -a "$RES"; exit 1; }
    touch "$WORK/corpus.ready"
fi
echo "corpus: $(du -sh "$CORPUS" | cut -f1) $(find "$CORPUS" | wc -l) entries $(find "$CORPUS" -type f | wc -l) files" | tee -a "$RES"

t0=$(date +%s)

# ---- 1. InvariantFS -------------------------------------------------------
echo "---- [invfs] mkfs+import" | tee -a "$RES"
export INVFS_META_FRAC=16
S=$(date +%s); $B/invf-mkfs "$WORK/invfs.img" 8 >/dev/null 2>&1; echo "mkfs rc=$? $(( $(date +%s) - S ))s" | tee -a "$RES"
S=$(date +%s); $B/invf-import "$WORK/invfs.img" "$CORPUS" >/dev/null 2>&1; echo "import rc=$? $(( $(date +%s) - S ))s" | tee -a "$RES"
echo "---- [invfs] sweep" | tee -a "$RES"
S=$(date +%s); $B/invf-sweep "$WORK/invfs.img" >/dev/null 2>&1; echo "sweep rc=$? $(( $(date +%s) - S ))s" | tee -a "$RES"
echo "invfs image: apparent $(du -sb --apparent-size "$WORK/invfs.img" | cut -f1) on-disk $(du -sb "$WORK/invfs.img" | cut -f1)" | tee -a "$RES"
$B/invf-fsck "$WORK/invfs.img" 2>&1 | grep -E "^(OK|DAMAGED|REPAIRED)" | tee -a "$RES"

echo "---- [invfs] FUSE read runs" | tee -a "$RES"
mkdir -p "$WORK/mnt-invfs"
$B/invf-fuse "$WORK/invfs.img" "$WORK/mnt-invfs" 2>/dev/null || echo "FUSE mount failed" | tee -a "$RES"
if grep -q " $WORK/mnt-invfs " /proc/mounts 2>/dev/null; then
    for run in 1 2; do
        S=$(date +%s); BYTES=$(tar -cf - -C "$WORK/mnt-invfs" . 2>/dev/null | wc -c); DT=$(( $(date +%s) - S ))
        echo "invfs read run$run: ${DT}s ${BYTES}B" | tee -a "$RES"
    done
    umount "$WORK/mnt-invfs" 2>/dev/null || fusermount3 -u "$WORK/mnt-invfs" 2>/dev/null || true
else
    echo "invfs read: SKIPPED (no FUSE mount)" | tee -a "$RES"
fi

# ---- 2. squashfs ----------------------------------------------------------
echo "---- [squashfs] mksquashfs zstd:19" | tee -a "$RES"
S=$(date +%s); mksquashfs "$CORPUS" "$WORK/root.sqsh" -comp zstd -Xcompression-level 19 -noappend >/dev/null 2>&1; echo "mksquashfs rc=$? $(( $(date +%s) - S ))s" | tee -a "$RES"
echo "squashfs image: $(du -sb "$WORK/root.sqsh" | cut -f1)B" | tee -a "$RES"

# ---- 3/4/5. btrfs / ext4 / xfs (loop images, same corpus) ------------------
loop_test() { # fstype mkfscmd imgsize_G mountopts
    local fs=$1 mk=$2 size=$3 opts=$4
    local img=$WORK/$fs.img mnt=$WORK/mnt-$fs
    echo "---- [$fs] mkfs+cp" | tee -a "$RES"
    mkdir -p "$mnt"; rm -f "$img"; truncate -s "${size}G" "$img"
    # shellcheck disable=SC2086
    $mk "$img" >/dev/null 2>&1 || { echo "$fs mkfs FAILED" | tee -a "$RES"; return 0; }
    # shellcheck disable=SC2086
    sudo -n mount -o loop,$opts "$img" "$mnt" 2>>"$RES" || { echo "$fs mount FAILED" | tee -a "$RES"; return 0; }
    S=$(date +%s); sudo -n cp -a "$CORPUS"/. "$mnt"/ 2>>"$RES"; echo "$fs cp rc=$? $(( $(date +%s) - S ))s" | tee -a "$RES"
    sync
    if [ "$fs" = btrfs ]; then
        S=$(date +%s); sudo -n btrfs filesystem defrag -r -czstd -L15 "$mnt" >/dev/null 2>&1; echo "$fs defrag $(( $(date +%s) - S ))s" | tee -a "$RES"; sync
        sudo -n btrfs filesystem usage -b "$mnt" 2>/dev/null | grep -E "Data|Metadata" | grep -v free | tee -a "$RES" || true
        sudo -n compsize -x "$mnt" 2>/dev/null | tail -n 4 | tee -a "$RES" || true
    fi
    echo "$fs df-used: $(df -B1 "$mnt" | tail -1 | awk '{print $3}')B" | tee -a "$RES"
    for run in 1 2; do
        S=$(date +%s); BYTES=$(sudo -n tar -cf - -C "$mnt" . 2>/dev/null | wc -c); DT=$(( $(date +%s) - S ))
        echo "$fs read run$run: ${DT}s ${BYTES}B" | tee -a "$RES"
    done
    sudo -n umount "$mnt" 2>/dev/null || true
    echo "$fs image: $(du -sb "$img" | cut -f1)B" | tee -a "$RES"
}
loop_test btrfs "/sbin/mkfs.btrfs -q -f" 6 "compress-force=zstd:15"
loop_test ext4 "/sbin/mkfs.ext4 -q -F" 5 ""
loop_test xfs "/sbin/mkfs.xfs -q -f" 5 ""

echo "== bench-rootfs done in $(( $(date +%s) - t0 ))s @ $(date -u +%FT%TZ) ==" | tee -a "$RES"
