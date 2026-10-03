#!/bin/bash
# test-sweep-publish-rollback.sh — a v3 sweep transform that CANNOT publish
# its new recipe must roll back what it wrote, not keep it.
#
# THE DEFECT THIS IS THE RED CONTROL FOR
#
# vol_sweep_one_v3's multi-segment path (src/core/vol_sweep.c) allocated and
# wrote every replacement segment BEFORE it had anywhere to name them.
# vol_v3_recipe_store is the single point at which the file's new recipe
# becomes reachable, and it is the one step in the pass that can fail for
# want of space. When it did, the function still returned `any_swept ? 1 : 0`
# — "swept" — and every block it had written stayed ALLOCATED and referenced
# by nothing. The superseded segments were freed even earlier, at the remap,
# so a failed publish also left the on-disk recipe naming free blocks.
#
# Measured, pre-fix, on a 0.5 GiB volume holding one 300 MB incompressible
# file (the leg-[C] shape of tools/test-writepath.sh):
#
#   after the write            free = 39042 blocks
#   after one invf-sweep       free =   241 blocks, unclaimed = 152.2 MiB
#   after three more sweeps    free =   241 blocks, unclaimed = 152.2 MiB
#   after invf-fsck -f         free =   241 blocks, unclaimed = 152.2 MiB
#
# 152.2 MiB of the volume was gone for good, and the next write was refused
# with ENOSPC on a volume that reported 1.1 MiB free. Nothing recovered it.
#
# WHAT THIS ASSERTS
#
#   1. the pass really reached the rollback (the sweep log must say so), so
#      the suite cannot pass vacuously on a build that never got that far;
#   2. the free-block count does not drop across the sweep beyond the small
#      metadata churn a sweep legitimately does — pre-fix it drops by 17
#      blocks per orphaned segment, hundreds at a time;
#   3. the file is still RAW and still reads back byte-identical: the old
#      recipe was never disturbed, which is the data-safety half of the
#      same bug;
#   4. fsck is clean, and invf-stats reports no unclaimed growth.
#
# Run:  bash tools/test-sweep-publish-rollback.sh      (from make test)
#   or:  bash tools/run-e2e.sh tools/test-sweep-publish-rollback.sh
#
# THIS SUITE WENT RED AGAIN, AND IT WAS RIGHT BOTH TIMES
#
# dabc5e6 ("volume: the v3 pba-ref map was built once per session, at open")
# rewrote the publish block of vol_sweep_one_v3 and mis-braced it: the `else`
# carrying the rollback bound to `vol_ast_recipe_serialize`, which essentially
# never fails, instead of to `vol_v3_recipe_store`, which is the step that
# fails for want of space. The rollback became unreachable. A failed publish
# fell out of the outer `if` having done nothing at all: any_swept stayed 1,
# the function returned "swept", and every block remap[] had recorded stayed
# allocated and named by nothing.
#
# Measured on this suite's own corpus, 36 files, identical volume both runs:
#
#                        free before -> after     unclaimed        outcome
#   48191ad (parent)     257 -> 315 (drop  -58)  0.4 -> 0.2 MiB   rolled back 221 / 13
#   c64382d (regressed)  257 ->  94 (drop +163)  0.4 -> 1.0 MiB   NOTHING
#
# The brief assumed the pba_ref rebuild dabc5e6 added was the new cost inside
# this measurement. It is not: instrumenting pba_ref_ensure with counters that
# accumulate in alloc_blocks/vol_free_blocks while a rebuild is on the stack
# gives 1 rebuild and **0 blocks allocated** for the whole pass. The rebuild is
# a walk of the live inode set -- reads and a calloc, no allocation. The 163
# were the abandoned transform's stranded segments, whole.
#
# So the assertion below is UNCHANGED. The measurement is not what moved; the
# rollback is what stopped happening. bin/invf-sweep_publish_rollback_test is
# the by-address form of the same claim (it resolves the stranded set to pbas)
# and is what a future regression should trip first.
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
WORK=${INVFS_SWEEP_RB_WORK:-$(invfs_scratch_root)/sweep-rb-$$}
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

