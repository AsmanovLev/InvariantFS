#!/bin/bash
# test-meta-v3-fold.sh — WP-M14 e2e: fold (merge delta into base, atomic
# publish, reset).
#
# WP-M14's fold applies every live delta record to a COW copy of the base
# B+-tree, publishes the new root through the WP-M2 double slot, and only
# then resets the delta. This leg proves the whole contract on a real
# VOLF_V3 image through the public engine surface:
#   leg 0  mkfs v3 (empty base, empty recent tier);
#   leg 1  base rows are written, delta records update a base inode, delete
#          a base inode, add a delta-only inode and dirent, and delete a
#          base dirent; the base root is captured; fold runs (invf-fold_test
#          scenario) and asserts the base now holds every merged value, the
#          deleted keys are gone, the delta is empty, RT30.delta_pba == 0
#          and seq advanced by one, the published slot moved, a delta-first
#          reader resolves the merged value (add-before-remove), an empty
#          delta fold is a no-op, and the post-fold base tree is valid;
#   leg 2  a fresh mount persists it: the base tier alone carries the merged
#          namespace and the recent tier is empty (invf-fold_test verify);
#   leg 3  crash in the publish/reset window: a fold is SIGKILL'd after the
#          new base is published but before the delta is reset; a remount
#          replays the old delta against the new base and lands on identical
#          content (idempotent add-before-remove), fsck clean;
#   leg 4  fsck is clean and the superblock stays CLEAN.
#
# No sweep (WP-M18) or reclaim (WP-M15) is involved: the driver calls
# vol_v3_fold() directly and fold retains the displaced pages by design.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-fold.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3fold
IMG=metav3fold.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The fold driver is not part of `make test`; build it on demand.
if [ ! -x "$B/invf-fold_test" ]; then
    make -C "$REPO" bin/invf-fold_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-fold_test"
fi

echo "== leg 0: mkfs v3 =="
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.3 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"
echo "mkfs: v3 metadata skeleton"

echo "== leg 1: append deltas then fold (base merged, delta reset) =="
$B/invf-fold_test scenario "$IMG" >"$WORK/scenario.log" 2>&1 \
    || { cat "$WORK/scenario.log"; fail "fold scenario failed"; }
grep -q "SCENARIO PASS" "$WORK/scenario.log" \
    || { cat "$WORK/scenario.log"; fail "scenario did not report PASS"; }
cat "$WORK/scenario.log"

echo "== leg 2: remount persists the merged base (recent tier empty) =="
$B/invf-fold_test verify "$IMG" >"$WORK/verify.log" 2>&1 \
    || { cat "$WORK/verify.log"; fail "fold verify after remount failed"; }
grep -q "VERIFY PASS" "$WORK/verify.log" \
    || { cat "$WORK/verify.log"; fail "remount verify did not report PASS"; }
cat "$WORK/verify.log"

echo "== leg 3: crash in the publish/reset window (idempotent replay) =="
CIMG=metav3foldcrash.img
rm -f "$CIMG"
INVFS_V3=1 $B/invf-mkfs "$CIMG" 0.3 >/dev/null 2>&1 \
    || fail "crash-leg mkfs failed"
# The fold SIGKILLs itself right after publishing the new base and before the
# delta reset, so the process exits 137 and never returns.
set +e
INVFS_FOLD_ABORT_AT=published $B/invf-fold_test crash-setup "$CIMG" \
    >"$WORK/crash.log" 2>&1
rc=$?
set -e
[ "$rc" = "137" ] || { cat "$WORK/crash.log"; fail "fold did not die in the publish/reset window (rc=$rc)"; }
# The old delta must still be named (publish happened, reset did not).
python3 - "$CIMG" <<'PY' || fail "crash left the wrong on-disk state"
import struct, sys
b = open(sys.argv[1], 'rb').read(4096)
off = 0x9D0
slot0, slot1, delta, seq = struct.unpack_from('<QQQQ', b, off + 0xC)
assert delta != 0, "delta_pba cleared: reset ran despite the crash"
assert slot0 or slot1, "no published root slot after the crash"
print("crash state: delta_pba=%d seq=%d published slot(s)=%d/%d" %
      (delta, seq, slot0, slot1))
PY
$B/invf-fold_test crash-verify "$CIMG" >"$WORK/crashverify.log" 2>&1 \
    || { cat "$WORK/crashverify.log"; fail "crash remount verify failed"; }
grep -q "CRASH VERIFY PASS" "$WORK/crashverify.log" \
    || { cat "$WORK/crashverify.log"; fail "crash verify did not report PASS"; }
cat "$WORK/crashverify.log"
CFSCK=$($B/invf-fsck "$CIMG" 2>&1) || { echo "$CFSCK"; fail "crash-leg fsck nonzero"; }
echo "$CFSCK" | grep -q "^OK$" || { echo "$CFSCK"; fail "crash-leg fsck not OK"; }
echo "crash: publish/reset window replayed idempotently, fsck OK"

echo "== leg 4: fsck + CLEAN state =="
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }

# after the fold + remount the superblock must still read CLEAN
python3 - "$IMG" <<'PY' || fail "volume not CLEAN at the end"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN after fold + remount")
PY

echo "ALL META-V3 FOLD LEGS PASS"
