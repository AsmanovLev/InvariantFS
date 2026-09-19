#!/bin/bash
# test-fatfs.sh — fatfs containerpack (FAT12/16/32 + exFAT -> per-file
# members) end-to-end (persistent regression), WP16a + WP16b seekable.
#
#   fixtures: real images made with mkfs.vfat/mkfs.exfat and populated with
#   mtools (FAT) / a passwordless-sudo loop mount (exFAT; mtools cannot do
#   exFAT):
#     fat32.img — 64MB FAT32: a >20KB text (busybox .c, PPMd-batchable),
#       a real x86-64 ELF (ZSTD/BCJ-batchable), a 5MB random file, an LFN
#       file, a subdir, an empty file, and FRAG.BIN (1MB) hand-scattered
#       to 2048 single-cluster runs by FAT surgery (fsck-clean).
#     fat16.img — 16MB FAT16: two small files.
#     exfat.img — 32MB exFAT: same stage set (contiguous/NoFatChain) plus a
#       3MB fragmented file written after a create/delete pattern -> CHAIN
#       MODE (NoFatChain clear), fragmented root dir chain.
#     fat12.img — 1.44MB FAT12 (hand self-test leg only).
#     text.img  — a plain text file named .img: must be REFUSED.
#   Hand self-test first (enumerate/extract/strip/rebuild/cmp + a full
#   byte-level MRMP partition proof per image, refusals incl. truncated /
#   fake-BPB / loop-chain images), then the FS legs mirroring
#   test-containerpack.sh: sweep -> class stamps CONTAINER{18,1} / member
#   TEXT{PPMD} / BATCHED_BIN{ZSTD_BCJ} -> verify --deep -> sha256 bit-exact
#   (all three images) -> direct member reads -> ranged splice reads ->
#   pack-ABSENT reads via the self-describing map -> idempotent re-sweep ->
#   delete cascade -> fsck -> admission leg (tiny ARC ->
#   GENERIC_MEMLIMIT{18,1}).
#
#   PACK DIR SCOPING: the sweep/read legs export INVFS_CODECPACKS=$WORK/packs
#   (a symlink to fatfs.codecpack only), NOT the shared tools/codecpacks
#   dir. fatfs and rawdisk share the weak 55AA@510 magic and the .img ext;
#   the FS tries container packs in readdir order, first sniff hit wins,
#   and a decline breaks the loop -- with the shared dir, rawdisk (which
#   sorts before fatfs and strictly owns MBR/GPT partition tables) claims
#   every superfloppy FAT image first and declines it before fatfs ever
#   runs. The strict parses make the overlap sane pack-side (fatfs declines
#   MBR/GPT, rawdisk declines superfloppy filesystems); the scoping keeps
#   this test independent of which sibling packs are built.
#
# Run from the repo root after `make`:  bash tools/test-fatfs.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
PACK=$REPO/tools/codecpacks/fatfs.codecpack
WORK=/dev/shm/wp16fatfs
trap 'rm -rf "$WORK" /dev/shm/wp16fatfs*.img' EXIT
IMG=wp16fatfs.img
IMGMEM=wp16fatfs-mem.img
export MTOOLS_SKIP_CHECK=1
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/nopacks" \
    "$WORK/stage/subdir" "$WORK/mnt" "$WORK/mdir" "$WORK/packs"
ln -s "$PACK" "$WORK/packs/fatfs.codecpack"
export INVFS_CODECPACKS=$WORK/packs   # the sweep AND the reads (see header)
cd /dev/shm
rm -f "$IMG" "$IMGMEM"

echo "== build pack (cc -std=c11 -O2 -Wall -Wextra -Werror) =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/fatfs" "$PACK/fatfs.c"
FATFS="$PACK/bin/fatfs"

echo "== tools =="
for t in mkfs.vfat mkfs.exfat mcopy python3 sha256sum; do
    command -v $t >/dev/null || { echo "FAIL: $t not installed"; exit 1; }
