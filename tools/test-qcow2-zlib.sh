#!/bin/bash
# test-qcow2-zlib.sh — the qcow2 codecpack's ZLIB-COMPRESSED-CLUSTER lane
# end to end, and the manifest contract that lane depends on (WP107).
#
# WP119 retired the expectation this suite used to carry. It asserted that the
# containerpack size guard REFUSED this fixture, on the grounds that "the Q2R3
# strip keeps every deflate stream in the recipe and the members are pure
# addition on top", measured at +409 KiB of content blocks. That number does
# not reproduce, and the model behind it does not hold either: the guard is a
# CPU-FOR-SPACE trade, so a kind-2 (REPRO) entry may store nothing and the price
# is paid in re-deflate time instead of volume space. The guard's own
# diagnostic (INVFS_DEBUG=1) says so, for this exact fixture:
#
#     [cpack size guard] ACCEPT: orig=377344 projected=359605
#       (fixed=336565 content=20480 member_cost=2560)
#       gain=5/1000 headroom=17739 B  repro_max=65536  bound=4194304
#
# Two independent levers, and for THIS shape the space one passes vacuously:
# cpack_repro_stats (vol_cpack.c:2510) skips kind-2 entries, so the 23,040 B
# they occupy cost nothing in `content`/`member_cost` and a strip that
# re-deflates on every read looks nearly free; and `repro_max` is 64x under
# CPACK_REPRO_MAX. So the roles of the two arms are SWAPPED, and the reason is
# that the old arm was asserting a lever that cannot see this shape:
#
#   PASS   z.qcow2 with the stock pack -- a real gain, a small re-deflation
#   REFUSE z.qcow2 with a private pack whose map asks for a 4 MiB + 1
#          re-deflation -- the lever that actually constrains the shape.
#
# The bound is enforced once, by cpack_map_validate (vol_cpack.c:2268), which
# runs before the guard; the sweep's refusal therefore names it as a map-shape
# failure. That is what this suite asserts.
#
# WHAT THIS PINS
#   An 80%-compressed qcow2 used to be silently abandoned by the sweep with
#     "map does not partition the container, decomposition abandoned"
#   because the pack's `map` template said {in}. The engine hands the pack
#   BOTH the scratch image ({in}) and the strip output ({recipe}) and lets
#   the template pick; a reproducible compressed cluster is only nameable by
#   a v2 REPRO entry, and the deflate parameters that make it reproducible
#   live only in the recipe. With {in} the pack renders the legacy v1 MRMP
#   map, whose RECIPE src_off values are numbered against a recipe layout
#   strip no longer produces, and cpack_map_validate correctly refuses it.
#
#   Every other container pack in the registry maps from {in} (its layout IS
#   derivable from the image), so nothing in the engine can guess which one
#   wants what: the manifest is the contract, and this suite asserts it.
#
#   fixtures (generated here, no external images):
#     z.qcow2   32 MiB, qemu-img convert -c -o compression_type=zlib.
#               ~135 reproducible clusters -> ~135 REPRO map entries.
#     s.qcow2   the same bytes through -o compression_type=zstd: the twin
#               that MUST stay declined, at pack gate 1 (the qcow2 v3
#               incompatible_features "compression type" bit), which is a
#               header-parse check and is nowhere near the map.
#   The raw source is a multiple of the 64 KiB cluster on purpose: the pack
#   declines an image whose virtual_size is not cluster-aligned (its last
#   partial guest cluster has no full cluster of backing), which is a
#   fixture property, not the thing under test.
#
# LEGS
#   manifest contract -> sweep decomposes -> bit-exact member == qemu-img
#   convert -O raw -> bit-exact container (direct + whole file in windows) ->
#   ranged reads across the MRM2 splice (REPRO entries actually regenerate) ->
#   the zstd twin is declined at gate 1 with no siblings and reads back
#   verbatim -> sweep 2 does not re-fire -> NEGATIVE 1: a private pack copy
#   with `map {in}` restored goes back to "decomposition abandoned" ->
#   NEGATIVE 2: a private pack copy whose map asks for a re-deflation over
#   CPACK_REPRO_MAX is declined with the re-deflate bound named, writes no
#   sibling and leaves the file bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-qcow2-zlib.sh
# $QCOW2_PACK overrides the pack directory (default:
# $REPO/tools/codecpacks/qcow2.codecpack). Uses /dev/shm like the other
# suites: relative image paths, because blkio treats /dev/* as raw devices.
# The pack is OPTIONAL by design (AGENTS.md §2.8): a host that cannot build
# or run it skips this suite, it does not fail it -- see the gate below.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
B=$REPO/bin
PACK="${QCOW2_PACK:-$REPO/tools/codecpacks/qcow2.codecpack}"

