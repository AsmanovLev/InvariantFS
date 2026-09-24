#!/bin/bash
# test-vdi.sh — the vdi containerpack (tools/codecpacks/vdi.codecpack,
# algo 21) end-to-end (persistent regression). WP16a containerpack ABI +
# WP16b seekable (map) flow over VirtualBox VDI disk images.
#
#   fixtures (inline python3 for GENERATION only — hand-crafted per the VDI
#   v1.1 spec, cross-checked against qemu-img when available):
#     diska.vdi — dynamic, 8 of 16 blocks allocated, interleaved AND
#       slot-scrambled, unused slots 7/8 left as junk-filled holes inside
#       the data area, junk in the bmap padding, 1000 bytes of trailing
#       junk. Block contents: zeros / ELF / busybox text / random.
#     diskb.vdi — dynamic, realistic inner content: the virtual disk's
#       first 4 MiB are a coherent raw disk image (GPT + one 2 MiB Linux
#       partition; no 55AA protective MBR on purpose — see the fixture
#       comment); blocks 0-3 allocated, so the member stream IS that disk
#       (the composition leg).
#     qgen.vdi  — (qemu-img only) a qemu-generated dynamic VDI from random
#       raw content: external ground truth through the same FS legs.
#     stat.vdi  — static (type 2): the pack must DECLINE (rawdisk
#       territory); falls through to generic, no siblings, no stamp.
#     diff.vdi  — uuidLink != 0 (differencing): DECLINED.
#     broke.vdi — bmap slot beyond EOF (corrupt): DECLINED.
#   pack-level round-trip (enumerate/extract/strip/rebuild/map + memcmp)
#   runs on every fixture BEFORE the FS legs (the pack is its own first
#   arbiter; qemu-img info/convert cross-validates the hand-crafted files).
#   mkfs -> cp -> INVFS_CODECPACKS=$REPO/tools/codecpacks invf-sweep
#   (expect "vdi (codecpack)" for diska/diskb/qgen, silence for the
#   declines) -> CONTAINER{21,1} stamps -> verify --deep -> sha256
#   bit-exact (containers AND the direct member read) -> RANGED reads
#   through the MRMP splice (head / recipe->extent boundary / hole /
#   mid-extent / tail / whole-by-windows) -> pack-ABSENT reads still
#   bit-exact -> sweep 2 stores the members (no re-decomposition) ->
#   sweep 3 idle -> delete cascade kills every "!"-sibling -> fsck clean ->
#   survivors bit-exact.
#   Composition leg: the rawdisk pack's helper is resolved the way the FS
#   probe resolves it — <pack>/bin/rawdisk, else rawdisk on PATH; when the
#   pack's own bin/ is empty the test builds rawdisk.c into $WORK/bin and
#   prepends it to PATH (the pack directory is never touched). With the
#   tool live, a later sweep must decompose diskb's member stream (a GPT
#   disk) further — nested containers, read back through two map splices,
#   pack-present and pack-absent. Without it, the member's actual pipeline
#   fate is reported honestly (the leg never fails on rawdisk's absence).
#   Admission leg: fresh image, INVFS_DEC_MEM_LIMIT=64K ->
#   GENERIC_MEMLIMIT{21,1}, no decomposition, bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-vdi.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
PACK=$REPO/tools/codecpacks/vdi.codecpack
WORK=/dev/shm/wp16vdi
trap 'rm -rf "$WORK" /dev/shm/wp16vdi*.img' EXIT
IMG=wp16vdi.img
IMGMEM=wp16vdi-mem.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/nopacks" "$WORK/bin"
cd /dev/shm
rm -f "$IMG" "$IMGMEM"

echo "== tools =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
if command -v qemu-img >/dev/null; then HAVE_QEMU=1; else HAVE_QEMU=0; fi
echo "  qemu-img: $([ $HAVE_QEMU = 1 ] && echo yes || echo 'no (cross-check leg skipped)')"

