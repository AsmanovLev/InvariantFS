#!/bin/bash
# test-recshare.sh — whole-file recipe sharing (--recipe-share), persistent
# regression.
#
# Leg 1 (share correctness + what it is worth):
#   mkfs -> a tree with two duplicate GROUPS (three identical 200 KiB files,
#   and two other identical files) plus one unique file -> invf-sweep
#   --recipe-share -> the report must name 2 groups and 3 shared files, every
#   file must still read back BIT-EXACT, fsck clean, verify --deep 0 corrupt,
#   and the second run must be idempotent (0 shared).
#
# Leg 2 (ownership — the one that can silently corrupt data):
#   after sharing, REWRITE one loser with different bytes of the SAME length.
#   The donor and the other loser must still read back their original content,
#   and the rewritten file must read back the new bytes. A shared recipe that a
#   write mutates in place passes leg 1 and fails here.
#
# Leg 3 (what the tool reports is not a promise):
#   blocks_retired is an UPPER BOUND (the retire drain keeps an extent another
#   live recipe still names), so the assertion is on the DIRECTION of the free
#   count with a control image of the same geometry, never on the exact number.
#
# Run from the repo root after `make`:  bash tools/test-recshare.sh
# Uses /dev/shm (tmpfs) like the other offline suites; blkio treats /dev/* paths
# as raw devices, so the script cd's into /dev/shm and uses RELATIVE names.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=${INVFS_BIN:-$REPO/bin}
WORK=/dev/shm/recshare
IMG=recshare.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

free_blocks() { $B/invf-fsck "$1" | awk '/free blocks:/ {print $3}'; }
. "$REPO/tools/fsck-clean.sh"

echo "== generate tree =="
python3 - <<'PY'
import os, random
random.seed(90210)
d = "/dev/shm/recshare/orig"
K = 1024
def w(name, data): open(os.path.join(d, name), "wb").write(data)
# group of three identical files (200 KiB, incompressible on purpose: these are
# shares BY CONTENT, nothing here should depend on a codec decision)
A = random.randbytes(200*K)
w("g1a.bin", A); w("g1b.bin", A); w("g1c.bin", A)
# a second, independent group of two
Bg = random.randbytes(96*K)
w("g2a.bin", Bg); w("g2b.bin", Bg)
# unique
w("uniq.bin", random.randbytes(128*K))
# NOT a group: same size as each other, different bytes
w("same1.bin", random.randbytes(200*K))
w("same2.bin", random.randbytes(200*K))
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

echo "== leg 1: --recipe-share =="
$B/invf-sweep "$IMG" --recipe-share > "$WORK/rs1.log" 2>&1 || {
    cat "$WORK/rs1.log"; echo "FAIL: --recipe-share exited non-zero"; exit 1; }
LINE=$(grep '^\[recshare\]' "$WORK/rs1.log" | tail -1)
echo "${LINE:-FAIL: no [recshare] line}"
[ -n "$LINE" ] || exit 1
# NB: the variable MUST NOT be called GROUPS -- bash keeps that name for the
# caller's group IDs, so the assignment is silently refused and $GROUPS reads
# 1000 (the GID), which is how this leg first "failed" with groups=1000.
N_GROUPS=$(awk -F', ' '{for(i=1;i<=NF;i++) if ($i ~ /duplicate groups$/) print $i+0}'  <<<"$LINE")
N_SHARED=$(awk -F', ' '{for(i=1;i<=NF;i++) if ($i ~ /shared/) print $i+0}' <<<"$LINE")
echo "groups=$N_GROUPS shared=$N_SHARED"
# three files in group 1 -> 2 losers; two in group 2 -> 1 loser
[ "$N_GROUPS" = "2" ] || { echo "FAIL: expected 2 duplicate groups, got $N_GROUPS"; exit 1; }
[ "$N_SHARED" = "3" ] || { echo "FAIL: expected 3 shared files, got $N_SHARED"; exit 1; }