echo "== build a volume and an incompressible multi-segment file =="
# 0.07 GiB. Small on purpose: this suite is about block accounting, not
# throughput, and every probe here is an O(volume) pass.
MKT=$("$B/invf-mkfs" "$IMG" 0.07 2>&1) || { echo "$MKT"; fail "invf-mkfs"; }
RAW_B=$(echo "$MKT" | sed -n 's/.*raw zone:.*(\([0-9][0-9]*\) blocks.*/\1/p')
SHADOW_B=$(echo "$MKT" | sed -n 's/.*shadow zone:.*(\([0-9][0-9]*\) blocks.*/\1/p')
[ -n "$RAW_B" ] && [ -n "$SHADOW_B" ] ||
    fail "could not read the zone geometry from invf-mkfs"
POOL=$((RAW_B + SHADOW_B))
echo "  data pool: $POOL blocks (raw $RAW_B + shadow $SHADOW_B)"

# Incompressible by construction: the sweep's generic floor cannot shrink it,
# so the per-segment remap path is the one that runs and the recipe publish
# is the step that has to fail. 1 MiB = 16 segments, 17 blocks each.
head -c 1048576 /dev/urandom > "$REF/t.bin"
"$B/invf-cp" "$IMG" "$REF/t.bin" t.bin >/dev/null || fail "invf-cp t.bin"

echo "== fill the volume to the point where the recipe cannot be published =="
# One bulk filler sized from the geometry, then a short bounded step loop to
# land on the target slack. The state the defect needs is: room to write
# replacement segments, no room to name them.
#
# The filler goes in 1 MiB pieces, not one huge file: vol_write_range's
# atomic ENOSPC precheck (src/core/vol_write.c) models a call's worst case as
# every touched segment stored verbatim, so one 38 MB write needs more blocks
# than the whole data pool has and is refused before a byte lands. A piece is
# 16 segments = 272 blocks of demand, which fits until the very end.
head -c 1048576 /dev/urandom > "$REF/piece.bin"
head -c 65536 /dev/urandom > "$REF/step.bin"
for n in $(seq 1 200); do
    F=$(free_blocks "$IMG")
    [ -n "$F" ] || fail "could not read the free-block count"
    [ "$F" -le 300 ] && break
    "$B/invf-cp" "$IMG" "$REF/piece.bin" "piece.$n" >>"$WORK/cp.log" 2>&1 || break
done
# The step loop runs until a write is REFUSED, not until a target is met:
# the refusal is the floor, and it is where the defect lives -- a file's
# per-segment remap can no longer complete, so the pass writes what it can
# and then has nowhere to name it.
for n in $(seq 1 200); do
    "$B/invf-cp" "$IMG" "$REF/step.bin" "step.$n" >>"$WORK/cp.log" 2>&1 || break
done
FREE_BEFORE=$(free_blocks "$IMG")
[ -n "$FREE_BEFORE" ] || fail "could not read the free-block count"
# 300 blocks is the floor this loop can reach from above: every write, even a
# 4 KiB one, is a whole 64 KiB segment and vol_write_range's atomic ENOSPC
# precheck demands a full 17-block slot for it, so the last successful write
# lands the volume just above reserved_blocks + hard_min + 17.
[ "$FREE_BEFORE" -le 300 ] || { tail -5 "$WORK/cp.log" >&2 2>/dev/null || true; }
[ "$FREE_BEFORE" -le 300 ] ||
    fail "could not fill the volume down to a publish-hostile slack (still $FREE_BEFORE free)"
UNC_BEFORE=$(unclaimed_mib "$IMG")
echo "  free before the sweep: $FREE_BEFORE blocks; unclaimed: ${UNC_BEFORE:-0} MiB"

