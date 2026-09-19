#!/bin/bash
# test-meta-v3-delta.sh — WP-M10 e2e: the metadata-v3 delta log.
#
# Exercises the recent-changes tier end to end on a real VOLF_V3 image:
#   leg 0  mkfs v3 leaves RT30.delta_pba == 0 (empty recent tier);
#   leg 1  appending through the engine fills the active segment and RT30
#          now names it (structure-before-reference);
#   leg 2  a fresh mount replays the segment chain into the in-memory index
#          (crash/remount reconstruction) — a second remount must agree;
#   leg 3  a torn tail is truncated at the last CRC-valid record: the final
#          delete marker is dropped, every earlier record survives.
#
# No overlay reads (WP-M11) or fold (WP-M14) are involved; the harness
# drives vol_delta_append/vol_delta_lookup directly.
#
# Run from the repo root after `make`/`make test`:
#   bash tools/run-e2e.sh tools/test-meta-v3-delta.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3delta
IMG=metav3delta.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The unit/e2e harness is built by `make test`; build it on demand so the
# suite is runnable on its own too.
if [ ! -x "$B/invf-delta_test" ]; then
    make -C "$REPO" bin/invf-delta_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-delta_test"
fi

N=40

echo "== leg 0: mkfs v3 (empty recent tier) =="
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.3 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"
python3 - "$IMG" <<'PY' || fail "RT30 not empty at mkfs"
import struct, sys
blk = open(sys.argv[1], 'rb').read(4096)
assert blk[0x9D0:0x9D4] == b'RT30', "RT30 magic missing"
ver, = struct.unpack_from('<I', blk, 0x9D4)
delta, = struct.unpack_from('<Q', blk, 0x9EC)
assert ver == 1, "RT30 version %d" % ver
assert delta == 0, "delta_pba=%d, want 0" % delta
print("mkfs: RT30 v1, delta_pba=0")
PY

echo "== leg 1: append records (fill the active segment) =="
$B/invf-delta_test append "$IMG" $N >"$WORK/append.log" 2>&1 \
    || { cat "$WORK/append.log"; fail "delta append failed"; }
grep -q "e2e append: $N records" "$WORK/append.log" \
    || { cat "$WORK/append.log"; fail "append did not report the fixture"; }
python3 - "$IMG" <<'PY' || fail "RT30 did not learn the active segment"
import struct, sys
blk = open(sys.argv[1], 'rb').read(4096)
delta, = struct.unpack_from('<Q', blk, 0x9EC)
seq, = struct.unpack_from('<Q', blk, 0x9F4)
assert delta != 0, "delta_pba still 0 after append"
print("append: RT30 delta_pba=%d seq=%d" % (delta, seq))
PY

echo "== leg 2: remount replay reconstructs the index =="
$B/invf-delta_test verify "$IMG" $N >"$WORK/verify.log" 2>&1 \
    || { cat "$WORK/verify.log"; fail "replay verify failed"; }
grep -q "e2e verify: $N keys replayed" "$WORK/verify.log" \
    || { cat "$WORK/verify.log"; fail "verify did not replay the fixture"; }
# idempotence: a second mount must reconstruct the same index
$B/invf-delta_test verify "$IMG" $N >"$WORK/verify2.log" 2>&1 \
    || { cat "$WORK/verify2.log"; fail "second remount verify failed"; }
echo "remount: index reconstructed twice identically"

echo "== leg 3: torn tail truncates at the last valid record =="
$B/invf-delta_test tear "$IMG" >"$WORK/tear.log" 2>&1 \
    || { cat "$WORK/tear.log"; fail "tear failed"; }
$B/invf-delta_test verify-torn "$IMG" $N >"$WORK/verifytorn.log" 2>&1 \
    || { cat "$WORK/verifytorn.log"; fail "torn-tail replay failed"; }
grep -q "torn delete dropped" "$WORK/verifytorn.log" \
    || { cat "$WORK/verifytorn.log"; fail "torn tail was not truncated"; }
# the earlier records must still be present after the mount replay
grep -q "torn tail; truncated" "$WORK/verifytorn.log" \
    || fail "replay did not report the torn-tail truncation"
echo "torn tail: replay kept the valid prefix, dropped the torn record"

# after all remounts the superblock must still read CLEAN (the delta writes
# are self-durable; no v2 flush path is involved)
python3 - "$IMG" <<'PY' || fail "volume not CLEAN after the delta round-trips"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN after the delta round-trips")
PY

echo "ALL META-V3 DELTA LEGS PASS"