# tools/test-fuzz.sh's model: a check that cannot run says so and skips. A
# suite that always fails tells nobody anything -- and this one is optional by
# design, so a host without the encoder gets a note, not a red build.
skip() { echo "NOTE: skipping tools/test-qcow2-zlib.sh -- $*"; exit 0; }

echo "== can this host run the suite at all? =="
command -v cc       >/dev/null 2>&1 || skip "cc is not installed"
command -v python3  >/dev/null 2>&1 || skip "python3 is not installed"
command -v qemu-img >/dev/null 2>&1 || skip "qemu-img is not installed"
[ -f "$PACK/qcow2.c" ] || skip "no pack source at $PACK"
OBJ="$REPO/build/obj"
STOCK_OBJS=("$OBJ"/zlib_stock_*.o)
for f in "$OBJ/deflate_repro.o" "$OBJ/deflate_backend_system.o" \
         "$OBJ/deflate_backend_stock.o" "${STOCK_OBJS[0]}"; do
    [ -f "$f" ] || skip "the tree is not built (missing $f) -- run make first"
done
[ -f "$REPO/build/core_objs.txt" ] \
    || skip "the tree is not built (no build/core_objs.txt) -- run make first"
for b in invf-mkfs invf-cp invf-sweep invf-cat invf-ls invf-verify; do
    [ -x "$B/$b" ] || skip "the tree is not built (missing $B/$b) -- run make first"
done
# blkio treats /dev/* as raw devices, so this suite's images MUST live in
# /dev/shm. Gate the capability, not the path's existence.
( mkdir -p /dev/shm/wp107probe && rm -rf /dev/shm/wp107probe ) 2>/dev/null \
    || skip "/dev/shm is not writable and this suite's images must live there"
echo "  qemu-img: $(qemu-img --version | head -1)"
echo "  pack:     $PACK"
# The pack needs the ENCODER, not just qemu-img: a qemu built without the zlib
# deflate method cannot make the -c compression_type=zlib image this suite
# exists for, and the fixture would then be declined for a reason that has
# nothing to do with the lane.
qemu-img convert -c -O qcow2 -o compression_type=zlib "$REPO/AGENTS.md" \
    /dev/shm/wp107probe.qcow2 >/dev/null 2>&1 \
    || skip "qemu-img cannot convert with -c compression_type=zlib"
rm -f /dev/shm/wp107probe.qcow2

WORK=/dev/shm/wp107qcow2
trap 'rm -rf "$WORK" /dev/shm/wp107qcow2*.img /dev/shm/wp107probe*' EXIT
rm -rf "$WORK"
mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" "$WORK/packs" "$WORK/nbad" "$WORK/nrepro"
cd /dev/shm
rm -f wp107qcow2*.img

# -I$REPO/src/include is what lets a copy of the pack source that lives OUTSIDE
# the tree still resolve its ivpack/deflate headers (qcow2.c picks them with
# __has_include). For the in-tree build both branches name the same headers.
build_pack() {   # <qcow2.c> <out-binary>; warnings on stdout, build on stderr
    local src=$1 out=$2 tag
    tag=$(basename "$out")
    cc -std=c11 -O2 -Wall -Wextra -Werror -I"$REPO/src/codecs" \
       -I"$REPO/src/zlib" -I"$REPO/src/include" -DZ_PREFIX -c "$src" \
       -o "$WORK/$tag.o" &&
    cc -o "$out" "$WORK/$tag.o" \
       "$OBJ/deflate_repro.o" "$OBJ/deflate_backend_system.o" \
       "$OBJ/deflate_backend_stock.o" "${STOCK_OBJS[@]}" -lz -ldl
}

echo "== build the pack (the -Wall -Wextra -Werror gate) =="
# A warning here is an in-tree defect, not an absent capability, so this stays
# a FAIL where the probe above skips.
WARN=$( build_pack "$PACK/qcow2.c" "$WORK/bin_qcow2" 2>&1 ) \
    || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }

echo "== pack dir: a private registry-shaped copy (the engine scans *.codecpack) =="
# WP220: --no-preserve=ownership, for the reason given at length in
# tools/test-qcow2.sh (this is its twin; the reasoning is identical and is not
# repeated here). `cp -a` preserves ownership, this suite runs ISOLATED under
# `unshare -r`, and a user namespace's uid map holds one uid -- so cp fails:
#   cp: failed to preserve ownership for '.../qcow2.codecpack/bin': Invalid argument
# The fixture copy needs contents and modes, never ownership.
cp -a --no-preserve=ownership "$PACK" "$WORK/packs/qcow2.codecpack"
mkdir -p "$WORK/packs/qcow2.codecpack/bin"
cp "$WORK/bin_qcow2" "$WORK/packs/qcow2.codecpack/bin/qcow2"
rm -f "$WORK/bin_qcow2"
# The negative-test pack: identical, with the map template reverted to {in}.
cp -a --no-preserve=ownership "$WORK/packs/qcow2.codecpack" "$WORK/nbad/qcow2.codecpack"
sed -i 's|^map = .*|map = bin/qcow2 map {in} {out}|' "$WORK/nbad/qcow2.codecpack/manifest"

echo "== MANIFEST CONTRACT (the thing WP107 fixed) =="
MAPLINE=$(grep '^map *=' "$WORK/packs/qcow2.codecpack/manifest" | head -1)
DGLINE=$(grep '^decomp_gen *=' "$WORK/packs/qcow2.codecpack/manifest" | head -1)
echo "  $MAPLINE"
echo "  ${DGLINE:-(decomp_gen absent)}"
case "$MAPLINE" in
  *'{recipe}'*) ;;
  *) echo "FAIL: the qcow2 map must render from {recipe}."
     echo "      The engine passes the scratch image as {in} and the strip"
     echo "      output as {recipe}; a reproducible compressed cluster is"
     echo "      only nameable from the recipe, so {in} makes the pack emit"
     echo "      a v1 MRMP map whose recipe offsets cannot validate and the"
     echo "      sweep abandons every compressed image. See WP107."
     exit 1 ;;
esac
case "$DGLINE" in
  *"1"*) ;;
  *) echo "FAIL: qcow2 must declare decomp_gen = 1 (the FS owns the map"
     echo "      header's generation field; without the opt-in a pack"
     echo "      generation bump never migrates a stored decomposition)."
     exit 1 ;;
esac
export INVFS_CODECPACKS=$WORK/packs   # the sweep AND the reads

echo "== fixtures: a zlib image and its zstd twin =="
python3 - "$WORK/orig/src.raw" <<'PY'
import sys
d = b''
for i in range(4000):
    d += (b'The quick brown fox jumps over the lazy dog %d\n' % i) * 40
d += bytes(range(256)) * 4096
d = d[:16 << 20]
d += b'\0' * ((32 << 20) - len(d))     # cluster-aligned: the pack declines
open(sys.argv[1], 'wb').write(d)        # a non-aligned virtual_size
PY
[ $(( $(stat -c %s "$WORK/orig/src.raw") % 65536 )) -eq 0 ] \
    || { echo "FAIL: fixture must be a multiple of the 64K cluster"; exit 1; }
qemu-img convert -c -O qcow2 -o compression_type=zlib \
    "$WORK/orig/src.raw" "$WORK/orig/z.qcow2" >/dev/null
qemu-img convert -c -O qcow2 -o compression_type=zstd \
    "$WORK/orig/src.raw" "$WORK/orig/s.qcow2" >/dev/null
qemu-img convert -O raw "$WORK/orig/z.qcow2" "$WORK/orig/z.ref" >/dev/null
ls -la "$WORK/orig"/z.qcow2 "$WORK/orig"/s.qcow2 \
       "$WORK/orig"/src.raw | awk '{print "  "$9" "$5" bytes"}'

echo "== pack-level: the zlib image maps to MRM2 with REPRO entries =="
PK=$WORK/packs/qcow2.codecpack/bin/qcow2
"$PK" enumerate "$WORK/orig/z.qcow2" "$WORK/out/tbl" >/dev/null \
    || { echo "FAIL: the pack declined the zlib image at enumerate"; exit 1; }
grep -q "^1	diskimg	$((32 * 1024 * 1024))$" "$WORK/out/tbl" \
    || { echo "FAIL: member table wrong: $(cat "$WORK/out/tbl")"; exit 1; }
