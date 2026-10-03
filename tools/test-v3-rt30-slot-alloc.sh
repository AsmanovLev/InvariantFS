#!/bin/bash
# test-v3-rt30-slot-alloc.sh — WP123: the RT30 READER must enforce the
# allocation invariant, not just the page-integrity one.
#
# RT30 is a double-slot root descriptor (WP86): mbuf_root_publish writes one
# slot per publish, so the other keeps naming the previous root, and
# mbuf_root_read falls back to it when the newest root page fails
# mbuf_page_validate. That fallback is only sound while the page it falls
# back to is still ALLOCATED -- and mbuf_page_validate checks magic + CRC32C
# and nothing else. A freed-but-not-yet-reused page validates perfectly.
#
# Every leg here is built so it CAN FAIL:
#
#   leg 0  build: content written through the public write path, folded into
#          the base, then forced base generations. Asserts RT30 names two
#          DISTINCT roots and that the reader adopted an ALLOCATED one --
#          otherwise every later leg is vacuous.
#   leg 1  SAFE baseline: probe the untouched volume in 'safe' mode.
#   leg 2  HAZARD, via the reclaimer primitive: free the root the reader
#          adopts with the page bytes left intact (a freed page still
#          validates), then probe in 'safe' mode. On a reader that consults
#          the bitmap this refuses; on the pre-fix reader it adopts a freed
#          block. This is the case WP86's design intent does not cover.
#   leg 3  HAZARD, no damage at all: after leg 2, hand blocks to mbuf_alloc
#          and see whether the allocator re-hands a block an RT30 slot still
#          names. REUSED > 0 means the descriptor points into the free pool,
#          so the next reader that trusts it adopts somebody else's page.
#   leg 4  HAZARD, through a SHIPPED path: damage the newest root so the
#          reader falls back, damage a non-root page of the fallback tree so
#          invf-fsck has something to quarantine, and let invf-fsck's
#          reachability diff (vol_fsck.c:1306) run. That diff is NOT guarded
#          against the second RT30 slot, so it frees the fallback root out
#          from under the descriptor. Then probe.
#   leg 5  fsck + readback: the volume must still open, its root slot must
#          still name an allocated block, and a census of every file name must
#          come back complete with no file that IS readable being wrong. A
#          reader that silently adopted a stale root would still pass
#          invf-fsck -- that is the point of this suite, so the leg states it.
#          MEASURED: on fsck.img the survivor count is ZERO -- leg 4's damage
#          takes the dirent page, so the repair legitimately destroys every
#          name and there is nothing left to compare. The old header claimed
#          "every file must still read back byte-identical" here; that was not
#          constructible, and nothing noticed, because nothing read a byte.
#          The STRICT bit-exactness leg is therefore the one on good.img, where
#          all 40 files must be present and byte-identical.
#
# BOTH directions are permanent. Leg 2 asserts the check REJECTS a freed
# root; the red control beside it asserts the hazard is ABSENT. To
# re-observe the original pre-fix behaviour, run `probe <img> hazard` by hand
# against a pre-WP123 binary -- it exits 0 only when the reader adopts the
# freed block, which is exactly the bug.
#
# Run:  bash tools/run-e2e.sh tools/test-v3-rt30-slot-alloc.sh
#   or:  bash tools/test-v3-rt30-slot-alloc.sh     (from make test)
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# WP205: one scratch-root answer for the whole suite set (see
# tools/lib-scratch.sh). The default used to name the author's bench disk,
# which no other host has.
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
T=$B/invf-rt30_slot_test
# WP123-FIX: a single fixed scratch made this suite collide with itself.
# Every worktree's `make test` starts with `rm -rf "$WORK"`, so two agents
# running `make test` concurrently deleted each other's images and reported
# it as an engine failure -- with a DIFFERENT leg failing each run (observed
# leg 4, then leg 1, then leg 2), which is the signature of the collision
# rather than of any one bug. Default to a per-process directory; the
# explicit override is still honoured, and each run cleans up only its own.
WORK=${INVFS_RT30_SLOT_WORK:-$(invfs_scratch_root)/rt30-slot-$$}
NFILES=40
NGENS=60

rm -rf "$WORK" && mkdir -p "$WORK" || exit 1
cd "$WORK" || exit 1

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$T" ] || make -C "$REPO" bin/invf-rt30_slot_test >/dev/null 2>&1 \
    || fail "cannot build bin/invf-rt30_slot_test"
