#!/bin/bash
# test-heat.sh — WP19 heat counters + adaptive tiering + profile ladder e2e
# (persistent regression).
#
# Leg 1 (heat + promotion):
#   mkfs -> 30 texts + 2 blobs -> sweep (texts batched) -> read 3 texts 20x
#   and one 4x via invf-cat loops -> meta_probe heat (persistence across
#   unmount/remount) -> sweep -> hot members promoted to GENERIC{ZSTD},
#   bit-exact; unread stay TEXT; idle sweep halves rheat (decay).
# Leg 2 (write-heat + INVFS_HEAT_INIT):
#   rewrite a file 3x -> wheat carries (meta_probe) -> sweep skips the heavy
#   codecs for it (generic floor), control file still batches; HEAT_INIT
#   seeds read-heat of newly created entries.
# Leg 3 (profile ladder):
#   turbo = verbatim store (write + sweep), fastest = LZ4, unset = ZSTD-19,
#   none of it sticky -- per-segment AST algo via meta_probe.
#
# Run from the repo root after `make`:  bash tools/test-heat.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp19heat
IMG=wp19heat.img
IMG2=wp19heat2.img
IMG3=wp19heat3.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMG2" "$IMG3"

cls() { $B/meta_probe "$1" --heat "$2" | grep "^class="; }
ast0() { $B/meta_probe "$1" --heat "$2" | grep "^ast i=0"; }
heat_max() { # max of a heat field (rheat|wheat) over a file's entries
    $B/meta_probe "$1" --heat "$2" | grep "^entry " |
      sed "s/.*$3=//" | awk '{if($1>m)m=$1} END{print m+0}';
}

echo "== leg 1: generate tree =="
python3 - <<'PY'
import os, random
random.seed(1919)
d = "/dev/shm/wp19heat/orig"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t heat tier\n").split()
for i in range(30):
    n, out = 0, []
    size = 3000 + (i % 7) * 900
    while n < size:
        w = random.choice(WORDS)
        out.append(w); n += len(w) + 1
    open(os.path.join(d, "t%02d.txt" % i), "w").write(" ".join(out)[:size])
open(os.path.join(d, "blob1.bin"), "wb").write(random.randbytes(100*1024))
open(os.path.join(d, "blob2.bin"), "wb").write(random.randbytes(100*1024))
print("tree:", len(os.listdir(d)), "files")
PY

echo "== leg 1: mkfs + import =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
for f in $(cd "$WORK/orig" && ls); do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== leg 1: sweep #1 (texts batch) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
TZL=$(grep -c "text -> PPMd batch" "$WORK/sweep1.log" || true)
echo "text->PPMd lines: $TZL"
[ "$TZL" = "30" ] || { echo "FAIL: expected 30 deferred texts, got $TZL"; exit 1; }
for i in $(seq -w 0 29); do
    cls "$IMG" "t$i.txt" | grep -q "^class=7 algo=2" || {
        echo "FAIL: t$i.txt not TEXT{PPMD} after sweep #1: $(cls "$IMG" "t$i.txt")"; exit 1; }
done
echo "all 30 texts batched (class=7 algo=2)"

echo "== leg 1: read hot subset (t00-t02 x20, t03 x4) =="
for i in $(seq 20); do
    for f in t00.txt t01.txt t02.txt; do
        $B/invf-cat "$IMG" "$f" >/dev/null
    done
done
for i in $(seq 4); do $B/invf-cat "$IMG" t03.txt >/dev/null; done
# heat persisted across the 64 unmount/remount cycles above (each invf-cat
# is a full open+close), and a probe run does not itself accrue heat
R00=$(heat_max "$IMG" t00.txt rheat)
R00B=$(heat_max "$IMG" t00.txt rheat)
R03=$(heat_max "$IMG" t03.txt rheat)
R04=$(heat_max "$IMG" t04.txt rheat)
echo "rheat: t00=$R00 t03=$R03 t04=$R04"
[ "$R00" = "20" ] || { echo "FAIL: t00 rheat $R00 != 20 (persistence broken)"; exit 1; }
[ "$R00B" = "20" ] || { echo "FAIL: probe accrued heat ($R00B != 20)"; exit 1; }
[ "$R03" = "4" ] || { echo "FAIL: t03 rheat $R03 != 4"; exit 1; }
[ "$R04" = "0" ] || { echo "FAIL: unread t04 rheat $R04 != 0"; exit 1; }

