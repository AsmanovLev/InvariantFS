#!/bin/bash
# test-sweep-lane-retire.sh — WP612: sweep-lane superseded frees ride retire.
#
# WP607 moved the WRITE path's superseded frees to retire-on-fence (staged
# rows are not durable; a fence-failure doom that restores the old recipe
# while its blocks are already freed hands live bytes to the next
# allocator -- the s35/s23 use-after-free). The OFFLINE sweep lanes freed
# the same way in the same situation: six lane bodies through
# vol_release_superseded_blob, vol_publish_blob_inode / window_inode, the
# text/batch member commit, the generic-floor segment remap, the migrate
# move commit, and the dedupe loser loop -- every one of them after a
# vol_inode_delta_put whose fence may never land. They now retire instead,
# and the retire entries drain at the covering fence (group-commit barrier
# mid-pass, vol_close at the end).
#
# WHAT THIS ASSERTS (12-TAR supersede-heavy corpus + dedupe + batching):
#
#   1. sweep #1 completes; every file invf-cats byte-identical afterwards:
#      no live recipe dangles (a freed-too-early block reused by the sweep
#      itself would corrupt the member or the recipe that names it).
#      NOTE: sweep #1 is NOT expected to raise the free-block count. The
#      sweep captures an SPT0 savepoint (AGENTS.md 2.5) that pins every
#      block the pre-sweep generation named, so the superseded segments
#      stay allocated for one generation even though the retire drain
#      discharged them. Free after sweep #1 normally FALLS (new encoding
#      + metadata, old blocks pinned) -- measured -132 on this corpus.
#      The retire-drain proof lives in lane_release_test (close drains,
#      0 of 63 superseded blocks still allocated), not here.
#   2. fsck is clean after each sweep.
#   3. sweep #2 discharges the window: its capture drops sweep #1's
#      savepoint and reclaims what no live recipe names, so free must
#      RISE substantially (the documented one-sweep lag, AGENTS.md 2.5:
#      478 stranded, 456 reclaimed by the next bare sweep). If the
#      retire entries were dropped or the reclaim broke, nothing comes
#      back and this leg goes red.
#   4. unclaimed does not grow across the pair (no stranded allocation
#      beyond the discharged window), and sweep #2 never falls beyond
#      small metadata churn (no per-sweep leak).
#   5. the sweep log shows dedupe merges, so the suite cannot pass
#      vacuously on a build whose dedupe never ran (the dedupe loser loop
#      is one of the converted sites).
#
# Deliberately NOT asserted here: the flakey-device half (a doom between
# the staged publish and the fence must keep the blocks). That needs
# dm-flakey/dm-error injection and lives with test-flakey.sh /
# test-sweep-flushfail.sh, not in an offline accounting suite.
#
# Run:  bash tools/test-sweep-lane-retire.sh      (from make test)
#   or:  bash tools/run-e2e.sh tools/test-sweep-lane-retire.sh
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
WORK=${INVFS_SWEEP_RETIRE_WORK:-$(invfs_scratch_root)/sweep-retire-$$}
IMG=$WORK/vol.img
REF=$WORK/ref
LOG=$WORK/sweep.log
LOG2=$WORK/sweep2.log

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

echo "== fixtures: 12 TARs (shared members), text, dup binaries, small bin =="
python3 - "$REF" <<'PYEOF'
import os, sys, tarfile, io, random
REF = sys.argv[1]
random.seed(612)
WORDS = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta sweep lane retire fence\n')
# members shared across TARs: every TAR carries m1 (identical everywhere,
# so dedupe cross-merges the lane output too) plus two per-TAR members.
shared = (WORDS * 900)[:50000]
names = []
for t in range(12):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w') as tf:
        for n, d in (('m1.txt', shared),
                     ('m2.txt', (WORDS * (700 + t * 13))[:42000]),
                     ('m3.txt', (bytes([65 + ((t * 7) % 26)]) * 30000))):
            ti = tarfile.TarInfo(n)
            ti.size = len(d)
            tf.addfile(ti, io.BytesIO(d))
    fn = 'a%02d.tar' % t
    open(os.path.join(REF, fn), 'wb').write(buf.getvalue())
    names.append(fn)
# batching lane: 1 MiB of text (multi-slice member commit)
open(os.path.join(REF, 'big.txt'), 'wb').write((WORDS * 22000)[:1048576])
names.append('big.txt')
# dedupe + generic-floor remap: two identical 512 KiB random binaries
# (8 RAW segments each; the remap publishes a new recipe per file)
dup = random.randbytes(524288)
open(os.path.join(REF, 'dup1.bin'), 'wb').write(dup)
open(os.path.join(REF, 'dup2.bin'), 'wb').write(dup)
names += ['dup1.bin', 'dup2.bin']
# single-segment generic floor (vol_publish_blob_inode path)
open(os.path.join(REF, 'small.bin'), 'wb').write(random.randbytes(32768))
names.append('small.bin')
print("  staged %d files" % len(names))
PYEOF
[ -e "$REF/a00.tar" ] || fail "fixture generation failed"