done
sudo -n true || { echo "FAIL: passwordless sudo (exFAT loop mount)"; exit 1; }

echo "== generate fixtures =="
python3 - "$WORK/stage" <<'PY'
import os, sys
d = sys.argv[1]
# text member: a real busybox .c, >20KB (content-sniffs as text; the name
# "README" carries no extension on purpose, like the splt fixture)
text = None
src = "/home/user/InvariantFS/tools/busybox-src"
for root, _dirs, files in os.walk(src):
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                text = open(p, "rb").read()
                break
    if text:
        break
assert text, "no busybox .c fixture found"
open(os.path.join(d, "README"), "wb").write(text)
# ELF member: a real x86-64 binary, >= 64KB (binary-family magic gate)
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
open(os.path.join(d, "elf.bin"), "wb").write(elf)
print("  README %d bytes, elf.bin %d bytes" % (len(text), len(elf)))
PY
head -c 5000000 /dev/urandom > "$WORK/stage/big.bin"
echo "long file name fixture" > "$WORK/stage/Long File Name Number One.txt"
echo nested > "$WORK/stage/subdir/nested.txt"
: > "$WORK/stage/empty.dat"

# --- fat32.img: 64MB, mtools-populated, then FRAG.BIN scattered ----------
dd if=/dev/zero of="$WORK/orig/fat32.img" bs=1M count=64 status=none
mkfs.vfat -F 32 "$WORK/orig/fat32.img" >/dev/null
mcopy -i "$WORK/orig/fat32.img" -s "$WORK/stage"/* ::
head -c 1048576 /dev/urandom > "$WORK/frag.bin"
mcopy -i "$WORK/orig/fat32.img" "$WORK/frag.bin" ::frag.bin
python3 - "$WORK/orig/fat32.img" <<'PY'
# hand-scatter FRAG.BIN's chain: copy each cluster to a strided free
# cluster and re-link (both FAT copies). fsck-clean genuine fragmentation.
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
spc = b[13]; res = struct.unpack_from('<H', b, 14)[0]; nf = b[16]
fsz = struct.unpack_from('<I', b, 36)[0]
first = res + nf*fsz
clus = spc*512
def ent(c, k=0): return struct.unpack_from('<I', b, res*512 + k*fsz*512 + 4*c)[0] & 0x0FFFFFFF
def setent(c, v):
    for k in range(nf):
        o = res*512 + k*fsz*512 + 4*c
        old = struct.unpack_from('<I', b, o)[0]
        struct.pack_into('<I', b, o, (old & 0xF0000000) | v)
def coff(c): return (first + (c-2)*spc)*512
root = struct.unpack_from('<I', b, 44)[0]
chain = []; c = root
while c < 0x0FFFFFF8: chain.append(c); c = ent(c)
data = b''.join(bytes(b[coff(c):coff(c)+clus]) for c in chain)
fc = sz = diroff = None
for i in range(0, len(data), 32):
    e = data[i:i+32]
    if e[0] == 0: break
    if e[0] == 0xE5 or e[11] == 0x0F: continue
    if e[0:8].decode('ascii', 'replace').strip() == 'FRAG':
        fc = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        sz = struct.unpack_from('<I', e, 28)[0]
        diroff = coff(chain[i // clus]) + (i % clus)
        break
assert fc is not None, "FRAG.BIN not found"
need = (sz + clus - 1)//clus
cl = [fc]
for _ in range(need-1): cl.append(ent(cl[-1]))
nclu = (len(b)//512 - first)//spc
used = set(cl)
frees = [c for c in range(2, nclu+2) if c not in used and ent(c) == 0]
stride = max(1, len(frees)//need)
targets = [frees[(i*stride) % len(frees)] for i in range(need)]
assert len(set(targets)) == need
for s, d in zip(cl, targets):
    b[coff(d):coff(d)+clus] = b[coff(s):coff(s)+clus]
for i in range(need-1): setent(targets[i], targets[i+1])
setent(targets[-1], 0x0FFFFFFF)
for c in cl: setent(c, 0)
struct.pack_into('<H', b, diroff+20, (targets[0] >> 16) & 0xFFFF)
struct.pack_into('<H', b, diroff+26, targets[0] & 0xFFFF)
open(p, 'wb').write(bytes(b))
print("  fat32.img: FRAG.BIN scattered to %d single-cluster runs" % need)
PY
fsck.vfat -n "$WORK/orig/fat32.img" 2>&1 | tail -1

# --- fat16.img: 16MB ------------------------------------------------------
dd if=/dev/zero of="$WORK/orig/fat16.img" bs=1M count=16 status=none
mkfs.vfat -F 16 "$WORK/orig/fat16.img" >/dev/null
mcopy -i "$WORK/orig/fat16.img" "$WORK/stage/README" ::README.TXT
mcopy -i "$WORK/orig/fat16.img" "$WORK/stage/Long File Name Number One.txt" ::

# --- exfat.img: 32MB, loop-mount populated (mtools has no exFAT) ----------
dd if=/dev/zero of="$WORK/orig/exfat.img" bs=1M count=32 status=none
mkfs.exfat "$WORK/orig/exfat.img" >/dev/null
sudo -n mount -o loop,uid="$(id -u)" "$WORK/orig/exfat.img" "$WORK/mnt"
cp -r "$WORK/stage"/* "$WORK/mnt/"
head -c 3145728 /dev/urandom > "$WORK/mnt/frag.bin"
sync
sudo -n umount "$WORK/mnt"
# the kernel driver usually lands frag.bin contiguous (NoFatChain). Chain
# mode is part of the format, so force it deterministically: scatter
# frag.bin's clusters to strided free clusters, re-link the FAT chain,
# flip NoFatChain off, and repair the allocation bitmap + the entry-set
# checksum -- a genuine, fsck-clean, chain-mode fragmented file.
python3 - "$WORK/orig/exfat.img" <<'PY'
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
bps = 1 << b[108]; clus = bps << b[109]
fat_off = struct.unpack_from('<I', b, 80)[0]*bps
heap = struct.unpack_from('<I', b, 88)[0]*bps
nclu = struct.unpack_from('<I', b, 92)[0]
root = struct.unpack_from('<I', b, 96)[0]
def ent(c): return struct.unpack_from('<I', b, fat_off + 4*c)[0]
def setent(c, v): struct.pack_into('<I', b, fat_off + 4*c, v)
def coff(c): return heap + (c-2)*clus
chain = []; c = root
while c < 0xFFFFFFF8: chain.append(c); c = ent(c)
data = b''.join(bytes(b[coff(c):coff(c)+clus]) for c in chain)
def dir_img_off(byteoff):
    return coff(chain[byteoff // clus]) + (byteoff % clus)
bmp_fc = bmp_len = None
frag = None
i = 0
while i+32 <= len(data) and data[i] != 0:
    t = data[i]
    if t == 0x81:   # allocation bitmap
        bmp_fc = struct.unpack_from('<I', data, i+20)[0]
        bmp_len = struct.unpack_from('<Q', data, i+24)[0]
    if t == 0x85:
        ns = data[i+1]
        sx = data[i+32:i+64]
        name = b''
        for j in range(2, ns+1):
            e = data[i+32*j:i+32*j+32]
            if e[0] == 0xC1: name += e[2:32]
        if name.decode('utf-16-le', 'replace').rstrip('\0') == 'frag.bin':
            frag = (i, ns, sx[1], struct.unpack_from('<Q', sx, 8)[0],
                    struct.unpack_from('<I', sx, 20)[0])
        i += 32*(1+ns)
    else:
        i += 32
assert frag and bmp_fc, "fixtures not found"
fi, ns, flags, valid, fc = frag
if not (flags & 2):
    print("  exfat.img: frag.bin already chain-mode, leaving it alone")
    sys.exit(0)
need = (valid + clus - 1)//clus
old = [fc + k for k in range(need)]
bmp = bytearray(b[coff(bmp_fc):coff(bmp_fc)+bmp_len])
def bit(c): return (bmp[(c-2)//8] >> ((c-2) % 8)) & 1
def setbit(c, v):
    o = (c-2)//8; m = 1 << ((c-2) % 8)
    bmp[o] = (bmp[o] | m) if v else (bmp[o] & ~m)
used_old = set(old)
frees = [c for c in range(2, nclu+2) if c not in used_old and not bit(c)]
stride = max(1, len(frees)//need)
targets = [frees[(k*stride) % len(frees)] for k in range(need)]
assert len(set(targets)) == need
for s, d in zip(old, targets):
    b[coff(d):coff(d)+clus] = b[coff(s):coff(s)+clus]
for k in range(need-1): setent(targets[k], targets[k+1])
setent(targets[-1], 0xFFFFFFFF)
for c in old: setent(c, 0)
for c in targets: setbit(c, 1)
for c in old: setbit(c, 0)
b[coff(bmp_fc):coff(bmp_fc)+bmp_len] = bytes(bmp)
so = dir_img_off(fi)
sx_off = so + 32
assert b[sx_off] == 0xC0
b[sx_off+1] = flags & ~0x02                    # NoFatChain clear: chain mode
struct.pack_into('<I', b, sx_off+20, targets[0])
cs = 0                                          # entry-set checksum: rotate
for k, byte in enumerate(b[so:so + 32*(1+ns)]): # right 1 + add, skipping the
    if k in (2, 3): continue                    # checksum field itself
    cs = ((cs << 15) | (cs >> 1)) & 0xFFFF
    cs = (cs + byte) & 0xFFFF
struct.pack_into('<H', b, so+2, cs)
open(p, 'wb').write(bytes(b))
print("  exfat.img: frag.bin scattered to %d chain-mode runs (checksum %#06x)"
      % (need, cs))
PY
fsck.exfat -n "$WORK/orig/exfat.img" 2>&1 | tail -1

# --- fat12.img (hand leg only) + the refused text fixture -----------------
dd if=/dev/zero of="$WORK/orig/fat12.img" bs=1K count=1440 status=none
mkfs.vfat -F 12 "$WORK/orig/fat12.img" >/dev/null
head -c 20000 "$WORK/stage/README" > "$WORK/f12.txt"
mcopy -i "$WORK/orig/fat12.img" "$WORK/f12.txt" ::F12.TXT
mmd -i "$WORK/orig/fat12.img" ::SUB
mcopy -i "$WORK/orig/fat12.img" "$WORK/f12.txt" ::SUB/INNER.TXT
head -c 2000 "$WORK/stage/README" > "$WORK/orig/text.img"

echo "== hand self-test (pack only, all four images) =="
cat > "$WORK/mapcheck.py" <<'PY'
import struct, sys
imgf, tablef, recipef, mapf, mdir = sys.argv[1:6]
orig = open(imgf, 'rb').read()
recipe = open(recipef, 'rb').read()
blob = open(mapf, 'rb').read()
assert blob[:4] == b'MRMP', "bad magic"
(count,) = struct.unpack_from('<I', blob, 4)
assert len(blob) == 8 + count*29, "blob length"
mem = {}
for line in open(tablef):
    idx, sname, usize = line.rstrip('\n').split('\t')
    mem[int(idx)] = (int(usize), open("%s/%s" % (mdir, idx), 'rb').read())
pos = 0
runs = {}
for i in range(count):
    off, ln, kind, idx, src = struct.unpack_from('<QQBIQ', blob, 8 + i*29)
    assert off == pos and ln > 0, "entry %d: not a partition" % i
    if kind == 0:
        assert idx == 0 and src + ln <= len(recipe), "entry %d: recipe OOB" % i
        assert recipe[src:src+ln] == orig[off:off+ln], "entry %d: recipe bytes" % i
    else:
        assert idx in mem, "entry %d: unknown idx" % i
        usize, mbytes = mem[idx]
        assert src + ln <= usize, "entry %d: member OOB" % i
        assert mbytes[src:src+ln] == orig[off:off+ln], "entry %d: member bytes" % i
        runs[idx] = runs.get(idx, 0) + 1
    pos = off + ln
assert pos == len(orig), "partition short"
print("    map: %d entries partition [0,%d) byte-exact; multi-run members: %s"
      % (count, len(orig), {k: v for k, v in sorted(runs.items()) if v > 1}))
PY
for img in fat32.img fat16.img exfat.img fat12.img; do
    echo "  == $img =="
    $FATFS enumerate "$WORK/orig/$img" "$WORK/out/$img.table"
    $FATFS strip "$WORK/orig/$img" "$WORK/out/$img.recipe"
    rm -rf "$WORK/mdir" && mkdir -p "$WORK/mdir"
    while IFS=$'\t' read -r idx sname usize; do
        $FATFS extract "$WORK/orig/$img" "$idx" "$WORK/mdir/$idx"
        [ "$(stat -c%s "$WORK/mdir/$idx")" = "$usize" ] \
            || { echo "FAIL: $img member $idx: got $(stat -c%s "$WORK/mdir/$idx"), want $usize"; exit 1; }
    done < "$WORK/out/$img.table"
    $FATFS rebuild "$WORK/out/$img.recipe" "$WORK/mdir" "$WORK/out/$img.rebuilt"
    cmp "$WORK/orig/$img" "$WORK/out/$img.rebuilt" \
        || { echo "FAIL: $img rebuild not bit-exact"; exit 1; }
    echo "    enumerate/extract/strip/rebuild bit-exact ($(stat -c%s "$WORK/orig/$img") bytes, recipe $(stat -c%s "$WORK/out/$img.recipe"))"
    $FATFS map "$WORK/orig/$img" "$WORK/out/$img.map"
    python3 "$WORK/mapcheck.py" "$WORK/orig/$img" "$WORK/out/$img.table" \
        "$WORK/out/$img.recipe" "$WORK/out/$img.map" "$WORK/mdir"
    case "$img" in
    fat32.img|exfat.img)
        # the fragmentation fixtures must really be fragmented: at least one
        # member with many runs (FAT32 FRAG.BIN: 2048; exFAT frag.bin: 768)
        python3 - "$WORK/out/$img.map" <<'PY'
import struct, sys
blob = open(sys.argv[1], 'rb').read()
(count,) = struct.unpack_from('<I', blob, 4)
runs = {}
for i in range(count):
    off, ln, kind, idx, src = struct.unpack_from('<QQBIQ', blob, 8 + i*29)
    if kind == 1: runs[idx] = runs.get(idx, 0) + 1
best = max(runs.values()) if runs else 0
assert best > 100, "fragmented fixture lost its fragmentation (best %d)" % best
print("    fragmentation gate: max %d runs in one member" % best)
PY
        ;;
    esac
    est=$($FATFS estimate "$WORK/orig/$img")
    sum=$(awk -F'\t' '{s += $3} END {print s}' "$WORK/out/$img.table")
    [ "$est" = "$((sum + 64*1024*1024))" ] \
        || { echo "FAIL: $img estimate $est != sum($sum)+64MiB"; exit 1; }
    echo "    estimate: $est (= sum(usize) $sum + 64MiB)"
done
# layout facts for the ranged-read legs (from the fat32 map)
python3 - "$WORK/out/fat32.img.table" "$WORK/out/fat32.img.map" "$WORK" <<'PY'
import struct, sys
tablef, mapf, work = sys.argv[1:4]
elf = frag = None
for line in open(tablef):
    idx, sname, usize = line.rstrip('\n').split('\t')
    if sname == 'ELF.BIN': elf = int(idx)
    if sname == 'FRAG.BIN': frag = int(idx)
assert elf is not None and frag is not None
blob = open(mapf, 'rb').read()
(count,) = struct.unpack_from('<I', blob, 4)
offs = {}
for i in range(count):
    off, ln, kind, idx, src = struct.unpack_from('<QQBIQ', blob, 8 + i*29)
    if kind == 1 and idx in (elf, frag) and idx not in offs:
        offs[idx] = off
with open(work + "/orig/fat32.layout", "w") as f:
    f.write("elf_idx=%d elf_off=%d frag_idx=%d frag_off=%d\n"
            % (elf, offs[elf], frag, offs[frag]))
PY
. "$WORK/orig/fat32.layout"   # elf_idx elf_off frag_idx frag_off
grep -q "ELF.BIN" "$WORK/out/fat32.img.table" || { echo "FAIL: LFN/8.3 table"; exit 1; }
grep -q "Long_File_Name" "$WORK/out/fat32.img.table" || { echo "FAIL: LFN not assembled"; exit 1; }
grep -q "NESTED.TXT" "$WORK/out/fat32.img.table" || { echo "FAIL: subdir not walked"; exit 1; }

echo "== hand refusals (want exit 3) =="
head -c 100000 "$WORK/orig/fat32.img" > "$WORK/out/trunc.img"
python3 -c "
b=bytearray(4096); b[510]=0x55; b[511]=0xAA; b[12]=2; b[13]=1
open('$WORK/out/fake.img','wb').write(bytes(b))"
python3 - "$WORK/orig/fat32.img" "$WORK/out/loop.img" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
res = struct.unpack_from('<H', b, 14)[0]
fsz = struct.unpack_from('<I', b, 36)[0]
for k in range(2):   # cluster 100 is inside BIG.BIN: a self-loop
    struct.pack_into('<I', b, res*512 + k*fsz*512 + 4*100, 100)
open(sys.argv[2], 'wb').write(bytes(b))
PY
for bad in "$WORK/orig/text.img" "$WORK/out/trunc.img" "$WORK/out/fake.img" "$WORK/out/loop.img"; do
    if $FATFS enumerate "$bad" /dev/null 2>/dev/null; then
        echo "FAIL: $bad was not refused"; exit 1
    fi
    echo "  refused: $(basename "$bad")"
done
if $FATFS extract "$WORK/orig/fat32.img" 999 /dev/null 2>/dev/null; then
    echo "FAIL: bad idx not refused"; exit 1
fi
echo "  refused: member idx out of range"

# class-stamp reader + ranged-read harness + unlink helper (repo objects,
# the test-containerpack.sh pattern)
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/helper_exec.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_meta_merge.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
for t in classof rngread cbrm; do
    gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/$t" "$WORK/$t.c" \
        $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
done

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
for f in fat32.img fat16.img exfat.img text.img; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 (containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
for f in fat32.img fat16.img exfat.img; do
    grep -q "$f: fatfs (codecpack)" "$WORK/sweep1.log" \
        || { echo "FAIL: $f not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
done
if grep -q "text.img: fatfs" "$WORK/sweep1.log"; then
    echo "FAIL: text.img was decomposed"; cat "$WORK/sweep1.log"; exit 1
fi
# members batch in the SAME run their container decomposed (WP14b pattern)
grep -q "fat32.img!\*: .* parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text member not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "fat32.img!\*: .* parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|parts -> " "$WORK/sweep1.log"

echo "== sibling sets (incl. the WP16b member maps) =="
for f in fat32.img fat16.img exfat.img; do
    $B/invf-ls "$IMG" | grep -q "$f!mbrt"  || { echo "FAIL: $f member table missing"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbrmap" || { echo "FAIL: $f member map missing"; exit 1; }
done
$B/invf-ls "$IMG" | grep "fat32.img!mbr[0-9]*-EMPTY.DAT" | grep -q "0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; $B/invf-ls "$IMG"; exit 1; }
if $B/invf-ls "$IMG" | grep -q "text\.img!"; then
    echo "FAIL: refused text.img gained siblings"; exit 1
fi
echo "members + tables + maps present; empty member is 0 bytes; text.img clean"

echo "== class stamps =="
for f in fat32.img fat16.img exfat.img; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=3 algo=18 gen=1" ] || { echo "FAIL: want CONTAINER{FATFS=18,1}"; exit 1; }
done
# member classes via the stored member table (idx <-> sname)
$B/invf-cat "$IMG" "fat32.img!mbrt" "$WORK/out/fat32.mbrt" >/dev/null
README_IDX=$(awk -F'\t' '$2=="README"{print $1}' "$WORK/out/fat32.mbrt")
ELF_IDX=$(awk -F'\t'   '$2=="ELF.BIN"{print $1}' "$WORK/out/fat32.mbrt")
RAND_IDX=$(awk -F'\t'  '$2=="BIG.BIN"{print $1}' "$WORK/out/fat32.mbrt")
[ -n "$README_IDX" ] && [ -n "$ELF_IDX" ] && [ -n "$RAND_IDX" ] \
    || { echo "FAIL: member table incomplete"; cat "$WORK/out/fat32.mbrt"; exit 1; }
MBR=$(printf "fat32.img!mbr%04d-README" "$README_IDX")
C=$("$WORK/classof" "$IMG" "$MBR")
echo "  $MBR: $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1}"; exit 1; }
MBR=$(printf "fat32.img!mbr%04d-ELF.BIN" "$ELF_IDX")
C=$("$WORK/classof" "$IMG" "$MBR")
echo "  $MBR: $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; }
MBR=$(printf "fat32.img!mbr%04d-BIG.BIN" "$RAND_IDX")
C=$("$WORK/classof" "$IMG" "$MBR")
echo "  $MBR: $C"
[ "$C" = "none" ] || { echo "FAIL: random member should stay unclassified"; exit 1; }
C=$("$WORK/classof" "$IMG" text.img)
echo "  text.img: $C"
case "$C" in *algo=18*) echo "FAIL: text.img carries a pack stamp"; exit 1;; esac

echo "== verify --deep (reads every container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (all three images + the refused text) =="
ok=1
for f in fat32.img fat16.img exfat.img text.img; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    echo "  $f: $a"
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1

echo "== direct member reads bit-exact =="
MBR=$(printf "fat32.img!mbr%04d-README" "$README_IDX")
$B/invf-cat "$IMG" "$MBR" "$WORK/out/m.readme" >/dev/null
cmp -s "$WORK/stage/README" "$WORK/out/m.readme" || { echo "MISMATCH README member"; exit 1; }
MBR=$(printf "fat32.img!mbr%04d-ELF.BIN" "$ELF_IDX")
$B/invf-cat "$IMG" "$MBR" "$WORK/out/m.elf" >/dev/null
cmp -s "$WORK/stage/elf.bin" "$WORK/out/m.elf" || { echo "MISMATCH ELF member"; exit 1; }
$B/invf-cat "$IMG" "exfat.img!mbrt" "$WORK/out/exfat.mbrt" >/dev/null
XELF_IDX=$(awk -F'\t' '$2=="elf.bin"{print $1}' "$WORK/out/exfat.mbrt")
[ -n "$XELF_IDX" ] || { echo "FAIL: exfat member table"; cat "$WORK/out/exfat.mbrt"; exit 1; }
MBR=$(printf "exfat.img!mbr%04d-elf.bin" "$XELF_IDX")
$B/invf-cat "$IMG" "$MBR" "$WORK/out/m.xelf" >/dev/null
cmp -s "$WORK/stage/elf.bin" "$WORK/out/m.xelf" || { echo "MISMATCH exfat ELF member"; exit 1; }
echo "README / ELF.BIN (fat32) / elf.bin (exfat) members bit-exact"

echo "== ranged reads (the WP16b local splice) =="
rng() {  # rng <off> <len> <tag>
    dd if="$WORK/orig/fat32.img" of="$WORK/out/ref.$3" bs=1 skip="$1" count="$2" 2>/dev/null
    "$WORK/rngread" "$IMG" fat32.img "$1" "$2" "$WORK/out/got.$3" >/dev/null
    cmp -s "$WORK/out/ref.$3" "$WORK/out/got.$3" \
        || { echo "FAIL: range $3 (off=$1 len=$2) mismatch"; exit 1; }
}
rng 0 4096 head                          # boot + reserved (recipe)
rng $((elf_off + 4096)) 65536 midmember  # strictly inside the ELF member
rng $((frag_off)) 65536 fragruns         # ~128 scattered 512B member runs
rng $((64*1024*1024 - 1000)) 1000 tail   # the container tail (free space)
"$WORK/rngread" "$IMG" fat32.img 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/fat32.img" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole image via 64K ranged windows mismatch"; exit 1; }
echo "head / mid-member / fragmented-runs / tail / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fat32.img "$WORK/out/absent.f32" >/dev/null
cmp -s "$WORK/orig/fat32.img" "$WORK/out/absent.f32" \
    || { echo "FAIL: pack-absent fat32.img read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" exfat.img "$WORK/out/absent.x" >/dev/null
cmp -s "$WORK/orig/exfat.img" "$WORK/out/absent.x" \
    || { echo "FAIL: pack-absent exfat.img read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fat32.img \
    $((elf_off + 4096)) 65536 "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
echo "pack-absent: fat32 + exfat whole reads and a ranged read bit-exact, zero pack exec"

echo "== sweep #2: idempotent =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-fired a containerpack"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt" <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
echo "no re-decomposition"

echo "== delete cascade (vol_unlink, the FUSE path) =="
"$WORK/cbrm" "$IMG" fat32.img exfat.img
if $B/invf-ls "$IMG" | grep -q "fat32\.img!\|exfat\.img!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "fat32.img + exfat.img: all members, tables and maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivors bit-exact =="
for f in fat16.img text.img; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/surv.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/surv.$f" || { echo "FAIL: $f drifted"; exit 1; }
done
echo "fat16.img + text.img bit-exact after the deletes"

echo "== admission leg: INVFS_ARC_BYTES=8M (tiny ARC) =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/fat16.img" fat16.img >/dev/null
# the 16MB container exceeds the 8MB whole-file ARC budget: policy refusal
# before any strip/extract; GENERIC_MEMLIMIT{18,1}, bit-exact generic read
INVFS_ARC_BYTES=8M $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "fatfs (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under an 8MB ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" fat16.img)
echo "  fat16.img (tiny ARC): $C"
[ "$C" = "cls=5 algo=18 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{FATFS=18,1}"; exit 1; }
$B/invf-cat "$IMGMEM" fat16.img "$WORK/out/memlim.f16" >/dev/null
cmp -s "$WORK/orig/fat16.img" "$WORK/out/memlim.f16" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "== ratio demo =="
for f in fat32.img fat16.img exfat.img fat12.img; do
    osz=$(stat -c%s "$WORK/orig/$f")
    rsz=$(stat -c%s "$WORK/out/$f.recipe")
    msz=$(awk -F'\t' '{s += $3} END {print s}' "$WORK/out/$f.table")
    printf "  %-10s original %9d  recipe %9d (%.1f%%)  member bytes %9d\n" \
        "$f" "$osz" "$rsz" "$(python3 -c "print(100.0*$rsz/$osz)")" "$msz"
done

echo "FATFS E2E: PASS"
