#!/bin/bash
# test-dedupe.sh — WP12(h) offline per-segment dedupe e2e (persistent regression).
#
# Leg 1 (merge correctness):
#   mkfs -> tree with known SEGMENT-aligned duplicates -> sweep (runs the
#   dedupe pass between the walk and the text GC) -> verify --deep ->
#   invf-cat bit-exact vs originals -> assert merged/freed counts AND that
#   the free-block delta equals the freed-blocks report -> second sweep
#   idempotent (0 new merges) -> fsck clean.
# Leg 2 (TEXT safety):
#   a volume of batched text files (including two identical texts, which
#   also exercises the deferred-candidate skip: dedupe runs before
#   vol_tz_flush seals their batch) — dedupe must not touch zone==TEXT
#   entries (hashed count = only the non-TEXT segments), members stay
#   bit-exact, fsck clean.
#
# NOTE on fixture geometry: dedupe operates at the engine's 64 KB SEGMENT
# granularity (one L2P entry per segment), so a shared span only merges
# when it lands on the 64 KB segment grid in both files. The two "shared
# middle" files are therefore 164 KB (64 KB head + 64 KB shared middle +
# 36 KB tail), not 100 KB — a non-aligned middle would share zero segments
# by construction.
#
# Run from the repo root after `make`:  bash tools/test-dedupe.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp12dedupe
IMG=wp12dedupe.img
IMGT=wp12dedupe-tz.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGT"

free_blocks() { $B/invf-fsck "$1" | awk '/free blocks:/ {print $3}'; }

echo "== generate tree (leg 1) =="
python3 - <<'PY'
import os, random
random.seed(4242)
d = "/dev/shm/wp12dedupe/orig"
K = 1024
SHARED = random.randbytes(64*K)          # the shared middle
def w(name, data): open(os.path.join(d, name), "wb").write(data)
# two 164KB files sharing a 64KB middle at offset 65536 (segment grid)
w("a1.bin", random.randbytes(64*K) + SHARED + random.randbytes(36*K))
w("a2.bin", random.randbytes(64*K) + SHARED + random.randbytes(36*K))
# one fully duplicated file (150KB = 3 segments), under two names
D = random.randbytes(150*K)
w("dup1.bin", D)
w("dup2.bin", D)
# one unique file
w("uniq.bin", random.randbytes(96*K))
for f in sorted(os.listdir(d)):
    print(" ", f, os.path.getsize(os.path.join(d, f)), "bytes")
PY

FILES=$(cd "$WORK/orig" && ls)
echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
FREE0=$(free_blocks "$IMG")
echo "free blocks after import: $FREE0"

echo "== sweep #1 (walk -> dedupe -> GC -> flush) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
DEDUP_LINE=$(grep "dedupe: merged" "$WORK/sweep1.log" || true)
echo "${DEDUP_LINE:-FAIL: no dedupe line}"
[ -n "$DEDUP_LINE" ] || { echo "FAIL: sweep ran no dedupe pass"; exit 1; }
MERGED=$(echo "$DEDUP_LINE" | awk '{print $3}')
FREED=$(echo "$DEDUP_LINE" | awk '{print $6}')
# 1 shared middle segment (a1/a2) + 3 fully-shared segments (dup1/dup2)
[ "$MERGED" = "4" ] || { echo "FAIL: expected 4 merged segments, got $MERGED"; exit 1; }
# merged blocks must be back in the bitmap: free-after == free-before + freed
# WP21: the sweep held its frees in the retention registry (the live
# checkpoint); realize it first so the bitmap reflects the dedupe (the
# no-op re-sweep inside --realize leaves no new checkpoint behind).
$B/invf-sweep "$IMG" --realize >> "$WORK/sweep1.log" 2>&1 || {
    cat "$WORK/sweep1.log"; exit 1; }
FREE1=$(free_blocks "$IMG")
echo "free blocks after sweep: $FREE1 (freed by dedupe: $FREED)"
[ "$((FREE0 + FREED))" = "$FREE1" ] || {
    echo "FAIL: free-block delta $((FREE1 - FREE0)) != freed $FREED"; exit 1; }
echo "bitmap cross-check OK: +$FREED blocks returned"

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== cat bit-exact =="
ok=1
for f in $FILES; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$FILES" | wc -w) files bit-exact"

