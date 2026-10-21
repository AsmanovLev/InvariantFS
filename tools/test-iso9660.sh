#!/bin/bash
# test-iso9660.sh — the iso9660 containerpack (tools/codecpacks/
# iso9660.codecpack, algo 26) contract test. WP636.
#
#   fixtures: tools/codecpacks/iso9660.codecpack/mk-fixtures.py writes them
#   byte by byte (this box has no ISO writer, and the UDF walker needs
#   coverage that no file on the box provides):
#     iso.iso       primary tree: text, a real ELF, incompressible bytes, an
#                   empty file, a subdir, a two-record multi-extent file, a
#                   hard link (two records, ONE extent)
#     joliet.iso    the same extents with DIFFERENT Joliet names
#     udf.iso       UDF rev 2: short_ad files, a hole (AD type 1), a file
#                   embedded in its own File Entry (no member), empty, subdir
#     bridge.iso    UDF bridge: both trees, naming different extents
#     vrs-iso.iso   ISO tree after the UDF VRS, no UDF anchor -> the ISO
#                   fallback must skip BEA01/NSR02/TEA01
#     decline-*     broken images (no members / torn volume space / bad
#                   both-endian field / no volume descriptor): exit 3
#
#   pack-level legs (the pack is its own first arbiter, no FS involved):
#     enumerate / extract-every-member / strip / rebuild == byte-exact /
#     map (MRMP re-verified as an exact partition of [0,size)) / estimate /
#     the python ground-truth walker (verify) / declines exit 3.
#
#   FS legs: mkfs -> cp the images -> invf-sweep with INVFS_CODECPACKS
#   pointing at a PRIVATE copy of the pack dir (the repo's shared pack dir
#   is full at INVFS_PACK_MAX, and a private copy makes registration
#   deterministic) -> "iso9660 (codecpack)" per container -> siblings
#   (<name>!mbrNNNN-<member>, !mbrt, !mbrmap) -> CONTAINER{26,1} stamps ->
#   verify --deep 0 corrupt -> sha256 of every container (bit-exact) ->
#   member reads match the pack's own extract -> sweep 2 idle -> delete
#   cascade (siblings go with the container) -> fsck OK -> declines have no
#   siblings and no stamp.
#
# Run from the repo root after `make`:  bash tools/test-iso9660.sh
# Uses /dev/shm like the sibling pack tests; if it needs the e2e lock run it
# through tools/run-e2e.sh (AGENTS.md 1.5).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
B=$REPO/bin
PACK=$REPO/tools/codecpacks/iso9660.codecpack
WORK=/dev/shm/wp636iso
IMG=wp636iso.img
rm -rf "$WORK"
mkdir -p "$WORK/orig" "$WORK/out" "$WORK/packs" "$WORK/ext"
trap 'rm -rf "$WORK" /dev/shm/wp636iso*.img' EXIT
cd "$WORK"                               # relative image paths: blkio treats
                                         # /dev/* as a raw device

echo "== build the pack helper in place =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/iso9660" "$PACK/iso9660.c"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/iso9660" "$PACK/iso9660.c"
set +e
"$PACK/bin/iso9660" >/dev/null 2>&1; rc=$?
set -e
[ "$rc" = 2 ] || { echo "FAIL: helper usage exit $rc, want 2"; exit 1; }
cp -a --no-preserve=ownership "$PACK" "$WORK/packs/iso9660.codecpack"
P=$WORK/packs/iso9660.codecpack/bin/iso9660
export INVFS_CODECPACKS=$WORK/packs        # the sweep AND the reads

echo "== fixtures =="
KINDS="iso joliet udf bridge vrs-iso"
DECL="decline-nomembers decline-vss decline-blocksize decline-notcd"
for k in $KINDS $DECL; do
    python3 "$PACK/mk-fixtures.py" "$k" "$WORK/orig/$k.iso"
done

# ------------------------------------------------------------- pack legs ----
enum_count() { grep -c . "$1"; }
enum_sum()   { awk -F'\t' '{s+=$3} END{print s+0}' "$1"; }