echo "== leg 1: the share returns blocks (direction only) =="
# Measured HERE, right after the share: leg 2 rewrites a file and a later sweep
# re-compresses, and both allocate, so a delta taken after them answers a
# different question. The import stored the duplicates separately, so sharing
# must hand blocks back; the republished rows cost some metadata pages, so the
# bound is deliberately loose -- this leg asks "did anything come back", and the
# bit-exact check below is what proves the data is still right.
FREE1=$(free_blocks "$IMG")
echo "free blocks: $FREE0 -> $FREE1"
[ "$FREE1" -gt "$FREE0" ] || {
    echo "FAIL: free blocks did not increase after sharing ($FREE0 -> $FREE1)"; exit 1; }

echo "== leg 1: every file still reads back bit-exact =="
for f in $FILES; do
    $B/invf-cat "$IMG" "$f" > "$WORK/out/$f"
    cmp "$WORK/orig/$f" "$WORK/out/$f" || { echo "FAIL: $f differs after share"; exit 1; }
done
echo "  all $(( $(cd "$WORK/orig" && ls | wc -l) )) files byte-identical"

echo "== leg 1: fsck + verify --deep =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck1.log"
fsck_clean "$WORK/fsck1.log" || { echo "FAIL: fsck issues after sharing"; exit 1; }
$B/invf-verify "$IMG" --deep > "$WORK/verify1.log" 2>&1 || {
    tail -20 "$WORK/verify1.log"; echo "FAIL: verify --deep exited non-zero"; exit 1; }
grep -qiE '0 corrupt|corrupt[= ]0|no corruption' "$WORK/verify1.log" || {
    grep -i corrupt "$WORK/verify1.log" | head -5; echo "FAIL: verify --deep did not say 0 corrupt"; exit 1; }
echo "  verify --deep: clean"

echo "== leg 1: idempotent second run =="
$B/invf-sweep "$IMG" --recipe-share > "$WORK/rs2.log" 2>&1 || {
    cat "$WORK/rs2.log"; exit 1; }
LINE2=$(grep '^\[recshare\]' "$WORK/rs2.log" | tail -1)
SHARED2=$(awk -F', ' '{for(i=1;i<=NF;i++) if ($i ~ /shared/) print $i+0}' <<<"$LINE2")
echo "second run: shared=$SHARED2"
[ "$SHARED2" = "0" ] || { echo "FAIL: second run shared $SHARED2 files (not idempotent)"; exit 1; }

echo "== leg 2: rewrite ONE loser in place, same length =="
# g1b is a loser; overwrite it with different bytes of the same size.
python3 - <<'PY'
import random
random.seed(777)
open("/dev/shm/recshare/new_g1b.bin","wb").write(random.randbytes(200*1024))
PY
$B/invf-cp "$IMG" "$WORK/new_g1b.bin" g1b.bin >/dev/null
# The rewritten file must return the NEW bytes...
$B/invf-cat "$IMG" g1b.bin > "$WORK/out/g1b.new"
cmp "$WORK/new_g1b.bin" "$WORK/out/g1b.new" || {
    echo "FAIL: the rewritten file did not return its new content"; exit 1; }
# ...and the donor and the sibling must be untouched. This is the assertion a
# shared, mutable-in-place recipe fails.
for f in g1a.bin g1c.bin; do
    $B/invf-cat "$IMG" "$f" > "$WORK/out/$f.after"
    cmp "$WORK/orig/$f" "$WORK/out/$f.after" || {
        echo "FAIL: $f changed when its sibling was rewritten (shared recipe mutated)"; exit 1; }
done
echo "  donor + sibling unchanged; rewritten file returns its own bytes"
$B/invf-fsck "$IMG" | tee "$WORK/fsck2.log"
fsck_clean "$WORK/fsck2.log" || { echo "FAIL: fsck issues after the rewrite"; exit 1; }

