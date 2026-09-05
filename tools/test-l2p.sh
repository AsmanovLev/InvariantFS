#!/bin/bash
# test-l2p.sh — WP-L2Q e2e: L2P session index + heat off the journal hot
# path (persistent regression).
#
# Leg 1 (stress): random MAP / re-MAP / UNMAP sequences with interleaved
#   flushes and close/reopen cycles; every probe compares vol_lookup_entry
#   (the session index) against an independent newest-wins linear scan of
#   the table. Two seeds, plus a run with INVFS_JRN_FORCE_COMPACT=1 (every
#   flush flips journal slots mid-stream) and one with INVFS_L2P_IDX=0
#   (the index disabled -> pure fallback path must still answer).
# Leg 2 (quiet): pure reads never touch the WAL -- the metadata prefix
#   (superblock + bitmap + journal) stays byte-identical while read heat
#   folds into the inode-area records at close (the WP27 persistence
#   point). A writing session MUST change the image (the leg can tell).
# Leg 3 (pump): read heat persists across sessions via the close fold.
#   4 pumped reads of one file -> rheat 4 after remount.
# Leg 4 (bench): a 50k-file volume full-tree read wall time (the WP27
#   read path is record-direct: no WAL consult to toggle, so the
#   INVFS_L2P_IDX=0 arm is a same-path control; the assertion is
#   anti-pathology, not a win).
#
# Run from the repo root after `make`:  bash tools/run-e2e.sh tools/test-l2p.sh
set -e
set -o pipefail

REPO=${REPO:-/home/user/InvariantFS}   # override with the worktree when testing a branch
B=$REPO/bin
WORK=/dev/shm/l2q
IMG=l2q_stress.img
IMGQ=l2q_quiet.img
IMGB=l2q_bench.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG" "$IMGQ" "$IMGB" "$IMGB".ref

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== leg 1: lookup-parity stress =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
$B/invf-l2ptest stress "$IMG" 30000 12345 || fail "stress seed 12345"
$B/invf-l2ptest stress "$IMG" 30000 777777 || fail "stress seed 777777"
INVFS_JRN_FORCE_COMPACT=1 $B/invf-l2ptest stress "$IMG" 30000 999 \
    || fail "stress under forced compaction"
INVFS_L2P_IDX=0 $B/invf-l2ptest stress "$IMG" 30000 4242 \
    || fail "stress with index disabled (fallback scan)"
echo "leg 1 OK"

echo "== leg 2: pure reads leave the image byte-identical =="
$B/invf-mkfs "$IMGQ" 0.2 >/dev/null
python3 - <<'PY'
import random
random.seed(4242)
for i in range(4):
    open("/dev/shm/l2q/q%d.txt" % i, "w").write(
        "quiet-read probe %d\n" % i * 1000)
PY
for i in 0 1 2 3; do
    $B/invf-cp "$IMGQ" "$WORK/q$i.txt" "q$i.txt" >/dev/null
done
# a control: the journal is slotted and quiet NOW (import flushed)
cp "$IMGQ" "$WORK/q.before"
# pure reads: 3 invf-cat sessions per file (each a full open+close) ...
for i in $(seq 3); do
    for f in q0.txt q1.txt q2.txt q3.txt; do
        $B/invf-cat "$IMGQ" "$f" >/dev/null
    done
done
# ... plus an explicit vol_flush inside a read-only session
$B/invf-l2ptest readflush "$IMGQ" q0.txt q1.txt q2.txt q3.txt >/dev/null
# WP27: the read path never consults the owner WAL, and heat folds only
# at the sweep's decay pass / an explicit vol_heat_persist -- so a pure
# read session writes NOTHING (the same quiet-image guarantee the WP-L2Q
# leg pinned, now for the whole image: journal AND inode area).
cmp "$IMGQ" "$WORK/q.before" \
    || fail "pure-read session changed the image (journal/superblock)"
echo "image byte-identical after pure reads + vol_flush"
# sanity: a writing session MUST change the image (the leg can tell)
echo change >> "$WORK/q0.txt"
$B/invf-cp "$IMGQ" "$WORK/q0.txt" q0.txt >/dev/null
if cmp -s "$IMGQ" "$WORK/q.before"; then
    fail "a writing session left the image byte-identical"
fi
echo "write session did change the image (control OK)"

echo "== leg 3: read heat persists via the close-fold (pump) =="
# 4 pumped reads (open + read + close per round): each close folds the
# session's accrued touch into the record's heat TLV. rheat must read 4
# after the remounts.
for i in $(seq 4); do
    $B/invf-l2ptest pump "$IMGQ" q1.txt >/dev/null
done
RH=$($B/meta_probe "$IMGQ" --heat q1.txt | grep "^heat " | sed "s/.*rheat=//" | awk '{print $1+0}')
[ "$RH" = "4" ] || fail "q1.txt rheat $RH != 4 after 4 pumped reads"
# a pure read-only probe accrues nothing (and writes nothing)
$B/meta_probe "$IMGQ" --heat q1.txt >/dev/null
RH=$($B/meta_probe "$IMGQ" --heat q1.txt | grep "^heat " | sed "s/.*rheat=//" | awk '{print $1+0}')
[ "$RH" = "4" ] || fail "probe run changed rheat ($RH != 4)"
echo "leg 3 OK (rheat=4 persisted through close-folds only)"

echo "== leg 4: read microbench (50k files) =="
INVFS_META_FRAC=32 $B/invf-mkfs "$IMGB" 1.5 >/dev/null
$B/invf-l2ptest mkfiles "$IMGB" 50000 2048
cp --sparse=always "$IMGB" "$IMGB".ref
# WP27: the read path resolves pbas from the records directly -- there is
# no per-read WAL consult to toggle, so the WP-L2Q-era indexed-vs-scan
# comparison is one path measured twice. Keep the numbers visible (the
# wall time IS the regression signal), with the session-index env arm
# kept as the no-op control.
echo "-- default (WP27: record-direct reads)"
T_IDX=$($B/invf-l2ptest readall "$IMGB" 50000 | tee /dev/stderr | \
        sed -n 's/.*loop_ms=\([0-9.]*\).*/\1/p')
cp "$IMGB".ref "$IMGB"   # same start state for every run
echo "-- control (INVFS_L2P_IDX=0, same binary)"
T_OFF=$(INVFS_L2P_IDX=0 $B/invf-l2ptest readall "$IMGB" 50000 | \
        tee /dev/stderr | sed -n 's/.*loop_ms=\([0-9.]*\).*/\1/p')
cp "$IMGB".ref "$IMGB"
echo "bench: readall=${T_IDX}ms control=${T_OFF}ms"
# both arms are the same v2 code path: assert no pathological divergence
# (3x) rather than a strict win
awk "BEGIN{exit !($T_IDX < $T_OFF * 3)}" \
    || fail "readall pathologies: ${T_IDX} ms vs ${T_OFF} ms"
echo "leg 4 OK"

echo "ALL L2P L2Q LEGS PASS"
