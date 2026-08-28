#!/bin/bash
# bench-sweep-read.sh — read latency/speed on the SAME corpus, same volume,
# BEFORE sweep (RAW zone, LZ4 segments) and AFTER sweep (ZSTD/PPMD batches,
# containers). Cold caches (remount + drop_caches) before each phase.
# Usage: bash tools/bench-sweep-read.sh
set -e
set -o pipefail

REPO=/home/user/InvariantFS
IMG=swread.img
MNT=/var/tmp/bench/swmnt
CORPUS=${CORPUS_DIR:-/var/tmp/bench/silesia}
WORK=/dev/shm/swread-work

rm -rf "$WORK" && mkdir -p "$WORK" "$MNT"
cd /dev/shm
rm -f "$IMG"
fusermount3 -u "$MNT" 2>/dev/null || true

echo "== build volume (no sweep yet) =="
INVFS_META_FRAC=24 "$REPO/bin/invf-mkfs" "$IMG" 1.2G >/dev/null
"$REPO/bin/invf-import" "$IMG" "$CORPUS" | tail -1

measure() { # $1 = label
    fusermount3 -u "$MNT" 2>/dev/null || true
    (cd /dev/shm && "$REPO/bin/invf-fuse" "$IMG" "$MNT" >/dev/null 2>&1 &)
    for i in $(seq 1 50); do mountpoint -q "$MNT" && break; sleep 0.1; done
    mountpoint -q "$MNT" || { echo "mount FAILED"; exit 1; }
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    python3 - "$MNT" "$1" <<'PY'
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

def fmt(lat_list):
    lat_list.sort()
    q = lambda x: lat_list[int(x * (len(lat_list) - 1))] * 1000
    return q(.5), q(.9), q(.99)

print("## %s" % label)
for blk, n in ((4096, 500), (1 << 20, 150)):
    lats, nbytes = [], 0
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
    med, p90, p99 = fmt(lats)
    print("rand-%-3s: med %8.3f ms  p90 %8.3f ms  p99 %8.3f ms  | %7.1f MB/s"
          % ("4K" if blk == 4096 else "1M", med, p90, p99, nbytes / 1048576 / dt))

t0 = time.perf_counter()
for p in files:
    with open(p, "rb") as f:
        while f.read(1 << 20):
            pass
dt = time.perf_counter() - t0
print("full-seq: %6.1f MB in %6.2f s -> %7.1f MB/s"
      % (total / 1048576, dt, total / 1048576 / dt))
PY
    fusermount3 -u "$MNT"
}

measure "BEFORE sweep (RAW/LZ4)"

echo "== sweep =="
SW_T0=$SECONDS
"$REPO/bin/invf-sweep" "$IMG" > "$WORK/sweep.log" 2>&1
SW_DT=$((SECONDS - SW_T0))
grep -E "sweep done|batches flushed|dedupe" "$WORK/sweep.log" || tail -3 "$WORK/sweep.log"
echo "sweep wall time: ${SW_DT} s ($(python3 -c "print('%.1f' % (202.1/$SW_DT if $SW_DT else 0))") MB/s over corpus)"

measure "AFTER sweep (ZSTD/PPMD/containers)"

echo "== stats =="
"$REPO/bin/invf-stats" "$IMG" 2>/dev/null | grep -E "RAW|SHADOW|TEXT|logical|ratio" || true