"$PK" strip "$WORK/orig/z.qcow2" "$WORK/out/r.bin" >/dev/null
"$PK" map "$WORK/out/r.bin" "$WORK/out/m.bin" >/dev/null
python3 - "$WORK/out/m.bin" <<'PY'
import collections, struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:4] == b'MRM2', b[:4]
n = struct.unpack_from('<I', b, 4)[0]
assert len(b) == 12 + 40 * n, (len(b), 12 + 40 * n)
kinds = collections.Counter(b[12 + i * 40 + 16] for i in range(n))
print('  MRM2: %d entries, kinds %s' % (n, dict(kinds)))
assert kinds[2] > 0, 'no REPRO entries: the fixture does not exercise the lane'
PY
echo "  (kind 2 = REPRO: a raw member range plus the deflate parameters)"
# The Q2R3 recipe of a -c image carries every deflate stream, so the sweep's
# decomposition RE-deflates on read; the proofs that used to ride on the stored
# decomposition (the Q2R3 rebuild == source, diskimg == qemu-img) run at the
# pack level here, which never consults the size guard, and again through the
# volume in the FS leg below.
mkdir -p "$WORK/out/mbr"
for i in 1 2; do
    "$PK" extract "$WORK/orig/z.qcow2" $i "$WORK/out/mbr/$i" >/dev/null \
        || { echo "FAIL: extract z.qcow2 member $i"; exit 1; }
done
"$PK" rebuild "$WORK/out/r.bin" "$WORK/out/mbr" "$WORK/out/rebuilt.qcow2" >/dev/null \
    || { echo "FAIL: rebuild from the Q2R3 recipe + members"; exit 1; }
cmp -s "$WORK/orig/z.qcow2" "$WORK/out/rebuilt.qcow2" \
    || { echo "FAIL: the Q2R3 rebuild (REPRO regen) is not bit-exact"; exit 1; }
cmp -s "$WORK/orig/z.ref" "$WORK/out/mbr/1" \
    || { echo "FAIL: the diskimg member is not bit-exact vs qemu-img"; exit 1; }
echo "  Q2R3 rebuild (every REPRO range re-deflated) == source"
echo "  member 1 (diskimg) == qemu-img convert -O raw: $(sha256sum "$WORK/out/mbr/1" | cut -d' ' -f1)"

echo "== pack-level: the zstd twin is declined AT GATE 1 =="
# Gate 1 is the qcow2 v3 header's incompatible_features "compression type"
# bit (qcow2.c: ***** GATE 1 -- THE ZSTD GATE *****, the `incompat != 0`
# refusal at header parse). The two fixtures are the same bytes through the
# same converter and differ in exactly that field, so: the twin sets it and
# is refused, the zlib image clears it and is admitted. Read the field here
# rather than trusting a trace string, so this leg does not depend on which
# qcow2.c revision the pack directory carries.
INCOMPAT=$(python3 - "$WORK/orig/z.qcow2" "$WORK/orig/s.qcow2" <<'PY'
import sys
out = []
for p in sys.argv[1:]:
    h = open(p, 'rb').read(112)
    assert h[:4] == b'QFI\xFB', p
    assert int.from_bytes(h[4:8], 'big') == 3, p
    out.append('%#x' % int.from_bytes(h[72:80], 'big'))
print(' '.join(out))
PY
)
set -- $INCOMPAT
ZINCOMPAT=$1; SINCOMPAT=$2
echo "  incompatible_features: zlib=$ZINCOMPAT zstd=$SINCOMPAT"
[ "$ZINCOMPAT" = "0x0" ] \
    || { echo "FAIL: the zlib fixture must have incompatible_features == 0"; exit 1; }
[ $(( SINCOMPAT )) -ne 0 ] \
    || { echo "FAIL: the zstd twin must set the compression-type bit"; exit 1; }
[ $(( SINCOMPAT & 8 )) -ne 0 ] \
    || { echo "FAIL: expected incompatible_features bit 3 (compression type)"; exit 1; }
set +e
"$PK" enumerate "$WORK/orig/s.qcow2" "$WORK/out/tbl2" >/dev/null
RC=$?
set -e
[ "$RC" -eq 3 ] || { echo "FAIL: zstd twin: expected exit 3, got $RC"; exit 1; }
INVFS_QCOW2_TRACE=1 "$PK" enumerate "$WORK/orig/s.qcow2" "$WORK/out/tbl2" \
    >/dev/null 2>"$WORK/out/trace-s" || true
grep "gate1" "$WORK/out/trace-s" | sed 's/^/  /' || true
echo "  exit 3 at gate 1, before the L2 walk and long before the map"

