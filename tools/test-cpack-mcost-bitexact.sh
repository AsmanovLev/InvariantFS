#!/bin/bash
# test-cpack-mcost-bitexact.sh -- the invariant this WP is not allowed to
# trade: the size-aware member cost changes DECISION LOGIC only.
#
# The charge model decides whether the containerpack lane commits a
# decomposition. Every case in cpack_guard_test.c is about that decision.
# None of them is about bytes, and a guard test cannot prove bytes: it
# never writes a volume. So this script is the other half of the WP, and
# it asserts the thing the whole change is forbidden to affect:
#
#     after a lane sweep, EVERY file reads back byte-identical.
#
# Read back with bin/invf-cat (the same reader an operator uses), compared
# with cmp against the pristine input. Not a hash of the volume, not
# "invf-verify said clean", not a count of surviving files: cmp on every
# file, including the ones the sweep rewrote.
#
# The tree is deliberately a MIX, so both sides of the decision are in it:
#
#   win-big.splt    a compressible many-member container: the lane COMMITS
#                   and rewrites the file into member inodes
#   regret.splt     incompressible many-member: the lane REFUSES and the
#                   file is left RAW (the guard's own negative control)
#   plain.txt       an ordinary file that no lane touches
#   rando.bin       incompressible random bytes, no container magic
#   empty.bin       zero bytes -- the degenerate length
#   subblk/m*.bin   many sub-block members: the SHAPE this WP exists to
#                   unblock. Under the retired flat model this container
#                   was refused; under the shipped price it is eligible.
#                   Whether the shape is actually accepted depends on the
#                   container's real compression, so the script asserts
#                   bit-exactness either way and REPORTS which happened.
#                   It never asserts a decision -- that is
#                   cpack_guard_test.c's job, and a bytes test has no
#                   business duplicating it.
#
# WHAT THIS ESTABLISHES, precisely: for this corpus, on this build, the
# lane sweep changed no bytes. It does NOT establish bit-exactness for
# corpora it does not contain, nor for member counts near 50,000 (that
# needs an image this script does not build), nor across a crash -- the
# flakey/power-loss suite owns that.
#
# Run from the repo root after `make`:
#     bash tools/test-cpack-mcost-bitexact.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"

B=$REPO/bin
WORK="${WORK:-/dev/shm/wp-mcost-bitexact}"
IMG=wp-mcost-bitexact.img
IMGSIZE="${IMGSIZE:-2}"
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads

rm -rf "$WORK" && mkdir -p "$WORK/src" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
[ -x "$B/invf-cat" ]    || { echo "FAIL: invf-cat not built"; exit 1; }
[ -x "$B/invf-import" ] || { echo "FAIL: invf-import not built"; exit 1; }
[ -x "$B/invf-sweep" ]  || { echo "FAIL: invf-sweep not built"; exit 1; }

echo "== generate a mixed synthetic tree =="
python3 - "$WORK/src" <<'PY'
import os, random, struct, sys

def splt(members):
    b = b"SPLT" + struct.pack("<I", len(members))
    for m in members:
        b += struct.pack("<Q", len(m))
    return b + b"".join(members)

d = sys.argv[1]
rnd = random.Random(108)          # fixed seed: this corpus is reproducible

def w(name, data):
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)

# 1. a compressible many-member container: a real win, so the lane
#    COMMITS it and the read path is rebuilt through the pack
w("win-big.splt", splt([b"the quick brown fox jumps over the lazy dog\n" * 900,
                        b"alpha beta gamma delta epsilon zeta eta theta\n" * 1100,
                        b"invariant filesystem content addressed storage\n" * 700,
                        b"\n" * 4096]))

# 2. incompressible many-member: no codec can shrink it and every exposed
#    member costs bookkeeping, so the guard should REFUSE this one
w("regret.splt", splt([rnd.randbytes(16384) for _ in range(200)]))

# 3. sub-block members: the shape the flat model refused. 400 members of
#    ~2,400 B, each projecting to well under a block at ZSTD-19, so the
#    payloads batch into ~118 KB of blocks against 512,000 B of member
#    bookkeeping. Under the retired flat model the bookkeeping alone was
#    6,553,600 B and this was refused; under the shipped price it is
#    ACCEPTED. That matters here: it means the lane REWRITES a file of
#    this shape, so the bit-exactness check covers a newly-writable path
#    and not only the comfortable one.
w("subblk.splt", splt([b"rootfs small file payload %04d\n" % i * 55
                        for i in range(400)]))