echo "== build the pack (the -Wall -Wextra gate) =="
mkdir -p "$PACK/bin"
WARN=$(cc -std=c11 -O2 -Wall -Wextra -o "$PACK/bin/vdi" "$PACK/vdi.c" 2>&1) \
    || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }
echo "  bin/vdi built, -Wall -Wextra clean"

echo "== composition target: resolve the rawdisk pack's tool =="
# The nested-decomposition leg needs the rawdisk pack's helper. Its manifest
# resolves the bare name "rawdisk" from <pack>/bin/ first, then PATH — so a
# build into $WORK/bin + a PATH prepend exercises the REAL composition
# without touching the rawdisk pack directory. Best effort: if the pack is
# absent or its source does not build here, the leg reports and skips.
RAWDISK=$REPO/tools/codecpacks/rawdisk.codecpack
if [ ! -x "$RAWDISK/bin/rawdisk" ] && ! command -v rawdisk >/dev/null 2>&1 \
        && [ -f "$RAWDISK/rawdisk.c" ]; then
    if cc -std=c11 -O2 -Wall -Wextra -o "$WORK/bin/rawdisk" \
            "$RAWDISK/rawdisk.c" 2> "$WORK/rawdisk-build.log"; then
        export PATH="$WORK/bin:$PATH"
        echo "  rawdisk.c built into \$WORK/bin (PATH-resolved, pack dir untouched)"
    else
        echo "  NOTE: rawdisk.c present but does not build here (parallel work"
        echo "        in flight?) — nested leg will report and skip:"
        sed 's/^/    /' "$WORK/rawdisk-build.log" | head -5
    fi
elif [ -x "$RAWDISK/bin/rawdisk" ]; then
    echo "  rawdisk pack tool present at $RAWDISK/bin/rawdisk"
elif command -v rawdisk >/dev/null 2>&1; then
    echo "  rawdisk resolved on PATH: $(command -v rawdisk)"
else
    echo "  NOTE: no rawdisk pack/tool — nested leg will report and skip"
fi

echo "== generate fixtures =="
python3 - "$WORK/orig" <<'PY'
import os
import random
import struct
import sys
import zlib

d = sys.argv[1]
rnd = random.Random(21)
MB = 1048576