echo "== build the ranged-read helper =="
cat > "$WORK/rngread.c" <<'C'
/* rngread.c — ranged reads through vol_read_range (the seekable-container
 * local-splice read path). len < 0 reads the whole file in windows. */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id, off, fsz = 0, chunk;
    long long len;
    FILE *o;
    uint8_t *buf;
    int rc = 1;

    if (argc < 6) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { fprintf(stderr, "not found: %s\n", argv[2]); goto out; }
    off = strtoull(argv[3], NULL, 0);
    len = strtoll(argv[4], NULL, 0);
    chunk = (argc > 6) ? strtoull(argv[6], NULL, 0) : 65536;
    if (!chunk) chunk = 65536;
    o = fopen(argv[5], "wb");
    if (!o) goto out;
    buf = (uint8_t *)malloc(chunk);
    if (!buf) { fclose(o); goto out; }
    if (vol_stat_full(v, argv[2], NULL, &fsz, NULL) != 0) goto free_out;
    if (len >= 0) {
        if (off + (uint64_t)len > fsz) len = (long long)(fsz > off ? fsz - off : 0);
        fsz = off + (uint64_t)len;
    }
    while (off < fsz) {
        uint64_t want = fsz - off < chunk ? fsz - off : chunk;
        int got = vol_read_range(v, id, off, (size_t)want, buf);
        if (got <= 0) { fprintf(stderr, "read failed at %llu\n",
                                (unsigned long long)off); goto free_out; }
        if (fwrite(buf, 1, (size_t)got, o) != (size_t)got) goto free_out;
        off += (uint64_t)got;
    }
    rc = 0;
free_out:
    free(buf);
    fclose(o);
out:
    vol_close(v);
    return rc;
}
C
# Linking rngread against the core objects needs libzstd.so.1 by SONAME; a host
# without it cannot run this leg, which is a skip and not a failure.
CORE_O="$(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt")"
cc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs \
   -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread 2>"$WORK/out/rngread.err" \
    || skip "cannot link the ranged-read helper against the core objects ($(tail -1 "$WORK/out/rngread.err"))"

echo "== FS leg: sweep a fresh volume (PASS arm -- this image MUST decompose) =="
# WP119 retired the opposite assertion. The guard ACCEPTS this shape: measured
# on this tree, orig=377344 projected=359605 -- a 17,739 B gain -- with
# repro_max=65536 against a 4 MiB bound. A content projection cannot see the
# cost of a kind-2 entry at all (it stores nothing), so a re-deflate-heavy
# strip reads as nearly free and the re-deflate bound is the lever that
# actually constrains this shape. NEGATIVE 2 below is that lever.
IMG=wp107qcow2.img
"$B/invf-mkfs" "$IMG" 1 >/dev/null 2>&1
"$B/invf-cp" "$IMG" "$WORK/orig/z.qcow2" z.qcow2 >/dev/null
"$B/invf-cp" "$IMG" "$WORK/orig/s.qcow2" s.qcow2 >/dev/null
"$B/invf-sweep" "$IMG" > "$WORK/out/sweep1.log" 2>&1 \
    || { echo "FAIL: sweep failed"; cat "$WORK/out/sweep1.log"; exit 1; }
if grep -q "decomposition abandoned" "$WORK/out/sweep1.log"; then
    echo "FAIL: a zlib qcow2 was abandoned:"
    grep "sweep:" "$WORK/out/sweep1.log"
    exit 1
fi
grep -q "  z.qcow2: qcow2 (codecpack)" "$WORK/out/sweep1.log" \
    || { echo "FAIL: the zlib image was NOT decomposed (this is the pass arm):"; cat "$WORK/out/sweep1.log"; exit 1; }
if grep -q "size guard refused.*z\.qcow2" "$WORK/out/sweep1.log"; then
    echo "FAIL: the size guard declined the zlib image, which is the pass arm:"; cat "$WORK/out/sweep1.log"; exit 1
fi
grep "  z.qcow2: qcow2 (codecpack)" "$WORK/out/sweep1.log" | sed 's/^sweep: /  /'
grep -q "  s.qcow2: qcow2 (codecpack)" "$WORK/out/sweep1.log" \
    && { echo "FAIL: the zstd twin was decomposed"; exit 1; }