echo "== mkfs + import =="
MKT=$("$B/invf-mkfs" "$IMG" 0.07 2>&1) || { echo "$MKT"; fail "invf-mkfs"; }
for f in "$REF"/*; do
    n=$(basename "$f")
    "$B/invf-cp" "$IMG" "$f" "$n" >/dev/null || fail "invf-cp $n"
done
FREE0=$(free_blocks "$IMG")
[ -n "$FREE0" ] || fail "could not read the free-block count"
UNC0=$(unclaimed_mib "$IMG")
echo "  free after import: $FREE0 blocks; unclaimed: ${UNC0:-0} MiB"

echo "== sweep #1: lanes supersede, retire drains at the fence =="
"$B/invf-sweep" "$IMG" > "$LOG" 2>&1 || { cat "$LOG"; fail "invf-sweep exited non-zero"; }
grep -q "sweep done" "$LOG" || grep -q "^sweep: " "$LOG" ||
    { cat "$LOG"; fail "sweep did not complete"; }
tail -2 "$LOG"
# the dedupe loser loop is a converted site: prove it ran.
grep -qE "merged=[1-9][0-9]*" "$LOG" ||
    { cat "$LOG"; fail "sweep ran no dedupe merges, so this suite proved nothing about the dedupe retire path"; }
grep -E "merged=[0-9]+" "$LOG" | tail -1 | sed 's/^/  /'
FREE1=$(free_blocks "$IMG")
[ -n "$FREE1" ] || fail "could not read the free-block count"
echo "  free after sweep #1: $FREE1 blocks (was $FREE0; the savepoint pin
  keeps the superseded blocks allocated for one generation, so no rise
  is expected here -- see the header)"

echo "== every file reads back byte-identical (no dangling live recipe) =="
for f in "$REF"/*; do
    n=$(basename "$f")
    "$B/invf-cat" "$IMG" "$n" > "$WORK/cat.out" 2>/dev/null ||
        fail "invf-cat $n (its recipe does not load)"
    cmp "$f" "$WORK/cat.out" || fail "$n is not byte-identical after sweep #1"
done
echo "  all $(ls "$REF" | wc -l) files bit-exact"

echo "== fsck clean after sweep #1 =="
"$B/invf-fsck" "$IMG" | tail -1 | grep -q "^OK$" || fail "fsck not clean"
UNC1=$(unclaimed_mib "$IMG")
echo "  unclaimed: ${UNC1:-0} MiB (was ${UNC0:-0})"

echo "== sweep #2: the savepoint window discharges, no per-sweep leak =="
"$B/invf-sweep" "$IMG" > "$LOG2" 2>&1 || { cat "$LOG2"; fail "second invf-sweep exited non-zero"; }
FREE2=$(free_blocks "$IMG")
[ -n "$FREE2" ] || fail "could not read the free-block count"
DROP=$((FREE1 - FREE2))
GAIN2=$((FREE2 - FREE1))
echo "  free after sweep #2: $FREE2 blocks (was $FREE1; discharge $GAIN2)"
# Sweep #2's capture drops sweep #1's savepoint window and reclaims what
# no live recipe still names: the superseded blocks must come back now.
# A fall beyond small metadata churn is a leak, and no rise at all means
# the window never discharged (dropped retires / broken reclaim).
[ "$DROP" -le 64 ] ||
    fail "sweep #2 lost $DROP blocks it did not give back (free $FREE1 -> $FREE2)"
[ "$GAIN2" -ge 100 ] ||
    fail "sweep #2 discharged only $GAIN2 blocks (free $FREE1 -> $FREE2); the one-generation window did not come back"
"$B/invf-fsck" "$IMG" | tail -1 | grep -q "^OK$" || fail "fsck not clean after sweep #2"
UNC2=$(unclaimed_mib "$IMG")
echo "  unclaimed: ${UNC2:-0} MiB (was ${UNC0:-0})"
awk -v a="${UNC2:-0}" -v b="${UNC0:-0}" 'BEGIN{exit !(a<=b)}' ||
    fail "unclaimed grew from $UNC0 MiB to $UNC2 MiB across the two sweeps"

echo
echo "SWEEP LANE RETIRE: PASS (free $FREE0 -> $FREE1 -> $FREE2 blocks)"
