#!/bin/bash
# test-v3-batch-owner.sh — the v3 batch REGISTRY is a block owner.
#
# Red control for the recipe-blob loss: a sweep could free a block that the
# v3 batch registry still owned, the block was handed straight back to the
# shared free pool, and the same sweep's stage-6 tz_v3_gc then freed it a
# SECOND time through the row it never dropped -- by then as a live base
# B+-tree page. The fold after it rebuilt the base from the pre-transform
# root, and a file written seconds earlier became unreadable ("recipe blob
# missing/corrupt"). End-to-end repro:
#
#   python3 tools/fuzz/opseq.py --seed 0x5e9 --only-image 1 --ops 81
#
#   STEP realize rc=0
#   STEP POST-realize verify rc=1        <-- the sweep just wrote the volume away
#
# The window is INTRA-SWEEP -- stage 1 frees the block, stage 6 frees it
# again -- so an outside observer cannot see it: after the sweep returns,
# both the broken and the fixed tree have the block free and the row gone.
# That is why leg 2 calls the sweep's own prepare (spt0_drop +
# spt0_capture, tools/invf-sweep.c) and looks at the allocation bitmap
# between the two frees. Everything else here is the ordinary CLI.
#
#   leg 0  build the state: a text batch that is LIVE and PINNED by a live
#          save point, so the next capture's reclaim has it in its worklist.
#   leg 1  the reclaim must not free a block the registry still owns.
#          THIS is the red control: without the fix it reports
#          RECLAIMED_DEAD>0 and names the blocks.
#   leg 2  the same, driven through the REAL `invf-sweep` binary, twice,
#          with a full bit-exact readback after each: the fix must not cost
#          correctness, and the dead batch must still be reclaimed (a fix
#          that simply stopped reclaiming would pass leg 1 and leak).
#   leg 3  the dead batch IS reclaimed: the registry loses its row and the
#          blocks come back, one generation later, which is the whole point
#          of skipping it in the reclaim.
#
# Run:  bash tools/run-e2e.sh tools/test-v3-batch-owner.sh
#   or:  bash tools/test-v3-batch-owner.sh     (from make test)
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
T=$B/invf-batch_owner_test
WORK=${INVFS_BATCH_OWNER_WORK:-/dev/shm/v3batchowner}
NFILES=6
NSWEEP=2

rm -rf "$WORK" && mkdir -p "$WORK" || exit 1
cd "$WORK" || exit 1

fail() { echo "FAIL: $*" >&2; exit 1; }
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

if [ ! -x "$T" ]; then
    make -C "$REPO" bin/invf-batch_owner_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-batch_owner_test"
fi
for b in invf-mkfs invf-cp invf-sweep invf-verify invf-fsck; do
    [ -x "$B/$b" ] || fail "bin/$b missing (run make)"
done

# Deterministic text: long enough that the text lane batches it, varied
# enough that the six files are six distinct segments.
gen() {  # gen <tag> <words> <outfile>
    python3 -c "
import sys
tag, n = sys.argv[1], int(sys.argv[2])
sys.stdout.write(''.join('%s%d ' % (tag, i % 97) for i in range(n)))
" "$1" "$2" > "$3"
}
for i in $(seq 1 $NFILES); do gen "alpha$i " 4000 "orig$i.txt"; done

sweep() {  # sweep <img> -- the offline sweep, quietly; keeps a running log
    timeout 300 "$B/invf-sweep" "$1" >"$1.sweep.log" 2>&1 \
        || { cat "$1.sweep.log"; fail "sweep failed on $1"; }
    cat "$1.sweep.log" >> "$1.sweep.all.log"
}

echo "== leg 0: build a LIVE, PINNED text batch =="
rm -f m.img
INVFS_V3=1 "$B/invf-mkfs" m.img 0.3 >m.img.mkfs 2>&1 \
    || { cat m.img.mkfs; fail "mkfs failed"; }
for i in $(seq 1 $NFILES); do
    "$B/invf-cp" m.img "orig$i.txt" "a$i.txt" >/dev/null 2>&1 \
        || fail "cp a$i.txt failed"