def write_vdi(path, cblocks, blk_slot, contents, cbdisk=None, tail=b"",
              pad_junk=b"", vtype=1, uuid_link=bytes(16),
              text=b"<<< Oracle VM VirtualBox Disk Image >>>\n"):
    """blk_slot: {blk: slot}; contents: {blk: bytes(cbblock)}."""
    cbblock = MB
    if cbdisk is None:
        cbdisk = cblocks * cbblock
    offblocks = 0x200
    offdata = 0x200 + ((4 * cblocks + 511) // 512) * 512
    hdr = bytearray(offdata)
    hdr[0:len(text)] = text
    struct.pack_into('<I', hdr, 0x40, 0xBEDA107F)   # signature
    struct.pack_into('<I', hdr, 0x44, 0x00010001)   # version 1.1
    struct.pack_into('<I', hdr, 0x48, 0x190)        # header size (VBox-ish)
    struct.pack_into('<I', hdr, 0x4C, vtype)        # image type
    struct.pack_into('<I', hdr, 0x154, offblocks)
    struct.pack_into('<I', hdr, 0x158, offdata)
    struct.pack_into('<I', hdr, 0x168, 512)         # sector size
    struct.pack_into('<Q', hdr, 0x170, cbdisk)
    struct.pack_into('<I', hdr, 0x178, cbblock)
    struct.pack_into('<I', hdr, 0x180, cblocks)
    struct.pack_into('<I', hdr, 0x184, len(blk_slot))
    hdr[0x188:0x198] = rnd.randbytes(16)            # uuidCreate
    hdr[0x198:0x1A8] = rnd.randbytes(16)            # uuidModify
    hdr[0x1A8:0x1B8] = uuid_link                    # uuidLinkage
    bmap = [0xFFFFFFFF] * cblocks
    for b, s in blk_slot.items():
        bmap[b] = s
    struct.pack_into('<%dI' % cblocks, hdr, offblocks, *bmap)
    if pad_junk:                                    # bmap padding region
        hdr[offblocks + 4 * cblocks:][:len(pad_junk)] = pad_junk
    nslots = max(blk_slot.values()) + 1 if blk_slot else 0
    data = bytearray(rnd.randbytes(nslots * cbblock))  # slot holes stay junk
    for b, s in blk_slot.items():
        data[s * cbblock:(s + 1) * cbblock] = contents[b]
    out = bytes(hdr) + bytes(data) + tail
    open(path, 'wb').write(out)
    return out

def tile(blob, n):
    return (blob * (n // len(blob) + 1))[:n]

# content sources: a real busybox C source + a real x86-64 ELF
text = None
for root, _dirs, files in os.walk("/home/user/InvariantFS/tools/busybox-src"):
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                text = open(p, "rb").read()
                break
    if text:
        break
assert text, "no busybox .c fixture found"
elf = None
for p in ("/usr/bin/passwd", "/usr/bin/gpg", "/bin/ls", "/usr/bin/ls",
          "/bin/bash", "/usr/bin/bash"):
    if os.path.isfile(p) and not os.path.islink(p):
        with open(p, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF" and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = open(p, "rb").read()
            break
assert elf, "no x86-64 ELF fixture found"

zeros = bytes(MB)
# --- diska.vdi: 8 of 16 blocks, interleaved blocks, scrambled slots,
#     unused slots 7+8 (junk holes inside the data area), junk bmap
#     padding, trailing junk ------------------------------------------------
ablk = {0: zeros, 2: tile(elf, MB), 5: tile(text, MB), 7: rnd.randbytes(MB),
        8: zeros, 10: tile(text, MB), 13: rnd.randbytes(MB), 15: tile(elf, MB)}
aslot = {0: 0, 2: 3, 5: 1, 7: 6, 8: 2, 10: 9, 13: 4, 15: 5}
tail = rnd.randbytes(1000)
pad = rnd.randbytes(0x400 - (0x200 + 4 * 16))
img = write_vdi(os.path.join(d, "diska.vdi"), 16, aslot, ablk, tail=tail,
                pad_junk=pad)
# reference member stream: allocated blocks in TABLE order
open(os.path.join(d, "member-a.ref"), "wb").write(
    b"".join(ablk[b] for b in sorted(ablk)))
offdata = 0x200 + ((4 * 16 + 511) // 512) * 512
with open(os.path.join(d, "diska.layout"), "w") as f:
    f.write("size=%d offdata=%d hole_off=%d ext9_off=%d tail_off=%d\n"
            % (len(img), offdata, offdata + 7 * MB, offdata + 9 * MB,
               offdata + 10 * MB))

# --- diskb.vdi: the virtual disk's first 4 MiB are a coherent raw disk
#     image (GPT + one 2 MiB Linux-data partition); blocks 0-3 allocated
#     (slots scrambled), so the compacted member stream IS that disk. The
#     GPT carries NO protective-MBR 55AA on purpose: the fatfs pack sniffs
#     55AA@510 like rawdisk does, and a pack-claimed member waits on ITS
#     probe — the GPT-only signature keeps the composition channel
#     deterministically on rawdisk ("EFI PART" @ LBA1). ---------------------
gpt = bytearray(4 * MB)
NLBA = 4 * MB // 512
ent = bytearray(128 * 128)
# Linux filesystem data GUID, GPT mixed-endian on-disk form
ent[0:16] = bytes.fromhex("af3dc60f83847247798e793dd69d8477de")
ent[16:32] = bytes.fromhex("00112233445566778899aabbccddeeff")
struct.pack_into('<Q', ent, 32, 2048)           # first_lba
struct.pack_into('<Q', ent, 40, 6143)           # last_lba (2 MiB)
nm = "vdicompose".encode('utf-16-le')
ent[56:56 + len(nm)] = nm
gpt[2 * 512:2 * 512 + len(ent)] = ent
ecrc = zlib.crc32(ent) & 0xFFFFFFFF

def gpt_header(my_lba, alt_lba, ent_lba, ecrc):
    h = bytearray(512)
    h[0:8] = b"EFI PART"
    struct.pack_into('<I', h, 8, 0x00010000)    # revision 1.0
    struct.pack_into('<I', h, 12, 92)           # header size
    struct.pack_into('<Q', h, 24, my_lba)
    struct.pack_into('<Q', h, 32, alt_lba)
    struct.pack_into('<Q', h, 40, 34)           # first usable
    struct.pack_into('<Q', h, 48, 8158)         # last usable
    h[56:72] = bytes.fromhex("aabbccddeeff00112233445566778899")
    struct.pack_into('<Q', h, 72, ent_lba)
    struct.pack_into('<I', h, 80, 128)          # num entries
    struct.pack_into('<I', h, 84, 128)          # entry size
    struct.pack_into('<I', h, 88, ecrc)
    struct.pack_into('<I', h, 16, zlib.crc32(h[:92]) & 0xFFFFFFFF)
    return h

gpt[512:1024] = gpt_header(1, NLBA - 1, 2, ecrc)
gpt[8159 * 512:8159 * 512 + len(ent)] = ent     # backup entries
gpt[8191 * 512:] = gpt_header(8191, 1, 8159, ecrc)  # backup header
bblk = {i: bytes(gpt[i * MB:(i + 1) * MB]) for i in range(4)}
bslot = {0: 2, 1: 0, 2: 3, 3: 1}
img = write_vdi(os.path.join(d, "diskb.vdi"), 16, bslot, bblk)
open(os.path.join(d, "member-b.ref"), "wb").write(
    b"".join(bblk[b] for b in sorted(bblk)))

# --- decline fixtures -------------------------------------------------------
# static (type 2): 4 blocks, all allocated
sblk = {i: rnd.randbytes(MB) for i in range(4)}
write_vdi(os.path.join(d, "stat.vdi"), 4, {i: i for i in range(4)}, sblk,
          vtype=2)
# differencing: a valid dynamic image but with a parent UUID link
dblk = {0: rnd.randbytes(MB)}
write_vdi(os.path.join(d, "diff.vdi"), 2, {0: 0}, dblk,
          uuid_link=b"\x42" + bytes(15))
# corrupt: bmap slot whose extent lies beyond EOF
img = bytearray(write_vdi(os.path.join(d, "broke.vdi"), 4, {1: 0},
                          {1: rnd.randbytes(MB)}))
struct.pack_into('<I', img, 0x200 + 2 * 4, 77)    # block 2 -> slot 77
open(os.path.join(d, "broke.vdi"), "wb").write(bytes(img))

for f in sorted(os.listdir(d)):
    if f.endswith(".vdi"):
        print("  %-10s %d bytes" % (f, os.path.getsize(os.path.join(d, f))))
PY
. "$WORK/orig/diska.layout"   # size offdata hole_off ext9_off tail_off

echo "== qemu cross-validation of the hand-crafted fixtures =="
if [ "$HAVE_QEMU" = 1 ]; then
    for f in diska.vdi diskb.vdi; do
        qemu-img info "$WORK/orig/$f" >/dev/null \
            || { echo "FAIL: qemu-img refuses fixture $f"; exit 1; }
        echo "  qemu-img info accepts $f"
    done
    # the virtual disk content qemu serves must match the generator's intent:
    # allocated blocks in place, unallocated read as zeros
    qemu-img convert -f vdi -O raw "$WORK/orig/diskb.vdi" "$WORK/out/diskb.raw"
    python3 - "$WORK" <<'PY'
import sys
w = sys.argv[1]
raw = open(w + "/out/diskb.raw", "rb").read()
ref = open(w + "/orig/member-b.ref", "rb").read()
MB = 1048576
assert len(raw) == 16 * MB, len(raw)
assert raw[0:4 * MB] == ref, "virtual disk prefix != the GPT disk image"
assert raw[4 * MB:] == bytes(12 * MB), "unallocated tail not zeros"
assert raw[512:520] == b"EFI PART", "the member stream is a GPT disk"
print("  qemu-img convert: virtual disk = GPT image + zero tail, as generated")
PY
    # and a qemu-GENERATED vdi as external ground truth for the FS legs
    python3 -c "
import random, sys
rnd = random.Random(77)
MB = 1048576
buf = bytearray(16 * MB)
buf[0:MB] = rnd.randbytes(MB)          # block 0
buf[3*MB:4*MB] = rnd.randbytes(MB)     # block 3
buf[3*MB:3*MB+16] = b'INVARIANTFS-VDI!'  # marker
open('$WORK/orig/qgen.raw','wb').write(bytes(buf))
"
    qemu-img convert -f raw -O vdi "$WORK/orig/qgen.raw" "$WORK/orig/qgen.vdi"
    echo "  qgen.vdi: qemu-generated fixture $(stat -c%s "$WORK/orig/qgen.vdi") bytes"
else
    echo "  qemu-img absent: fixture self-validation only"
fi

echo "== pack-level round-trip (the pack is its own first arbiter) =="
V=$PACK/bin/vdi
ok=1
for f in diska.vdi diskb.vdi; do
    rm -rf "$WORK/pk" && mkdir -p "$WORK/pk/mbr"
    "$V" enumerate "$WORK/orig/$f" "$WORK/pk/table" || { echo "FAIL: enumerate $f"; ok=0; continue; }
    [ "$(cat "$WORK/pk/table")" = "1	diskimg	$(stat -c%s "$WORK/orig/member-${f:4:1}.ref")" ] \
        || { echo "FAIL: $f table: $(cat "$WORK/pk/table")"; ok=0; }
    "$V" extract "$WORK/orig/$f" 1 "$WORK/pk/mbr/1" \
        || { echo "FAIL: extract $f"; ok=0; }
    cmp -s "$WORK/orig/member-${f:4:1}.ref" "$WORK/pk/mbr/1" \
        || { echo "FAIL: $f member stream mismatch"; ok=0; }
    "$V" strip "$WORK/orig/$f" "$WORK/pk/recipe" || { echo "FAIL: strip $f"; ok=0; }
    "$V" map "$WORK/orig/$f" "$WORK/pk/map" || { echo "FAIL: map $f"; ok=0; }
    "$V" rebuild "$WORK/pk/recipe" "$WORK/pk/mbr" "$WORK/pk/out" \
        || { echo "FAIL: rebuild $f"; ok=0; }
    cmp -s "$WORK/orig/$f" "$WORK/pk/out" \
        || { echo "FAIL: $f rebuild not bit-exact"; ok=0; }
    "$V" estimate "$WORK/orig/$f" >/dev/null || { echo "FAIL: estimate $f"; ok=0; }
    echo "  $f: enumerate/extract/strip/map/rebuild/estimate OK, rebuild bit-exact"
done
if [ "$HAVE_QEMU" = 1 ]; then
    rm -rf "$WORK/pk" && mkdir -p "$WORK/pk/mbr"
    "$V" strip "$WORK/orig/qgen.vdi" "$WORK/pk/recipe" \
        && "$V" extract "$WORK/orig/qgen.vdi" 1 "$WORK/pk/mbr/1" \
        && "$V" rebuild "$WORK/pk/recipe" "$WORK/pk/mbr" "$WORK/pk/out" \
        && cmp -s "$WORK/orig/qgen.vdi" "$WORK/pk/out" \
        && echo "  qgen.vdi (qemu-generated): pack round-trip bit-exact" \
        || { echo "FAIL: qgen.vdi pack round-trip"; ok=0; }
fi
for f in stat.vdi diff.vdi broke.vdi; do
    if "$V" enumerate "$WORK/orig/$f" "$WORK/pk.table" 2>/dev/null; then
        echo "FAIL: $f was NOT declined"; ok=0
    else
        echo "  $f: declined (rc=$?)"
    fi
done
[ "$ok" = 1 ] || exit 1

# class-stamp reader + ranged-read harness (the test-containerpack.sh pattern)
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file.
 * usage: classof <image> <name>  ->  "cls=<n> algo=<n> gen=<n>" or "none" */
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
cat > "$WORK/rngread.c" <<'C'
/* rngread.c — ranged reads through vol_read_range (the WP16b local-splice
 * read path for a seekable container).
 * usage: rngread <image> <name> <off> <len> <out> [chunk]
 *   len < 0 reads the whole file in <chunk>-byte windows (default 65536). */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id, off, fsz = 0;
    long long len;
    uint64_t chunk;
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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

VDIS="diska.vdi diskb.vdi"
[ "$HAVE_QEMU" = 1 ] && VDIS="$VDIS qgen.vdi"

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in $VDIS stat.vdi diff.vdi broke.vdi; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 (vdi containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
for f in $VDIS; do
    grep -q "$f: vdi (codecpack)" "$WORK/sweep1.log" \
        || { echo "FAIL: $f not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
done
for f in stat.vdi diff.vdi broke.vdi; do
    if grep -q "$f: vdi (codecpack)" "$WORK/sweep1.log"; then
        echo "FAIL: $f was decomposed (must be declined)"; cat "$WORK/sweep1.log"; exit 1
    fi
done
grep -E "codecpack" "$WORK/sweep1.log"

echo "== sibling set (member + table + map) =="
for f in $VDIS; do
    N=$($B/invf-ls "$IMG" | grep -c "$f!" || true)
    echo "  $f!* names: $N"
    [ "$N" -eq 3 ] || { echo "FAIL: $f: want 3 (member + table + map)"; $B/invf-ls "$IMG"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbr0001-diskimg" \
        || { echo "FAIL: $f: member sibling missing"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbrt" || { echo "FAIL: $f: member table missing"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbrmap" || { echo "FAIL: $f: member map missing"; exit 1; }
done
$B/invf-ls "$IMG" | grep "diska\.vdi!mbr0001-diskimg" | grep -q "8388608 bytes" \
    || { echo "FAIL: diska member not 8 MiB"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep "diskb\.vdi!mbr0001-diskimg" | grep -q "4194304 bytes" \
    || { echo "FAIL: diskb member not 4 MiB"; $B/invf-ls "$IMG"; exit 1; }
for f in stat.vdi diff.vdi broke.vdi; do
    if $B/invf-ls "$IMG" | grep -q "$f!"; then
        echo "FAIL: declined $f gained siblings"; $B/invf-ls "$IMG"; exit 1
    fi
done
echo "member/table/map siblings present; declines clean"

echo "== class stamps =="
for f in $VDIS; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=3 algo=21 gen=1" ] || { echo "FAIL: $f: want CONTAINER{VDI=21,1}"; exit 1; }
done
for f in stat.vdi diff.vdi broke.vdi; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    case "$C" in *algo=21*) echo "FAIL: declined $f carries a pack stamp"; exit 1;; esac
done

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in $VDIS stat.vdi diff.vdi broke.vdi; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
# the members are real inodes: read them directly
$B/invf-cat "$IMG" "diska.vdi!mbr0001-diskimg" "$WORK/out/ma" >/dev/null
$B/invf-cat "$IMG" "diskb.vdi!mbr0001-diskimg" "$WORK/out/mb" >/dev/null
cmp -s "$WORK/orig/member-a.ref" "$WORK/out/ma" || { echo "MISMATCH diska member"; ok=0; }
cmp -s "$WORK/orig/member-b.ref" "$WORK/out/mb" || { echo "MISMATCH diskb member"; ok=0; }
[ "$ok" = 1 ] || exit 1
echo "containers and members bit-exact"
# the member table rides verbatim as !mbrt
$B/invf-cat "$IMG" "diska.vdi!mbrt" "$WORK/out/mbrt-a" >/dev/null
[ "$(cat "$WORK/out/mbrt-a")" = "1	diskimg	8388608" ] \
    || { echo "FAIL: mbrt content drifted: $(cat "$WORK/out/mbrt-a")"; exit 1; }
echo "member table sibling verbatim"

echo "== ranged reads (the WP16b local splice over diska.vdi) =="
rng() {  # rng <off> <len> <tag>
    dd if="$WORK/orig/diska.vdi" of="$WORK/out/ref.$3" bs=1 skip="$1" count="$2" 2>/dev/null
    "$WORK/rngread" "$IMG" diska.vdi "$1" "$2" "$WORK/out/got.$3" >/dev/null
    cmp -s "$WORK/out/ref.$3" "$WORK/out/got.$3" \
        || { echo "FAIL: range $3 (off=$1 len=$2) mismatch"; exit 1; }
}
rng 0 64 head                          # the recipe head (preheader text)
rng 64 4 sig                           # the signature itself (recipe)
rng $((offdata - 16)) 64 rec2mbr       # recipe -> first extent boundary
rng $((hole_off - 32)) 64 mbr2hole     # extent -> junk-hole boundary
rng $hole_off 2048 hole                # inside the junk hole (recipe)
rng $((ext9_off + 4096)) 65536 midmbr  # strictly inside a member extent
rng $((tail_off - 16)) 512 tailgap     # last extent -> trailing junk
rng $((size - 1000)) 1000 tail         # the trailing junk itself (recipe)
"$WORK/rngread" "$IMG" diska.vdi 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/diska.vdi" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
echo "head / boundaries / hole / mid-extent / tail / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" diska.vdi "$WORK/out/absent" >/dev/null
cmp -s "$WORK/orig/diska.vdi" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" diska.vdi \
    $((ext9_off + 4096)) 65536 "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midmbr" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" diska.vdi 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/orig/diska.vdi" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent ranged whole read mismatch"; exit 1; }
echo "pack-absent: whole read + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: members store through the normal pipeline, no re-fire =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q ": vdi (codecpack)" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) \
    || { echo "FAIL: corrupt after sweep 2"; exit 1; }
for f in $VDIS; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/s2.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/s2.$f" || { echo "FAIL: $f drifted"; exit 1; }
    C=$("$WORK/classof" "$IMG" "$f")
    [ "$C" = "cls=3 algo=21 gen=1" ] || { echo "FAIL: $f stamp drifted: $C"; exit 1; }
done
echo "containers stable and bit-exact after sweep 2"

echo "== composition leg: the member stream is a raw disk image =="
MAC=$("$WORK/classof" "$IMG" "diska.vdi!mbr0001-diskimg")
MBC=$("$WORK/classof" "$IMG" "diskb.vdi!mbr0001-diskimg")
echo "  diska member stream: $MAC (after sweep 2)"
echo "  diskb member stream: $MBC (after sweep 2)"
# The nested-decomposition contract: if the rawdisk pack's tool RESOLVES
# (the FS probe convention: <pack>/bin/rawdisk executable or rawdisk on
# PATH), sweep 2 — the first sweep the member stream was eligible for —
# must have decomposed it further (nested containers), and the container
# read-back goes through two map splices. If the pack directory is absent
# or its helper unbuilt, the member honestly waits RAW (sniff-claimed by
# rawdisk, probe-failed) or stores generic — report which, skip the leg.
RAWDISK=$REPO/tools/codecpacks/rawdisk.codecpack
if [ -x "$RAWDISK/bin/rawdisk" ] || command -v rawdisk >/dev/null 2>&1; then
    if ! grep -q "diskb.vdi!mbr0001-diskimg: rawdisk (codecpack)" "$WORK/sweep2.log"; then
        # not on its first eligible sweep — one more chance before failing
        $B/invf-sweep "$IMG" > "$WORK/sweep2b.log" 2>&1 || { cat "$WORK/sweep2b.log"; exit 1; }
        grep -q "diskb.vdi!mbr0001-diskimg: rawdisk (codecpack)" "$WORK/sweep2b.log" \
            || { echo "FAIL: rawdisk resolves but the member stream was never decomposed"; \
                 cat "$WORK/sweep2.log"; cat "$WORK/sweep2b.log"; exit 1; }
    fi
    grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) \
        || { echo "FAIL: corrupt after the nested decomposition"; exit 1; }
    $B/invf-cat "$IMG" diskb.vdi "$WORK/out/nested.vdi" >/dev/null
    cmp -s "$WORK/orig/diskb.vdi" "$WORK/out/nested.vdi" \
        || { echo "FAIL: nested container read-back not bit-exact"; exit 1; }
    INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" diskb.vdi \
        "$WORK/out/nested2.vdi" >/dev/null
    cmp -s "$WORK/orig/diskb.vdi" "$WORK/out/nested2.vdi" \
        || { echo "FAIL: pack-absent NESTED read not bit-exact"; exit 1; }
    echo "  rawdisk present: member decomposed nested (a map splice through a"
    echo "  map splice), read-back bit-exact with the packs present AND absent"
else
    echo "  NOTE: rawdisk tool not resolvable ($RAWDISK/bin/rawdisk not built,"
    echo "        no rawdisk on PATH) — nested-decomposition leg SKIPPED."
    echo "        The member stream took the normal pipeline (stamps above:"
    echo "        RAW-waiting under a sniff-claiming-but-unbuilt rawdisk, or"
    echo "        generic ZSTD with the pack absent); bit-exactness was"
    echo "        verified by the direct member reads above."
fi

echo "== sweep #3: idle =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1 || { cat "$WORK/sweep3.log"; exit 1; }
if grep -q ": vdi (codecpack)" "$WORK/sweep3.log"; then
    echo "FAIL: sweep 3 re-fired the vdi pack"; cat "$WORK/sweep3.log"; exit 1
fi
grep -q " 0 corrupt" <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
echo "no re-decomposition"

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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/cbrm" "$IMG" $VDIS
if $B/invf-ls "$IMG" | grep -q "vdi!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + tables + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivors bit-exact =="
ok=1
for f in stat.vdi diff.vdi broke.vdi; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/surv.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/surv.$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "declined files bit-exact after the deletes"

echo "== admission leg: INVFS_DEC_MEM_LIMIT=64K =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/diska.vdi" diska.vdi >/dev/null
# the pack's estimate (member bytes + 64 MiB) exceeds 64K -> policy refusal
# before any extract; GENERIC_MEMLIMIT{21,1}
INVFS_DEC_MEM_LIMIT=64K $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q ": vdi (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 64K decode-memory limit"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" diska.vdi)
echo "  diska.vdi (memlimit): $C"
[ "$C" = "cls=5 algo=21 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{VDI=21,1}"; exit 1; }
$B/invf-cat "$IMGMEM" diska.vdi "$WORK/out/diskamem.vdi" >/dev/null
cmp -s "$WORK/orig/diska.vdi" "$WORK/out/diskamem.vdi" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "VDI CONTAINERPACK E2E: PASS"
