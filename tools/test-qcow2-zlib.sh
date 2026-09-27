#!/bin/bash
# test-qcow2-zlib.sh — the qcow2 codecpack's ZLIB-COMPRESSED-CLUSTER lane
# end to end, and the manifest contract that lane depends on (WP107).
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
#   verbatim -> sweep 2 does not re-fire -> NEGATIVE: a private pack copy
#   with `map {in}` restored goes back to "decomposition abandoned".
#
# Run from the repo root after `make`:  bash tools/test-qcow2-zlib.sh
# $QCOW2_PACK overrides the pack directory (default:
# $REPO/tools/codecpacks/qcow2.codecpack). Uses /dev/shm like the other
# suites: relative image paths, because blkio treats /dev/* as raw devices.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
B=$REPO/bin
PACK="${QCOW2_PACK:-$REPO/tools/codecpacks/qcow2.codecpack}"
WORK=/dev/shm/wp107qcow2
trap 'rm -rf "$WORK" /dev/shm/wp107qcow2*.img' EXIT
rm -rf "$WORK"
mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" "$WORK/packs" "$WORK/nbad"
cd /dev/shm
rm -f wp107qcow2*.img

echo "== tools =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
command -v qemu-img >/dev/null || { echo "SKIP: qemu-img not installed"; exit 0; }
[ -f "$PACK/qcow2.c" ] || { echo "SKIP: no pack source at $PACK"; exit 0; }
echo "  qemu-img: $(qemu-img --version | head -1)"
echo "  pack:     $PACK"

echo "== build the pack (the -Wall -Wextra -Werror gate) =="
OBJ="$REPO/build/obj"
STOCK_OBJS=("$OBJ"/zlib_stock_*.o)
for f in "$OBJ/deflate_repro.o" "$OBJ/deflate_backend_system.o" \
         "$OBJ/deflate_backend_stock.o" "${STOCK_OBJS[0]}"; do
    [ -f "$f" ] || { echo "FAIL: run make first (missing $f)"; exit 1; }
done
WARN=$( {
    cc -std=c11 -O2 -Wall -Wextra -Werror -I"$REPO/src/codecs" \
       -I"$REPO/src/zlib" -DZ_PREFIX -c "$PACK/qcow2.c" \
       -o "$WORK/qcow2.o" &&
    cc -o "$WORK/packs/bin_qcow2" "$WORK/qcow2.o" \
       "$OBJ/deflate_repro.o" "$OBJ/deflate_backend_system.o" \
       "$OBJ/deflate_backend_stock.o" "${STOCK_OBJS[@]}" -lz -ldl
} 2>&1 ) || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }

echo "== pack dir: a private registry-shaped copy (the engine scans *.codecpack) =="
cp -a "$PACK" "$WORK/packs/qcow2.codecpack"
mkdir -p "$WORK/packs/qcow2.codecpack/bin"
cp "$WORK/packs/bin_qcow2" "$WORK/packs/qcow2.codecpack/bin/qcow2"
rm -f "$WORK/packs/bin_qcow2"
# The negative-test pack: identical, with the map template reverted to {in}.
cp -a "$WORK/packs/qcow2.codecpack" "$WORK/nbad/qcow2.codecpack"
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
CORE_O="$(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt")"
cc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs \
   -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== FS leg: sweep a fresh volume =="
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
    || { echo "FAIL: the zlib image was not decomposed by the qcow2 pack:"; cat "$WORK/out/sweep1.log"; exit 1; }
grep -q "  s.qcow2: qcow2 (codecpack)" "$WORK/out/sweep1.log" \
    && { echo "FAIL: the zstd twin was decomposed"; exit 1; }
echo "  z.qcow2 decomposed, s.qcow2 declined, no abandonment"

echo "== siblings =="
for s in '!mbr0001-diskimg' '!mbrt' '!mbrmap'; do
    "$B/invf-cat" "$IMG" "z.qcow2$s" "$WORK/out/sib" >/dev/null 2>&1 \
        || { echo "FAIL: missing sibling z.qcow2$s"; exit 1; }
done
"$B/invf-cat" "$IMG" 'z.qcow2!mbrt' "$WORK/out/mbrt" >/dev/null
# The diskimg row is the invariant every qcow2 pack revision shares (it is
# the full virtual-size guest image). The row COUNT is not: the pre-WP100
# revision also publishes the compacted rankimg stream beside it. Assert the
# row, print the table.
grep -qx "1	diskimg	$((32 * 1024 * 1024))" "$WORK/out/mbrt" \
    || { echo "FAIL: member table wrong: $(cat "$WORK/out/mbrt")"; exit 1; }
sed 's/^/  mbrt: /' "$WORK/out/mbrt"
"$B/invf-cat" "$IMG" 'z.qcow2!mbrmap' "$WORK/out/map" >/dev/null
[ "$(od -A n -t x1 -N 4 "$WORK/out/map" | tr -d ' \n')" = "4d524d32" ] \
    || { echo "FAIL: the stored map is not MRM2"; exit 1; }
