#!/bin/bash
# test-ntfs.sh — WP16a/WP16b containerpack e2e for the ntfs pack
# (tools/codecpacks/ntfs.codecpack: NTFS image -> per-file members).
#
#   fixtures: mkfs.ntfs (ntfs-3g) 64 MB image, populated over a sudo
#   loop-mount: text file (busybox .c -> PPMd-batchable member), an
#   x86-64 ELF (ZSTD/BCJ-batchable), a subdir file, a 16 MB sparse file
#   (hole runs -> zeros), an empty file, a tiny resident-$DATA file (NOT
#   a member), a hardlink pair (ONE member), and a >4 MB multi-run file
#   (free space fragmented by zero-pin files + hole punching; ntfs-3g
#   writeback coalesces interleaved writes, so pins it is).
#   Hand self-test first (enumerate/extract/strip/rebuild/map/cmp, plus
#   the decline legs: 4K-sector image, truncated image, bad magic,
#   corrupt fixup on record 5 AND record 0 — and the tolerance control:
#   a broken fixup trailer on a FREE record must NOT refuse), then the
#   FS e2e mirroring test-containerpack.sh:
#   sweep -> class stamps CONTAINER{20,1} / TEXT / BATCHED_BIN ->
#   verify --deep -> sha256 bit-exact (container AND direct member
#   reads) -> ranged reads (mid-member / recipe<->member / member<->
#   member / head / tail / whole-by-windows) -> pack-ABSENT reads via
#   the self-describing map -> sweep 2 idle -> ratio demo -> delete
#   cascade -> fsck -> survivor bit-exact.
#   Separate-image legs: DEFER_ENOSPC (cls=9{20,1} on a tight volume,
#   re-arms after free), admission (GENERIC_MEMLIMIT{20,1} via a 64K
#   decode limit AND via a tiny ARC), and a decline volume holding the
#   two refused images (4K sectors; zero-member image -> generic,
#   bit-exact, no pack stamp, fsck clean).
#
#   GOTCHA (pre-existing, pack-independent): the FS's sweep-time dedupe
#   shares identical compressed segments across files. Zero-content
#   members (the pins, the sparse file's holes) and a zero-heavy generic
#   survivor (any fresh fs image) DO share such segments, and the
#   delete cascade's block accounting does not refcount across that
#   boundary: fsck then reports "missing" blocks (referenced but freed).
#   Minimal repro, no packs involved: import two 8 MB all-zero files,
#   sweep once, delete one -> fsck "missing: 1". The pack's own cascade
#   is fsck-clean; the test therefore keeps the main volume's survivor
#   zero-free (urandom) and runs the decline images on their own volume.
#   See test-fatfs.sh, whose all-random members dodge the same trap.
#
# Run from the repo root after `make`:  bash tools/test-ntfs.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
PACK=$REPO/tools/codecpacks/ntfs.codecpack
WORK=/dev/shm/wp16ntfs
IMG=wp16ntfs.img
IMGD=wp16ntfs-decl.img
IMGT=wp16ntfs-tight.img
IMGM=wp16ntfs-mem.img
IMGA=wp16ntfs-arc.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/nopacks" "$WORK/mbr" "$WORK/src"
cd /dev/shm
rm -f "$IMG" "$IMGD" "$IMGT" "$IMGM" "$IMGA"

cleanup_mount() { sudo -n umount "$WORK/mnt" 2>/dev/null || true; }
trap cleanup_mount EXIT

echo "== tools =="
command -v cc        >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3   >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
command -v mkfs.ntfs >/dev/null || { echo "FAIL: mkfs.ntfs (ntfs-3g) not installed"; exit 1; }
command -v ntfs-3g   >/dev/null || { echo "FAIL: ntfs-3g not installed"; exit 1; }
sudo -n true         2>/dev/null || { echo "FAIL: passwordless sudo required (loop mounts)"; exit 1; }