echo "== sweep #2 (idempotent) =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
DEDUP2=$(grep "dedupe: merged" "$WORK/sweep2.log" || true)
echo "${DEDUP2:-FAIL: no dedupe line on sweep #2}"
echo "$DEDUP2" | grep -q "merged 0 segments, freed 0 blocks" || {
    echo "FAIL: second sweep merged again"; exit 1; }

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck1.log"
grep -q "orphans:      0" "$WORK/fsck1.log" || { echo "FAIL: orphans"; exit 1; }
grep -q "missing:      0" "$WORK/fsck1.log" || { echo "FAIL: missing"; exit 1; }

echo
echo "== leg 2: TEXT-safety =="
python3 - <<'PY'
import os, random
random.seed(777)
d = "/dev/shm/wp12dedupe/orig2"
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def textfile(name, size):
    out, n = [], 0
    while n < size:
        w = random.choice(WORDS)
        out.append(w)
        n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
textfile("one.txt", 40_000)
T = open(os.path.join(d, "one.txt")).read()   # two identical texts
open(os.path.join(d, "two.txt", ), "w").write(T)
textfile("notes.md", 25_000)
open(os.path.join(d, "blob.bin"), "wb").write(random.randbytes(96*1024))
for f in sorted(os.listdir(d)):
    print(" ", f, os.path.getsize(os.path.join(d, f)), "bytes")
PY

$B/invf-mkfs "$IMGT" 0.5 >/dev/null
TFILES=$(cd "$WORK/orig2" && ls)
for f in $TFILES; do
    $B/invf-cp "$IMGT" "$WORK/orig2/$f" "$f" >/dev/null
done

echo "== sweep (texts batch; dedupe must skip TEXT + deferred) =="
$B/invf-sweep "$IMGT" > "$WORK/sweept1.log" 2>&1 || { cat "$WORK/sweept1.log"; exit 1; }
TZL=$(grep -c "text -> PPMd batch" "$WORK/sweept1.log" || true)
echo "text->PPMd lines: $TZL"
[ "$TZL" -ge 3 ] || { echo "FAIL: expected >=3 deferred texts"; exit 1; }
DEDUPT=$(grep "dedupe: " "$WORK/sweept1.log" || true)
echo "$DEDUPT"
# only blob.bin is non-TEXT content: 2 segments hashed, nothing merged.
# (Without the deferred-candidate skip, the two identical texts WOULD
#  merge here and the flush's retire would corrupt the canonical copy.)
echo "$DEDUPT" | grep -q "hashed 2 live segments" || {
    echo "FAIL: dedupe hashed more than the 2 non-TEXT segments"; exit 1; }
echo "$DEDUPT" | grep -q "merged 0 segments, freed 0 blocks" || {
    echo "FAIL: dedupe touched deferred/TEXT content"; exit 1; }

echo "== members bit-exact + stats + verify =="
ok=1
for f in $TFILES; do
    $B/invf-cat "$IMGT" "$f" "$WORK/out/$f.t" >/dev/null
    cmp -s "$WORK/orig2/$f" "$WORK/out/$f.t" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$TFILES" | wc -w) files bit-exact"
$B/invf-stats "$IMGT" | grep "TEXT  :" || { echo "FAIL: no TEXT stats line"; exit 1; }
$B/invf-verify "$IMGT" --deep | tail -1

echo "== re-sweep (idempotent; TEXT entries never hashed) =="
$B/invf-sweep "$IMGT" > "$WORK/sweept2.log" 2>&1 || { cat "$WORK/sweept2.log"; exit 1; }
DEDUPT2=$(grep "dedupe: merged" "$WORK/sweept2.log" || true)
echo "${DEDUPT2:-FAIL: no dedupe line on text re-sweep}"
echo "$DEDUPT2" | grep -q "merged 0 segments, freed 0 blocks" || {
    echo "FAIL: text re-sweep merged"; exit 1; }
ok=1
for f in $TFILES; do
    $B/invf-cat "$IMGT" "$f" "$WORK/out/$f.t2" >/dev/null
    cmp -s "$WORK/orig2/$f" "$WORK/out/$f.t2" || { echo "MISMATCH2 $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1

echo "== fsck (text volume) =="
$B/invf-fsck "$IMGT" | tee "$WORK/fsck2.log"
grep -q "orphans:      0" "$WORK/fsck2.log" || { echo "FAIL: orphans"; exit 1; }
grep -q "missing:      0" "$WORK/fsck2.log" || { echo "FAIL: missing"; exit 1; }

echo "DEDUPE E2E: PASS"