[ -x "$B/invf-fsck" ] || fail "bin/invf-fsck missing (run make)"

mkvol() {  # mkvol <img>
    rm -f "$1"
    INVFS_V3=1 "$B/invf-mkfs" "$1" 0.3 >"$1.mkfs" 2>&1 \
        || { cat "$1.mkfs"; fail "mkfs failed for $1"; }
}

field() {  # field <line> <KEY>
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

build() {  # build <img>
    mkvol "$1"
    OUT=$("$T" build "$1" "$NFILES" "$NGENS") || { echo "$OUT"; fail "build failed for $1"; }
    echo "$OUT"
}

echo "== leg 0: build a multi-generation v3 volume =="
OUT=$(build good.img)
echo "$OUT"
S0=$(field "$OUT" SLOT0); S1=$(field "$OUT" SLOT1)
[ -n "$S0" ] && [ -n "$S1" ] || fail "build did not report both RT30 slots"
P0=${S0%%:*}; P1=${S1%%:*}
[ "$P0" != "$P1" ] || fail "RT30 names the same root in both slots; the double-slot construction is vacuous"
AA=$(field "$OUT" ADOPTED_ALLOC)
[ "$AA" = "1" ] || fail "the reader did not adopt an ALLOCATED root on a clean volume; the baseline is wrong"

echo "== leg 1: SAFE baseline (untouched volume) =="
OUT=$("$T" probe good.img safe) || { echo "$OUT"; fail "leg 1: clean volume failed the safe probe"; }
echo "$OUT"

echo "== leg 2: freed-but-intact root page -- the check must REJECT it =="
OUT=$(build freed.img) || { echo "$OUT"; fail "leg 2 build failed"; }
OUT=$("$T" free-adopted freed.img) || { echo "$OUT"; fail "leg 2: cannot construct the freed-root case"; }
echo "$OUT"
grep -q '^FREED=' <<<"$OUT" || fail "free-adopted did not report a freed root"
OUT=$("$T" probe freed.img safe) || { echo "$OUT"; fail "leg 2: the reader adopted a FREED root"; }
echo "$OUT"
# DIRECTION 2, the permanent red control. `probe ... hazard` ASSERTS the
# pre-fix behaviour (the reader adopts the freed slot), so on a fixed build
# it must FAIL. This leg re-runs the identical construction and requires the
# hazard to be ABSENT, which is what stops leg 2 passing vacuously: a future
# change that quietly drops the allocation check makes this leg fail. To
# re-observe the original hazard, check out the pre-fix binary and run
# RT30_SLOT_SHOW_HAZARD=1, or just run `probe freed.img hazard` by hand.
if "$T" probe freed.img hazard >hazard.out 2>&1; then
    cat hazard.out
    fail "leg 2 red control: the reader STILL adopts a freed root -- the allocation check is not in effect"
fi
echo "  red control: the same construction no longer reproduces the hazard"
sed 's/^/  /' hazard.out | tail -2

echo "== leg 3: does free/reuse churn alone carry a slot's block? (no damage) =="
# The sharpest form of the hazard: the allocator re-hands the exact block a
# slot names, and mbuf_alloc writes it as a fresh valid BPG3 leaf at a HIGHER
# gen, so the reader adopts a foreign page with no fallback and no torn
# write anywhere. This leg asserts the churn happens; leg 2's red control
# asserts the reader no longer cares.
OUT=$("$T" reuse freed.img 400) || { echo "$OUT"; fail "leg 3: reuse probe failed"; }
echo "$OUT"
grep -q '^REUSE=confirmed' <<<"$OUT" || fail "leg 3: the allocator did not re-hand a "\
"slot-named block, so this build cannot demonstrate the churn form of the hazard"

echo "== leg 4: a SHIPPED path must not free a block a live slot names =="
# Recorded as a NEGATIVE result, deliberately kept in the suite. invf-fsck
# -f reaches vol_reclaim_mark_and_free(vol_fsck.c:1306) with the fallback
# root still named by the other slot, and it does NOT consult the slots the
# way vol_fold.c:277 does -- yet it does not reproduce, because
# mbuf_root_publish writes slot seq&1 and then bumps seq, so the next
# publish always targets the slot the reader is not on and consumes the
# fallback's name before the diff runs. If a future change breaks that
# alternation, this leg is the one that catches it.
OUT=$(build fsck.img) || { echo "$OUT"; fail "leg 4 build failed"; }
OUT=$("$T" damage-newest fsck.img) || { echo "$OUT"; fail "leg 4: damage-newest failed"; }
echo "$OUT"
OUT=$("$T" damage-tree-page fsck.img) || { echo "$OUT"; fail "leg 4: damage-tree-page failed"; }
echo "$OUT"
"$B/invf-fsck" -f fsck.img >fsck.out 2>&1
grep -q 'excised' fsck.out || { cat fsck.out; fail "leg 4: invf-fsck excised nothing, so the reachability diff never ran and this leg is vacuous"; }
echo "  invf-fsck repaired; the reachability diff ran"
OUT=$("$T" probe fsck.img safe) || { echo "$OUT"; fail "leg 4: invf-fsck left a slot naming a freed block"; }
echo "$OUT"

echo "== leg 5: the volume must still open and read back =="
# A reader that silently adopted a stale root passes invf-fsck, so this leg
# is stated explicitly rather than left implicit.
"$B/invf-fsck" good.img >fsck2.out 2>&1 || { cat fsck2.out; fail "leg 5: invf-fsck reported damage in an UNTOUCHED volume"; }
# The cleanliness assertion above runs against good.img, NOT fsck.img. fsck.img
# had its newest root torn AND a non-root tree page destroyed by leg 4, and the
# repair excised the damage -- so it legitimately still carries damage this test
# caused on purpose: excised dirent pages leave live inodes that no directory
# entry names, and invf-fsck's exit code never softens that (it reports DEGRADED
# with the reason rather than OK). Asserting "clean" there asserted that
# deliberate destruction was repairable, a much stronger claim than this WP
# makes. What leg 5 is for is the READER, asserted on both volumes below.
"$B/invf-fsck" fsck.img >fsck3.out 2>&1
OUT=$("$T" probe fsck.img safe) || { echo "$OUT"; cat fsck3.out; fail "leg 5: the repaired volume's root no longer reads"; }
echo "  the repaired volume opens and its root reads (its damage is deliberate and expected)"
OUT=$("$T" probe good.img safe) || { echo "$OUT"; fail "leg 5: the untouched volume regressed"; }
echo "$OUT"

# `probe ... safe` answers exactly one question -- is the root slot's block
# allocated -- and the suite header's leg 5 claims "every file must still read
# back byte-identical". No byte of any file was read anywhere in this file, so
# that claim had nothing behind it: a repair that took content with it leaves a
# structurally perfect volume and `probe safe` passes. This is btree_repair_test
# reproduced in a shell suite -- the repair verified with the repairer's own
# criteria -- so leg 5 now asks a different tool, invf-cat's read path, on the
# far side of the operation.
#
# MEASURED, and it is worth stating plainly rather than papering over: on
# fsck.img, `verify ... tolerant` reports PRESENT=0 ABSENT=40. Leg 4 tears the
# newest root and then a non-root page, and `invf-fsck -f` excises the damage;
# the page that goes carries the directory entries, so every name is gone and
# there is no file left to compare. This suite therefore CANNOT assert
# bit-exactness across its own repair -- not because the assertion was written
# badly, but because the repair legitimately destroys the corpus it would be
# compared against. The header's old claim that "every file must still read
# back byte-identical" was not constructible here, and nothing in the file
# noticed, because nothing read a byte.
#
# So: the census is asserted (bad == 0, every name accounted for), the survivor
# count is printed, and the actual bit-exactness leg is the STRICT one on
# good.img, where all 40 files must be present and byte-identical. If leg 4's
# damage ever stops eating the dirent page, PRESENT rises above 0 and the
# tolerant run starts carrying real weight on its own -- with the count on
# screen either way, so nobody has to guess which happened.
OUT=$("$T" verify fsck.img "$NFILES" census) || { echo "$OUT"; \
    fail "leg 5: a file that WAS readable on the repaired volume did not read back byte-identical"; }
echo "  repaired volume census: $OUT"
case "$OUT" in
    *"PRESENT=0 "*) echo "  NOTE: leg 4's damage took every directory entry, so"
                   echo "        no file survives to compare on fsck.img. The"
                   echo "        bit-exactness leg is the strict one below." ;;
esac
OUT=$("$T" verify good.img "$NFILES" strict) || { echo "$OUT"; \
    fail "leg 5: a file on the UNTOUCHED volume did not read back byte-identical"; }
echo "  untouched volume readback: $OUT"

echo
echo "PASS: the reader refuses a root slot whose block is not allocated, and"
echo "      the red control confirms the hazard no longer reproduces."