echo "== build the pack (cc -O2 -Wall -Wextra -Werror; warning-free required) =="
WOUT=$(cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/ntfs" "$PACK/ntfs.c" 2>&1) \
    || { echo "FAIL: pack build failed"; echo "$WOUT"; exit 1; }
[ -z "$WOUT" ] || { echo "FAIL: pack build not warning-free"; echo "$WOUT"; exit 1; }
NTFS=$PACK/bin/ntfs
echo "pack binary: $NTFS"

echo "== generate content fixtures =="
# text member: a real busybox .c (>100 KB); the .txt name keeps it in the
# text family for the same-run PPMd batching leg
for c in editors/awk.c editors/vi.c miscutils/bc.c networking/tls.c shell/ash.c; do
    if [ -f "$REPO/tools/busybox-src/$c" ] && [ "$(stat -c%s "$REPO/tools/busybox-src/$c")" -gt 100000 ]; then
        cp "$REPO/tools/busybox-src/$c" "$WORK/src/big.txt"; break
    fi
done
[ -s "$WORK/src/big.txt" ] || { echo "FAIL: no >100KB busybox .c fixture"; exit 1; }
# a second, smaller .c for the subdir member
for c in coreutils/ls.c util-linux/fdisk.c networking/ping.c shell/hush.c; do
    if [ -f "$REPO/tools/busybox-src/$c" ] && [ "$(stat -c%s "$REPO/tools/busybox-src/$c")" -gt 20000 ]; then
        cp "$REPO/tools/busybox-src/$c" "$WORK/src/nested.c"; break
    fi
done
[ -s "$WORK/src/nested.c" ] || { echo "FAIL: no >20KB busybox .c fixture"; exit 1; }
# ELF member: a real x86-64 binary
for p in /usr/bin/ls /bin/ls /usr/bin/passwd /usr/bin/gpg /bin/bash; do
    if [ -f "$p" ] && [ ! -L "$p" ] && [ "$(stat -c%s "$p")" -gt 65536 ] && \
       [ "$(dd if="$p" bs=4 count=1 status=none | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
        cp "$p" "$WORK/src/program.elf"; break
    fi
done
[ -s "$WORK/src/program.elf" ] || { echo "FAIL: no x86-64 ELF fixture"; exit 1; }
# frag.bin: 10 MB of a repeated 4K random block (compressible; content is
# irrelevant, the RUNS are the fixture)
python3 - "$WORK" <<'PY'
import random, sys
w = sys.argv[1]
r = random.Random(5)
blk = r.randbytes(4096)
with open(w + "/src/frag.bin", "wb") as f:
    for _ in range(2560):
        f.write(blk)
# sparse.bin pristine copy: 16 MB, real data only at two 16 KB windows
p1 = r.randbytes(16384)
p2 = r.randbytes(16384)
open(w + "/src/sparse-p1.bin", "wb").write(p1)
open(w + "/src/sparse-p2.bin", "wb").write(p2)
buf = bytearray(16 * 1024 * 1024)
buf[100*4096 : 100*4096+16384] = p1
buf[3000*4096 : 3000*4096+16384] = p2
open(w + "/src/sparse.bin", "wb").write(bytes(buf))
PY
echo "  big.txt      $(stat -c%s "$WORK/src/big.txt") (text member)"
echo "  nested.c     $(stat -c%s "$WORK/src/nested.c") (subdir text member)"
echo "  program.elf  $(stat -c%s "$WORK/src/program.elf") (ELF member)"
echo "  frag.bin     $(stat -c%s "$WORK/src/frag.bin") (multi-run member)"
echo "  sparse.bin   $(stat -c%s "$WORK/src/sparse.bin") (sparse member)"

echo "== build the NTFS image (mkfs.ntfs + sudo loop-mount population) =="
mkdir -p "$WORK/mnt"
dd if=/dev/zero of="$WORK/fs.ntfs" bs=1M count=64 status=none
mkfs.ntfs -Q -F "$WORK/fs.ntfs" >/dev/null
sudo -n mount -o loop -t ntfs-3g "$WORK/fs.ntfs" "$WORK/mnt"
sudo -n bash -s "$WORK" <<'SH'
set -e
W=$1
M=$W/mnt
cp "$W/src/big.txt"     "$M/big.txt"
cp "$W/src/program.elf" "$M/program.elf"
mkdir -p "$M/subdir"
cp "$W/src/nested.c"    "$M/subdir/nested.c"
printf 'tiny resident content\n' > "$M/tiny.txt"   # resident $DATA: never a member
touch "$M/empty.txt"                               # no/rezident-0 $DATA: no member
ln "$M/big.txt" "$M/hard.txt"                      # hardlink: ONE member, two names
# sparse: 16 MB nominal, two 16 KB real windows
truncate -s 16M "$M/sparse.bin"
dd if="$W/src/sparse-p1.bin" of="$M/sparse.bin" bs=4096 seek=100 conv=notrunc status=none
dd if="$W/src/sparse-p2.bin" of="$M/sparse.bin" bs=4096 seek=3000 conv=notrunc status=none
# fragment the free space: zero pins to near-full, punch every other hole
for i in 1 2 3 4 5 6 7 8 9; do dd if=/dev/zero of="$M/pin_$i.bin" bs=1M count=6 status=none; done
sync
rm -f "$M/pin_2.bin" "$M/pin_4.bin" "$M/pin_6.bin" "$M/pin_8.bin"
sync
cp "$W/src/frag.bin" "$M/frag.bin"                 # >4MB, forced into holes
sync
SH
sudo -n umount "$WORK/mnt"
echo "  fs.ntfs: $(stat -c%s "$WORK/fs.ntfs") bytes, populated"

echo "== decline-leg fixtures =="
# 4K-sector image: valid NTFS, outside the v1 support window -> decline
dd if=/dev/zero of="$WORK/fs4k.ntfs" bs=1M count=32 status=none
mkfs.ntfs -Q -F -s 4096 "$WORK/fs4k.ntfs" >/dev/null
sudo -n mount -o loop -t ntfs-3g "$WORK/fs4k.ntfs" "$WORK/mnt"
echo "four k sector file" | sudo -n tee "$WORK/mnt/afile.txt" >/dev/null
sudo -n umount "$WORK/mnt"
# zero-member image: only resident/tiny files -> enumerate ok, empty table
dd if=/dev/zero of="$WORK/fs-resident.ntfs" bs=1M count=16 status=none
mkfs.ntfs -Q -F "$WORK/fs-resident.ntfs" >/dev/null
sudo -n mount -o loop -t ntfs-3g "$WORK/fs-resident.ntfs" "$WORK/mnt"
echo "small" | sudo -n tee "$WORK/mnt/small.txt" >/dev/null
sudo -n touch "$WORK/mnt/empty.txt"
sudo -n umount "$WORK/mnt"
# corrupt: fixup trailer of an in-use record broken (record 5 = root dir)
cp "$WORK/fs.ntfs" "$WORK/fs-corrupt.ntfs"
python3 - "$WORK/fs-corrupt.ntfs" <<'PY'
import sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
img[4*4096 + 5*1024 + 510] ^= 0xFF      # record 5 sector-0 USN trailer
open(p, "wb").write(bytes(img))
PY
# corrupt: record 0's ($MFT) own first-sector USN trailer broken
cp "$WORK/fs.ntfs" "$WORK/fs-corrupt0.ntfs"
python3 - "$WORK/fs-corrupt0.ntfs" <<'PY'
import sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
img[4*4096 + 510] ^= 0xFF               # record 0 sector-0 USN trailer
open(p, "wb").write(bytes(img))
PY
# control: a FREE record's trailer broken (the deleted pins leave free
# records behind) — stale metadata the pack must TOLERATE: the record's
# bytes are recipe verbatim either way
cp "$WORK/fs.ntfs" "$WORK/fs-freefix.ntfs"
python3 - "$WORK/fs-freefix.ntfs" <<'PY'
import struct, sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
bps, cs, recsz = 512, 4096, 1024
def fixup(rec):
    uo = struct.unpack_from("<H", rec, 4)[0]
    un = struct.unpack_from("<H", rec, 6)[0]
    usn = rec[uo:uo+2]
    r = bytearray(rec)
    for i in range(len(rec)//bps):
        t = (i+1)*bps-2
        assert r[t:t+2] == usn
        r[t:t+2] = rec[uo+2+2*i:uo+4+2*i]
    return bytes(r)
def runlist(buf, off):
    runs = []; pos = off; lcn = 0
    while True:
        h = buf[pos]; pos += 1
        if h == 0: break
        ll = h & 0xF; ol = h >> 4
        rl = int.from_bytes(buf[pos:pos+ll], "little"); pos += ll
        dl = int.from_bytes(buf[pos:pos+ol], "little", signed=True); pos += ol
        lcn += dl; runs.append((lcn, rl))
    return runs
rec0 = fixup(img[4*cs:4*cs+recsz])
aoff = struct.unpack_from("<H", rec0, 20)[0]
mruns = None; mreal = None
while True:
    t = struct.unpack_from("<I", rec0, aoff)[0]
    if t == 0xFFFFFFFF: break
    al = struct.unpack_from("<I", rec0, aoff+4)[0]
    if t == 0x80:
        ro = struct.unpack_from("<H", rec0, aoff+32)[0]
        mruns = runlist(rec0, aoff+ro); mreal = struct.unpack_from("<Q", rec0, aoff+48)[0]
    aoff += al
def recoff(rn):
    off = rn*recsz; vcn = off//cs; w = off % cs; pos = 0
    for lcn, rl in mruns:
        if pos <= vcn < pos+rl: return (lcn+vcn-pos)*cs+w
        pos += rl
    raise ValueError
n = 0
for rn in range(16, mreal//recsz):
    raw = bytes(img[recoff(rn):recoff(rn)+recsz])
    if raw[:4] != b"FILE": continue
    flags = struct.unpack_from("<H", raw, 22)[0]   # raw flags: below the
    if flags & 1: continue                          # first trailer
    img[recoff(rn) + 510] ^= 0xFF                   # break its USN trailer
    n += 1
    if n == 2: break
assert n >= 2, "want >=2 free records to corrupt"
open(p, "wb").write(bytes(img))
print("  fs-freefix.ntfs: %d free records' trailers broken" % n)
PY
# truncated + bad-magic images
head -c 30000 "$WORK/fs.ntfs" > "$WORK/fs-trunc.ntfs"
echo "NOTNTFS" > "$WORK/fs-bad.ntfs"
# tailend regression: rewrite tiny.txt's record so the attribute END marker
# lands in the record's last 16 bytes (a real parse bug once: the walker
# demanded 16 free bytes before the 4-byte END type and declined a whole
# busy image). Resident $DATA is padded; fixups recomputed; the file stays
# resident (NOT a member) and the image stays valid.
cp "$WORK/fs.ntfs" "$WORK/fs-tailend.ntfs"
python3 - "$WORK/fs-tailend.ntfs" <<'PY'
import struct, sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
bps, cs, recsz = 512, 4096, 1024
def unfix(rec):
    uo = struct.unpack_from("<H", rec, 4)[0]
    un = struct.unpack_from("<H", rec, 6)[0]
    usn = rec[uo:uo+2]
    r = bytearray(rec)
    for i in range(len(rec)//bps):
        t = (i+1)*bps-2
        assert r[t:t+2] == usn
        r[t:t+2] = rec[uo+2+2*i:uo+4+2*i]
    return bytes(r), uo, un, usn
def refix(r, uo, un, usn):
    r = bytearray(r)
    for i in range(len(r)//bps):
        t = (i+1)*bps-2
        r[uo+2+2*i] = r[t]; r[uo+3+2*i] = r[t+1]
        r[t] = usn[0]; r[t+1] = usn[1]
    return r
def runlist(buf, off):
    runs = []; pos = off; lcn = 0
    while True:
        h = buf[pos]; pos += 1
        if h == 0: break
        ll = h & 0xF; ol = h >> 4
        rl = int.from_bytes(buf[pos:pos+ll], "little"); pos += ll
        dl = int.from_bytes(buf[pos:pos+ol], "little", signed=True); pos += ol
        lcn += dl; runs.append((lcn, rl))
    return runs
rec0, _, _, _ = unfix(img[4*cs:4*cs+recsz])
aoff = struct.unpack_from("<H", rec0, 20)[0]
while True:
    t = struct.unpack_from("<I", rec0, aoff)[0]
    if t == 0xFFFFFFFF: break
    al = struct.unpack_from("<I", rec0, aoff+4)[0]
    if t == 0x80:
        ro = struct.unpack_from("<H", rec0, aoff+32)[0]
        mruns = runlist(rec0, aoff+ro); mreal = struct.unpack_from("<Q", rec0, aoff+48)[0]
    aoff += al
def recoff(rn):
    off = rn*recsz; vcn = off//cs; w = off % cs; pos = 0
    for lcn, rl in mruns:
        if pos <= vcn < pos+rl: return (lcn+vcn-pos)*cs+w
        pos += rl
    raise ValueError
# find tiny.txt's record
target = None
for rn in range(16, mreal//recsz):
    raw = bytes(img[recoff(rn):recoff(rn)+recsz])
    if raw[:4] != b"FILE": continue
    rec, uo, un, usn = unfix(raw)
    if not (struct.unpack_from("<H", rec, 22)[0] & 1): continue
    aoff = struct.unpack_from("<H", rec, 20)[0]
    while True:
        t = struct.unpack_from("<I", rec, aoff)[0]
        if t == 0xFFFFFFFF: break
        al = struct.unpack_from("<I", rec, aoff+4)[0]
        if t == 0x30:
            co = struct.unpack_from("<H", rec, aoff+20)[0]; c = rec[aoff+co:]
            if c[66:66+c[64]*2].decode("utf-16le") == "tiny.txt":
                target = (rn, uo, un, usn)
        aoff += al
    if target: break
assert target, "tiny.txt record not found"
rn, uo, un, usn = target
rec, _, _, _ = unfix(img[recoff(rn):recoff(rn)+recsz])
# rebuild the record: keep every attribute up to the resident $DATA,
# regrow $DATA so the END marker lands at 1016 (>= recsz-16, the old
# decline window), keep everything else identical
aoff = struct.unpack_from("<H", rec, 20)[0]
out = bytearray()
data_attr = None
while True:
    t = struct.unpack_from("<I", rec, aoff)[0]
    if t == 0xFFFFFFFF: break
    al = struct.unpack_from("<I", rec, aoff+4)[0]
    if t == 0x80 and not rec[aoff+8]:
        data_attr = bytearray(rec[aoff:aoff+al])
        break
    out += rec[aoff:aoff+al]
    aoff += al
assert data_attr is not None, "tiny.txt has no resident $DATA"
first_off = struct.unpack_from("<H", rec, 20)[0]
# content sits at coff within the attr; pick a new content length so that
# first_off + len(kept) + new_alen == 1016, alen 8-aligned
coff = struct.unpack_from("<H", data_attr, 20)[0]
new_alen = 1016 - first_off - len(out)
assert new_alen >= coff + 8 and new_alen % 8 == 0, new_alen
new_clen = new_alen - coff
payload = (b"TAILEND-REGRESSION " * (new_clen // 19 + 1))[:new_clen]
na = bytearray(data_attr[:coff])
struct.pack_into("<I", na, 4, new_alen)       # attribute length
struct.pack_into("<I", na, 16, new_clen)      # content size
na += payload
out += na
newrec = bytearray(rec[:first_off]) + out
newrec += struct.pack("<I", 0xFFFFFFFF)       # END at 1016
newrec += bytes(recsz - len(newrec))
struct.pack_into("<I", newrec, 24, len(newrec))   # used size (incl. tail)
img[recoff(rn):recoff(rn)+recsz] = refix(newrec, uo, un, usn)
open(p, "wb").write(bytes(img))
print("  fs-tailend.ntfs: tiny.txt record %d, END marker at %d" % (rn, 1016))
PY
# a zero-free survivor for the cascade leg: urandom shares no dedupe
# segments with the container's zero-heavy members (see the gotcha in the
# test header)
head -c 1048576 /dev/urandom > "$WORK/survivor.bin"
echo "  fs4k / fs-resident / fs-corrupt / fs-corrupt0 / fs-freefix / fs-tailend / fs-trunc / fs-bad / survivor ready"

echo "== hand self-test: enumerate =="
$NTFS enumerate "$WORK/fs.ntfs" "$WORK/table"
cat "$WORK/table"
python3 - "$WORK" <<'PY'
import sys
w = sys.argv[1]
rows = []
for line in open(w + "/table"):
    line = line.rstrip("\n")
    if not line:
        continue
    idx, name, usize = line.split("\t")
    rows.append((int(idx), name, int(usize)))
assert rows, "no members"
idxs = [r[0] for r in rows]
assert len(idxs) == len(set(idxs)), "duplicate idx"
names = sorted(r[1] for r in rows)
print("  members:", len(rows), "names:", names)
# expected membership: 5 pins + frag + sparse + big-or-hard + elf + nested
assert sum(1 for n in names if n.startswith("pin_")) == 5, "want 5 pins"
assert "frag.bin" in names and "sparse.bin" in names
assert "program.elf" in names and "nested.c" in names
# the hardlink pair shares ONE record -> exactly one of the two names
assert ("big.txt" in names) != ("hard.txt" in names), "hardlink must be one member"
# resident/empty files are NOT members
assert "tiny.txt" not in names and "empty.txt" not in names
for _i, n, u in rows:
    if n == "sparse.bin": assert u == 16*1024*1024
    if n == "frag.bin":   assert u == 10*1024*1024
open(w + "/members", "w").write("\n".join("%d %s %d" % r for r in rows) + "\n")
PY

echo "== hand self-test: extract every member, byte-compare =="
while read -r idx name usize; do
    $NTFS extract "$WORK/fs.ntfs" "$idx" "$WORK/mbr/$idx"
    got=$(stat -c%s "$WORK/mbr/$idx")
    [ "$got" = "$usize" ] || { echo "FAIL: member $idx size $got != $usize"; exit 1; }
    case "$name" in
        big.txt|hard.txt) cmp -s "$WORK/src/big.txt" "$WORK/mbr/$idx" || { echo "FAIL: big.txt member content"; exit 1; };;
        nested.c)         cmp -s "$WORK/src/nested.c" "$WORK/mbr/$idx" || { echo "FAIL: nested.c member content"; exit 1; };;
        program.elf)      cmp -s "$WORK/src/program.elf" "$WORK/mbr/$idx" || { echo "FAIL: elf member content"; exit 1; };;
        frag.bin)         cmp -s "$WORK/src/frag.bin" "$WORK/mbr/$idx" || { echo "FAIL: frag.bin member content"; exit 1; };;
        sparse.bin)       cmp -s "$WORK/src/sparse.bin" "$WORK/mbr/$idx" || { echo "FAIL: sparse member content (holes->zeros)"; exit 1; };;
        pin_*.bin)        python3 -c "
import sys
d = open('$WORK/mbr/$idx','rb').read()
assert len(d) == 6291456 and d == bytes(6291456), 'pin not all zeros'
" || { echo "FAIL: pin member $idx not all zeros"; exit 1; };;
        *) echo "FAIL: unexpected member $name"; exit 1;;
    esac
done < "$WORK/members"
echo "  all members extract bit-exact (sparse holes = zeros)"

echo "== hand self-test: strip -> rebuild -> cmp =="
$NTFS strip "$WORK/fs.ntfs" "$WORK/recipe"
echo "  recipe: $(stat -c%s "$WORK/recipe") bytes (image: $(stat -c%s "$WORK/fs.ntfs"))"
$NTFS rebuild "$WORK/recipe" "$WORK/mbr" "$WORK/rebuilt.ntfs"
cmp -s "$WORK/fs.ntfs" "$WORK/rebuilt.ntfs" || { echo "FAIL: rebuild not bit-exact"; exit 1; }
echo "  rebuild bit-exact"

echo "== hand self-test: map (MRMP) validation =="
$NTFS map "$WORK/fs.ntfs" "$WORK/map"
python3 - "$WORK" <<'PY'
import struct, sys
w = sys.argv[1]
img = open(w + "/fs.ntfs", "rb").read()
mapb = open(w + "/map", "rb").read()
recipe = open(w + "/recipe", "rb").read()
assert mapb[:4] == b"MRMP"
n, = struct.unpack_from("<I", mapb, 4)
assert len(mapb) == 8 + n * 29, "map blob size"
pos = 0
members = {}
frag_runs = 0
first_member = None
r2m = m2r = m2m = None
prev = None
ents = []
for i in range(n):
    off, ln, kind, idx, so = struct.unpack_from("<QQBIQ", mapb, 8 + i*29)
    ents.append((off, ln, kind, idx, so))
    assert off == pos and ln > 0, "partition broken at %d" % i
    if kind == 0:
        assert idx == 0 and so + ln <= len(recipe)
        assert recipe[so:so+ln] == img[off:off+ln], "recipe range %d lies" % i
    else:
        members.setdefault(idx, 0)
        members[idx] += ln
        mbr = open(w + "/mbr/%d" % idx, "rb").read()
        assert so + ln <= len(mbr)
        assert mbr[so:so+ln] == img[off:off+ln], "member range %d lies" % i
    pos += ln
assert pos == len(img), "partition does not cover the image"
# member disk bytes <= usize (sparse member: far less)
tbl = {}
for line in open(w + "/table"):
    i_, _n, u_ = line.split("\t")
    tbl[int(i_)] = int(u_)
for idx, b in members.items():
    assert b <= tbl[idx], "member %d map bytes over usize" % idx
# frag.bin must be multi-run
frag_idx = next(i_ for i_, n_, u_ in
                (l.split("\t") for l in open(w + "/table")) if n_ == "frag.bin")
frag_idx = int(frag_idx)
frag_ents = [e for e in ents if e[2] == 1 and e[3] == frag_idx]
assert len(frag_ents) >= 2, "frag.bin is not multi-run"
# the sparse member must have exactly 2 real runs of 16 KB
sp_idx = int(next(i_ for i_, n_, u_ in
                  (l.split("\t") for l in open(w + "/table")) if n_ == "sparse.bin"))
sp_ents = [e for e in ents if e[2] == 1 and e[3] == sp_idx]
assert len(sp_ents) == 2 and all(e[1] == 16384 for e in sp_ents), "sparse runs"
print("  partition exact: %d entries; frag.bin runs: %d; sparse real runs: %d"
      % (n, len(frag_ents), len(sp_ents)))
# ranged-read offsets for the FS legs
def emit(name, off, ln):
    print_off = "%s_OFF=%d %s_LEN=%d" % (name, off, name, ln)
    return print_off
out = []
# mid-member: inside frag's first run (clamped to stay strictly inside)
e = frag_ents[0]
mo = e[0] + max(0, e[1]//2 - 32768)
ml = min(65536, e[1] - (mo - e[0]))
out.append(emit("MIDM", mo, ml))
# recipe->member boundary: first RECIPE followed by MEMBER
for i in range(n - 1):
    if ents[i][2] == 0 and ents[i+1][2] == 1:
        b = ents[i+1][0]
        out.append(emit("R2M", b - 32, 64))
        break
# member->recipe boundary
for i in range(n - 1):
    if ents[i][2] == 1 and ents[i+1][2] == 0:
        b = ents[i+1][0]
        out.append(emit("M2R", b - 32, 64))
        break
# member->member boundary (two members' runs adjacent)
for i in range(n - 1):
    if ents[i][2] == 1 and ents[i+1][2] == 1 and ents[i][3] != ents[i+1][3]:
        b = ents[i+1][0]
        out.append(emit("M2M", b - 32, 64))
        break
# sparse member: read across its hole (content window 95K..130K crosses
# the first real run into the big hole): served via member bytes
out.append(emit("SPARSE", 0, 0))   # placeholder; the sparse member's DISK
                                   # offsets are not contiguous — ranged
                                   # reads target the image, not content
out.append(emit("TAIL", len(img) - 1000, 1000))
out.append(emit("HEAD", 0, 64))
open(w + "/layout", "w").write("\n".join(out) + "\n")
print("  layout:", " ".join(x.split()[0] for x in out if "OFF" in x))
PY
. "$WORK/layout"

echo "== hand self-test: estimate =="
EST=$($NTFS estimate "$WORK/fs.ntfs")
SUM=$(awk -F'\t' '{s+=$3} END {print s}' "$WORK/table")
[ "$EST" = "$((SUM + 67108864))" ] || { echo "FAIL: estimate $EST != sum+64MiB $((SUM + 67108864))"; exit 1; }
echo "  estimate $EST == sum($SUM) + 64 MiB"

echo "== hand self-test: declines (exit 3) / tolerances =="
$NTFS enumerate "$WORK/fs4k.ntfs" /dev/null      && { echo "FAIL: 4K-sector accepted"; exit 1; } || [ $? = 3 ] || { echo "FAIL: 4K-sector rc != 3"; exit 1; }
$NTFS enumerate "$WORK/fs-trunc.ntfs" /dev/null  && { echo "FAIL: truncated accepted"; exit 1; } || [ $? = 3 ] || { echo "FAIL: trunc rc != 3"; exit 1; }
$NTFS enumerate "$WORK/fs-bad.ntfs" /dev/null    && { echo "FAIL: bad magic accepted"; exit 1; } || [ $? = 3 ] || { echo "FAIL: bad-magic rc != 3"; exit 1; }
$NTFS enumerate "$WORK/fs-corrupt.ntfs" /dev/null && { echo "FAIL: corrupt fixup (rec 5) accepted"; exit 1; } || [ $? = 3 ] || { echo "FAIL: corrupt rc != 3"; exit 1; }
$NTFS enumerate "$WORK/fs-corrupt0.ntfs" /dev/null && { echo "FAIL: corrupt fixup (rec 0) accepted"; exit 1; } || [ $? = 3 ] || { echo "FAIL: corrupt0 rc != 3"; exit 1; }
# fixup tolerance: a broken trailer on a FREE record is NOT corruption the
# pack must prove against — enumerate yields the identical table, and
# strip/rebuild stay bit-exact (the flipped bytes are recipe verbatim)
$NTFS enumerate "$WORK/fs-freefix.ntfs" "$WORK/tableff"
cmp -s "$WORK/table" "$WORK/tableff" || { echo "FAIL: free-record fixup changed the table"; exit 1; }
$NTFS strip "$WORK/fs-freefix.ntfs" "$WORK/recipeff"
$NTFS rebuild "$WORK/recipeff" "$WORK/mbr" "$WORK/rebuiltff.ntfs"
cmp -s "$WORK/fs-freefix.ntfs" "$WORK/rebuiltff.ntfs" || { echo "FAIL: freefix rebuild not bit-exact"; exit 1; }
# tailend: END marker in the record's last 16 bytes must parse fine, keep
# the same member table (tiny.txt stays resident), rebuild bit-exact
$NTFS enumerate "$WORK/fs-tailend.ntfs" "$WORK/tablete" \
    || { echo "FAIL: tailend image refused"; exit 1; }
cmp -s "$WORK/table" "$WORK/tablete" || { echo "FAIL: tailend changed the table"; exit 1; }
$NTFS strip "$WORK/fs-tailend.ntfs" "$WORK/recipete"
$NTFS rebuild "$WORK/recipete" "$WORK/mbr" "$WORK/rebuiltte.ntfs"
cmp -s "$WORK/fs-tailend.ntfs" "$WORK/rebuiltte.ntfs" || { echo "FAIL: tailend rebuild not bit-exact"; exit 1; }
$NTFS enumerate "$WORK/fs-resident.ntfs" "$WORK/table0"
[ ! -s "$WORK/table0" ] || { echo "FAIL: zero-member image table not empty"; exit 1; }
echo "  declines: 4K-sector / truncated / bad-magic / fixup rec5 / fixup rec0: exit 3"
echo "  tolerated: free-record fixup + END-in-tail record (identical tables, bit-exact rebuilds)"

# class-stamp reader + ranged-read harness + cascade delete (built from the
# repo objects, same as test-containerpack.sh)
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
 * usage: rngread <image> <name> <off> <len> <out> [chunk] */
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o"
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
# 0.5 GiB: the DEFER_ENOSPC admission charges sum(usize)/2 + 64 MiB for
# the container sweep; on a 0.2 GiB volume with these imports that would
# (correctly) defer instead of decomposing.
# The main volume carries ONLY fs.ntfs + a zero-free urandom survivor:
# the container's zero-heavy members (pins, sparse holes) dedupe-share
# compressed-zero segments with ANY zero-heavy generic file that survives
# the cascade, and the FS's dedupe/delete block accounting does not
# refcount across that boundary (a pre-existing, pack-independent issue —
# reproducible with builtin generic files alone). The survivor is the
# cascade witness instead.
$B/invf-mkfs "$IMG" 0.5 >/dev/null
$B/invf-cp "$IMG" "$WORK/fs.ntfs" fs.ntfs >/dev/null
$B/invf-cp "$IMG" "$WORK/survivor.bin" survivor.bin >/dev/null
# the decline volume: refused images live (and stay) here — no deletes
$B/invf-mkfs "$IMGD" 0.3 >/dev/null
$B/invf-cp "$IMGD" "$WORK/fs4k.ntfs" fs4k.ntfs >/dev/null
$B/invf-cp "$IMGD" "$WORK/fs-resident.ntfs" fs-resident.ntfs >/dev/null

echo "== sweep #1 (containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "fs.ntfs: ntfs (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: fs.ntfs not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
if grep -q "survivor.bin: ntfs" "$WORK/sweep1.log"; then
    echo "FAIL: survivor mis-sniffed as NTFS"; cat "$WORK/sweep1.log"; exit 1
fi
# members batch in the SAME run their container decomposed (WP14b pattern):
# two text members (big.txt + nested.c), one ELF member
grep -q "fs.ntfs!\*: 2 parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text members not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "fs.ntfs!\*: 1 parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|refused|partition|parts -> " "$WORK/sweep1.log" || true

echo "== sibling set (incl. the WP16b member map) =="
NM=$(grep -c . "$WORK/members")
N=$($B/invf-ls "$IMG" | grep -c "fs\.ntfs!" || true)
echo "fs.ntfs!* names: $N (want $((NM + 2)): $NM members + table + map)"
[ "$N" -eq $((NM + 2)) ] || { echo "FAIL: wrong sibling count"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep -q "fs\.ntfs!mbrt"  || { echo "FAIL: member table sibling missing"; exit 1; }
$B/invf-ls "$IMG" | grep -q "fs\.ntfs!mbrmap" || { echo "FAIL: member map sibling missing"; exit 1; }
# the member table on disk is the enumerate output verbatim
$B/invf-cat "$IMG" "fs.ntfs!mbrt" "$WORK/out/mbrt" >/dev/null
cmp -s "$WORK/table" "$WORK/out/mbrt" || { echo "FAIL: !mbrt != enumerate output"; exit 1; }
echo "members + table + map present; !mbrt is the verbatim table"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" fs.ntfs)
echo "  fs.ntfs: $C"
[ "$C" = "cls=3 algo=20 gen=1" ] || { echo "FAIL: want CONTAINER{NTFS=20,1}"; exit 1; }
TEXTMBR=$($B/invf-ls "$IMG" | grep -oE "fs\.ntfs!mbr[0-9]+-(big|hard)\.txt" | head -1)
[ -n "$TEXTMBR" ] || { echo "FAIL: text member sibling not found"; $B/invf-ls "$IMG"; exit 1; }
C=$("$WORK/classof" "$IMG" "$TEXTMBR")
echo "  $TEXTMBR: $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1}"; exit 1; }
ELFMBR=$($B/invf-ls "$IMG" | grep -oE "fs\.ntfs!mbr[0-9]+-program\.elf" | head -1)
C=$("$WORK/classof" "$IMG" "$ELFMBR")
echo "  $ELFMBR: $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; }
NESTMBR=$($B/invf-ls "$IMG" | grep -oE "fs\.ntfs!mbr[0-9]+-nested\.c" | head -1)
C=$("$WORK/classof" "$IMG" "$NESTMBR")
echo "  $NESTMBR: $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: subdir member want TEXT{PPMD,1}"; exit 1; }

echo "== verify --deep (reads the container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (container + direct member reads) =="
ok=1
for f in fs.ntfs survivor.bin; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
# members are real inodes: read them directly (text = PPMd batch slice,
# elf = ZSTD_BCJ batch slice, sparse = still RAW this sweep)
$B/invf-cat "$IMG" "$TEXTMBR" "$WORK/out/mtext" >/dev/null
cmp -s "$WORK/src/big.txt" "$WORK/out/mtext" || { echo "MISMATCH text member"; ok=0; }
$B/invf-cat "$IMG" "$ELFMBR" "$WORK/out/melf" >/dev/null
cmp -s "$WORK/src/program.elf" "$WORK/out/melf" || { echo "MISMATCH elf member"; ok=0; }
SPMBR=$($B/invf-ls "$IMG" | grep -oE "fs\.ntfs!mbr[0-9]+-sparse\.bin" | head -1)
$B/invf-cat "$IMG" "$SPMBR" "$WORK/out/msparse" >/dev/null
cmp -s "$WORK/src/sparse.bin" "$WORK/out/msparse" || { echo "MISMATCH sparse member"; ok=0; }
FRAGMBR=$($B/invf-ls "$IMG" | grep -oE "fs\.ntfs!mbr[0-9]+-frag\.bin" | head -1)
$B/invf-cat "$IMG" "$FRAGMBR" "$WORK/out/mfrag" >/dev/null
cmp -s "$WORK/src/frag.bin" "$WORK/out/mfrag" || { echo "MISMATCH frag member"; ok=0; }
[ "$ok" = 1 ] || exit 1
echo "container and members bit-exact"

echo "== ranged reads (the WP16b local splice) =="
rng() {  # rng <off> <len> <tag>
    dd if="$WORK/fs.ntfs" of="$WORK/out/ref.$3" bs=1 skip="$1" count="$2" 2>/dev/null
    "$WORK/rngread" "$IMG" fs.ntfs "$1" "$2" "$WORK/out/got.$3" >/dev/null
    cmp -s "$WORK/out/ref.$3" "$WORK/out/got.$3" \
        || { echo "FAIL: range $3 (off=$1 len=$2) mismatch"; exit 1; }
}
rng "$MIDM_OFF" "$MIDM_LEN" midmember      # strictly inside frag.bin run 1
rng "$R2M_OFF"  "$R2M_LEN"  rec2mbr        # recipe -> member boundary
rng "$M2R_OFF"  "$M2R_LEN"  mbr2rec        # member -> recipe boundary
rng "$TAIL_OFF" "$TAIL_LEN" tail           # the container tail (recipe)
rng "$HEAD_OFF" "$HEAD_LEN" head           # the boot sector (recipe)
if [ -n "${M2M_OFF:-}" ]; then
    rng "$M2M_OFF" "$M2M_LEN" mbr2mbr      # member -> member boundary
fi
"$WORK/rngread" "$IMG" fs.ntfs 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
IMGSZ=$(stat -c%s "$WORK/fs.ntfs")
"$WORK/rngread" "$IMG" fs.ntfs $((IMGSZ + 4096)) 100 "$WORK/out/eof.rng" >/dev/null
[ -s "$WORK/out/eof.rng" ] && { echo "FAIL: read past EOF returned bytes"; exit 1; }
echo "mid-member / boundaries / head / tail / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fs.ntfs "$WORK/out/absent" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fs.ntfs \
    "$MIDM_OFF" "$MIDM_LEN" "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fs.ntfs 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent ranged whole read mismatch"; exit 1; }
echo "pack-absent: whole read + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: idle (no re-fire), members stay exact =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "ntfs (codecpack)" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-fired the containerpack"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt after sweep 2"; exit 1; }
$B/invf-cat "$IMG" fs.ntfs "$WORK/out/fs2.ntfs" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/fs2.ntfs" || { echo "FAIL: fs.ntfs drifted"; exit 1; }
grep -q "^OK" <($B/invf-fsck "$IMG") || { echo "FAIL: fsck not OK pre-cascade"; exit 1; }
echo "no re-decomposition; container still bit-exact; fsck clean"

echo "== ratio demo (informational) =="
python3 - "$WORK" <<'PY'
import os, sys
w = sys.argv[1]
orig = os.path.getsize(w + "/fs.ntfs")
rec = os.path.getsize(w + "/recipe")
tab = os.path.getsize(w + "/table")
mapb = os.path.getsize(w + "/map")
musz = 0
for line in open(w + "/table"):
    _i, _n, u = line.rstrip("\n").split("\t")
    musz += int(u)
print("  fs.ntfs: %d B (%d MiB) decomposes into:" % (orig, orig >> 20))
print("    recipe blob  %12d B  (every non-member byte verbatim: boot" % rec)
print("                 %12s    sectors, the whole $MFT incl. fixup" % "")
print("                 %12s    trailers, $Bitmap/$LogFile/$Boot/$Secure/" % "")
print("                 %12s    $Upcase/$Extend, directory indexes," % "")
print("                 %12s    resident file data, member slack tails," % "")
print("                 %12s    and all unallocated clusters)" % "")
print("    %2d members   %12d B  (the file contents, now first-class files" %
      (sum(1 for _ in open(w + "/table")), musz))
print("                 %12s    flowing through PPMd/ZSTD batching +" % "")
print("                 %12s    dedupe)" % "")
print("    table + map  %12d B" % (tab + mapb))
tot = rec + musz + tab + mapb
print("    => the pack stores %d B (%.3fx) before the pipeline; the win is"
      % (tot, tot / orig))
print("       member-level compression/dedupe and seekable pack-free reads,")
print("       not the recipe (a filesystem's free space is metadata here)")
PY

echo "== delete cascade (vol_unlink, the FUSE path) =="
"$WORK/cbrm" "$IMG" fs.ntfs
if $B/invf-ls "$IMG" | grep -q "fs\.ntfs!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "container + all members + table + map deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivor bit-exact =="
$B/invf-cat "$IMG" survivor.bin "$WORK/out/survivor2.bin" >/dev/null
cmp -s "$WORK/survivor.bin" "$WORK/out/survivor2.bin" \
    || { echo "FAIL: survivor.bin drifted after the cascade"; exit 1; }
echo "survivor bit-exact"

echo "== decline volume: refused images stay generic, bit-exact, fsck clean =="
$B/invf-sweep "$IMGD" > "$WORK/sweepd.log" 2>&1 || { cat "$WORK/sweepd.log"; exit 1; }
if grep -q "ntfs (codecpack)" "$WORK/sweepd.log"; then
    echo "FAIL: a declined image was decomposed"; cat "$WORK/sweepd.log"; exit 1
fi
if $B/invf-ls "$IMGD" | grep -q "fs4k\.ntfs!\|fs-resident\.ntfs!"; then
    echo "FAIL: declined images gained siblings"; $B/invf-ls "$IMGD"; exit 1
fi
for f in fs4k.ntfs fs-resident.ntfs; do
    C=$("$WORK/classof" "$IMGD" "$f")
    echo "  $f: $C"
    case "$C" in *algo=20*) echo "FAIL: $f carries a pack stamp"; exit 1;; esac
    $B/invf-cat "$IMGD" "$f" "$WORK/out/decl-$f" >/dev/null
    cmp -s "$WORK/$f" "$WORK/out/decl-$f" || { echo "FAIL: $f not bit-exact"; exit 1; }
done
grep -q "orphans:      0" <($B/invf-fsck "$IMGD") \
    || { echo "FAIL: fsck on the decline volume reports orphans"; exit 1; }
grep -q "^OK" <($B/invf-fsck "$IMGD") || { echo "FAIL: decline volume fsck not OK"; exit 1; }
echo "declined images: no pack stamp, no siblings, bit-exact, fsck clean"

echo "== DEFER_ENOSPC (cls=9): tight volume defers, freed space re-arms =="
python3 - "$WORK" <<'PY'
import random, sys
random.Random(99).randbytes(1)   # seed once
rnd = random.Random(99)
open(sys.argv[1] + "/filler.bin", "wb").write(rnd.randbytes(120 * 1024 * 1024))
PY
# 0.24 GiB: enough payload to import filler (120 MB) + the 64 MB image,
# while free space after the imports (~52 MiB) still falls under the
# DEFER_ENOSPC charge (sum(usize)/2 + 64 MiB ~= 94 MiB for this fixture)
$B/invf-mkfs "$IMGT" 0.24 >/dev/null
$B/invf-cp "$IMGT" "$WORK/filler.bin" filler.bin >/dev/null
$B/invf-cp "$IMGT" "$WORK/fs.ntfs" fs.ntfs >/dev/null
$B/invf-sweep "$IMGT" > "$WORK/sweept1.log" 2>&1 || { cat "$WORK/sweept1.log"; exit 1; }
if grep -q "fs.ntfs: ntfs (codecpack)" "$WORK/sweept1.log"; then
    echo "FAIL: decomposition ran on a volume too tight for it"; exit 1
fi
C=$("$WORK/classof" "$IMGT" fs.ntfs)
echo "  fs.ntfs (tight): $C"
[ "$C" = "cls=9 algo=20 gen=1" ] || { echo "FAIL: want DEFER_ENOSPC{NTFS=20,1}"; exit 1; }
$B/invf-cat "$IMGT" fs.ntfs "$WORK/out/tight1.ntfs" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/tight1.ntfs" \
    || { echo "FAIL: DEFER_ENOSPC file not bit-exact"; exit 1; }
$B/invf-sweep "$IMGT" > "$WORK/sweept2.log" 2>&1 || { cat "$WORK/sweept2.log"; exit 1; }
if grep -q "fs.ntfs: ntfs (codecpack)" "$WORK/sweept2.log"; then
    echo "FAIL: decomposition ran on the still-tight re-sweep"; exit 1
fi
C=$("$WORK/classof" "$IMGT" fs.ntfs)
[ "$C" = "cls=9 algo=20 gen=1" ] || { echo "FAIL: DEFER_ENOSPC re-stamp drifted"; exit 1; }
"$WORK/cbrm" "$IMGT" filler.bin
$B/invf-sweep "$IMGT" > "$WORK/sweept3.log" 2>&1 || { cat "$WORK/sweept3.log"; exit 1; }
grep -q "fs.ntfs: ntfs (codecpack)" "$WORK/sweept3.log" \
    || { echo "FAIL: freed space did not re-arm the decomposition"; cat "$WORK/sweept3.log"; exit 1; }
C=$("$WORK/classof" "$IMGT" fs.ntfs)
echo "  fs.ntfs (after free): $C"
[ "$C" = "cls=3 algo=20 gen=1" ] || { echo "FAIL: want CONTAINER{NTFS=20,1} after free"; exit 1; }
$B/invf-ls "$IMGT" | grep -q "fs\.ntfs!mbrmap" \
    || { echo "FAIL: re-armed decomposition has no member map"; exit 1; }
$B/invf-cat "$IMGT" fs.ntfs "$WORK/out/tight3.ntfs" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/tight3.ntfs" \
    || { echo "FAIL: re-armed container not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMGT" fs.ntfs \
    "$MIDM_OFF" "$MIDM_LEN" "$WORK/out/tight3.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/tight3.rng" \
    || { echo "FAIL: re-armed container pack-absent ranged read mismatch"; exit 1; }
echo "DEFER_ENOSPC: cls=9 wait RAW -> re-sweep after free decomposes"

echo "== admission leg: INVFS_DEC_MEM_LIMIT=64K =="
$B/invf-mkfs "$IMGM" 0.2 >/dev/null
$B/invf-cp "$IMGM" "$WORK/fs.ntfs" fs.ntfs >/dev/null
# the pack's estimate (sum of member usizes + 64 MiB) exceeds 64K ->
# policy refusal before any strip/extract; GENERIC_MEMLIMIT{20,1}
INVFS_DEC_MEM_LIMIT=64K $B/invf-sweep "$IMGM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "fs.ntfs: ntfs (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 64K decode-memory limit"; exit 1
fi
C=$("$WORK/classof" "$IMGM" fs.ntfs)
echo "  fs.ntfs (memlimit): $C"
[ "$C" = "cls=5 algo=20 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{NTFS=20,1}"; exit 1; }
$B/invf-cat "$IMGM" fs.ntfs "$WORK/out/memlim.ntfs" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/memlim.ntfs" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "== admission leg: tiny ARC (INVFS_ARC_BYTES=1M) =="
$B/invf-mkfs "$IMGA" 0.2 >/dev/null
$B/invf-cp "$IMGA" "$WORK/fs.ntfs" fs.ntfs >/dev/null
# the 64 MB container exceeds a 1 MB whole-file ARC budget -> the same
# GENERIC_MEMLIMIT{20,1} refusal, via the ARC admission this time
INVFS_ARC_BYTES=1M $B/invf-sweep "$IMGA" > "$WORK/sweep-arc.log" 2>&1 \
    || { cat "$WORK/sweep-arc.log"; exit 1; }
if grep -q "fs.ntfs: ntfs (codecpack)" "$WORK/sweep-arc.log"; then
    echo "FAIL: decomposition ran under a 1M ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGA" fs.ntfs)
echo "  fs.ntfs (tiny ARC): $C"
[ "$C" = "cls=5 algo=20 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{NTFS=20,1}"; exit 1; }
$B/invf-cat "$IMGA" fs.ntfs "$WORK/out/arc.ntfs" >/dev/null
cmp -s "$WORK/fs.ntfs" "$WORK/out/arc.ntfs" \
    || { echo "FAIL: tiny-ARC read not bit-exact"; exit 1; }
echo "ARC refusal stored generic, bit-exact"

echo "NTFS CONTAINERPACK E2E: PASS"
