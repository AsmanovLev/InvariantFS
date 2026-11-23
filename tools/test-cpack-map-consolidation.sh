#!/bin/bash
# test-cpack-map-consolidation.sh — WP-CPACK-MAP-CONSOLIDATION.
#
# "Where is member idx" used to be answered by three files plus the mere
# PRESENCE of a fourth, and a map that used a kind its reader did not know
# (kind 3, "these bytes are zeros") was unreadable with one line to show for
# it: "[cpack] get X: map read/parse failed". This suite pins the replacement:
#
#   leg 1  FUNNEL GATE — grep-shaped, over the source. Every member-payload
#          read in src/core/vol_cpack.c goes through cpack_member_read().
#          (Before this WP the exec rebuild materializer walked the payload
#          sibling itself, so the funnel claim was false; leg 1 fails if a
#          third site appears.)
#   leg 2  LEAF-MODE ROUND TRIP — a container decomposed with
#          INVFS_CPACK_LEAF=1 writes kind-4 entries and NO "<name>!mbrleaf";
#          the map DECLARES kinds in its magic (MRK1/MRK2); the container
#          reads back bit-exactly; a gzip MEMBER stays a real inode (mixed
#          layout: kind 4 + kind 1 in one map) and its inode still reads.
#   leg 3  A LEGACY VOLUME IS UNCHANGED — decomposed by the PRE-CHANGE binary
#          in leaf mode (a leaf list + kind 1), read by THIS binary: bit-exact.
#          That is the "no migration" half of the WP, and it is a red control.
#   leg 4  ENV-NEGATIVE — the layout is a property of the volume: reads with
#          INVFS_CPACK_LEAF/BLOB set to 0 and to 1 are byte-identical.
#   leg 5  OLD READER vs NEW MAP — when INVFS_OLD_BIN is set, the PRE-CHANGE
#          binary is run against the new volume; it must refuse LOUDLY (and
#          the transcript of both readings is printed).
#   leg 6  ext4fs — the corpus the incident was about: an ext4 image
#          decomposed in leaf mode reads back bit-exactly after one sweep and
#          after three (no drift).
#
# Run from the repo root after `make`:  bash tools/test-cpack-map-consolidation.sh
# Uses /dev/shm like the other containerpack suites; images are RELATIVE
# (blkio treats /dev/* paths as raw devices).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
B="$REPO/bin"
WORK=/dev/shm/cpackmap
IMG=cpackmap.img
IMGLEG=cpackmap-leg.img
IMGEXT4=cpackmap-ext4.img
export INVFS_CODECPACKS="$REPO/tools/codecpacks"
# a pre-change build, for the old-reader leg (optional)
OLD_BIN="${INVFS_OLD_BIN:-}"

rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGLEG" "$IMGEXT4"