done
# Sweep 1 batches them. The capture happens in prepare, BEFORE the batch
# exists, so the first pin cannot hold the batch segment -- which is why a
# second sweep is needed and why the repro needs a save point from an
# EARLIER generation.
sweep m.img
grep -q "text batches flushed" m.img.sweep.log \
    || { cat m.img.sweep.log; fail "sweep 1 did not batch the text files; \
the state under test was never built"; }
# Sweep 2 captures a pin that DOES name the batch, because now the members'
# recipes point at it.
sweep m.img
OUT=$("$T" rows m.img) || { echo "$OUT"; fail "rows failed"; }
echo "$OUT"
ROWS=$(field "$OUT" REGISTRY_ROWS)
BLK=$(field "$OUT" BLOCKS)
FREE=$(field "$OUT" FREE)
[ -n "$ROWS" ] || fail "the driver printed no REGISTRY_ROWS"
[ "$ROWS" -ge 1 ] || fail "the batch registry is empty; legs 1-3 would be vacuous"
[ "$FREE" = "0" ] || fail "$FREE registry block(s) are already free before the capture"
echo "  registry: $ROWS row(s), $BLK block(s), all allocated"
[ -s m.img ] || fail "no image"

echo "== leg 1: the reclaim must not free a block the registry owns =="
# The batch has to be DEAD for the reclaim to even consider its block: no
# live recipe may name it. Rewriting every member does that, and leaves the
# registry row in place -- the exact window the bug lived in.
for i in $(seq 1 $NFILES); do
    gen "omega$i " 3000 "new$i.txt"
    "$B/invf-cp" m.img "new$i.txt" "a$i.txt" >/dev/null 2>&1 \
        || fail "rewrite of a$i.txt failed"
done
OUT=$("$T" capture m.img) 2>&1
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] \
    || { cat <<'EOF'
FAIL: the savepoint reclaim freed a block the v3 batch registry still owns.

That is the recipe-blob loss. The registry row is now a stale pointer into
the free pool; the next base-page allocation may take the block, and this
sweep's own stage-6 tz_v3_gc will then free that LIVE page through the row.
EOF
        exit 1; }
DEAD=$(field "$OUT" RECLAIMED_DEAD)
[ -n "$DEAD" ] || fail "the driver printed no RECLAIMED_DEAD count"
[ "$DEAD" = "0" ] || fail "the driver reported $DEAD freed block(s) yet exited 0"
echo "  reclaim freed none of the registry's $BLK block(s)"

echo "== leg 2: the real sweep, twice, with a bit-exact readback =="
sweep m.img
"$B/invf-verify" m.img --deep >v1.log 2>&1 \
    || { cat v1.log; fail "verify --deep failed after the first real sweep"; }
grep -q "0 corrupt" v1.log || { cat v1.log; fail "corruption after sweep 1"; }
sed -n '/files ok/p' v1.log
sweep m.img
"$B/invf-verify" m.img --deep >v2.log 2>&1 \
    || { cat v2.log; fail "verify --deep failed after the second real sweep"; }
grep -q "0 corrupt" v2.log || { cat v2.log; fail "corruption after sweep 2"; }
sed -n '/files ok/p' v2.log
# Bit-exact, through the public reader, not just "fsck says the tree is fine".
for i in $(seq 1 $NFILES); do
    "$B/invf-cat" m.img "a$i.txt" "out$i.txt" >/dev/null 2>&1 \
        || fail "invf-cat a$i.txt failed"
    cmp -s "new$i.txt" "out$i.txt" \
        || fail "a$i.txt did not come back byte-identical"
done
echo "  $NFILES files byte-identical after two sweeps"
FSCK=$("$B/invf-fsck" m.img 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }

echo "== leg 3: the dead batch is still reclaimed, one generation later =="
# The fix defers the batch's blocks to tz_v3_gc instead of never freeing
# them. A "fix" that only added a veto would pass leg 1 and leak forever,
# so this leg is what tells the two apart.
OUT=$("$T" rows m.img) || { echo "$OUT"; fail "rows failed after the sweeps"; }
echo "$OUT"
AFTER=$(field "$OUT" REGISTRY_ROWS)
[ "$AFTER" -lt "$ROWS" ] \
    || fail "the registry still holds $AFTER row(s) (was $ROWS): the dead \
batch's row was never retired, so its blocks are leaked"
"$B/invf-sweep" m.img >m.img.sweep3.log 2>&1 \
    || { cat m.img.sweep3.log; fail "the third sweep failed"; }
grep -q "text gc: [1-9]" m.img.sweep.all.log \
    || { sed -n '/text gc/p' m.img.sweep.all.log; fail "no sweep in this \
run reclaimed a dead batch; the blocks leg 1 declined to free would be \
leaked forever"; }
sed -n '/text gc:/p;/reclaim:/p' m.img.sweep.all.log
echo "  dead batch reclaimed and its row retired"

echo "PASS: the v3 batch registry is honoured as a block owner"
exit 0
