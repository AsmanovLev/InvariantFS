#!/bin/bash
# test-put-failure.sh — a sweep remap whose row publish FAILS must roll
# back, not free the live blocks (leg2 wrong-bytes root cause, 52eba7a).
#
# THE DEFECT THIS IS THE RED CONTROL FOR
#
# vol_sweep_one_v3's multi-segment remap (src/core/vol_sweep.c) set
# published=1 unconditionally after vol_inode_delta_put, ignoring its
# return code. A put failure mid-pass (EIO under dm-error in leg2) ran
# the supersede below and freed blocks the live row still named; the
# next allocator handed them out and the victim read the donor's bytes
# with valid framing -- silent cross-file corruption, fsck-blind, with
# content-blind recipe keys colliding so both rows name one blob.
# 52eba7a checks the put: a failed publish prints "row publish failed"
# and falls into the existing rollback (frees the new blocks, keeps the
# old, stays unstamped); refmap validate and stamps run only when the
# publish landed.
#
# THE INJECTION
#
# INVFS_FAULT="inode_delta_put:1" fails the FIRST vol_inode_delta_put
# of the sweep process (src/core/vol_btree.c:3932, seam documented in
# src/core/vol_fault.h). The volume holds ONE multi-segment RAW file,
# so the first put of the pass is that file's remap publish -- nothing
# else puts before it (collect is read-only, the savepoint is SPT0).
# The fault is armed for the sweep process only; mkfs/cp run clean.
# The ":1" is load-bearing: a bare INVFS_FAULT="inode_delta_put" with
# no ":n" never arms (vol_fault.h parses the countdown from the
# colon), so the suite would run against the healthy path and prove
# nothing. The "row publish failed" log assertion below is what would
# catch that: it is printed only by the guard, only when a put fails.
#
# WHAT THIS ASSERTS (patched build)
#
#   1. the pass really reached the failed publish (the sweep log must
#      say "row publish failed" AND "rolled back"), so the suite cannot
#      pass vacuously on a build where the fault never fired;
#   2. the free-block count does not drop across the sweep beyond the
#      small metadata churn a sweep legitimately does (DROP <= 8, the
#      same ceiling tools/test-sweep-publish-rollback.sh uses): the
#      rollback hands back every rewritten block;
#   3. unclaimed does not grow past the display quantum (the stranded
#      twin of the same defect: new segments allocated and named by
#      nothing);
#   4. the file reads back byte-identical (invf-cat + cmp -- NOT
#      invf-verify --deep, which checks readability and length only);
#   5. fsck is clean and invf-verify --deep reports 0 corrupt;
#   6. recovery: a clean re-sweep exits 0 and the file is still exact
#      (the abandoned pass left the volume healthy, the file RAW).
#
# WHY ASSERTIONS 4-5 CANNOT BE THE BITE (read before "fixing" this)
#
# On a roomy volume the unpatched corruption is LATENT: the freed-
# while-named blocks still hold the victim's bytes until something
# reuses them, so cmp, fsck and verify all pass on the broken build
# (measured: all three green unpatched). Reuse from inside the same
# pass cannot be forced from outside: sweep (shadow-class) allocations
# never take RAW-range blocks -- "the reverse crossing does not exist"
# (src/core/volume.c, alloc_raw_or_shadow) -- so only a later RAW-class
# write could land there, and only if no other free run came first in
# scan order. The bite below is therefore the ACCOUNTING twin of the
# defect, which is exact, not circumstantial: pre-fix the pass strands
# the 78 new blocks it wrote (unclaimed +0.3 MiB) and drops 80 free
# blocks; post-fix it rolls all 78 back (drop 2: the stored recipe blob
# plus churn) and unclaimed does not move. Measured on this suite's own
# corpus (one 300000 B seeded-random file, 5 segments), identical
# fixture both runs:
#
#                        free before -> after     unclaimed    sweep log
#   patched (52eba7a)    7566 -> 7564 (drop 2)    0.1 -> 0.1  publish-failed + rolled back 78/5
#   unpatched (reverted) 7566 -> 7486 (drop 80)   0.1 -> 0.4  SILENT (rc still 0)
#
# Run:  bash tools/test-put-failure.sh      (from make e2e via run-e2e.sh)
#   or:  INVFS_E2E_AGENT=wp304 bash tools/run-e2e.sh tools/test-put-failure.sh
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# WP205: one scratch-root answer for the whole suite set (see
# tools/lib-scratch.sh).
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
# A per-process scratch dir: every worktree's `make test` starts by tearing
# down its own $WORK, and two agents running `make test` concurrently would
# otherwise delete each other's images (the collision WP123-FIX documents).
WORK=${INVFS_PUTFAIL_WORK:-$(invfs_scratch_root)/put-fail-$$}
IMG=$WORK/vol.img
REF=$WORK/ref
LOG=$WORK/sweep.log

