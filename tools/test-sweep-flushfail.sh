#!/bin/bash
# test-sweep-flushfail.sh — a sweep whose DURABILITY POINT fails must be
# visible in the exit code.
#
# Stage 7 of a sweep ("finalize: checkpoint + volume flush") is the point
# where the run's data rewrite is supposed to reach stable storage. Before
# this suite, tools/invf-sweep.c printed
#
#     warning: final flush failed
#
# and then RETURNED 0. The sibling failure two lines of intent away (the
# batch flush, invf-sweep.c:2072) did `failed++` and exited 1, so the same
# class of failure was reported two opposite ways -- and the one that lied
# was the last one, after everything had already been rewritten.
#
# The consequence was worse than a wrong number. A failed flush on a
# rewritten volume leaves the data unverified and the volume latched dirty,
# but the bytes themselves still read back fine and `invf-fsck` still says
# OK -- so the exit code was the ONLY signal the caller had, and it was 0.
#
# The injection is INVFS_FLUSH_FAIL_AT=N: the Nth vol_flush of the process
# latches an I/O error and returns -1 (src/core/volume.c, the same shape as
# the existing INVFS_SYNC_FAIL_AT hook). N=1 is the final flush -- a
# seven-stage sweep without --seal issues exactly one vol_flush, so this is
# deterministic, not a race.
#
# Legs:
#   A  a failed durability point exits NONZERO and says the run did not
#      complete
#   B  a healthy sweep still exits 0  (the fix must not break the good path)
#   C  the volume is still bit-exact after the failed run (invf-cat + cmp --
#      NOT invf-verify --deep, which checks readability and length only)
#   D  the volume recovers: a second sweep with no injection exits 0 and is
#      still bit-exact
#
# Why this cannot flake: every assertion is on an exit code or on a fixed
# string, and the injection is a counted call, not a timing window. There is
# no sleep, no timeout race, and no dependence on how full the volume is.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
WORK=/dev/shm/sweeplf
IMG=$WORK/slf.img
rm -rf "$WORK" && mkdir -p "$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }

# 6 incompressible files, each multi-segment once written RAW (a 65536 B
# segment size over 300 KB gives ~5 each), so the sweep really does rewrite
# segments -- the point of the leg is a rewrite whose flush then fails.
python3 - "$WORK" <<'PY'
import os, random, sys
r = random.Random(0x5EED)
for i in range(1, 7):
    with open(os.path.join(sys.argv[1], "b%d.bin" % i), "wb") as f:
        f.write(bytes(r.getrandbits(8) for _ in range(300000)))
PY

fixture() {
    rm -f "$IMG"
    $B/invf-mkfs "$IMG" 0.0625 >"$WORK/mkfs.log" 2>&1 || fail "mkfs"
    local i
    for i in 1 2 3 4 5 6; do
        $B/invf-cp "$IMG" "$WORK/b$i.bin" "b$i.bin" >/dev/null 2>&1 \
            || fail "cp b$i.bin"
    done
}

# bit-exactness via invf-cat + cmp. invf-verify --deep is NOT an oracle
# here: it checks readability and length only, because
# invfs_ast_block_entry carries a pba and no content hash, so a recipe
# resolving to a valid-but-wrong segment reports "0 corrupt".
assert_bit_exact() {
    local tag="$1" i
    for i in 1 2 3 4 5 6; do
        $B/invf-cat "$IMG" "b$i.bin" "$WORK/out.bin" >/dev/null 2>&1 \
            || fail "$tag: b$i.bin unreadable"
        cmp -s "$WORK/b$i.bin" "$WORK/out.bin" \
            || fail "$tag: b$i.bin is NOT bit-exact"
    done
}

echo "== leg B: a healthy sweep exits 0 =="
fixture
set +e
$B/invf-sweep "$IMG" >"$WORK/healthy.log" 2>&1
RC=$?
set -e
[ "$RC" -eq 0 ] || { cat "$WORK/healthy.log"; fail "healthy sweep rc=$RC (want 0)"; }
grep -q "volume durable" "$WORK/healthy.log" \
    || { cat "$WORK/healthy.log"; fail "healthy sweep did not report durability"; }
grep -q "FATAL" "$WORK/healthy.log" \
    && { cat "$WORK/healthy.log"; fail "a healthy sweep printed FATAL"; }
echo "   rc=0 and the stage reports the volume durable"

echo "== leg A: a failed durability point exits nonzero =="
fixture
set +e
INVFS_FLUSH_FAIL_AT=1 $B/invf-sweep "$IMG" >"$WORK/failflush.log" 2>&1
RC=$?
set -e
[ "$RC" -ne 0 ] \
    || { cat "$WORK/failflush.log"; fail "a FAILED final flush exited 0 -- the defect"; }
grep -q "FATAL: the final volume flush failed" "$WORK/failflush.log" \
    || { cat "$WORK/failflush.log"; fail "the failed flush was not reported as fatal"; }
grep -q "This run did NOT complete" "$WORK/failflush.log" \
    || { cat "$WORK/failflush.log"; fail "the message does not tell the caller the run is partial"; }
grep -q "flush failed" "$WORK/failflush.log" \
    || { cat "$WORK/failflush.log"; fail "the finalize stage did not report the failure"; }
echo "   rc=$RC, FATAL reported, the caller is told this is partial success"

echo "== leg C: the failed run did not lose data =="
assert_bit_exact "after the failed flush"
echo "   all 6 files bit-exact (invf-cat + cmp)"

echo "== leg D: the volume recovers on a clean re-sweep =="
set +e
$B/invf-sweep "$IMG" >"$WORK/resweep.log" 2>&1
RC=$?
set -e
[ "$RC" -eq 0 ] || { cat "$WORK/resweep.log"; fail "recovery sweep rc=$RC (want 0)"; }
assert_bit_exact "after the recovery sweep"
echo "   rc=0 and all 6 files still bit-exact"

echo "PASS: a failed durability point is visible in invf-sweep's exit code"