echo "== sweep: the transform must not be able to strand what it writes =="
"$B/invf-sweep" "$IMG" > "$LOG" 2>&1 || { cat "$LOG"; fail "invf-sweep exited non-zero"; }
FREE_AFTER=$(free_blocks "$IMG")
[ -n "$FREE_AFTER" ] || fail "could not read the free-block count"
# The primary assertion, and the one that IS the defect: a sweep must not
# leave the volume smaller than it found it by more than the small metadata
# churn a sweep does on its own. Pre-fix this is
# `FREE_BEFORE - 17 * (segments rewritten)` -- hundreds of blocks that no
# later sweep and no invf-fsck -f ever gives back. 8 blocks is slack for the
# legitimate churn and a hard ceiling the pre-fix behaviour cannot reach.
DROP=$((FREE_BEFORE - FREE_AFTER))
echo "  free after the sweep:  $FREE_AFTER blocks (drop $DROP)"
[ "$DROP" -le 8 ] ||
    fail "the sweep consumed $DROP blocks it did not give back (pre-fix: 17 per orphaned segment)"

# And the pass must SAY it rolled back, so the suite cannot pass vacuously
# on a build that simply never got into the state.
grep -q "rolled back" "$LOG" ||
    { cat "$LOG"; fail "the sweep never reached the publish-rollback path, so this suite proved nothing"; }
grep "rolled back" "$LOG" | sed 's/^/  /'

# And the fingerprint, in the end-to-end suite too: the rollback hands back
# exactly 17 physical blocks per rewritten segment (a 64 KiB segment is 17
# 4 KiB blocks with its 8-byte header). It is the "17 per orphaned segment"
# the failure message above names, INVERTED -- the number GIVEN BACK rather
# than the number stranded. dabc5e6 mis-braced the publish block so the
# rollback's `else` bound to the serialize test instead of the store test,
# which made the whole branch unreachable: this suite went red with
# "drop 163" and the sweep log had no "rolled back" line in it at all.
RB=$(grep -m1 -o 'rolled back [0-9]* rewritten block(s) across [0-9]* segment(s)' "$LOG")
RB_BLOCKS=$(echo "$RB" | sed -n 's/rolled back \([0-9]*\) .*/\1/p')
RB_SEGS=$(echo "$RB" | sed -n 's/.*across \([0-9]*\) .*/\1/p')
[ -n "$RB_BLOCKS" ] && [ -n "$RB_SEGS" ] &&
    [ "$RB_SEGS" -ge 1 ] ||
    fail "could not read the rollback's block/segment counts out of the sweep log"
echo "  rolled back: $RB_BLOCKS block(s) across $RB_SEGS segment(s)"
[ "$RB_BLOCKS" -eq $((17 * RB_SEGS)) ] ||
    fail "the rollback gave back $RB_BLOCKS blocks for $RB_SEGS orphaned segment(s); it owes exactly 17 each"

echo "== the file survived the abandoned pass, byte for byte =="
# The superseded segments are freed only AFTER a successful publish now, so
# the old recipe is intact: this reads through it.
"$B/invf-cat" "$IMG" t.bin > "$WORK/t.out" 2>/dev/null || fail "invf-cat t.bin"
cmp "$REF/t.bin" "$WORK/t.out" || fail "t.bin is not byte-identical after the abandoned sweep"

echo "== fsck clean, and no unclaimed growth =="
"$B/invf-fsck" "$IMG" | tail -1 | grep -q "^OK$" || fail "fsck not clean"
UNC_AFTER=$(unclaimed_mib "$IMG")
echo "  unclaimed after the sweep: ${UNC_AFTER:-0} MiB (was ${UNC_BEFORE:-0})"
awk -v a="${UNC_AFTER:-0}" -v b="${UNC_BEFORE:-0}" 'BEGIN{exit !(a<=b)}' ||
    fail "unclaimed grew from $UNC_BEFORE MiB to $UNC_AFTER MiB"

echo
echo "SWEEP PUBLISH ROLLBACK: PASS (free $FREE_BEFORE -> $FREE_AFTER blocks)"