echo "== leg 1: sweep #2 (decay + promotion) =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
HEAT_LINE=$(grep "^heat: " "$WORK/sweep2.log" || true)
echo "${HEAT_LINE:-FAIL: no heat line}"
[ "$HEAT_LINE" = "heat: 3 hot text member(s), 3 promoted (budget 3 of 30 live)" ] || {
    echo "FAIL: unexpected heat line"; exit 1; }
for f in t00.txt t01.txt t02.txt; do
    cls "$IMG" "$f" | grep -q "^class=4 algo=1" || {
        echo "FAIL: $f not GENERIC{ZSTD} after promotion: $(cls "$IMG" "$f")"; exit 1; }
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || { echo "FAIL: $f not bit-exact"; exit 1; }
done
echo "promoted files are GENERIC{ZSTD} and bit-exact"
R03=$(heat_max "$IMG" t03.txt rheat)
echo "t03 rheat after decay: $R03"
[ "$R03" = "2" ] || { echo "FAIL: t03 rheat $R03 != 2 after decay"; exit 1; }
for i in $(seq -w 3 29); do
    cls "$IMG" "t$i.txt" | grep -q "^class=7" || {
        echo "FAIL: unread t$i.txt left TEXT: $(cls "$IMG" "t$i.txt")"; exit 1; }
done
echo "unread files stay TEXT"

echo "== leg 1: sweep #3 (idle -> rheat halves again, nothing promotes) =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1 || { cat "$WORK/sweep3.log"; exit 1; }
grep -q "promoted [1-9]" "$WORK/sweep3.log" && {
    echo "FAIL: idle sweep promoted"; cat "$WORK/sweep3.log"; exit 1; }
R03=$(heat_max "$IMG" t03.txt rheat)
echo "t03 rheat after idle sweep: $R03"
[ "$R03" = "1" ] || { echo "FAIL: t03 rheat $R03 != 1 after idle decay"; exit 1; }
for i in $(seq -w 3 29); do
    cls "$IMG" "t$i.txt" | grep -q "^class=7" || {
        echo "FAIL: t$i.txt promoted on cold heat"; exit 1; }
done

echo "== leg 1: full bit-exact + fsck =="
ok=1
for f in $(cd "$WORK/orig" && ls); do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all files bit-exact"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log" | tail -1
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }
$B/invf-fsck "$IMG" | tee "$WORK/fsck1.log"
grep -q "orphans:      0" "$WORK/fsck1.log" || { echo "FAIL: orphans"; exit 1; }
grep -q "missing:      0" "$WORK/fsck1.log" || { echo "FAIL: missing"; exit 1; }

echo
echo "== leg 2: write-heat + HEAT_INIT =="
python3 - <<'PY'
import os, random
random.seed(2929)
d = "/dev/shm/wp19heat/orig2"
os.makedirs(d, exist_ok=True)
WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa\n").split()
def tf(name, size):
    n, out = 0, []
    while n < size:
        w = random.choice(WORDS)
        out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
tf("w.txt", 5000); tf("c.txt", 5000); tf("init.txt", 3000)
PY
$B/invf-mkfs "$IMG2" 0.5 >/dev/null
mkdir -p "$WORK/orig2"
for f in w.txt c.txt; do
    $B/invf-cp "$IMG2" "$WORK/orig2/$f" "$f" >/dev/null