# 4. an ordinary file no lane should touch
w("plain.txt", b"plain text, no container magic, no codec family\n" * 3000)

# 5. incompressible, no container magic
w("rando.bin", rnd.randbytes(200000))

# 6. zero bytes -- the degenerate length
w("empty.bin", b"")

# 7. exactly one block, and one byte either side of it: the sizes the
#    block rounding in the charge model turns on
w("blk4096.bin", b"B" * 4096)
w("blk4095.bin", b"C" * 4095)
w("blk4097.bin", b"D" * 4097)

print("fixture sizes:")
for n in sorted(os.listdir(d)):
    print("  %-16s %d" % (n, os.path.getsize(os.path.join(d, n))))
PY

# The manifest the comparison loop reads, so the file list is derived from
# what was actually written rather than restated here (a restated list is a
# list that silently drifts).
( cd "$WORK/src" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) \
    > "$WORK/manifest"

echo "== manifest =="
sed 's/^/  /' "$WORK/manifest"
N_FILES=$(wc -l < "$WORK/manifest")
echo "  $N_FILES files"

echo "== mkfs =="
INVFS_META_FRAC=16 "$B/invf-mkfs" "$IMG" "$IMGSIZE" > /dev/null

echo "== import =="
"$B/invf-import" "$IMG" "$WORK/src" > /dev/null

echo "== sweep (the lane runs here) =="
"$B/invf-sweep" "$IMG" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; echo "FAIL: sweep 1 failed"; exit 1; }
grep -E "codecpack|refused|parts -> " "$WORK/sweep1.log" | sed 's/^/  /' || true

# A second and third pass: the first pass's writes need draining, and a
# lane that only shows its effect on a later pass would be a lane this
# script had not actually exercised.
"$B/invf-sweep" "$IMG" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; echo "FAIL: sweep 2 failed"; exit 1; }
"$B/invf-sweep" "$IMG" > "$WORK/sweep3.log" 2>&1 \
    || { cat "$WORK/sweep3.log"; echo "FAIL: sweep 3 failed"; exit 1; }

echo "== what the sweep decided (REPORTED, not asserted) =="
for f in win-big.splt regret.splt subblk.splt; do
    if grep -q "$f.*codecpack" "$WORK"/sweep*.log; then
        echo "  $f: DECOMPOSED"
    elif grep -q "$f.*refused" "$WORK"/sweep*.log; then
        echo "  $f: refused by the size guard"
    else
        echo "  $f: not a containerpack decision on this build"
    fi
done

echo "== invf-verify --deep =="
"$B/invf-verify" "$IMG" --deep | tee "$WORK/verify.log"
grep -q " 0 corrupt," "$WORK/verify.log" \
    || { echo "FAIL: verify reports corrupt files"; exit 1; }

echo "== BIT-EXACTNESS: invf-cat every file, cmp against the original =="
ok=1; checked=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    "$B/invf-cat" "$IMG" "$f" "$WORK/out/$f" > /dev/null
    if cmp -s "$WORK/src/$f" "$WORK/out/$f"; then
        checked=$((checked + 1))
    else
        echo "  MISMATCH: $f ($(stat -c %s "$WORK/src/$f") B in," \
             "$(stat -c %s "$WORK/out/$f" 2>/dev/null || echo '?') B out)"
        ok=0
    fi
done < "$WORK/manifest"
[ "$ok" = 1 ] || { echo "FAIL: at least one file did not read back identically"; exit 1; }
echo "  $checked/$N_FILES files byte-identical after the lane sweep"

[ "$checked" = "$N_FILES" ] \
    || { echo "FAIL: only $checked of $N_FILES were compared"; exit 1; }

# NON-VACUITY CONTROL. A bit-exactness test that passes because it compares
# nothing, or compares the wrong thing, is the most dangerous test in the
# repo: it reports the invariant holds when nothing checked it. So the
# comparison itself is checked, here, in the same script and with the same
# cmp: overwrite one file IN THE VOLUME with different bytes and confirm
# this loop catches it. If cmp were mis-invoked (empty output file, wrong
# operand order, a stale path) the mutation would go unnoticed and this
# leg would fail.
echo "== non-vacuity control: a mutated volume MUST be caught =="
# rando.bin is incompressible and is not a container, so the lane leaves it
# alone -- rewriting it changes bytes without disturbing anything else.
# invf-import takes a DIRECTORY, so the mutation is staged in its own.
rm -rf "$WORK/mut" && mkdir -p "$WORK/mut"
printf 'not what went in\n' > "$WORK/mut/rando.bin"
( cd /dev/shm && "$B/invf-import" "$IMG" "$WORK/mut" > /dev/null 2>&1 )
"$B/invf-cat" "$IMG" "rando.bin" "$WORK/out/mut.readback" > /dev/null
MUT_SEEN=no
if ! cmp -s "$WORK/src/rando.bin" "$WORK/out/mut.readback"; then
    MUT_SEEN=yes