echo "  z.qcow2 decomposed, s.qcow2 declined at gate 1, no abandonment"
echo "  z.qcow2 gained its decomposition siblings:"
for s in 'mbr0001-diskimg' 'mbr0002-rankimg' 'mbrmap' 'mbrt'; do
    "$B/invf-cat" "$IMG" "z.qcow2!$s" "$WORK/out/nope" >/dev/null 2>&1 \
        || { echo "FAIL: the decomposed image has no sibling z.qcow2!$s"; cat "$WORK/out/sweep1.log"; exit 1; }
    echo "    z.qcow2!$s"
done
# ...and the set is EXACTLY that. A decomposition committed in part (the map
# without its member siblings, or the reverse) would leave the container
# reading back as a hole, and the per-sibling checks above cannot see it: each
# one only proves its own name is there.
"$B/invf-ls" "$IMG" 2>/dev/null | awk '{print $NF}' \
    | grep '^z\.qcow2!' | LC_ALL=C sort > "$WORK/out/sibs.txt"
printf 'z.qcow2!mbr0001-diskimg\nz.qcow2!mbr0002-rankimg\nz.qcow2!mbrmap\nz.qcow2!mbrt\n' \
    > "$WORK/out/sibs.want"
cmp -s "$WORK/out/sibs.txt" "$WORK/out/sibs.want" \
    || { echo "FAIL: the decomposition's sibling set is not what the sweep commits:"; \
         diff "$WORK/out/sibs.want" "$WORK/out/sibs.txt" | sed 's/^/  /'; exit 1; }
echo "  s.qcow2 gained no sibling (a gate-1 refusal writes nothing):"
for s in '!mbr0001-diskimg' '!mbr0002-rankimg' '!mbrmap' '!mbrt'; do
    "$B/invf-cat" "$IMG" "s.qcow2$s" "$WORK/out/nope" >/dev/null 2>&1 \
        && { echo "FAIL: declined s.qcow2 grew sibling s.qcow2$s"; exit 1; }
done
echo "    (none)"

echo "== BIT-EXACTNESS: the decomposed container reads back, direct and in windows =="
"$B/invf-cat" "$IMG" z.qcow2 "$WORK/out/back.qcow2" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/back.qcow2" \
    || { echo "FAIL: the container drifted on a whole read"; exit 1; }
echo "  whole read:        $(sha256sum "$WORK/out/back.qcow2" | cut -d' ' -f1)"
echo "  source:            $(sha256sum "$WORK/orig/z.qcow2" | cut -d' ' -f1)"
"$WORK/rngread" "$IMG" z.qcow2 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/whole.rng" \
    || { echo "FAIL: the container drifted read in 64K windows"; exit 1; }
echo "  64K windows:       $(sha256sum "$WORK/out/whole.rng" | cut -d' ' -f1)"
# ...and the member sibling, which is the other half of the splice: every
# kind-2 range on that path is re-deflated on the fly.
"$B/invf-cat" "$IMG" 'z.qcow2!mbr0001-diskimg' "$WORK/out/back.diskimg" >/dev/null
cmp "$WORK/orig/z.ref" "$WORK/out/back.diskimg" \
    || { echo "FAIL: the diskimg member drifted from qemu-img -O raw"; exit 1; }
echo "  diskimg member:    $(sha256sum "$WORK/out/back.diskimg" | cut -d' ' -f1)  (== qemu-img)"

echo "== the declined zstd twin is still bit-exact (stored verbatim) =="
"$B/invf-cat" "$IMG" s.qcow2 "$WORK/out/back-s.qcow2" >/dev/null
cmp "$WORK/orig/s.qcow2" "$WORK/out/back-s.qcow2" \
    || { echo "FAIL: the declined zstd twin drifted"; exit 1; }
echo "  $(sha256sum "$WORK/out/back-s.qcow2" | cut -d' ' -f1)  (== source)"

echo "== sweep 2 does not re-fire, and everything is still exact =="
"$B/invf-sweep" "$IMG" > "$WORK/out/sweep2.log" 2>&1 \
    || { echo "FAIL: sweep 2 failed"; cat "$WORK/out/sweep2.log"; exit 1; }
if grep -q "qcow2 (codecpack)" "$WORK/out/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/out/sweep2.log"; exit 1
fi
"$B/invf-cat" "$IMG" z.qcow2 "$WORK/out/s2.qcow2" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/s2.qcow2" \
    || { echo "FAIL: the container drifted after sweep 2"; exit 1; }
"$B/invf-verify" "$IMG" --deep 2>&1 | grep -q " 0 corrupt," \
    || { echo "FAIL: corruption after sweep 2"; "$B/invf-verify" "$IMG" --deep; exit 1; }
