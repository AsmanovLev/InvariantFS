#!/bin/bash
# test-dedupe.sh — WP12(h) offline per-segment dedupe e2e (persistent regression).
#
# Leg 1 (merge correctness):
#   mkfs -> tree with known SEGMENT-aligned duplicates -> sweep (runs the
#   dedupe pass between the walk and the text GC) -> verify --deep ->
#   invf-cat bit-exact vs originals -> assert merged/freed counts AND,
#   after bare sweeps converge the multi-generation retire discharge,
#   that the converged free-block delta equals the freed-blocks report
#   (control-adjusted) -> second sweep idempotent (0 new merges) ->
#   fsck clean.
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
IMGCTL=wp12dedupe-ctl.img
IMGT=wp12dedupe-tz.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/origctl" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGCTL" "$IMGT"

free_blocks() { $B/invf-fsck "$1" | awk '/free blocks:/ {print $3}'; }

# The sweep reports the dedupe counters in its stage line:
#   [5/8] dedupe  100.0%  171/171 ... merged=171 cross=163 intra=8 freed=4.1 MiB
#   (stage totals vary with the migrate/seal/tier stages; dedupe keeps position 5 single-device)
# Older builds printed a standalone "dedupe: merged N segments, freed N MiB"
# line. Read either shape and normalise to "MERGED FREED_MiB" so the
# assertions below do not care which build produced the log.
dedupe_line() {   # <log> -> the dedupe line, or empty
    local log=$1 line
    line=$(grep -E "merged=[0-9]+ cross=[0-9]+" "$log" 2>/dev/null | tail -1)
    [ -n "$line" ] || line=$(grep "dedupe: merged" "$log" 2>/dev/null | tail -1)
    printf '%s' "$line"
}

dedupe_field() {  # <log> <merged|cross|intra|freed>
    local log=$1 want=$2 line
    line=$(dedupe_line "$log")
    [ -n "$line" ] || return 1
    case "$want" in
    merged) sed -n 's/.*merged[= ]\([0-9]*\).*/\1/p' <<<"$line" ;;
    cross)  sed -n 's/.*cross[= ]\([0-9]*\).*/\1/p' <<<"$line" ;;
    intra)  sed -n 's/.*intra[= ]\([0-9]*\).*/\1/p' <<<"$line" ;;
    freed)  sed -n 's/.*freed[= ]\([0-9.]*\).*/\1/p' <<<"$line" ;;
    esac
}


# v3 fsck reports base-tree health (bad pages / cycles) rather than the v2
# orphan/missing counters, so the gate is format-aware: fsck_clean lives in
# tools/fsck-clean.sh and asserts what each format actually verifies
# (WP75: the test now reaches this section on the default v3 format).
. "$REPO/tools/fsck-clean.sh"

# Meta-v3 stores text as generic ZSTD (no deferred PPMd batches), so the
# leg-2 expectations differ between formats. Meta-v3 is the default.
is_meta() { $B/invf-fsck "$1" 2>/dev/null | grep -q "format:       v0"; }

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

# Control tree: byte-identical to the dedupe tree except every would-be
# shared 64 KB segment has one byte flipped, so the dedupe pass merges
# nothing. Keeping the rest identical keeps the sweep's non-dedupe metadata
# footprint (and compression geometry) the same, so the control image
# captures the metadata allocations that make the naive
# FREE0+FREED==FREE1 identity false on Meta (WP75 Bug C).
import shutil
c = "/dev/shm/wp12dedupe/origctl"
def copy_flip(src, dst, offs):
    b = bytearray(open(os.path.join(d, src), "rb").read())
    for off in offs:
        b[off] ^= 0xFF
    open(os.path.join(c, dst), "wb").write(bytes(b))