done
INVFS_HEAT_INIT=7 $B/invf-cp "$IMG2" "$WORK/orig2/init.txt" init.txt >/dev/null
RI=$(heat_max "$IMG2" init.txt rheat)
WI=$(heat_max "$IMG2" init.txt wheat)
echo "init.txt after HEAT_INIT=7 import: rheat=$RI wheat=$WI"
[ "$RI" = "7" ] || { echo "FAIL: INVFS_HEAT_INIT did not seed (rheat=$RI)"; exit 1; }
[ "$WI" = "1" ] || { echo "FAIL: create wheat=$WI != 1"; exit 1; }
$B/invf-sweep "$IMG2" > "$WORK/s2sweep1.log" 2>&1 || { cat "$WORK/s2sweep1.log"; exit 1; }
cls "$IMG2" w.txt | grep -q "^class=7" || { echo "FAIL: w.txt not batched"; exit 1; }
cls "$IMG2" c.txt | grep -q "^class=7" || { echo "FAIL: c.txt not batched"; exit 1; }
# rewrite w.txt 3x: wheat carries old+1 across every rewrite (rheat resets)
for i in 1 2 3; do
    python3 -c "
import random
random.seed($i)
WORDS='alpha beta gamma delta epsilon zeta eta theta iota kappa\n'.split()
out=[]; n=0
while n < 5000:
    w=random.choice(WORDS); out.append(w); n+=len(w)+1
open('$WORK/orig2/w.txt','w').write(' '.join(out)[:5000])"
    $B/invf-cp "$IMG2" "$WORK/orig2/w.txt" w.txt >/dev/null
done
W=$(heat_max "$IMG2" w.txt wheat)
echo "w.txt wheat after 3 rewrites: $W"
[ "$W" = "3" ] || { echo "FAIL: wheat $W != 3 after 3 rewrites"; exit 1; }
$B/invf-sweep "$IMG2" > "$WORK/s2sweep2.log" 2>&1 || { cat "$WORK/s2sweep2.log"; exit 1; }
cls "$IMG2" w.txt | grep -q "^class=4 algo=1" || {
    echo "FAIL: write-hot w.txt did not take the generic floor: $(cls "$IMG2" w.txt)"; exit 1; }
ast0 "$IMG2" w.txt | grep -q "zone=2 algo=1" || {
    echo "FAIL: w.txt not per-segment ZSTD BINARY: $(ast0 "$IMG2" w.txt)"; exit 1; }
cls "$IMG2" c.txt | grep -q "^class=7" || {
    echo "FAIL: control c.txt lost its batch: $(cls "$IMG2" c.txt)"; exit 1; }
echo "write-hot file took the generic floor (skips heavy codecs); control still TEXT"
$B/invf-cat "$IMG2" w.txt "$WORK/out/w.txt" >/dev/null
cmp -s "$WORK/orig2/w.txt" "$WORK/out/w.txt" || { echo "FAIL: w.txt not bit-exact"; exit 1; }
$B/invf-cat "$IMG2" c.txt "$WORK/out/c.txt" >/dev/null
cmp -s "$WORK/orig2/c.txt" "$WORK/out/c.txt" || { echo "FAIL: c.txt not bit-exact"; exit 1; }
# WP22d: frees made by a non-arming process while a checkpoint was live are
# held (never reused) but unregistered -- they sit as allocated orphans until
# the checkpoint resolves AND fsck -f reclaims them. Close the lifecycle
# explicitly here: realize the checkpoint, reclaim, then assert clean.
$B/invf-sweep "$IMG2" --realize >/dev/null 2>&1 || true
$B/invf-fsck "$IMG2" -f >/dev/null 2>&1 || true
$B/invf-fsck "$IMG2" | tee "$WORK/fsck2.log"
grep -q "orphans:      0" "$WORK/fsck2.log" || { echo "FAIL: orphans (leg2)"; exit 1; }
grep -q "missing:      0" "$WORK/fsck2.log" || { echo "FAIL: missing (leg2)"; exit 1; }