fail() { echo "FAIL: $*" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$REF" || fail "cannot create $WORK"
trap 'rm -rf "$WORK"' EXIT

free_blocks() {   # <img> -- integer free-block count from fsck
    "$B/invf-fsck" "$1" 2>/dev/null |
        sed -n 's/^[[:space:]]*free blocks:[[:space:]]*\([0-9][0-9]*\).*/\1/p'
}
unclaimed_mib() { # <img>
    "$B/invf-stats" "$1" 2>/dev/null |
        sed -n 's/.*unclaimed: \([0-9][0-9.]*\) MiB.*/\1/p'
}

echo "== build a volume and one multi-segment file =="
# 0.0625 GiB. Small on purpose: this suite is about publish accounting,
# not throughput. 300000 B at a 64 KiB segment size = 5 RAW segments,
# so the per-segment remap path is the one that runs. Seeded RNG, not
# /dev/urandom: the block counts below are deterministic only for a
# fixed corpus.
"$B/invf-mkfs" "$IMG" 0.0625 >"$WORK/mkfs.log" 2>&1 || fail "invf-mkfs"
python3 - "$REF/v.bin" <<'PY' || fail "fixture RNG"
import random, sys
r = random.Random(0x304)
open(sys.argv[1], "wb").write(bytes(r.getrandbits(8) for _ in range(300000)))
PY
"$B/invf-cp" "$IMG" "$REF/v.bin" v.bin >/dev/null || fail "invf-cp v.bin"
FREE_BEFORE=$(free_blocks "$IMG")
[ -n "$FREE_BEFORE" ] || fail "could not read the free-block count"
UNC_BEFORE=$(unclaimed_mib "$IMG")
[ -n "$UNC_BEFORE" ] || fail "could not read unclaimed"
echo "  free before the sweep: $FREE_BEFORE blocks; unclaimed: $UNC_BEFORE MiB"

echo "== fault sweep: the row publish must fail contained, not silent =="
# The fault is armed for THIS process only (one-shot first put). mkfs/cp
# above ran clean; the resweep below runs clean too.
set +e
INVFS_FAULT="inode_delta_put:1" "$B/invf-sweep" "$IMG" >"$LOG" 2>&1
RC=$?
set -e
[ "$RC" -eq 0 ] ||
    { cat "$LOG"; fail "fault sweep rc=$RC (want 0: the failure is contained, the pass completes)"; }
echo "  sweep rc=0 (the failure was contained)"

# The pass must SAY the publish failed, so the suite cannot pass
# vacuously on a build where the fault never fired (e.g. a bare
# INVFS_FAULT="inode_delta_put" with no ":n", which never arms) or on
# a build where the guard was removed (pre-fix the put failure is
# silent and rc is still 0).
grep -q "row publish failed" "$LOG" ||
    { cat "$LOG"; fail "the sweep never reached the failed-publish path, so this suite proved nothing"; }
grep "row publish failed" "$LOG" | sed 's/^/  /'

# And it must SAY it rolled back, with at least one segment handed back.
grep -q "rolled back" "$LOG" ||
    { cat "$LOG"; fail "the sweep reached the failed publish but never rolled back"; }
RB=$(grep -m1 -o 'rolled back [0-9]* rewritten block(s) across [0-9]* segment(s)' "$LOG")
RB_SEGS=$(echo "$RB" | sed -n 's/.*across \([0-9]*\) .*/\1/p')
[ -n "$RB_SEGS" ] && [ "$RB_SEGS" -ge 1 ] ||
    fail "could not read the rollback's segment count out of the sweep log"
echo "  $RB"

echo "== the abandoned pass kept every block it wrote =="
FREE_AFTER=$(free_blocks "$IMG")
[ -n "$FREE_AFTER" ] || fail "could not read the free-block count"
# The primary accounting assertion, and the one that IS the defect's
# stranded twin: a sweep that publishes nothing must leave the volume
# the size it found it, within the small metadata churn a sweep does on
# its own. Pre-fix this drops by the whole rewritten set (measured 80
# on this corpus: 78 stranded segments plus blob/churn) and nothing ever
# gives it back. 8 blocks is the same ceiling the publish-rollback
# sibling suite uses for the same claim.
DROP=$((FREE_BEFORE - FREE_AFTER))
echo "  free after the sweep:  $FREE_AFTER blocks (drop $DROP)"
[ "$DROP" -le 8 ] ||
    fail "the sweep consumed $DROP blocks it did not give back (pre-fix: 80 on this corpus)"

# The stranded twin from the other side: new segments allocated and
# named by nothing. Post-fix the rollback frees them, so unclaimed must
# not move past the half display quantum (pre-fix: +0.3 MiB here).
UNC_AFTER=$(unclaimed_mib "$IMG")
[ -n "$UNC_AFTER" ] || fail "could not read unclaimed"
echo "  unclaimed after the sweep: $UNC_AFTER MiB (was $UNC_BEFORE)"
awk -v a="$UNC_AFTER" -v b="$UNC_BEFORE" 'BEGIN{exit !(a<=b+0.05)}' ||
    fail "unclaimed grew from $UNC_BEFORE MiB to $UNC_AFTER MiB (pre-fix: +0.3 on this corpus)"

echo "== the file survived the abandoned pass, byte for byte =="
# The superseded segments are freed only AFTER a successful publish now,
# so the old recipe is intact: this reads through it.
"$B/invf-cat" "$IMG" v.bin > "$WORK/v.out" 2>/dev/null || fail "invf-cat v.bin"
cmp "$REF/v.bin" "$WORK/v.out" || fail "v.bin is not byte-identical after the abandoned sweep"

echo "== fsck clean, verify deep 0 corrupt =="
"$B/invf-fsck" "$IMG" 2>/dev/null | tail -1 | grep -q "^OK$" || fail "fsck not clean"
"$B/invf-verify" "$IMG" --deep 2>&1 | grep -q "0 corrupt" ||
    fail "invf-verify --deep did not report 0 corrupt"

echo "== recovery: a clean re-sweep still works and stays exact =="
set +e
"$B/invf-sweep" "$IMG" >"$WORK/resweep.log" 2>&1
RC=$?
set -e
[ "$RC" -eq 0 ] || { cat "$WORK/resweep.log"; fail "recovery sweep rc=$RC (want 0)"; }
"$B/invf-cat" "$IMG" v.bin > "$WORK/v.out2" 2>/dev/null || fail "invf-cat v.bin after resweep"
cmp "$REF/v.bin" "$WORK/v.out2" || fail "v.bin is not byte-identical after the recovery sweep"
"$B/invf-fsck" "$IMG" 2>/dev/null | tail -1 | grep -q "^OK$" || fail "fsck not clean after resweep"

echo
echo "PUT FAILURE ROLLBACK: PASS (free $FREE_BEFORE -> $FREE_AFTER blocks, $RB)"