shutil.copy(os.path.join(d, "a1.bin"), os.path.join(c, "a1.bin"))
copy_flip("a2.bin", "a2.bin", [65536])          # break a1/a2 shared middle
shutil.copy(os.path.join(d, "dup1.bin"), os.path.join(c, "dup1.bin"))
copy_flip("dup2.bin", "dup2.bin", [0, 65536, 131072])   # break all 3 dup segments
shutil.copy(os.path.join(d, "uniq.bin"), os.path.join(c, "uniq.bin"))
print("control tree: same geometry, no shared segments")
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
DEDUP_LINE=$(dedupe_line "$WORK/sweep1.log")
echo "${DEDUP_LINE:-FAIL: no dedupe line}"
[ -n "$DEDUP_LINE" ] || { echo "FAIL: sweep ran no dedupe pass"; exit 1; }
MERGED=$(dedupe_field "$WORK/sweep1.log" merged)
FREED=$(dedupe_field "$WORK/sweep1.log" freed)
# 1 shared middle segment (a1/a2) + 3 fully-shared segments (dup1/dup2)
[ "$MERGED" = "4" ] || { echo "FAIL: expected 4 merged segments, got $MERGED"; exit 1; }
# merged blocks must be back in the bitmap. On Meta-v3 the sweep also
# allocates metadata (recipe/base/delta pages), so the naive
# FREE0 + FREED == FREE1 identity is false. Compare against a no-dedupe
# control image with identical geometry: the control captures the sweep's
# non-dedupe metadata allocations, so
#   (FREE1_dup - FREE0_dup) - (FREE1_ctl - FREE0_ctl)
# recovers the blocks dedupe returned minus the handful of pages it
# allocates to rewrite the losing inodes (one recipe/inode page per merged
# file on v3).
#
# WP612 shape: superseded frees ride retire-on-fence and discharge
# across generations (retire drain at close, fold settling, savepoint
# windows dropping one per sweep), so ONE sweep no longer settles the
# books -- measured on this fixture: 120595 -> 120624 -> 120633 ->
# 120641 -> 120650 -> 120650 against a 120652 target (FREE0 + 57 -
# control churn), i.e. convergence over ~4 passes, zero leak. Assert
# the CONVERGED total, not the first-sweep level: sweep until free is
# stable across two consecutive passes (bounded), then compare. A real
# leak never reaches the target (stable shortfall); a stuck drain never
# stabilizes. DEDUPE_CONVERGE_EPS bounds per-pass settling noise.
DEDUPE_CONVERGE_EPS=8
DEDUPE_CONVERGE_MAX=6
# WP21: the sweep held its frees in the retention registry (the live
# checkpoint); realize it first so the bitmap reflects the dedupe (the
# no-op re-sweep inside --realize leaves no new checkpoint behind).
$B/invf-sweep "$IMG" --realize >> "$WORK/sweep1.log" 2>&1 || {
    cat "$WORK/sweep1.log"; exit 1; }
FREE1=$(free_blocks "$IMG")
echo "free blocks after sweep: $FREE1 (freed by dedupe: $FREED)"
# WP612: let the multi-generation discharge settle before comparing.
echo "== converge: bare sweeps until free stabilizes =="
PREV=$FREE1
STABLE=0
for i in $(seq 1 $DEDUPE_CONVERGE_MAX); do
    $B/invf-sweep "$IMG" > "$WORK/sweepc$i.log" 2>&1 || { cat "$WORK/sweepc$i.log"; exit 1; }
    CUR=$(free_blocks "$IMG")
    D=$((CUR > PREV ? CUR - PREV : PREV - CUR))
    echo "  converge sweep $i: free $CUR (delta $D)"
    if [ "$D" -le "$DEDUPE_CONVERGE_EPS" ]; then
        STABLE=$((STABLE + 1))
        [ "$STABLE" -ge 2 ] && { FREE1=$CUR; break; }
    else
        STABLE=0
    fi
    PREV=$CUR
    FREE1=$CUR
done
[ "$STABLE" -ge 2 ] || { echo "FAIL: free blocks did not stabilize in $DEDUPE_CONVERGE_MAX sweeps"; exit 1; }
echo "converged at $FREE1"

echo "== control image (no shared segments; identical geometry) =="
$B/invf-mkfs "$IMGCTL" 0.5 >/dev/null
CFILES=$(cd "$WORK/origctl" && ls)
for f in $CFILES; do
    $B/invf-cp "$IMGCTL" "$WORK/origctl/$f" "$f" >/dev/null
done
FREE0_CTL=$(free_blocks "$IMGCTL")
echo "free blocks after control import: $FREE0_CTL"
$B/invf-sweep "$IMGCTL" > "$WORK/sweepctl.log" 2>&1 || {
    cat "$WORK/sweepctl.log"; exit 1; }
DEDUP_CTL=$(dedupe_line "$WORK/sweepctl.log")
echo "${DEDUP_CTL:-FAIL: control ran no dedupe pass}"
[ "$(dedupe_field "$WORK/sweepctl.log" merged)" = "0" ] || {
    echo "FAIL: control tree still has shared segments"; exit 1; }
$B/invf-sweep "$IMGCTL" --realize >> "$WORK/sweepctl.log" 2>&1 || {
    cat "$WORK/sweepctl.log"; exit 1; }
FREE1_CTL=$(free_blocks "$IMGCTL")
DUP_DELTA=$((FREE1 - FREE0))
CTL_DELTA=$((FREE1_CTL - FREE0_CTL))
RESIDUAL=$((FREED - (DUP_DELTA - CTL_DELTA)))
echo "control free blocks after sweep: $FREE1_CTL " \
     "(dup delta $DUP_DELTA, control delta $CTL_DELTA, residual $RESIDUAL)"