echo
echo "== leg 3: profile ladder =="
python3 - <<'PY'
import os
d = "/dev/shm/wp19heat/orig3"
os.makedirs(d, exist_ok=True)
# compressible BINARY payloads: the generic floor is what profiles steer,
# and a .txt would defer into the PPMd batch accumulator before reaching it
payload = bytes(range(256)) * 32
for name in ("p1.bin", "p2.bin", "p3.bin", "p4.bin"):
    open(os.path.join(d, name), "wb").write(payload)
PY
$B/invf-mkfs "$IMG3" 0.5 >/dev/null
# turbo sweep: verbatim store (algo NONE, zone BINARY)
$B/invf-cp "$IMG3" "$WORK/orig3/p1.bin" p1.bin >/dev/null
INVFS_PROFILE=turbo $B/invf-sweep "$IMG3" > "$WORK/s3turbo.log" 2>&1
grep -q "profile: turbo (generic verbatim" "$WORK/s3turbo.log" || {
    echo "FAIL: turbo profile line"; cat "$WORK/s3turbo.log"; exit 1; }
ast0 "$IMG3" p1.bin | grep -q "zone=2 algo=0" || {
    echo "FAIL: turbo sweep not verbatim: $(ast0 "$IMG3" p1.bin)"; exit 1; }
echo "turbo sweep: verbatim (zone=2 algo=0)"
# fastest sweep: LZ4 segments
$B/invf-cp "$IMG3" "$WORK/orig3/p2.bin" p2.bin >/dev/null
INVFS_PROFILE=fastest $B/invf-sweep "$IMG3" > "$WORK/s3fastest.log" 2>&1
grep -q "profile: fastest (generic lz4" "$WORK/s3fastest.log" || {
    echo "FAIL: fastest profile line"; cat "$WORK/s3fastest.log"; exit 1; }
ast0 "$IMG3" p2.bin | grep -q "zone=2 algo=5" || {
    echo "FAIL: fastest sweep not LZ4: $(ast0 "$IMG3" p2.bin)"; exit 1; }
echo "fastest sweep: LZ4 (zone=2 algo=5)"
# unset profile: the historical ZSTD-19 (byte-identical default)
$B/invf-cp "$IMG3" "$WORK/orig3/p3.bin" p3.bin >/dev/null
$B/invf-sweep "$IMG3" > "$WORK/s3bal.log" 2>&1
ast0 "$IMG3" p3.bin | grep -q "zone=2 algo=1" || {
    echo "FAIL: default sweep not ZSTD: $(ast0 "$IMG3" p3.bin)"; exit 1; }
echo "unset profile: ZSTD (zone=2 algo=1)"
# turbo WRITE path: verbatim RAW segments; not sticky (next default sweep ZSTDs)
INVFS_PROFILE=turbo $B/invf-cp "$IMG3" "$WORK/orig3/p4.bin" p4.bin >/dev/null
ast0 "$IMG3" p4.bin | grep -q "zone=0 algo=0" || {
    echo "FAIL: turbo write not verbatim RAW: $(ast0 "$IMG3" p4.bin)"; exit 1; }
echo "turbo write: verbatim RAW (zone=0 algo=0)"
$B/invf-sweep "$IMG3" > "$WORK/s3p4.log" 2>&1
ast0 "$IMG3" p4.bin | grep -q "zone=2 algo=1" || {
    echo "FAIL: p4 did not default to ZSTD next sweep: $(ast0 "$IMG3" p4.bin)"; exit 1; }
ok=1
for f in p1.bin p2.bin p3.bin p4.bin; do
    $B/invf-cat "$IMG3" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig3/$f" "$WORK/out/$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all profile legs bit-exact"
$B/invf-fsck "$IMG3" | tee "$WORK/fsck3.log"
grep -q "orphans:      0" "$WORK/fsck3.log" || { echo "FAIL: orphans (leg3)"; exit 1; }
grep -q "missing:      0" "$WORK/fsck3.log" || { echo "FAIL: missing (leg3)"; exit 1; }

echo "HEAT E2E: PASS"