# MRMP partition check: the map must cover [0,size) exactly once, members
# must be the image's member extents, recipe pieces must not overlap them.
mrmp_check() {   # mrmp_check <map> <image>
python3 - "$1" "$2" <<'PY'
import struct, sys
mp = open(sys.argv[1], 'rb').read()
size = len(open(sys.argv[2], 'rb').read())
assert mp[:4] == b'MRMP', 'bad MRMP magic'
n, = struct.unpack_from('<I', mp, 4)
assert len(mp) == 8 + n * 29, 'MRMP length %d != 8+29*%d' % (len(mp), n)
ent = [struct.unpack_from('<QQBIQ', mp, 8 + i * 29) for i in range(n)]
kinds = {0: 0, 1: 0}
for off, ln, kind, idx, src in ent:
    assert ln, 'zero-length entry'
    assert kind in kinds, 'bad kind %d' % kind
    kinds[kind] += 1
# the recipe's own header: magic + size + nmem + nmem*20 + nrng
rec_hdr = 4 + 8 + 4 + kinds[1] * 20 + 4
pos = 0
src_next = rec_hdr + 16                 # first range payload (16B range hdr)
for off, ln, kind, idx, src in ent:
    assert off == pos, 'gap/overlap at %d (want %d)' % (off, pos)
    assert off + ln <= size, 'entry past EOF'
    if kind == 0:
        assert src == src_next, 'recipe src_off %d != %d' % (src, src_next)
        src_next = src + ln + 16
    else:
        assert idx, 'member with index 0'
    pos = off + ln
assert pos == size, 'map ends at %d, image is %d' % (pos, size)
print('    MRMP ok: %d ranges (%d recipe / %d member), exact partition'
      % (n, kinds[0], kinds[1]))
PY
}

for k in $KINDS; do
    img=$WORK/orig/$k.iso
    echo "-- pack: $k.iso"
    $P enumerate "$img" "$WORK/out/$k.enum" || { echo "FAIL: $k enumerate"; exit 1; }
    N=$(enum_count "$WORK/out/$k.enum"); S=$(enum_sum "$WORK/out/$k.enum")
    echo "    members: $N, bytes: $S"
    [ "$N" -gt 0 ] || { echo "FAIL: $k: no members"; exit 1; }
    rm -rf "$WORK/ext/$k"; mkdir -p "$WORK/ext/$k"
    i=1
    while [ "$i" -le "$N" ]; do
        $P extract "$img" "$i" "$WORK/ext/$k/$i" >/dev/null \
            || { echo "FAIL: $k extract $i"; exit 1; }
        i=$((i + 1))
    done
    # member sizes must be exactly what enumerate printed
    awk -F'\t' '{print $1"\t"$3}' "$WORK/out/$k.enum" > "$WORK/out/$k.sz"
    while IFS=$'\t' read -r idx len; do
        got=$(stat -c %s "$WORK/ext/$k/$idx")
        [ "$got" = "$len" ] || { echo "FAIL: $k member $idx size $got != $len"; exit 1; }
    done < "$WORK/out/$k.sz"
    $P strip "$img" "$WORK/out/$k.recipe" || { echo "FAIL: $k strip"; exit 1; }
    $P rebuild "$WORK/out/$k.recipe" "$WORK/ext/$k" "$WORK/out/$k.rebuilt" \
        || { echo "FAIL: $k rebuild"; exit 1; }
    cmp -s "$img" "$WORK/out/$k.rebuilt" \
        || { echo "FAIL: $k rebuild not byte-exact"; exit 1; }
    echo "    rebuild byte-exact"
    $P map "$img" "$WORK/out/$k.map" || { echo "FAIL: $k map"; exit 1; }
    mrmp_check "$WORK/out/$k.map" "$img"
    est=$($P estimate "$img") || { echo "FAIL: $k estimate"; exit 1; }
    [ "$est" -ge $((S + 64 * 1024 * 1024)) ] \
        || { echo "FAIL: $k estimate $est < sum(usize)+64MiB"; exit 1; }
    echo "    estimate: $est"
done

echo "-- pack: ground truth (python walker) and name/extent preferences"
for k in iso joliet vrs-iso; do
    python3 "$PACK/mk-fixtures.py" verify "$WORK/orig/$k.iso" "$WORK/out/$k.enum" \
        | sed "s/^/    $k: /"
done
# Joliet: same extents, LONG names (the primary tree's 8.3 names must not win)
grep -q "the quick brown fox.txt" "$WORK/out/joliet.enum" \
    || { echo "FAIL: joliet: Joliet name missing"; exit 1; }