[ "$RESIDUAL" -ge 0 ] && [ "$RESIDUAL" -le "$DEDUPE_CONVERGE_EPS" ] || {
    echo "FAIL: converged dedupe free-block delta $((DUP_DELTA - CTL_DELTA)) not within " \
         "$DEDUPE_CONVERGE_EPS of freed $FREED (residual $RESIDUAL)"; exit 1; }
echo "bitmap cross-check OK: +$FREED blocks returned vs control"

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

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
DEDUP2=$(dedupe_line "$WORK/sweep2.log")
echo "${DEDUP2:-FAIL: no dedupe line on sweep #2}"
[ "$(dedupe_field "$WORK/sweep2.log" merged)" = "0" ] || {
    echo "FAIL: second sweep merged again"; exit 1; }
[ "$(dedupe_field "$WORK/sweep2.log" freed)" = "0" ] || {
    echo "FAIL: second sweep freed blocks again"; exit 1; }

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck1.log"
fsck_clean "$WORK/fsck1.log" || { echo "FAIL: fsck issues"; exit 1; }

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

echo "== sweep (v3: generic text; v2: texts batch) =="
$B/invf-sweep "$IMGT" > "$WORK/sweept1.log" 2>&1 || { cat "$WORK/sweept1.log"; exit 1; }
TZL=$(grep -c "text -> PPMd batch" "$WORK/sweept1.log" || true)
DEDUPT=$(grep "dedupe: " "$WORK/sweept1.log" || true)
echo "$DEDUPT"
if is_meta "$IMGT"; then
    # WP78 taught v3 to defer texts into PPMd batches too (published as v3
    # recipe deltas), so v3 batches here exactly like v2 -- the old "v3 stores
    # text as generic ZSTD, expect no batching" expectation predates it. What
    # the pass must still deliver on v3 is a cross-file merge.
    echo "text->PPMd lines: $TZL (v3 batches texts since WP78)"
    [ "$TZL" -ge 3 ] || { echo "FAIL: v3 did not batch the texts"; exit 1; }
    echo "$DEDUPT" | grep -qE "merged [1-9][0-9]* segments" || {
        echo "FAIL: v3 dedupe merged nothing"; exit 1; }
else
    echo "text->PPMd lines: $TZL"
    [ "$TZL" -ge 3 ] || { echo "FAIL: expected >=3 deferred texts"; exit 1; }
    # only blob.bin is non-TEXT content: 2 segments hashed, nothing merged.
    # (Without the deferred-candidate skip, the two identical texts WOULD
    #  merge here and the flush's retire would corrupt the canonical copy.)
    echo "$DEDUPT" | grep -q "hashed 2 live segments" || {
        echo "FAIL: dedupe hashed more than the 2 non-TEXT segments"; exit 1; }
    echo "$DEDUPT" | grep -q "merged 0 segments, freed 0 blocks" || {
        echo "FAIL: dedupe touched deferred/TEXT content"; exit 1; }
fi

echo "== members bit-exact + stats + verify =="
ok=1
for f in $TFILES; do
    $B/invf-cat "$IMGT" "$f" "$WORK/out/$f.t" >/dev/null
    cmp -s "$WORK/orig2/$f" "$WORK/out/$f.t" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$TFILES" | wc -w) files bit-exact"
if is_meta "$IMGT"; then
    echo "v3: no TEXT class line (generic ZSTD), skipping stats check"
else
    $B/invf-stats "$IMGT" | grep "TEXT  :" || { echo "FAIL: no TEXT stats line"; exit 1; }
fi
$B/invf-verify "$IMGT" --deep | tail -1

echo "== re-sweep (idempotent) =="
$B/invf-sweep "$IMGT" > "$WORK/sweept2.log" 2>&1 || { cat "$WORK/sweept2.log"; exit 1; }
DEDUPT2=$(dedupe_line "$WORK/sweept2.log")
echo "${DEDUPT2:-FAIL: no dedupe line on text re-sweep}"
[ "$(dedupe_field "$WORK/sweept2.log" merged)" = "0" ] || {
    echo "FAIL: text re-sweep merged"; exit 1; }
ok=1
for f in $TFILES; do
    $B/invf-cat "$IMGT" "$f" "$WORK/out/$f.t2" >/dev/null
    cmp -s "$WORK/orig2/$f" "$WORK/out/$f.t2" || { echo "MISMATCH2 $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1

echo "== fsck (text volume) =="
$B/invf-fsck "$IMGT" | tee "$WORK/fsck2.log"
fsck_clean "$WORK/fsck2.log" || { echo "FAIL: fsck issues"; exit 1; }

echo "DEDUPE E2E: PASS"