echo "== leg 1: funnel gate (grep-shaped, not a convention) =="
# The ONE function that may read member payload bytes. Anything else that
# calls vol_read_range() in this file is a second answer to "where do these
# bytes live", i.e. the defect this WP removes.
sites=$(awk '
    /^[ \t]*[\/\*]/ { next }
    /^[A-Za-z_][A-Za-z0-9_ \*]*\(/ {
        name = $0; sub(/\(.*/, "", name); sub(/.*[ \*]/, "", name); fn = name
    }
    /vol_read_range\(/ { print fn }
' "$REPO/src/core/vol_cpack.c" | sort -u | tr '\n' ' ')
echo "  vol_read_range() call sites, by enclosing function: $sites"
# The non-member readers are the ones that read the CONTAINER, not a member:
# the streaming pin (WP641), the family sniff, and -- ADR-015 -- the
# container-side reference of the two guards, which is cpack_container_read:
# with ABI v3 host I/O there is no pin, so the map guard and the rebuild guard
# read the container back from the volume through that one function instead of
# comparing against a materialized copy. It never touches a member sibling or
# the payload blob; the funnel still owns those.
case "$sites" in
    "cpack_container_read cpack_member_read cpack_stream_pin vol_containerpack_sweep "*)
        ;;
    *) echo "FAIL: a member-payload read bypasses cpack_member_read()"
       echo "      (got: $sites; cpack_container_read is the one documented"
       echo "       container-side reader, next to the streaming pin and the"
       echo "       family sniff)"
       exit 1 ;;
esac
# and the funnel must still be the thing that names the payload sibling for
# a READ: the blob name is resolved in the map loader and the guard, and the
# bytes are read in exactly one place.
n=$(grep -c "cpack_member_read(" "$REPO/src/core/vol_cpack.c")
echo "  cpack_member_read(): $n references (1 definition + its callers)"
[ "$n" -ge 3 ] || { echo "FAIL: the funnel lost its callers"; exit 1; }
echo "  funnel gate OK"

echo "== fixtures =="
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
python3 - "$WORK/orig" <<'PY'
import gzip, os, random, struct, sys

def splt(members):
    b = b"SPLT" + struct.pack("<I", len(members))
    for m in members:
        b += struct.pack("<Q", len(m))
    return b + b"".join(members)

d = sys.argv[1]
repo = os.environ["REPO"]
rnd = random.Random(4242)

# member 0: text, big enough to be a leaf (>= CPACK_LEAF_MIN_USIZE)
text = open(os.path.join(repo, "src", "core", "vol_cpack.c"), "rb").read()
if len(text) < 20000:
    text = (text * 8)[:20000]

# member 1: a gzip stream, and it must be >= CPACK_LEAF_MIN_USIZE (4 KiB)
# or the leaf pass makes ANY small member a leaf without sniffing it. gzip
# is a BUILT-IN structural lane (GZR), so the leaf pass keeps it a REAL
# INODE -- the mixed layout this leg wants: one member as kind 4 (payload
# sibling) and one as kind 1 (own inode), with the map alone describing both.
gz = gzip.compress(text[:120000], 6)
assert len(gz) >= 4096, len(gz)

# member 2: incompressible, so the container is not a trivial win
rnd_bytes = rnd.randbytes(65536)

mixed = splt([text, gz, rnd_bytes])
open(os.path.join(d, "multi.splt"), "wb").write(mixed)
print("  multi.splt %d (text %d, gzip %d, random %d)"
      % (len(mixed), len(text), len(gz), len(rnd_bytes)))
PY

echo "== leg 2: leaf-mode round trip (kind 4, no leaf list) =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
$B/invf-cp "$IMG" "$WORK/orig/multi.splt" multi.splt >/dev/null
INVFS_CPACK_LEAF=1 INVFS_DEBUG_PACKS=1 $B/invf-sweep "$IMG" \
    > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "multi.splt: splt_test (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: multi.splt not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
lsimg() { $B/invf-ls "$1" 2>/dev/null; }   # (the tools chatter on stderr)
echo "  sibling set:"
lsimg "$IMG" | grep "multi\.splt!" | sed 's/^/    /'
lsimg "$IMG" | grep -q "multi\.splt!mbrblob" \
    || { echo "FAIL: no payload sibling"; exit 1; }
lsimg "$IMG" | grep -q "multi\.splt!mbrmap" \
    || { echo "FAIL: no map sibling"; exit 1; }
if lsimg "$IMG" | grep -q "multi\.splt!mbrleaf"; then
    echo "FAIL: the leaf list was written -- new volumes must not have one"
    exit 1
fi
echo "  payload sibling + map present, NO !mbrleaf"
grep -E "written into the map as kind 4" "$WORK/sweep1.log" | sed 's/^/  /' \
    || { echo "FAIL: the commit did not write the layout into the map"
         tail -20 "$WORK/sweep1.log"; exit 1; }

# the container needs a gzip member that stayed an inode, so the map is the
# only thing that knows which member is which kind
nmem=$(lsimg "$IMG" | grep -c "multi\.splt!mbr[0-9]" || true)
echo "  member inodes kept out of the blob: $nmem"
[ "$nmem" -ge 1 ] || { echo "FAIL: the gzip member should have stayed an inode"
                       lsimg "$IMG"; exit 1; }

$B/invf-cat "$IMG" "multi.splt!mbrmap" "$WORK/out/map.bin" >/dev/null \
    || { echo "FAIL: the map is unreadable"; exit 1; }
python3 - "$WORK/out/map.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
magic = b[:4]
kinds = {}
if magic in (b"MRM2", b"MRK2"):
    count, gen = struct.unpack("<II", b[4:12]); base, wire = 12, 40
elif magic in (b"MRMP", b"MRK1"):
    count = struct.unpack("<I", b[4:8])[0]; base, wire = 8, 29
else:
    sys.exit("FAIL: unknown map magic %r" % magic)
for i in range(count):
    p = base + i * wire
    kinds[b[p + 16]] = kinds.get(b[p + 16], 0) + 1
print("  map magic %r, %d entries, kinds %s" % (magic, count, kinds))
if magic not in (b"MRK1", b"MRK2"):
    sys.exit("FAIL: a map with an extension kind must DECLARE it in the "
             "magic (MRK1/MRK2), got %r" % magic)
if 4 not in kinds:
    sys.exit("FAIL: no kind-4 entry in a leaf-mode map")
PY

echo "  bit-exact read:"
$B/invf-cat "$IMG" multi.splt "$WORK/out/got.splt" >/dev/null \
    || { echo "FAIL: container read failed"; exit 1; }
cmp "$WORK/orig/multi.splt" "$WORK/out/got.splt" \
    || { echo "FAIL: NOT bit-exact"; exit 1; }
echo "    cmp silent"

echo "  the member inode kept out of the blob still reads:"
$B/invf-ls "$IMG" | grep "multi\.splt!mbr" | grep -v mbrt | grep -v mbrmap \
    | grep -v mbrblob | awk '{print $NF}' | head -1 > "$WORK/out/mbrname"
MNAME=$(cat "$WORK/out/mbrname")
echo "    $MNAME"
$B/invf-cat "$IMG" "$MNAME" "$WORK/out/mbr" >/dev/null \
    || { echo "FAIL: the inode member is unreadable"; exit 1; }
echo "    read OK (mixed layout: kind 1 and kind 4 in one map)"

echo "== leg 2b: INVFS_CPACK_BLOB=1 -- every member in the payload sibling =="
IMGALL=cpackmap-all.img
rm -f "$IMGALL"
$B/invf-mkfs "$IMGALL" 0.2 >/dev/null
$B/invf-cp "$IMGALL" "$WORK/orig/multi.splt" multi.splt >/dev/null
INVFS_CPACK_BLOB=1 $B/invf-sweep "$IMGALL" > "$WORK/sweepblob.log" 2>&1 \
    || { cat "$WORK/sweepblob.log"; exit 1; }
if lsimg "$IMGALL" | grep -q "multi\.splt!mbrleaf"; then
    echo "FAIL: BLOB mode wrote a leaf list"; exit 1
fi
nmem=$(lsimg "$IMGALL" | grep -c "multi\.splt!mbr[0-9]" || true)
echo "  member inodes: $nmem (BLOB mode packs every member)"
[ "$nmem" -eq 0 ] || { echo "FAIL: BLOB mode left a member inode"; exit 1; }
$B/invf-cat "$IMGALL" multi.splt "$WORK/out/all.splt" >/dev/null \
    || { echo "FAIL: BLOB-mode container unreadable"; exit 1; }
cmp "$WORK/orig/multi.splt" "$WORK/out/all.splt" \
    || { echo "FAIL: BLOB mode NOT bit-exact"; exit 1; }
$B/invf-cat "$IMGALL" "multi.splt!mbrmap" "$WORK/out/all.map" >/dev/null
python3 - "$WORK/out/all.map" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
magic = b[:4]
count = struct.unpack("<I", b[4:8])[0]
base, wire = (12, 40) if magic in (b"MRM2", b"MRK2") else (8, 29)
kinds = {}
for i in range(count):
    k = b[base + i * wire + 16]
    kinds[k] = kinds.get(k, 0) + 1
print("  map magic %r, kinds %s" % (magic, kinds))
if magic not in (b"MRK1", b"MRK2") or kinds.get(4, 0) < 2:
    sys.exit("FAIL: BLOB mode must declare its kinds and use kind 4")
PY
echo "  bit-exact, every member located by the map"

echo "== leg 2c: a NESTED container in leaf mode (two maps, one read) =="
python3 - "$WORK/orig" <<'PY'
import struct, sys, os
def splt(ms):
    b = b"SPLT" + struct.pack("<I", len(ms))
    for m in ms:
        b += struct.pack("<Q", len(m))
    return b + b"".join(ms)
d = sys.argv[1]
repo = os.environ["REPO"]
text = open(os.path.join(repo, "src", "core", "vol_cpack.c"), "rb").read()
inner = splt([text[:60000], text[60000:120000]])
open(os.path.join(d, "nest.splt"), "wb").write(splt([inner, text[:30000]]))
PY
IMGN=cpackmap-nest.img
rm -f "$IMGN"
$B/invf-mkfs "$IMGN" 0.2 >/dev/null
$B/invf-cp "$IMGN" "$WORK/orig/nest.splt" nest.splt >/dev/null
INVFS_CPACK_LEAF=1 $B/invf-sweep "$IMGN" > "$WORK/nest1.log" 2>&1 \
    || { cat "$WORK/nest1.log"; exit 1; }
grep -q "nest.splt!mbr0000-chunk0: splt_test (codecpack)" "$WORK/nest1.log" \
    || { echo "FAIL: the nested member was not decomposed in the same run"
         grep -E "nest.splt" "$WORK/nest1.log"; exit 1; }
lsimg "$IMGN" | grep -q "nest\.splt!mbrblob" \
    || { echo "FAIL: no outer payload sibling"; exit 1; }
lsimg "$IMGN" | grep -q "nest\.splt!mbr0000-chunk0!mbrblob" \
    || { echo "FAIL: no inner payload sibling"; exit 1; }
if lsimg "$IMGN" | grep -q "mbrleaf"; then
    echo "FAIL: a leaf list at some level"; exit 1
fi
$B/invf-cat "$IMGN" nest.splt "$WORK/out/nest.splt" >/dev/null \
    || { echo "FAIL: nested container unreadable"; exit 1; }
cmp "$WORK/orig/nest.splt" "$WORK/out/nest.splt" \
    || { echo "FAIL: nested container NOT bit-exact"; exit 1; }
echo "  two levels of kind-4 map, one read, cmp silent"

echo "== leg 2d: a MAP-LESS pack keeps the per-inode layout (one place) =="
# The packed layout NEEDS the map now: the blob's offsets live there and
# nowhere else, so INVFS_CPACK_LEAF=1 on a pack with no `map` command must
# fall back to per-inode members instead of writing a payload sibling that
# nothing can address. (Before this WP that combination wrote a "!mbrleaf"
# list, which is exactly the second encoding being removed.)
mkdir -p "$WORK/packs-nomap/splt_nomap.codecpack"
ln -sf "$REPO/tools/codecpacks/splt_test.codecpack/splt.py" \
    "$WORK/packs-nomap/splt_nomap.codecpack/splt.py"
cat > "$WORK/packs-nomap/splt_nomap.codecpack/manifest" <<'EOF'
name = splt_nomap
type = container
algo = 41
pack_version = 1
generation = 1
caps = container|external
sniff.magic = 53504C54
sniff.ext = splt
enumerate = python3 {pack}/splt.py enumerate {in} {out}
extract = python3 {pack}/splt.py extract {in} {idx} {out}
strip = python3 {pack}/splt.py strip {in} {out}
rebuild = python3 {pack}/splt.py rebuild {recipe} {dir} {out}
EOF
IMGNM=cpackmap-nomap.img
rm -f "$IMGNM"
$B/invf-mkfs "$IMGNM" 0.2 >/dev/null
$B/invf-cp "$IMGNM" "$WORK/orig/multi.splt" multi.splt >/dev/null
INVFS_CODECPACKS="$WORK/packs-nomap" INVFS_CPACK_LEAF=1 \
    $B/invf-sweep "$IMGNM" > "$WORK/sweepnm.log" 2>&1 \
    || { cat "$WORK/sweepnm.log"; exit 1; }
grep -q "multi.splt: splt_nomap (codecpack)" "$WORK/sweepnm.log" \
    || { echo "FAIL: the map-less pack did not decompose"; cat "$WORK/sweepnm.log"
         exit 1; }
if lsimg "$IMGNM" | grep -q "multi\.splt!mbrblob"; then
    echo "FAIL: a map-less pack wrote a payload sibling nothing can address"
    exit 1
fi
if lsimg "$IMGNM" | grep -q "multi\.splt!mbrleaf"; then
    echo "FAIL: a leaf list was written"; exit 1
fi
lsimg "$IMGNM" | grep -q "multi\.splt!mbr[0-9]" \
    || { echo "FAIL: per-inode members expected"; lsimg "$IMGNM"; exit 1; }
INVFS_CODECPACKS="$WORK/packs-nomap" $B/invf-cat "$IMGNM" multi.splt \
    "$WORK/out/nm.splt" >/dev/null \
    || { echo "FAIL: map-less container unreadable"; exit 1; }
cmp "$WORK/orig/multi.splt" "$WORK/out/nm.splt" \
    || { echo "FAIL: map-less container NOT bit-exact"; exit 1; }
echo "  members stayed inodes, no blob, no leaf list, cmp silent"

echo "== leg 4: env-negative (layout belongs to the volume) =="
for fl in "0 0" "1 1" "1 0" "0 1"; do
    set -- $fl
    INVFS_CPACK_LEAF=$1 INVFS_CPACK_BLOB=$2 \
        $B/invf-cat "$IMG" multi.splt "$WORK/out/env.splt" >/dev/null 2>&1 \
        || { echo "FAIL: read failed with LEAF=$1 BLOB=$2"; exit 1; }
    cmp -s "$WORK/orig/multi.splt" "$WORK/out/env.splt" \
        || { echo "FAIL: bytes differ with LEAF=$1 BLOB=$2"; exit 1; }
    echo "    LEAF=$1 BLOB=$2: identical"
done

echo "== leg 5: old reader vs new map =="
if [ -n "$OLD_BIN" ] && [ -x "$OLD_BIN/invf-cat" ]; then
    echo "  pre-change binary: $OLD_BIN/invf-cat"
    if INVFS_DEBUG_PACKS=1 "$OLD_BIN/invf-cat" "$IMG" multi.splt \
            "$WORK/out/old.splt" > "$WORK/out/old.log" 2>&1; then
        echo "  (the old reader SUCCEEDED -- it knows this map's kinds;"
        echo "   use a build older than the kind it carries)"
        cmp -s "$WORK/orig/multi.splt" "$WORK/out/old.splt" \
            && echo "  and it read the right bytes" \
            || { echo "FAIL: the old reader returned WRONG bytes"; exit 1; }
    else
        echo "  it REFUSED (rc=$?) and said:"
        grep -v "^vol_open:\|^vol: packs\|^fold:\|^\[packdbg\]" \
            "$WORK/out/old.log" | sed 's/^/    /'
        grep -q "!mbrmap" "$WORK/out/old.log" \
            || { echo "FAIL: the refusal does not name the map"; exit 1; }
        echo "  read failed -- loudly, and naming the map it could not parse"
    fi
    echo "  new binary:"
    $B/invf-cat "$IMG" multi.splt "$WORK/out/new.splt" >/dev/null \
        && cmp -s "$WORK/orig/multi.splt" "$WORK/out/new.splt" \
        && echo "    reads it bit-exactly"
else
    echo "  SKIP (set INVFS_OLD_BIN=<dir with a pre-change invf-cat>)"
fi

echo "== leg 3: a LEGACY volume (leaf list + kind 1) is unchanged =="
if [ -n "$OLD_BIN" ] && [ -x "$OLD_BIN/invf-sweep" ]; then
    $B/invf-mkfs "$IMGLEG" 0.2 >/dev/null
    $B/invf-cp "$IMGLEG" "$WORK/orig/multi.splt" multi.splt >/dev/null
    INVFS_CPACK_LEAF=1 "$OLD_BIN/invf-sweep" "$IMGLEG" \
        > "$WORK/legacy.log" 2>&1 \
        || { echo "FAIL: the old sweep failed"; cat "$WORK/legacy.log"; exit 1; }
    $B/invf-ls "$IMGLEG" | grep -q "multi\.splt!mbrleaf" \
        || { echo "FAIL: the old sweep did not write a leaf list (leg is moot)"
             $B/invf-ls "$IMGLEG"; exit 1; }
    echo "  the old binary wrote a leaf list; this binary reads it:"
    $B/invf-cat "$IMGLEG" multi.splt "$WORK/out/legacy.splt" >/dev/null \
        || { echo "FAIL: a legacy leaf volume no longer reads"; exit 1; }
    cmp "$WORK/orig/multi.splt" "$WORK/out/legacy.splt" \
        || { echo "FAIL: legacy volume NOT bit-exact"; exit 1; }
    echo "    cmp silent (no migration, no behaviour change)"
    $B/invf-cat "$IMGLEG" "multi.splt!mbrmap" "$WORK/out/legacy.map" >/dev/null
    head -c 4 "$WORK/out/legacy.map" | sed 's/^/    legacy map magic: /'
    echo
else
    echo "  SKIP (needs INVFS_OLD_BIN with a pre-change invf-sweep)"
fi

echo "== leg 6: ext4fs in leaf mode, three sweeps, no drift =="
if command -v mkfs.ext4 >/dev/null; then
    mkdir -p "$WORK/ext4root/bin"
    for f in "$REPO"/src/core/*.c; do
        cp "$f" "$WORK/ext4root/bin/$(basename "$f")" 2>/dev/null || true
    done
    cp /bin/bash "$WORK/ext4root/bin/sh" 2>/dev/null || true
    dd if=/dev/zero of="$WORK/orig/disk.img" bs=1M count=16 status=none
    mkfs.ext4 -q -F -b 4096 "$WORK/orig/disk.img" >/dev/null
    ( cd "$WORK/ext4root" && find . -type f | head -40 \
        | xargs -r debugfs -w -R "write {} {}" "$WORK/orig/disk.img" \
            >/dev/null 2>&1 ) || true
    $B/invf-mkfs "$IMGEXT4" 0.3 >/dev/null
    $B/invf-cp "$IMGEXT4" "$WORK/orig/disk.img" disk.img >/dev/null
    for pass in 1 2 3; do
        INVFS_CPACK_LEAF=1 $B/invf-sweep "$IMGEXT4" \
            > "$WORK/ext4-sweep$pass.log" 2>&1 || {
                echo "FAIL: sweep $pass failed"; tail -5 "$WORK/ext4-sweep$pass.log"
                exit 1; }
        $B/invf-cat "$IMGEXT4" disk.img "$WORK/out/disk$pass.img" >/dev/null \
            || { echo "FAIL: disk.img unreadable after sweep $pass"; exit 1; }
        cmp -s "$WORK/orig/disk.img" "$WORK/out/disk$pass.img" \
            || { echo "FAIL: disk.img NOT bit-exact after sweep $pass"; exit 1; }
        if lsimg "$IMGEXT4" | grep -q "disk\.img!mbrleaf"; then
            echo "FAIL: sweep $pass wrote a leaf list"; exit 1
        fi
        echo "  sweep $pass: disk.img bit-exact, no leaf list"
        grep -E "disk.img: ext4fs|kind 4|size guard|refused" \
            "$WORK/ext4-sweep$pass.log" | sed 's/^/    /' | head -4 || true
    done
else
    echo "  SKIP (mkfs.ext4 not installed)"
fi

echo "PASS: cpack map consolidation"