grep -q "TEXT.C" "$WORK/out/joliet.enum" \
    && { echo "FAIL: joliet: primary 8.3 name won"; exit 1; }
# iso vs vrs-iso: the VRS skip must recover the SAME tree
[ "$(enum_count "$WORK/out/vrs-iso.enum")" = "$(enum_count "$WORK/out/iso.enum")" ] \
    && [ "$(enum_sum "$WORK/out/vrs-iso.enum")" = "$(enum_sum "$WORK/out/iso.enum")" ] \
    || { echo "FAIL: vrs-iso tree != iso tree (VRS skip wrong)"; exit 1; }
echo "    vrs-iso recovers the iso tree across the VRS"
# bridge: UDF is authoritative -- 5 members (udf), not 7 (the ISO tree)
[ "$(enum_count "$WORK/out/bridge.enum")" = 5 ] \
    || { echo "FAIL: bridge: want the UDF tree (5 members)"; cat "$WORK/out/bridge.enum"; exit 1; }
grep -q "EMBEDDED" "$WORK/out/bridge.enum" \
    && { echo "FAIL: bridge: an embedded/no-extent file became a member"; exit 1; }
echo "    bridge: UDF extents win over the ISO tree"

echo "-- pack: declines (exit 3, and 3 means DECLINE not error)"
for k in $DECL; do
    set +e
    $P enumerate "$WORK/orig/$k.iso" "$WORK/out/$k.enum" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" = 3 ] || { echo "FAIL: $k exited $rc, want 3 (decline)"; exit 1; }
    set +e
    $P strip "$WORK/orig/$k.iso" "$WORK/out/$k.recipe" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" = 3 ] || { echo "FAIL: $k strip exited $rc, want 3"; exit 1; }
    echo "    $k: declined (3)"
done

# --------------------------------------------------------------- FS legs ----
echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 2 >/dev/null
for k in $KINDS $DECL; do
    $B/invf-cp "$IMG" "$WORK/orig/$k.iso" "$k.iso" >/dev/null
done

echo "== sweep (iso9660 containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; exit 1; }
for k in $KINDS; do
    grep -q "$k.iso: iso9660 (codecpack)" "$WORK/sweep1.log" \
        || { echo "FAIL: $k.iso not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
done
for k in $DECL; do
    grep -q "$k.iso: iso9660 (codecpack)" "$WORK/sweep1.log" \
        && { echo "FAIL: $k.iso was decomposed (must decline)"; exit 1; }
done
grep -E "iso9660 \(codecpack\)" "$WORK/sweep1.log"
grep -E "\.iso!\*: .* parts -> " "$WORK/sweep1.log" | head -6 || true

echo "== siblings =="
$B/invf-ls "$IMG" > "$WORK/ls1.log" 2>/dev/null
member_count() {  # members the pack reported for one container
    awk -F'\t' '$1 != "" {c++} END{print c+0}' "$WORK/out/$1.enum"
}
for k in $KINDS; do
    want=$(member_count "$k")
    have=$(grep -c "^ *[0-9]* bytes  inode .*  $k\.iso!mbr[0-9][0-9][0-9][0-9]-" "$WORK/ls1.log" || true)
    echo "  $k.iso: $have member sibling(s), pack said $want"
    [ "$have" = "$want" ] || { echo "FAIL: $k.iso sibling count"; cat "$WORK/ls1.log"; exit 1; }
    for s in mbrt mbrmap; do
        grep -q "$k\.iso!$s\$" "$WORK/ls1.log" \
            || { echo "FAIL: $k.iso!$s missing"; cat "$WORK/ls1.log"; exit 1; }
    done
done
for k in $DECL; do
    $B/invf-ls "$IMG" 2>/dev/null | grep -q "$k\.iso!" \
        && { echo "FAIL: declined $k.iso gained siblings"; exit 1; }
done
echo "  member/table/map siblings present; declines clean"

echo "== class stamps (CONTAINER / algo 26 / gen 1) =="
cat > "$WORK/classof.c" <<'C'
#include <stdio.h>
#include "volume.h"
#include "invarifs.h"
int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id;
    uint8_t cls, algo;
    uint16_t gen;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0)
        printf("none\n");
    else
        printf("cls=%u algo=%u gen=%u\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
CORE_O="$(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt")"
gcc -std=gnu11 -O2 -I"$REPO/src" -I"$REPO/src/core" -I"$REPO/src/codecs" \
    -I"$REPO/src/recipes" -I"$REPO/src/vendor7z" -o "$WORK/classof" \
    "$WORK/classof.c" $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