echo "  stable and bit-exact"

echo "== NEGATIVE 1: map {in} must go back to being abandoned =="
# The fix is proven by its failure: with the template reverted, the pack
# renders a v1 MRMP map whose recipe offsets do not fit the recipe strip
# actually wrote, cpack_map_validate refuses it, and the sweep abandons the
# decomposition. A validator that accepted this would be the bug.
BADIMG=wp107qcow2bad.img
INVFS_CODECPACKS=$WORK/nbad "$B/invf-mkfs" "$BADIMG" 1 >/dev/null 2>&1
INVFS_CODECPACKS=$WORK/nbad "$B/invf-cp" "$BADIMG" "$WORK/orig/z.qcow2" z.qcow2 >/dev/null
INVFS_CODECPACKS=$WORK/nbad "$B/invf-sweep" "$BADIMG" > "$WORK/out/sweep-bad.log" 2>&1
grep -q "map does not partition the container (.*), decomposition abandoned" \
    "$WORK/out/sweep-bad.log" \
    || { echo "FAIL: map {in} did not reproduce the abandonment:"; cat "$WORK/out/sweep-bad.log"; exit 1; }
INVFS_CODECPACKS=$WORK/nbad "$B/invf-cat" "$BADIMG" 'z.qcow2!mbrmap' "$WORK/out/x" \
    >/dev/null 2>&1 && { echo "FAIL: the abandoned run left a map sibling"; exit 1; }
INVFS_CODECPACKS=$WORK/nbad "$B/invf-cat" "$BADIMG" z.qcow2 "$WORK/out/bad-back.qcow2" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/bad-back.qcow2" \
    || { echo "FAIL: the abandoned image is not still bit-exact"; exit 1; }
rm -f "$BADIMG"
echo "  map {in} -> 'decomposition abandoned', no siblings, file still bit-exact"

echo "== NEGATIVE 2: a kind-2 entry over CPACK_REPRO_MAX must be declined =="
# The lever that actually constrains this shape. A kind-2 (REPRO) entry stores
# nothing and re-deflates its whole raw_len on every read request that touches
# it, so its cost is quadratic in the member size and INVISIBLE to any content
# projection: cpack_repro_stats (vol_cpack.c:2510) skips kind-2 entries, which
# is precisely why the PASS arm above decomposes an image whose 23,040 B of
# kind-2 members cost the volume nothing at all. CPACK_REPRO_MAX (4 MiB,
# src/core/volume_internal.h:2028) is the only thing between that
# CPU-for-space trade and a silent regression, so THAT is what a negative
# control has to trip.
#
# The stock pack cannot render such a map: qcow2 caps cluster_bits at 21, so a
# kind-2 raw_len tops out at 2 MiB -- half the bound, by design, so no
# conformant image is ever refused. The fixture is therefore a private copy of
# the pack, exactly like the {in} copy above, whose map renders
# raw_len = CPACK_REPRO_MAX + 1.
NREP=$WORK/nrepro/qcow2.codecpack
mkdir -p "$NREP/bin"
cp "$PACK/manifest" "$NREP/manifest"
sed 's|t->raw_len = (uint32_t)cs;|t->raw_len = (uint32_t)(4194304u + 1u);|' \
    "$PACK/qcow2.c" > "$WORK/nrepro/qcow2.c"
grep -q 't->raw_len = (uint32_t)(4194304u + 1u);' "$WORK/nrepro/qcow2.c" \
    || { echo "FAIL: the fixture patch did not apply (qcow2.c changed shape)"; exit 1; }
