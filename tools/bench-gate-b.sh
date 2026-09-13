#!/bin/bash
# bench-gate-b.sh — v1 vs v2 InvariantFS read-throughput comparison.
# Leg B1: on-disk image sizes + stats
# Leg B2: v1 full-recursive read (3 passes)
# Leg B3: v2 full-recursive read (3 passes)
# Leg B4: summary table + PASS/FAIL verdict
# Usage: bash tools/bench-gate-b.sh
set -e
set -o pipefail

V1_BIN=/tmp/opencode/v1-build/bin
V2_BIN=/home/user/InvariantFS/bin
V1_IMG=/tmp/opencode/bench-v1.img
V2_IMG=/tmp/opencode/bench-v2.img
V1_MNT=/tmp/opencode/mnt-v1
V2_MNT=/tmp/opencode/mnt-v2
PASSES=3

mkdir -p "$V1_MNT" "$V2_MNT"

# --- helpers ----------------------------------------------------------------
umount_all() {
    fusermount3 -u "$V1_MNT" 2>/dev/null || true
    fusermount3 -u "$V2_MNT" 2>/dev/null || true
}
trap umount_all EXIT

dropcaches() {
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
}

wait_mount() { # $1 = mountpoint
    for i in $(seq 1 100); do
        mountpoint -q "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "mount $1 FAILED"; return 1
}

measure_read() { # $1 = binary dir, $2 = image, $3 = mountpoint, $4 = pass number
    local bin_dir=$1 img=$2 mnt=$3 pass=$4

    # unmount any stale mount
    fusermount3 -u "$mnt" 2>/dev/null || true

    # start FUSE daemon (read-only)
    (cd "$(dirname "$img")" && exec "$bin_dir/invf-fuse" "$img" "$mnt" -o ro,allow_other) &
    wait_mount "$mnt"

    dropcaches

    local t0 t1
    t0=$(date +%s%N)
    find "$mnt" -type f -exec cat {} + >/dev/null 2>&1
    t1=$(date +%s%N)

    fusermount3 -u "$mnt" 2>/dev/null || true

    echo $(( (t1 - t0) / 1000000 ))  # ms
}

# --- Leg B1: on-disk stats --------------------------------------------------
echo "=========================================="
echo "Leg B1: on-disk comparison"
echo "=========================================="
echo ""
ls -lh "$V1_IMG" "$V2_IMG"
echo ""
echo "--- invf-stats v1 ---"
"$V1_BIN/invf-stats" "$V1_IMG" 2>/dev/null || true
echo ""
echo "--- invf-stats v2 ---"
"$V2_BIN/invf-stats" "$V2_IMG" 2>/dev/null || true
echo ""

# --- Leg B2: v1 read throughput ---------------------------------------------
echo "=========================================="
echo "Leg B2: v1 read throughput ($PASSES passes)"
echo "=========================================="
V1_TOTAL=0
for p in $(seq 1 $PASSES); do
    ms=$(measure_read "$V1_BIN" "$V1_IMG" "$V1_MNT" "$p")
    echo "  pass $p: ${ms} ms"
    V1_TOTAL=$((V1_TOTAL + ms))
done
V1_AVG=$((V1_TOTAL / PASSES))
echo "  v1 average: ${V1_AVG} ms"
echo ""

# --- Leg B3: v2 read throughput ---------------------------------------------
echo "=========================================="
echo "Leg B3: v2 read throughput ($PASSES passes)"
echo "=========================================="
V2_TOTAL=0
for p in $(seq 1 $PASSES); do
    ms=$(measure_read "$V2_BIN" "$V2_IMG" "$V2_MNT" "$p")
    echo "  pass $p: ${ms} ms"
    V2_TOTAL=$((V2_TOTAL + ms))
done
V2_AVG=$((V2_TOTAL / PASSES))
echo "  v2 average: ${V2_AVG} ms"
echo ""

# --- Leg B4: summary -------------------------------------------------------
echo "=========================================="
echo "Leg B4: summary"
echo "=========================================="
echo ""
echo "  v1 avg read time: ${V1_AVG} ms"
echo "  v2 avg read time: ${V2_AVG} ms"
if [ "$V1_AVG" -gt 0 ]; then
    RATIO=$(awk "BEGIN{printf \"%.2f\", $V2_AVG / $V1_AVG}")
else
    RATIO="N/A"
fi
echo "  speedup ratio (v2/v1): ${RATIO}"
echo ""
echo "  v1 image sparse size: $(stat -c %s "$V1_IMG") bytes"
echo "  v2 image sparse size: $(stat -c %s "$V2_IMG") bytes"
echo ""

# PASS if v2 <= v1 * 1.05
THRESHOLD=$(awk "BEGIN{printf \"%d\", $V1_AVG * 1.05}")
if [ "$V2_AVG" -le "$THRESHOLD" ]; then
    echo "  VERDICT: PASS (v2 ${V2_AVG}ms <= v1 ${V1_AVG}ms * 1.05 = ${THRESHOLD}ms)"
    exit 0
else
    echo "  VERDICT: FAIL (v2 ${V2_AVG}ms > v1 ${V1_AVG}ms * 1.05 = ${THRESHOLD}ms)"
    exit 1
fi