for k in $KINDS; do
    C=$("$WORK/classof" "$IMG" "$k.iso" 2>/dev/null | tail -1)
    echo "  $k.iso: $C"
    [ "$C" = "cls=3 algo=26 gen=1" ] \
        || { echo "FAIL: $k.iso: want CONTAINER{ISO9660=26,1}"; exit 1; }
done
for k in $DECL; do
    C=$("$WORK/classof" "$IMG" "$k.iso" 2>/dev/null | tail -1)
    case "$C" in *algo=26*) echo "FAIL: declined $k.iso carries a pack stamp"; exit 1;; esac
done
echo "  declines carry no pack stamp"

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep > "$WORK/verify1.log" 2>&1 \
    || { cat "$WORK/verify1.log"; exit 1; }
grep -E "files ok, .* corrupt" "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact: every container, and member reads =="
ok=1
for k in $KINDS $DECL; do
    $B/invf-cat "$IMG" "$k.iso" "$WORK/out/c.$k.iso" >/dev/null 2>&1
    a=$(sha256sum < "$WORK/orig/$k.iso")
    b=$(sha256sum < "$WORK/out/c.$k.iso")
    [ "$a" = "$b" ] || { echo "  MISMATCH container $k.iso"; ok=0; }
done
# members: the FS read path (sibling splice) must return what the pack
# itself extracted from the same image
for k in $KINDS; do
    awk -F'\t' '{print $1}' "$WORK/out/$k.enum" > "$WORK/out/$k.idx"
    while read -r idx; do
        sib=$($B/invf-ls "$IMG" 2>/dev/null | grep "  $k\.iso!mbr$(printf %04d "$idx")-" \
              | sed 's/^.*  //')
        [ -n "$sib" ] || { echo "  MISSING sibling for $k.iso member $idx"; ok=0; continue; }
        $B/invf-cat "$IMG" "$sib" "$WORK/out/m.$k.$idx" >/dev/null 2>&1
        cmp -s "$WORK/ext/$k/$idx" "$WORK/out/m.$k.$idx" \
            || { echo "  MISMATCH member $k.iso!$sib vs pack extract"; ok=0; }
    done < "$WORK/out/$k.idx"
done
[ "$ok" = 1 ] || { echo "FAIL: bit-exactness"; exit 1; }
echo "containers bit-exact; every member read matches the pack's extract"

echo "== sweep 2 (idle: the stamp holds) =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "iso9660 (codecpack)" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed an iso9660 container"; exit 1
fi
echo "  sweep 2 idle"

echo "== delete cascade (vol_unlink, the FUSE path) =="
cat > "$WORK/cbrm.c" <<'C'
#include <stdio.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc = 0;
    invfs_volume *v;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    for (int i = 2; i < argc; i++)
        if (vol_unlink(v, argv[i]) != 0) { rc = 1; fprintf(stderr, "rm %s failed\n", argv[i]); }
    vol_flush(v);
    vol_close(v);
    return rc;
}
C
gcc -std=gnu11 -O2 -I"$REPO/src" -I"$REPO/src/core" -I"$REPO/src/codecs" \
    -I"$REPO/src/recipes" -I"$REPO/src/vendor7z" -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
names=""; for k in $KINDS; do names="$names $k.iso"; done
"$WORK/cbrm" "$IMG" $names 2>/dev/null || { echo "FAIL: unlink"; exit 1; }
if $B/invf-ls "$IMG" 2>/dev/null | grep -q "\.iso!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"
    $B/invf-ls "$IMG" | head -20; exit 1
fi
echo "  containers + all members + tables + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" > "$WORK/fsck.log" 2>&1 || { cat "$WORK/fsck.log"; exit 1; }
tail -4 "$WORK/fsck.log"
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivors bit-exact (the declined images) =="
ok=1
for k in $DECL; do
    $B/invf-cat "$IMG" "$k.iso" "$WORK/out/s.$k.iso" >/dev/null 2>&1
    cmp -s "$WORK/orig/$k.iso" "$WORK/out/s.$k.iso" \
        || { echo "  MISMATCH survivor $k.iso"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "  declined images bit-exact after the deletes"

echo "ISO9660 CONTAINERPACK E2E: PASS"
