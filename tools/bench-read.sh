#!/bin/bash
# bench-read.sh — read-speed comparison: btrfs (kernel, zstd:15 image) vs
# InvariantFS (FUSE) on the same corpus. Per pass: RANDOM first (true cold),
# then full sequential. Two passes: cold (fresh mount + dropped caches) & warm.
# Usage: bash tools/bench-read.sh
set -e
set -o pipefail

REPO=/home/user/InvariantFS
BTRFS_IMG=/var/tmp/bench/btrfsbench.img
INVFS_IMG=/dev/shm/silesia14b.img
BMNT=/var/tmp/bench/btrfsmnt
IMNT=/var/tmp/bench/invfsmnt

mkdir -p "$BMNT" "$IMNT"
sudo umount "$BMNT" 2>/dev/null || true
fusermount3 -u "$IMNT" 2>/dev/null || true

mount_btrfs() { sudo mount -o loop,compress=zstd:15 "$BTRFS_IMG" "$BMNT"; }
mount_invfs() { (cd /dev/shm && "$REPO/bin/invf-fuse" -o "attr_t=${INVFS_ATTR_T:-1.0}" "$(basename "$INVFS_IMG")" "$IMNT" &)
                for i in $(seq 1 50); do mountpoint -q "$IMNT" && return 0; sleep 0.1; done
                echo "invfs mount FAILED"; return 1; }
dropcaches() { sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null; }
umount_all() { sudo umount "$BMNT" 2>/dev/null || true; fusermount3 -u "$IMNT" 2>/dev/null || true; }
trap umount_all EXIT

run_pass() { # $1 = mnt root, $2 = label
    python3 - "$1" "$2" <<'PY'
import os, random, sys, time

root, label = sys.argv[1], sys.argv[2]
files = []
for dp, _, fns in os.walk(root):
    for fn in fns:
        p = os.path.join(dp, fn)
        if os.path.isfile(p) and not os.path.islink(p) and os.path.getsize(p) > 0:
            files.append(p)
files.sort()
total = sum(os.path.getsize(p) for p in files)
rng = random.Random(42)
big = sorted(files, key=os.path.getsize)[-4:]

for blk, n in ((4096, 500), (1 << 20, 150)):
    lats = []
    nbytes = 0
    t0 = time.perf_counter()
    for _ in range(n):
        p = big[rng.randrange(len(big))]
        sz = os.path.getsize(p)
        if sz <= blk:
            continue
        off = rng.randrange(0, sz - blk) & ~4095
        r0 = time.perf_counter()
        with open(p, "rb") as f:
            f.seek(off)
            data = f.read(blk)
        lats.append(time.perf_counter() - r0)
        nbytes += len(data)
    dt = time.perf_counter() - t0
    lats.sort()
    q = lambda x: lats[int(x * (len(lats) - 1))] * 1000
    print("%s rand-%-3s: med %7.3f ms p90 %7.3f ms p99 %7.3f ms | %6.1f MB/s"
          % (label, "4K" if blk == 4096 else "1M", q(.5), q(.9), q(.99),
             nbytes / 1048576 / dt))

t0 = time.perf_counter()
for p in files:
    with open(p, "rb") as f:
        while f.read(1 << 20):
            pass
dt = time.perf_counter() - t0
print("%s full-seq: %7.1f MB in %6.2f s -> %7.1f MB/s"
      % (label, total / 1048576, dt, total / 1048576 / dt))
PY
}

mount_btrfs; dropcaches
run_pass "$BMNT" "btrfs-cold"
run_pass "$BMNT" "btrfs-warm"
sudo umount "$BMNT"

mount_invfs; dropcaches
run_pass "$IMNT" "invfs-cold"
run_pass "$IMNT" "invfs-warm"
fusermount3 -u "$IMNT"
trap - EXIT