WARN=$( build_pack "$WORK/nrepro/qcow2.c" "$NREP/bin/qcow2" 2>&1 ) \
    || { echo "FAIL: the oversized-repro pack did not build"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: the oversized-repro pack is not warning-clean:"; echo "$WARN"; exit 1; }
# Prove the fixture trips the bound and nothing else: the doctored map must be
# the stock map entry for entry, differing only in a kind-2 raw_len that is now
# over the bound.
NPK=$NREP/bin/qcow2
"$NPK" strip "$WORK/orig/z.qcow2" "$WORK/out/nrepro.r.bin" >/dev/null
"$NPK" map "$WORK/out/nrepro.r.bin" "$WORK/out/nrepro.m.bin" >/dev/null
python3 - "$WORK/out/m.bin" "$WORK/out/nrepro.m.bin" <<'PY'
import struct, sys
REPRO_MAX = 4 << 20

def entries(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'MRM2', (path, b[:4])
    n = struct.unpack_from('<I', b, 4)[0]
    assert len(b) == 12 + 40 * n, (path, len(b), 12 + 40 * n)
    out = []
    for i in range(n):
        p = b[12 + i * 40: 52 + i * 40]
        orig_off, ln = struct.unpack_from('<QQ', p, 0)
        out.append((orig_off, ln, p[16], struct.unpack_from('<I', p, 17)[0],
                    struct.unpack_from('<Q', p, 21)[0],
                    struct.unpack_from('<I', p, 29)[0]))
    return out

stock, bad = entries(sys.argv[1]), entries(sys.argv[2])
assert len(stock) == len(bad), (len(stock), len(bad))
over = 0
for s, d in zip(stock, bad):
    assert s[:5] == d[:5], ('the fixture changed the map shape', s, d)
    if d[2] == 2 and d[5] > REPRO_MAX:
        over += 1
assert over, 'no kind-2 entry over CPACK_REPRO_MAX: the fixture does not trip the bound'
print('  fixture: %d of %d kind-2 entries now ask for a re-deflation over %d B'
      ' (the largest %d B; the stock map\'s largest was %d B)'
      % (over, sum(1 for e in bad if e[2] == 2), REPRO_MAX,
         max(e[5] for e in bad), max(e[5] for e in stock)))
PY
NIMG=wp107qcow2nrepro.img
INVFS_CODECPACKS=$WORK/nrepro "$B/invf-mkfs" "$NIMG" 1 >/dev/null 2>&1
INVFS_CODECPACKS=$WORK/nrepro "$B/invf-cp" "$NIMG" "$WORK/orig/z.qcow2" z.qcow2 >/dev/null
INVFS_CODECPACKS=$WORK/nrepro "$B/invf-sweep" "$NIMG" > "$WORK/out/sweep-nrepro.log" 2>&1
# The bound is enforced by cpack_map_validate (vol_cpack.c:2268), which runs
# BEFORE the size guard, so the refusal reaches the log as a map-shape one.
# What this asserts is that the reason names the re-deflation -- not that the
# message happened to arrive from this layer rather than the next one down.
grep -q "a kind-2 entry re-deflates more than the read path bounds" \
    "$WORK/out/sweep-nrepro.log" \
    || { echo "FAIL: an over-CPACK_REPRO_MAX kind-2 entry was NOT refused for the re-deflate bound:"; cat "$WORK/out/sweep-nrepro.log"; exit 1; }
grep -q "decomposition abandoned" "$WORK/out/sweep-nrepro.log" \
    || { echo "FAIL: the over-bound map was not abandoned"; cat "$WORK/out/sweep-nrepro.log"; exit 1; }
if grep -q "qcow2 (codecpack)" "$WORK/out/sweep-nrepro.log"; then
    echo "FAIL: the over-bound map was decomposed anyway"; cat "$WORK/out/sweep-nrepro.log"; exit 1
fi
grep "kind-2 entry re-deflates" "$WORK/out/sweep-nrepro.log" | sed 's/^sweep: /  /'
echo "  refused on the re-deflate bound, and it wrote nothing:"
for s in 'mbr0001-diskimg' 'mbr0002-rankimg' 'mbrmap' 'mbrt'; do
    INVFS_CODECPACKS=$WORK/nrepro "$B/invf-cat" "$NIMG" "z.qcow2!$s" "$WORK/out/nope" \
        >/dev/null 2>&1 && { echo "FAIL: the refused run left a sibling z.qcow2!$s"; exit 1; }
done
echo "    (none)"
INVFS_CODECPACKS=$WORK/nrepro "$B/invf-cat" "$NIMG" z.qcow2 "$WORK/out/nrepro-back.qcow2" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/nrepro-back.qcow2" \
    || { echo "FAIL: the refused image is not still bit-exact"; exit 1; }
rm -f "$NIMG"
echo "  and the declined image is still bit-exact"
echo
echo "PASS: the qcow2 pack maps zlib clusters to MRM2 with REPRO entries and"
echo "      rebuilds the container bit-exactly; the sweep DECOMPOSES this"
echo "      compressed-cluster image (a 17.7 KB gain, re-deflating at most"
echo "      64 KiB against a 4 MiB bound) and refuses it when a kind-2 entry"
echo "      asks for more, naming the re-deflate bound; the manifest contract"
echo "      and the map validator are still asserted."