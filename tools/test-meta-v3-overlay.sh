#!/bin/bash
# test-meta-v3-overlay.sh — WP-M11 e2e: metadata-v3 overlay reads.
#
# Proves the two-tier read contract end to end on a real VOLF_V3 image:
#   leg 0  mkfs v3 (no base rows, empty recent tier);
#   leg 1  base rows are written, then delta records are appended through the
#          WP-M10 API (invf-overlay_test scenario): a delta value shadows an
#          existing base inode, a delta delete hides a base inode, a delta
#          update wins, a delta-only key resolves alone, and the base root
#          page is byte-identical throughout (base untouched on disk);
#   leg 2  the readdir merge (base a,b,d + delta b=202,c=103,delete d)
#          yields the sorted, deduped a,b=202,c and the same through the
#          public CLI (invf-v3inode get sees the delta row);
#   leg 3  a fresh mount replays the delta chain and every overlay result is
#          identical (remount persistence).
#
# No fold (WP-M14) or mutation wiring (WP-M12) is involved; the harness
# appends with vol_delta_append directly.
#
# Run from the repo root after `make`/`make test`:
#   bash tools/run-e2e.sh tools/test-meta-v3-overlay.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3overlay
IMG=metav3overlay.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The overlay driver is not part of `make test`; build it on demand.
if [ ! -x "$B/invf-overlay_test" ]; then
    make -C "$REPO" bin/invf-overlay_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-overlay_test"
fi

echo "== leg 0: mkfs v3 =="
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.3 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"
echo "mkfs: v3 metadata skeleton"

echo "== leg 1: overlay reads (delta shadows base; base untouched) =="
$B/invf-overlay_test scenario "$IMG" >"$WORK/scenario.log" 2>&1 \
    || { cat "$WORK/scenario.log"; fail "overlay scenario failed"; }
grep -q "SCENARIO PASS" "$WORK/scenario.log" \
    || { cat "$WORK/scenario.log"; fail "scenario did not report PASS"; }
grep -q "base root page byte-identical" "$WORK/scenario.log" \
    || fail "base was not proven untouched"
cat "$WORK/scenario.log"

echo "== leg 2: readdir merge + the public CLI sees the same overlay =="
# public by-name get: WP-M11 makes vol_v3_inode_get consult the delta first
OUT=$($B/invf-v3inode "$IMG" get 42) \
    || { echo "$OUT" >&2; fail "invf-v3inode get 42 failed"; }
echo "$OUT" | grep -q "mode=0666" \
    || { echo "$OUT" >&2; fail "public get did not see the delta update (0666)"; }
# a delta delete hides the base row from the public surface too
if $B/invf-v3inode "$IMG" get 43 >"$WORK/get43.out" 2>&1; then
    cat "$WORK/get43.out" >&2
    fail "invf-v3inode get 43 present (want hidden by the delta delete)"
fi
echo "public get: inode 42 = 0666 (delta), inode 43 absent (delta delete)"

echo "== leg 3: remount persistence =="
$B/invf-overlay_test verify "$IMG" >"$WORK/verify.log" 2>&1 \
    || { cat "$WORK/verify.log"; fail "overlay verify after remount failed"; }
grep -q "VERIFY PASS" "$WORK/verify.log" \
    || { cat "$WORK/verify.log"; fail "remount verify did not report PASS"; }
cat "$WORK/verify.log"

FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }

# after all round-trips the superblock must still read CLEAN
python3 - "$IMG" <<'PY' || fail "volume not CLEAN at the end"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN after all round-trips")
PY

echo "ALL META-V3 OVERLAY LEGS PASS"