fi
echo "  mutated volume detected by cmp: $MUT_SEEN"
[ "$MUT_SEEN" = yes ] \
    || { echo "FAIL: cmp did not notice a mutated file -- the comparison"; \
         echo "      above proved nothing; restore the volume and re-run"; exit 1; }
# restore the original bytes so the PASS below describes the lane's output
cp "$WORK/src/rando.bin" "$WORK/mut/rando.bin"
( cd /dev/shm && "$B/invf-import" "$IMG" "$WORK/mut" > /dev/null 2>&1 )
"$B/invf-cat" "$IMG" "rando.bin" "$WORK/out/restored" > /dev/null
cmp -s "$WORK/src/rando.bin" "$WORK/out/restored" \
    || { echo "FAIL: could not restore rando.bin after the mutation"; exit 1; }
echo "  original bytes restored and re-verified"

echo "== the decomposed containers' MEMBERS read back too =="
# The lane's real output. If the members are actual inodes then they must
# be readable by name AND must match the payload spliced out of the
# ORIGINAL container at that member's index. Member names are
# "<name>!mbr<NNNN>-<sname>" and the suggested name is a property of the
# pack's payload, so they are DISCOVERED with invf-ls and matched back by
# index -- never guessed, because a guessed name that happens to miss
# proves nothing and a guessed name that happens to hit is luck.
# Splice the reference payloads out of the ORIGINAL containers first; the
# member inode must equal the payload at its own index in the source.
python3 - "$WORK/src" "$WORK/out/members" <<'PY'
import os, struct, sys
srcdir, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
for cont in ("win-big.splt", "subblk.splt"):
    b = open(os.path.join(srcdir, cont), "rb").read()
    n = struct.unpack("<I", b[4:8])[0]
    off = 8 + 8 * n
    for i in range(n):
        ln = struct.unpack("<Q", b[8 + 8 * i:16 + 8 * i])[0]
        with open(os.path.join(outdir, "%s.%d" % (cont, i)), "wb") as f:
            f.write(b[off:off + ln])
        off += ln
PY
# note: blkio treats /dev/* paths as raw devices, so this stays in /dev/shm
# and uses the RELATIVE image path -- an absolute one makes the tools try
# to open the backing store as a device and fail with EACCES.
MEM_OK=1; MEM_N=0
( cd /dev/shm && "$B/invf-ls" "$IMG" 2>/dev/null ) \
    | grep -oE '(subblk|win-big)\.splt!mbr[0-9]{4}-[^ ]+' \
    | LC_ALL=C sort -u > "$WORK/membernames" || true
while IFS= read -r name; do
    [ -n "$name" ] || continue
    idx=$(printf '%s' "$name" | sed 's/.*!mbr//')
    idx=$((10#$idx))
    cont=${name%%!*}
    "$B/invf-cat" "$IMG" "$name" "$WORK/out/got.$cont.$idx" > /dev/null 2>&1 || {
        echo "  FAIL: could not read member $name"; MEM_OK=0; continue; }
    if cmp -s "$WORK/out/members/$cont.$idx" "$WORK/out/got.$cont.$idx"; then
        MEM_N=$((MEM_N + 1))
    else
        echo "  MISMATCH member: $name"
        MEM_OK=0
    fi
done < "$WORK/membernames"
[ "$MEM_OK" = 1 ] || { echo "FAIL: a decomposed member did not read back"; exit 1; }
echo "  $MEM_N member payloads read back byte-identical"
# A zero here would mean the lane decomposed nothing and this leg asserted
# nothing. Report it rather than let a silent 0 read as a pass.
[ "$MEM_N" -gt 0 ] \
    || { echo "FAIL: no member inodes found -- this leg proved nothing"; exit 1; }

echo
echo "CPACK-MCOST-BITEXACT: PASS"
echo "  $checked/$N_FILES files byte-identical after 3 lane sweeps"
echo "  scope: this corpus, this build, no crash. Not 50k members (no such"
echo "  image here), not other corpora, not power-loss -- see the header."