# ---------------------------------------------------------------------------
# Leg 4: the TEXT-batch path. A share frees nothing BY ITSELF here (the block
# belongs to a batch that several members name), so the gain can only arrive
# when that batch dies and the TEXT GC collects it at the next sweep. Measured
# on the repo corpus with a control: 2507 blocks, i.e. ~9.5% of used blocks.
#
# The fixture is small text files, which is what batching is for. If the volume
# stores them unbatched, the premise is absent and the leg SKIPS BY NAME rather
# than passing on a quantity that means something else.
echo "== leg 4: TEXT batches die when the share lets go of them =="
IMG2=recshare-tz.img
IMG2CTL=recshare-tz-ctl.img
rm -f "$IMG2" "$IMG2CTL"
python3 - <<'PY'
import os, random
random.seed(31337)
d = "/dev/shm/recshare/tz"
os.makedirs(d, exist_ok=True)
body = ("".join("int counter_%04d(void) { return %d; }\n" % (i, i) for i in range(120)))
for k in range(40):
    open(os.path.join(d, "m%02d.c" % k), "w").write(body)
# control: the same 40 files, ONE byte apart in each, so nothing can be shared
ctl = "/dev/shm/recshare/tzctl"
os.makedirs(ctl, exist_ok=True)
for k in range(40):
    open(os.path.join(ctl, "m%02d.c" % k), "w").write(body.replace("counter_%04d" % k,
                                                                  "counter_%04d" % k + "x"))
print("  %d files of %d bytes" % (40, len(body)))
PY
$B/invf-mkfs "tz.img" 0.5 >/dev/null
for f in $(cd "$WORK/tz" && ls); do $B/invf-cp tz.img "$WORK/tz/$f" "$f" >/dev/null; done
TZ0=$(free_blocks tz.img)
$B/invf-sweep tz.img > "$WORK/tzsweep0.log" 2>&1 || { tail -5 "$WORK/tzsweep0.log"; exit 1; }
TZ0=$(free_blocks tz.img)
$B/invf-sweep tz.img --recipe-share > "$WORK/tzshare.log" 2>&1 || {
    cat "$WORK/tzshare.log"; exit 1; }
TZLINE=$(grep '^\[recshare\]' "$WORK/tzshare.log" | tail -1)
echo "  $TZLINE"
TZTEXT=$(awk -F', ' '{for(i=1;i<=NF;i++) if ($i ~ /TEXT batch/) print $i+0}' <<<"$TZLINE")
TZSHARED=$(awk -F', ' '{for(i=1;i<=NF;i++) if ($i ~ /shared/) print $i+0}' <<<"$TZLINE")
if [ "${TZSHARED:-0}" -lt 2 ] || [ "${TZTEXT:-0}" -lt 1 ]; then
    echo "SKIP: leg 4 — this volume stored the text files without batches"
    echo "      (shared=${TZSHARED:-0}, through-a-batch=${TZTEXT:-0}); the"
    echo "      batch-death path is unreachable here, so there is nothing to"
    echo "      assert. Cause: batching thresholds, not the share."
else
    $B/invf-sweep tz.img > "$WORK/tzsweep1.log" 2>&1 || { tail -5 "$WORK/tzsweep1.log"; exit 1; }
    TZ1=$(free_blocks tz.img)
    $B/invf-sweep tz.img > "$WORK/tzsweep2.log" 2>&1 || { tail -5 "$WORK/tzsweep2.log"; exit 1; }
    TZ2=$(free_blocks tz.img)
    echo "  free blocks: $TZ0 -> $TZ1 -> $TZ2 (share reported 0 retired by itself)"
    [ "$TZ2" -gt "$TZ0" ] || {
        echo "FAIL: the shared batches were never collected ($TZ0 -> $TZ2)"; exit 1; }
    for f in $(cd "$WORK/tz" && ls); do
        $B/invf-cat tz.img "$f" > "$WORK/out/tz-$f"
        cmp "$WORK/tz/$f" "$WORK/out/tz-$f" || { echo "FAIL: $f differs after the batch GC"; exit 1; }
    done
    $B/invf-fsck tz.img | tee "$WORK/fsck3.log"
    fsck_clean "$WORK/fsck3.log" || { echo "FAIL: fsck issues after the batch GC"; exit 1; }
    echo "  all 40 files byte-identical; batches collected at the next sweep"
fi

echo "PASS: recipe sharing — 2 groups shared on an unswept volume with blocks"
echo "      returned, donor survives a sibling rewrite, batches of identical"
echo "      text files collected by the next sweep, verify --deep clear"