NREP=$(python3 -c "
import struct,sys
b=open('$WORK/out/map','rb').read(); n=struct.unpack_from('<I',b,4)[0]
print(sum(1 for i in range(n) if b[12+i*40+16]==2))")
echo "  !mbr0001-diskimg, !mbrt, !mbrmap (MRM2, $NREP REPRO entries)"
[ "$NREP" -gt 0 ] || { echo "FAIL: the stored map has no REPRO entries"; exit 1; }
echo "  the zstd twin has NO siblings:"
for s in '!mbr0001-diskimg' '!mbrt' '!mbrmap'; do
    "$B/invf-cat" "$IMG" "s.qcow2$s" "$WORK/out/nope" >/dev/null 2>&1 \
        && { echo "FAIL: the declined zstd twin grew sibling s.qcow2$s"; exit 1; }
done
echo "    (none)"

echo "== BIT-EXACTNESS: the member == qemu-img convert -O raw =="
"$B/invf-cat" "$IMG" 'z.qcow2!mbr0001-diskimg' "$WORK/out/member.raw" >/dev/null
cmp "$WORK/orig/z.ref" "$WORK/out/member.raw" \
    || { echo "FAIL: the diskimg member is not bit-exact"; exit 1; }
echo "  $(sha256sum "$WORK/out/member.raw" | cut -d' ' -f1)  (member, $((32 * 1024 * 1024)) B)"
echo "  $(sha256sum "$WORK/orig/z.ref"       | cut -d' ' -f1)  (qemu-img convert -O raw)"

echo "== BIT-EXACTNESS: the container reads back, direct and in windows =="
"$B/invf-cat" "$IMG" z.qcow2 "$WORK/out/back.qcow2" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/back.qcow2" \
    || { echo "FAIL: the container drifted on a whole read"; exit 1; }
echo "  whole read:        $(sha256sum "$WORK/out/back.qcow2" | cut -d' ' -f1)"
echo "  source:            $(sha256sum "$WORK/orig/z.qcow2" | cut -d' ' -f1)"
"$WORK/rngread" "$IMG" z.qcow2 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/whole.rng" \
    || { echo "FAIL: the container drifted read in 64K windows"; exit 1; }
echo "  64K windows:       $(sha256sum "$WORK/out/whole.rng" | cut -d' ' -f1)"

echo "== RANGED reads across the MRM2 splice (REPRO really regenerates) =="
# The windows are derived from the STORED map, not from guessed offsets: the
# head, each kind transition (recipe -> member, member -> recipe), the
# inside of a REPRO entry, and the tail. A REPRO entry that failed to
# regenerate would break these reads even though the member itself is
# intact -- which is the whole content of this leg.
RANGES=$(python3 - "$WORK/out/map" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', b, 4)[0]
e = [(struct.unpack_from('<Q', b, 12 + i * 40)[0],
      struct.unpack_from('<Q', b, 12 + i * 40 + 8)[0],
      b[12 + i * 40 + 16]) for i in range(n)]
size = e[-1][0] + e[-1][1]
out = [(0, 64)]
for i in range(1, n):
    if e[i][2] != e[i - 1][2]:                 # a kind transition
        out.append((max(0, e[i][0] - 32), 64))
    if e[i][2] == 2 and e[i][1] > 4096:        # inside a regenerable cluster
        out.append((e[i][0] + (e[i][1] - 4096) // 2, 4096))
out.append((max(0, size - 4096), min(4096, size)))
seen, uniq = set(), []
for o, l in out:
    if (o, l) in seen:
        continue
    seen.add((o, l))
    uniq.append((o, min(l, size - o)))
print(' '.join('%d:%d' % (o, l) for o, l in uniq))
PY
)
echo "  windows: $RANGES"
i=0
for spec in $RANGES; do
    i=$((i + 1))
    off=${spec%%:*}
    len=${spec##*:}
    "$WORK/rngread" "$IMG" z.qcow2 "$off" "$len" "$WORK/out/got.$i" >/dev/null
    dd if="$WORK/orig/z.qcow2" of="$WORK/out/ref.$i" bs=1 skip="$off" \
       count="$len" status=none
    cmp -s "$WORK/out/got.$i" "$WORK/out/ref.$i" \
        || { echo "FAIL: range off=$off len=$len mismatch"; exit 1; }
    echo "  off=$off len=$len OK"
done

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

echo "== NEGATIVE: map {in} must go back to being abandoned =="
# The fix is proven by its failure: with the template reverted, the pack
# renders a v1 MRMP map whose recipe offsets do not fit the recipe strip
# actually wrote, cpack_map_validate refuses it, and the sweep abandons the
# decomposition. A validator that accepted this would be the bug.
BADIMG=wp107qcow2bad.img
INVFS_CODECPACKS=$WORK/nbad "$B/invf-mkfs" "$BADIMG" 1 >/dev/null 2>&1
INVFS_CODECPACKS=$WORK/nbad "$B/invf-cp" "$BADIMG" "$WORK/orig/z.qcow2" z.qcow2 >/dev/null
INVFS_CODECPACKS=$WORK/nbad "$B/invf-sweep" "$BADIMG" > "$WORK/out/sweep-bad.log" 2>&1
grep -q "map does not partition the container, decomposition abandoned" \
    "$WORK/out/sweep-bad.log" \
    || { echo "FAIL: map {in} did not reproduce the abandonment:"; cat "$WORK/out/sweep-bad.log"; exit 1; }
INVFS_CODECPACKS=$WORK/nbad "$B/invf-cat" "$BADIMG" 'z.qcow2!mbrmap' "$WORK/out/x" \
    >/dev/null 2>&1 && { echo "FAIL: the abandoned run left a map sibling"; exit 1; }
INVFS_CODECPACKS=$WORK/nbad "$B/invf-cat" "$BADIMG" z.qcow2 "$WORK/out/bad-back.qcow2" >/dev/null
cmp "$WORK/orig/z.qcow2" "$WORK/out/bad-back.qcow2" \
    || { echo "FAIL: the abandoned image is not still bit-exact"; exit 1; }
rm -f "$BADIMG"
echo "  map {in} -> 'decomposition abandoned', no siblings, file still bit-exact"
echo
echo "PASS: qcow2 zlib clusters decompose, read back bit-exact, and the"
echo "      manifest contract that makes it work is asserted."
