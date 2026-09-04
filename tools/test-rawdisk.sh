#!/bin/bash
# test-rawdisk.sh — rawdisk containerpack (WP16a/WP16b) end-to-end
# (persistent regression).
#
#   Pack under test: tools/codecpacks/rawdisk.codecpack/ (algo 16). The
#   helper is C11/libc-only (rawdisk.c); this script compiles it with
#   cc -O2 -Wall -Wextra into a temp bin dir at start and puts that dir on
#   PATH (the manifest resolves the helper BY NAME: <pack>/bin/, then
#   PATH). The pack is SEEKABLE (map command): reads splice locally from
#   the !mbrmap map + recipe + member siblings and never exec the pack.
#
#   fixtures (inline python, stdlib only):
#     disk-mbr.img — ~40 MiB MBR: t83 (10M busybox .c text), t07 (8M ELF
#       chunks+zeros), extended(0x05)+logical (5M zeros), a 4 MiB gap with
#       marker bytes, arbitrary (xorshift) content in every gap.
#     disk-gpt.img — ~48 MiB GPT: 3 named partitions (boot=text 12M,
#       rootfs=ELF 10M, data=zeros 6M), protective MBR, primary+backup
#       tables, PRNG gaps.
#     notes.img    — plain text named .img: the pack must REFUSE it (it
#       falls through to the text pipeline).
#
#   legs: helper self-test (enumerate/extract/strip/rebuild/map by hand +
#     cmp + decline/junk) -> mkfs -> import -> sweep (rawdisk lines for
#     a,b; notes.img refused to text) -> sibling set + classof stamps
#     (CONTAINER{16,1} / TEXT / BATCHED_BIN) -> verify --deep -> sha256
#     bit-exact + direct member reads -> ranged reads (the WP16b local
#     splice) -> pack-ABSENT reads still bit-exact (the map is
#     self-describing) -> idempotent re-sweep -> delete cascade -> fsck ->
#     map-deleted fallback (exec with the pack, LOUD without) -> admission
#     leg (tiny INVFS_ARC_BYTES -> GENERIC_MEMLIMIT{16,1}) -> ratio demo.
#
#   PACK DIR ISOLATION: the sweep/read legs use $WORK/packs (a symlink to
#   rawdisk.codecpack ONLY), not the shared $REPO/tools/codecpacks. The
#   containerpack sweep loop is first-sniff-hit-wins with break-on-decline
#   (volume.c: a declined pack starves the remaining packs for that file),
#   and pack order is readdir() order. fatfs.codecpack declares the same
#   weak sniffs as rawdisk (55AA@510 magic + the .img extension), so with
#   a shared pack dir whoever readdir()s first claims the other's fixtures
#   and the loser starves -- a per-filesystem coin toss. A per-test pack
#   dir is the deterministic, good-neighbor choice (the splt_test nomap
#   leg in test-containerpack.sh materializes its own pack dir the same
#   way). The pack-ABSENT legs use an empty dir ($WORK/nopacks).
#
# Run from the repo root after `make`:  bash tools/test-rawdisk.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
PACK=$REPO/tools/codecpacks/rawdisk.codecpack
WORK=/dev/shm/rawdiskwp
trap 'rm -rf "$WORK" /dev/shm/rawdiskwp*' EXIT
IMG=rawdiskwp.img
IMGMD=rawdiskwp-md.img
IMGMEM=rawdiskwp-mem.img
IMGR1=rawdiskwp-ratio1.img
IMGR2=rawdiskwp-ratio2.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" "$WORK/nopacks" "$WORK/packs"
ln -s "$PACK" "$WORK/packs/rawdisk.codecpack"
export PATH="$WORK/bin:$PATH"                 # the pack helper resolves by name
export INVFS_CODECPACKS=$WORK/packs           # the sweep AND the reads (isolated)
cd /dev/shm
rm -f "$IMG" "$IMGMD" "$IMGMEM" "$IMGR1" "$IMGR2"

echo "== tools + helper build (cc -O2 -Wall -Wextra) =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
cc -std=c11 -O2 -Wall -Wextra -o "$WORK/bin/rawdisk" "$PACK/rawdisk.c"
rc=0; "$WORK/bin/rawdisk" 2>/dev/null || rc=$?
[ "$rc" -eq 2 ] || { echo "FAIL: helper usage rc ($rc, want 2)"; exit 1; }
echo "helper compiled clean: $WORK/bin/rawdisk"

echo "== generate fixtures =="
python3 - "$WORK/orig" <<'PY'
import os
import struct
import sys
import zlib

SEC = 512
REPO = "/home/user/InvariantFS"

def xorshift_stream(seed, n):
    """Deterministic 'arbitrary content' for gaps (incompressible-ish)."""
    out = bytearray()
    x = seed & 0xFFFFFFFFFFFFFFFF
    while len(out) < n:
        x ^= (x << 13) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 7
        x ^= (x << 17) & 0xFFFFFFFFFFFFFFFF
        out += struct.pack("<Q", x)
    return bytes(out[:n])

def gap_fill(n, marker):
    b = bytearray(xorshift_stream(0x5D00D15C ^ n, n))
    m = marker.encode()
    b[:len(m)] = m
    return bytes(b)

def mark(img, off, m):
    """Exact-width marker write (never resizes the bytearray)."""
    img[off:off + len(m)] = m

def text_bytes(n):
    """A real busybox .c file, repeated/truncated to exactly n bytes."""
    src = None
    for root, _dirs, files in os.walk(os.path.join(REPO, "tools/busybox-src")):
        for fn in sorted(files):
            if fn.endswith(".c"):
                p = os.path.join(root, fn)
                if os.path.getsize(p) > 100000:
                    src = p
                    break
        if src:
            break
    assert src, "no big busybox .c found"
    data = open(src, "rb").read()
    reps = (n + len(data) - 1) // len(data)
    return (data * reps)[:n]

def elf_bytes(n):
    """Real x86-64 ELF chunks (busybox-static head) with zeros between."""
    elf = open(os.path.join(REPO, "bin/busybox-static"), "rb").read(1 << 20)
    assert elf[:4] == b"\x7fELF" and elf[18] == 62, "busybox-static not x86-64 ELF"
    b = bytearray(n)
    b[:len(elf)] = elf
    mid = n // 2
    b[mid:mid + len(elf)] = elf
    return bytes(b)

def mbr_entry(etype, start, count):
    return bytes(4) + bytes([etype]) + bytes(3) + struct.pack("<II", start, count)

def build_mbr(d):
    nsec = 81920                                # 40 MiB
    img = bytearray(gap_fill(nsec * SEC, "RAWDISK-FIXTURE-MBR boot-area"))
    m1 = text_bytes(10 << 20)
    m2 = elf_bytes(8 << 20)
    m11 = bytes(5 << 20)
    p1_start, p1_cnt = 2048, 20480              # 10 MiB  -> [2048,22528)
    p2_start, p2_cnt = 22528, 16384             # 8 MiB   -> [22528,38912)
    ext_start, ext_cnt = 47104, 16384           # 8 MiB   -> [47104,63488)
    # the 4 MiB marker gap [38912,47104) keeps its PRNG+marker content
    mark(img, 38912 * SEC, b"GAPMARKER-4MIB between p2 and extended")
    mark(img, 47104 * SEC + SEC, b"GAPMARKER-extended-slack")
    mark(img, 63488 * SEC, b"GAPMARKER-trailing-space")
    mbr = bytearray(SEC)
    mbr[0:16] = b"RAWDISK-FIXTURE!"
    mbr[446:462] = mbr_entry(0x83, p1_start, p1_cnt)
    mbr[462:478] = mbr_entry(0x07, p2_start, p2_cnt)
    mbr[478:494] = mbr_entry(0x05, ext_start, ext_cnt)
    mbr[510:512] = b"\x55\xAA"
    img[0:SEC] = mbr
    ebr = bytearray(SEC)
    ebr[446:462] = mbr_entry(0x83, 2048, 10240)  # rel to EBR -> [49152,59392)
    ebr[510:512] = b"\x55\xAA"
    img[ext_start * SEC:(ext_start + 1) * SEC] = ebr
    img[p1_start * SEC:(p1_start + p1_cnt) * SEC] = m1
    img[p2_start * SEC:(p2_start + p2_cnt) * SEC] = m2
    img[49152 * SEC:(49152 + 10240) * SEC] = m11
    open(os.path.join(d, "disk-mbr.img"), "wb").write(img)
    open(os.path.join(d, "mbr-m1.bin"), "wb").write(m1)
    open(os.path.join(d, "mbr-m2.bin"), "wb").write(m2)
    open(os.path.join(d, "mbr-m11.bin"), "wb").write(m11)
    return {"size": nsec * SEC,
            "p1_off": p1_start * SEC, "p1_len": p1_cnt * SEC,
            "p2_off": p2_start * SEC, "p2_len": p2_cnt * SEC,
            "p11_off": 49152 * SEC, "p11_len": 10240 * SEC}

def build_gpt(d):
    nsec = 98304                                # 48 MiB
    img = bytearray(gap_fill(nsec * SEC, "RAWDISK-FIXTURE-GPT boot-area"))
    m1 = text_bytes(12 << 20)
    m2 = elf_bytes(10 << 20)
    m3 = bytes(6 << 20)
    parts = [("boot", 2048, 24576, m1),         # -> [2048,26624)
             ("rootfs", 28672, 20480, m2),      # -> [28672,49152)
             ("data", 53248, 12288, m3)]        # -> [53248,65536)
    mark(img, 26624 * SEC, b"GAPMARKER-gpt-gap-1")
    mark(img, 49152 * SEC, b"GAPMARKER-gpt-gap-2")
    mark(img, 65536 * SEC, b"GAPMARKER-gpt-trailing")

    def guid_bytes(s):
        import uuid
        return uuid.UUID(s).bytes_le          # GPT wire format

    LINUX_FS = "0fc63daf-8483-4772-8e79-3d69d8477de4"
    entries = bytearray(128 * 128)
    for i, (nm, start, cnt, payload) in enumerate(parts):
        e = entries[i * 128:(i + 1) * 128]
        e[0:16] = guid_bytes(LINUX_FS)
        e[16:32] = guid_bytes("11111111-2222-3333-4444-%012d" % (i + 1))
        struct.pack_into("<QQ", e, 32, start, start + cnt - 1)
        nm16 = nm.encode("utf-16-le")
        e[56:56 + len(nm16)] = nm16
        entries[i * 128:(i + 1) * 128] = e
        img[start * SEC:(start + cnt) * SEC] = payload

    first_usable, last_usable = 2048, nsec - 34
    ent_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF

    def header(current, backup, entries_lba):
        h = bytearray(SEC)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)
        struct.pack_into("<I", h, 12, 92)
        struct.pack_into("<Q", h, 24, current)
        struct.pack_into("<Q", h, 32, backup)
        struct.pack_into("<Q", h, 40, first_usable)
        struct.pack_into("<Q", h, 48, last_usable)
        h[56:72] = guid_bytes("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
        struct.pack_into("<Q", h, 72, entries_lba)
        struct.pack_into("<I", h, 80, 128)
        struct.pack_into("<I", h, 84, 128)
        struct.pack_into("<I", h, 88, ent_crc)
        crc = zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF
        struct.pack_into("<I", h, 16, crc)
        return h

    pmbr = bytearray(SEC)
    pmbr[446:462] = mbr_entry(0xEE, 1, nsec - 1)
    pmbr[510:512] = b"\x55\xAA"
    img[0:SEC] = pmbr
    img[SEC:2 * SEC] = header(1, nsec - 1, 2)
    img[2 * SEC:34 * SEC] = entries
    img[(nsec - 33) * SEC:(nsec - 1) * SEC] = entries
    img[(nsec - 1) * SEC:nsec * SEC] = header(nsec - 1, 1, nsec - 33)
    open(os.path.join(d, "disk-gpt.img"), "wb").write(img)
    open(os.path.join(d, "gpt-m1.bin"), "wb").write(m1)
    open(os.path.join(d, "gpt-m2.bin"), "wb").write(m2)
    open(os.path.join(d, "gpt-m3.bin"), "wb").write(m3)
    return {"size": nsec * SEC,
            "p1_off": 2048 * SEC, "p1_len": 24576 * SEC,
            "p2_off": 28672 * SEC, "p2_len": 20480 * SEC,
            "p3_off": 53248 * SEC, "p3_len": 12288 * SEC}

d = sys.argv[1]
la = build_mbr(d)
lb = build_gpt(d)
notes = (b"These are plain notes about the disk images, not a disk.\n" * 200)
open(os.path.join(d, "notes.img"), "wb").write(notes)
with open(os.path.join(d, "layout.txt"), "w") as f:
    for k, v in [("a_size", la["size"]),
                 ("a_p1_off", la["p1_off"]), ("a_p1_len", la["p1_len"]),
                 ("a_p2_off", la["p2_off"]), ("a_p2_len", la["p2_len"]),
                 ("a_p11_off", la["p11_off"]), ("a_p11_len", la["p11_len"]),
                 ("b_size", lb["size"]),
                 ("b_p1_off", lb["p1_off"]), ("b_p1_len", lb["p1_len"]),
                 ("b_p2_off", lb["p2_off"]), ("b_p2_len", lb["p2_len"]),
                 ("b_p3_off", lb["p3_off"]), ("b_p3_len", lb["p3_len"])]:
        f.write("%s=%d\n" % (k, v))
print("  disk-mbr.img %d (members: 10M text, 8M elf+zeros, 5M zeros)" % la["size"])
print("  disk-gpt.img %d (members: 12M text, 10M elf+zeros, 6M zeros)" % lb["size"])
print("  notes.img    %d (plain text, must be refused)" % len(notes))
PY
. "$WORK/orig/layout.txt"   # a_size a_p1_off ... b_size b_p1_off ...

RD="$WORK/bin/rawdisk"
echo "== SELF-TEST: helper by hand on both fixtures =="
printf '1\tt83\t10485760\n2\tt07\t8388608\n11\tt83\t5242880\n' > "$WORK/want-enum.a"
printf '1\tboot\t12582912\n2\trootfs\t10485760\n3\tdata\t6291456\n' > "$WORK/want-enum.b"
"$RD" enumerate "$WORK/orig/disk-mbr.img" "$WORK/out/enum.a"
diff "$WORK/want-enum.a" "$WORK/out/enum.a" || { echo "FAIL: enum a"; exit 1; }
"$RD" enumerate "$WORK/orig/disk-gpt.img" "$WORK/out/enum.b"
diff "$WORK/want-enum.b" "$WORK/out/enum.b" || { echo "FAIL: enum b"; exit 1; }
echo "enumerate: idx/sname/usize exact (MBR 1,2,11; GPT 1,2,3 named)"
"$RD" estimate "$WORK/orig/disk-mbr.img" > "$WORK/out/est.a"
[ "$(cat "$WORK/out/est.a")" = "91226112" ] || { echo "FAIL: estimate a"; exit 1; }
"$RD" estimate "$WORK/orig/disk-gpt.img" > "$WORK/out/est.b"
[ "$(cat "$WORK/out/est.b")" = "96468992" ] || { echo "FAIL: estimate b"; exit 1; }
echo "estimate: sum(usize)+64MiB exact (91226112 / 96468992)"
mkdir -p "$WORK/mbr.a" "$WORK/mbr.b"
"$RD" extract "$WORK/orig/disk-mbr.img" 1  "$WORK/mbr.a/1"
"$RD" extract "$WORK/orig/disk-mbr.img" 2  "$WORK/mbr.a/2"
"$RD" extract "$WORK/orig/disk-mbr.img" 11 "$WORK/mbr.a/11"
"$RD" extract "$WORK/orig/disk-gpt.img" 1  "$WORK/mbr.b/1"
"$RD" extract "$WORK/orig/disk-gpt.img" 2  "$WORK/mbr.b/2"
"$RD" extract "$WORK/orig/disk-gpt.img" 3  "$WORK/mbr.b/3"
cmp "$WORK/orig/mbr-m1.bin"  "$WORK/mbr.a/1"
cmp "$WORK/orig/mbr-m2.bin"  "$WORK/mbr.a/2"
cmp "$WORK/orig/mbr-m11.bin" "$WORK/mbr.a/11"
cmp "$WORK/orig/gpt-m1.bin"  "$WORK/mbr.b/1"
cmp "$WORK/orig/gpt-m2.bin"  "$WORK/mbr.b/2"
cmp "$WORK/orig/gpt-m3.bin"  "$WORK/mbr.b/3"
echo "extract: all 6 members bit-exact"
"$RD" strip "$WORK/orig/disk-mbr.img" "$WORK/out/recipe.a"
"$RD" strip "$WORK/orig/disk-gpt.img" "$WORK/out/recipe.b"
"$RD" rebuild "$WORK/out/recipe.a" "$WORK/mbr.a" "$WORK/out/rebuilt.a"
"$RD" rebuild "$WORK/out/recipe.b" "$WORK/mbr.b" "$WORK/out/rebuilt.b"
cmp "$WORK/orig/disk-mbr.img" "$WORK/out/rebuilt.a"
cmp "$WORK/orig/disk-gpt.img" "$WORK/out/rebuilt.b"
echo "strip+rebuild: both images bit-exact (recipes $(stat -c%s "$WORK/out/recipe.a") / $(stat -c%s "$WORK/out/recipe.b") bytes)"
"$RD" map "$WORK/orig/disk-mbr.img" "$WORK/out/map.a"
"$RD" map "$WORK/orig/disk-gpt.img" "$WORK/out/map.b"
python3 - "$WORK" <<'PY'
import struct
import sys

def parse_recipe(path):
    b = open(path, "rb").read()
    assert b[:4] == b"RDR1", "bad recipe magic"
    size = struct.unpack_from("<Q", b, 4)[0]
    nmem = struct.unpack_from("<I", b, 12)[0]
    pos = 16
    mem = []
    for _ in range(nmem):
        mem.append(struct.unpack_from("<IQQ", b, pos))
        pos += 20
    nrng = struct.unpack_from("<I", b, pos)[0]
    pos += 4
    ranges = []
    for _ in range(nrng):
        off, ln = struct.unpack_from("<QQ", b, pos)
        ranges.append((off, ln, pos + 16))
        pos += 16 + ln
    assert pos == len(b), "recipe trailing garbage"
    return size, mem, ranges, b

def check(img, recipe, map_path):
    orig = open(img, "rb").read()
    size, mem, ranges, rblob = parse_recipe(recipe)
    assert size == len(orig)
    mb = open(map_path, "rb").read()
    assert mb[:4] == b"MRMP"
    count = struct.unpack_from("<I", mb, 4)[0]
    assert len(mb) == 8 + count * 29
    pos = 0
    n_rec = n_mem = 0
    for i in range(count):
        off, ln, kind, idx, src = struct.unpack_from("<QQBIQ", mb, 8 + i * 29)
        assert off == pos and ln > 0, "map does not partition"
        if kind == 0:
            assert rblob[src:src + ln] == orig[off:off + ln], "RECIPE bytes"
            n_rec += 1
        else:
            m = [m for m in mem if m[0] == idx]
            assert m and src + ln <= m[0][2], "MEMBER bounds"
            n_mem += 1
        pos += ln
    assert pos == len(orig)
    print("  %s: %d entries (%d RECIPE + %d MEMBER), partition + recipe bytes OK"
          % (img.split("/")[-1], count, n_rec, n_mem))

w = sys.argv[1]
check(w + "/orig/disk-mbr.img", w + "/out/recipe.a", w + "/out/map.a")
check(w + "/orig/disk-gpt.img", w + "/out/recipe.b", w + "/out/map.b")
PY
echo "-- decline + junk robustness --"
rc=0; "$RD" enumerate "$WORK/orig/notes.img" /dev/null || rc=$?
[ "$rc" -eq 3 ] || { echo "FAIL: notes.img must decline 3 (got $rc)"; exit 1; }
: > "$WORK/out/empty.img"
rc=0; "$RD" enumerate "$WORK/out/empty.img" /dev/null || rc=$?
[ "$rc" -eq 3 ] || { echo "FAIL: empty must decline 3 (got $rc)"; exit 1; }
rc=0; "$RD" map "$WORK/orig/notes.img" /dev/null || rc=$?
[ "$rc" -eq 3 ] || { echo "FAIL: notes.img map must decline 3 (got $rc)"; exit 1; }
printf 'GARBAGE' > "$WORK/out/badrecipe"
rc=0; "$RD" rebuild "$WORK/out/badrecipe" "$WORK/mbr.a" "$WORK/out/junk" || rc=$?
[ "$rc" -eq 1 ] || { echo "FAIL: junk recipe must error 1 (got $rc)"; exit 1; }
mkdir -p "$WORK/mbrshort" && cp "$WORK/mbr.a/1" "$WORK/mbr.a/2" "$WORK/mbrshort/" \
    && truncate -s -1 "$WORK/mbrshort/1" && cp "$WORK/mbr.a/11" "$WORK/mbrshort/"
rc=0; "$RD" rebuild "$WORK/out/recipe.a" "$WORK/mbrshort" "$WORK/out/junk2" || rc=$?
[ "$rc" -eq 1 ] || { echo "FAIL: member size mismatch must error 1 (got $rc)"; exit 1; }
rc=0; "$RD" extract "$WORK/orig/disk-mbr.img" 7 /dev/null || rc=$?
[ "$rc" -eq 1 ] || { echo "FAIL: unannounced idx must error 1 (got $rc)"; exit 1; }
echo "SELF-TEST: PASS (decline 3s, junk rebuild 1s, unannounced idx 1)"

# class-stamp reader + ranged-read harness + unlink helper (the
# test-containerpack.sh pattern, built from the repo objects)
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
 *   len < 0 reads the whole file in <chunk>-byte windows (default 65536).
 * Anything beyond EOF comes back short, exactly like a mounted read. */
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.3 >/dev/null
for f in disk-mbr.img disk-gpt.img notes.img; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 (rawdisk containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "disk-mbr.img: rawdisk (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: disk-mbr.img not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "disk-gpt.img: rawdisk (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: disk-gpt.img not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "disk-mbr.img!\*: 1 parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: MBR text member not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "disk-mbr.img!\*: 1 parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: MBR ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "disk-gpt.img!\*: 1 parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: GPT text member not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "disk-gpt.img!\*: 1 parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: GPT ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
if grep -q "notes.img: rawdisk" "$WORK/sweep1.log"; then
    echo "FAIL: notes.img was decomposed by rawdisk"; cat "$WORK/sweep1.log"; exit 1
fi
grep -q "notes.img: text -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: notes.img did not fall through to text"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "rawdisk|parts -> |notes" "$WORK/sweep1.log"

echo "== sibling set (members + member table + member map) =="
N=$($B/invf-ls "$IMG" | grep -c "disk-mbr\.img!" || true)
echo "disk-mbr.img!* names: $N"
[ "$N" -eq 5 ] || { echo "FAIL: want 5 (3 members + table + map)"; $B/invf-ls "$IMG"; exit 1; }
N=$($B/invf-ls "$IMG" | grep -c "disk-gpt\.img!" || true)
echo "disk-gpt.img!* names: $N"
[ "$N" -eq 5 ] || { echo "FAIL: want 5 (3 members + table + map)"; $B/invf-ls "$IMG"; exit 1; }
for s in "disk-mbr.img!mbr0001-t83" "disk-mbr.img!mbr0002-t07" \
         "disk-mbr.img!mbr0011-t83" "disk-mbr.img!mbrt" "disk-mbr.img!mbrmap" \
         "disk-gpt.img!mbr0001-boot" "disk-gpt.img!mbr0002-rootfs" \
         "disk-gpt.img!mbr0003-data" "disk-gpt.img!mbrt" "disk-gpt.img!mbrmap"; do
    $B/invf-ls "$IMG" | grep -q "$s" || { echo "FAIL: sibling $s missing"; $B/invf-ls "$IMG"; exit 1; }
done
if $B/invf-ls "$IMG" | grep -q "notes\.img!"; then
    echo "FAIL: refused notes.img gained siblings"; exit 1
fi
echo "3 members + table + map per image; notes.img clean"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" disk-mbr.img)
echo "  disk-mbr.img: $C"
[ "$C" = "cls=3 algo=16 gen=1" ] || { echo "FAIL: want CONTAINER{RAWDISK=16,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" disk-gpt.img)
echo "  disk-gpt.img: $C"
[ "$C" = "cls=3 algo=16 gen=1" ] || { echo "FAIL: want CONTAINER{RAWDISK=16,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "disk-mbr.img!mbr0001-t83")
echo "  mbr member1 (text): $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "disk-mbr.img!mbr0002-t07")
echo "  mbr member2 (elf): $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "disk-gpt.img!mbr0001-boot")
echo "  gpt member1 (text): $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "disk-gpt.img!mbr0002-rootfs")
echo "  gpt member2 (elf): $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "disk-mbr.img!mbr0011-t83")
echo "  mbr member11 (zeros): $C"
[ "$C" = "none" ] || { echo "FAIL: zeros member should stay unclassified"; exit 1; }
C=$("$WORK/classof" "$IMG" notes.img)
echo "  notes.img (refused): $C"
case "$C" in *algo=16*|*cls=3*) echo "FAIL: notes.img carries a pack stamp"; exit 1;; esac
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: notes.img should be TEXT{PPMD,1}"; exit 1; }

echo "== verify --deep (reads every container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in disk-mbr.img disk-gpt.img notes.img; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
# members are real inodes: read them directly (t83/boot = PPMd batch slice,
# t07/rootfs = ZSTD_BCJ batch slice, t83-11/data = RAW zeros)
for m in "disk-mbr.img!mbr0001-t83:mbr-m1.bin" \
         "disk-mbr.img!mbr0002-t07:mbr-m2.bin" \
         "disk-mbr.img!mbr0011-t83:mbr-m11.bin" \
         "disk-gpt.img!mbr0001-boot:gpt-m1.bin" \
         "disk-gpt.img!mbr0002-rootfs:gpt-m2.bin" \
         "disk-gpt.img!mbr0003-data:gpt-m3.bin"; do
    name=${m%%:*}; ref=${m##*:}
    $B/invf-cat "$IMG" "$name" "$WORK/out/mread" >/dev/null
    cmp -s "$WORK/orig/$ref" "$WORK/out/mread" || { echo "MISMATCH member $name"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "containers and members bit-exact"

echo "== ranged reads (the WP16b local splice) =="
rng() {  # rng <imgfile> <orig> <name> <off> <len> <tag>
    dd if="$WORK/orig/$2" of="$WORK/out/ref.$6" bs=1 skip="$4" count="$5" 2>/dev/null
    "$WORK/rngread" "$1" "$3" "$4" "$5" "$WORK/out/got.$6" >/dev/null
    cmp -s "$WORK/out/ref.$6" "$WORK/out/got.$6" \
        || { echo "FAIL: range $6 (off=$4 len=$5) mismatch"; exit 1; }
}
rng "$IMG" disk-gpt.img disk-gpt.img $((b_p2_off + 4096)) 65536 midmember
rng "$IMG" disk-gpt.img disk-gpt.img $((b_p2_off - 100)) 200 gap2mbr
rng "$IMG" disk-gpt.img disk-gpt.img $((b_p1_off + b_p1_len - 64)) 128 mbr2gap
rng "$IMG" disk-gpt.img disk-gpt.img 0 1024 head
rng "$IMG" disk-gpt.img disk-gpt.img $((b_size - 2048)) 2048 tail
rng "$IMG" disk-mbr.img disk-mbr.img $((a_p11_off - 600)) 1200 ebr2logical
rng "$IMG" disk-mbr.img disk-mbr.img $((38912 * 512 + 2097152)) 4096 markergap
"$WORK/rngread" "$IMG" disk-gpt.img 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/disk-gpt.img" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole GPT via 64K ranged windows mismatch"; exit 1; }
"$WORK/rngread" "$IMG" disk-mbr.img 0 -1 "$WORK/out/whole2.rng" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/whole2.rng" \
    || { echo "FAIL: whole MBR via 64K ranged windows mismatch"; exit 1; }
"$WORK/rngread" "$IMG" disk-gpt.img $((b_size + 4096)) 100 "$WORK/out/eof.rng" >/dev/null
[ -s "$WORK/out/eof.rng" ] && { echo "FAIL: read past EOF returned bytes"; exit 1; }
echo "mid-member / gap crossings / head / tail / marker gap / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
# a fresh process with an empty pack dir: algo 16 has no registry entry,
# and STILL every byte comes back, without any pack exec -- the point of
# a seekable container
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" disk-mbr.img "$WORK/out/absent.a" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/absent.a" \
    || { echo "FAIL: pack-absent MBR read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" disk-gpt.img "$WORK/out/absent.b" >/dev/null
cmp -s "$WORK/orig/disk-gpt.img" "$WORK/out/absent.b" \
    || { echo "FAIL: pack-absent GPT read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" disk-gpt.img \
    $((b_p2_off + 4096)) 65536 "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" disk-mbr.img 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent ranged whole read mismatch"; exit 1; }
echo "pack-absent: whole reads + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: idempotent (no re-decomposition) =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "rawdisk (codecpack)\|guard refused\|does not partition" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-fired rawdisk"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
$B/invf-cat "$IMG" disk-gpt.img "$WORK/out/resweep.b" >/dev/null
cmp -s "$WORK/orig/disk-gpt.img" "$WORK/out/resweep.b" \
    || { echo "FAIL: disk-gpt.img drifted on the idle sweep"; exit 1; }
$B/invf-cat "$IMG" disk-mbr.img "$WORK/out/resweep.a" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/resweep.a" \
    || { echo "FAIL: disk-mbr.img drifted on the idle sweep"; exit 1; }
echo "no re-decomposition; reads still exact"

echo "== delete cascade (vol_unlink, the FUSE path) =="
"$WORK/cbrm" "$IMG" disk-mbr.img disk-gpt.img
if $B/invf-ls "$IMG" | grep -q "disk-.*\.img!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivor bit-exact =="
$B/invf-cat "$IMG" notes.img "$WORK/out/notes2.img" >/dev/null
cmp -s "$WORK/orig/notes.img" "$WORK/out/notes2.img" \
    || { echo "FAIL: notes.img drifted after deletes"; exit 1; }

echo "== map-deleted fallback (exec when the map is gone) =="
$B/invf-mkfs "$IMGMD" 0.2 >/dev/null
$B/invf-cp "$IMGMD" "$WORK/orig/disk-mbr.img" disk-mbr.img >/dev/null
$B/invf-sweep "$IMGMD" > "$WORK/sweepmd.log" 2>&1 || { cat "$WORK/sweepmd.log"; exit 1; }
grep -q "disk-mbr.img: rawdisk (codecpack)" "$WORK/sweepmd.log" \
    || { echo "FAIL: map-deleted leg: not decomposed"; cat "$WORK/sweepmd.log"; exit 1; }
"$WORK/cbrm" "$IMGMD" "disk-mbr.img!mbrmap"
$B/invf-ls "$IMGMD" | grep -q "disk-mbr\.img!mbrmap" \
    && { echo "FAIL: !mbrmap sibling not deleted"; exit 1; }
# pack present + no map: the WP16a whole-file rebuild exec answers
$B/invf-cat "$IMGMD" disk-mbr.img "$WORK/out/md.a" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/md.a" \
    || { echo "FAIL: map-deleted exec fallback not bit-exact"; exit 1; }
# a ranged read falls back the same way (through the whole-file ARC divert)
"$WORK/rngread" "$IMGMD" disk-mbr.img $((a_p2_off + 4096)) 65536 "$WORK/out/md.rng" >/dev/null
dd if="$WORK/orig/disk-mbr.img" of="$WORK/out/md.ref" bs=1 skip=$((a_p2_off + 4096)) count=65536 2>/dev/null
cmp -s "$WORK/out/md.ref" "$WORK/out/md.rng" \
    || { echo "FAIL: map-deleted ranged read mismatch"; exit 1; }
# pack absent AND no map: LOUD failure (the WP16a semantics, unchanged)
if INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMGMD" disk-mbr.img \
        "$WORK/out/md.absent" 2> "$WORK/mdabsent.err"; then
    echo "FAIL: pack-absent map-less read SUCCEEDED (silent 1:1 break)"; exit 1
fi
grep -q "codecpack" "$WORK/mdabsent.err" \
    || { echo "FAIL: no loud pack-absent diagnostic"; cat "$WORK/mdabsent.err"; exit 1; }
echo "map deleted: exec fallback exact (pack present), LOUD (pack absent)"

echo "== admission leg: INVFS_ARC_BYTES=1M =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/disk-mbr.img" disk-mbr.img >/dev/null
# the container (40 MiB) exceeds the 1 MiB ARC budget -> policy refusal
# before any extract; GENERIC_MEMLIMIT{16,1}, stored generic, bit-exact
INVFS_ARC_BYTES=1M $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "rawdisk (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 1M ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" disk-mbr.img)
echo "  disk-mbr.img (memlimit): $C"
[ "$C" = "cls=5 algo=16 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{RAWDISK=16,1}"; exit 1; }
$B/invf-cat "$IMGMEM" disk-mbr.img "$WORK/out/mem.a" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/mem.a" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "== ratio demo: fixture stored vs raw =="
# R1: the pack decomposes; R2: no packs (generic ZSTD-19 floor). Same
# fixture, same 0.2 GiB images; "used" deltas exclude the (equal) meta.
used_mib() { $B/invf-stats "$1" | awk '/^  used/ {print $3}'; }
$B/invf-mkfs "$IMGR1" 0.2 >/dev/null
U0=$(used_mib "$IMGR1")
$B/invf-cp "$IMGR1" "$WORK/orig/disk-mbr.img" disk-mbr.img >/dev/null
URAW=$(used_mib "$IMGR1")
$B/invf-sweep "$IMGR1" >/dev/null 2>&1
$B/invf-sweep "$IMGR1" >/dev/null 2>&1   # settle: zeros member goes generic
UDEC=$(used_mib "$IMGR1")
$B/invf-mkfs "$IMGR2" 0.2 >/dev/null
$B/invf-cp "$IMGR2" "$WORK/orig/disk-mbr.img" disk-mbr.img >/dev/null
INVFS_CODECPACKS=$WORK/nopacks $B/invf-sweep "$IMGR2" >/dev/null 2>&1
UGEN=$(used_mib "$IMGR2")
awk -v raw="$a_size" -v u0="$U0" -v uraw="$URAW" -v udec="$UDEC" -v ugen="$UGEN" 'BEGIN {
    gib = 1048576.0;
    printf "  fixture disk-mbr.img      : %.1f MiB (raw file)\n", raw / gib;
    printf "  stored RAW (import)       : +%.1f MiB used (%.2fx)\n", (uraw - u0), raw / ((uraw - u0) * gib);
    printf "  stored DECOMPOSED (pack)  : +%.1f MiB used (%.2fx)\n", (udec - u0), raw / ((udec - u0) * gib);
    printf "  stored GENERIC (no packs) : +%.1f MiB used (%.2fx)\n", (ugen - u0), raw / ((ugen - u0) * gib);
}'
# sanity: decomposed must beat RAW storage, and reads must be exact
awk -v raw="$a_size" -v u0="$U0" -v udec="$UDEC" 'BEGIN {
    if ((udec - u0) * 1048576 >= raw) { print "FAIL: decomposition did not shrink the fixture"; exit 1 }
}' || exit 1
$B/invf-cat "$IMGR1" disk-mbr.img "$WORK/out/ratio.a" >/dev/null
cmp -s "$WORK/orig/disk-mbr.img" "$WORK/out/ratio.a" \
    || { echo "FAIL: ratio-demo image not bit-exact"; exit 1; }
echo "decomposition shrinks the fixture AND keeps it bit-exact"

echo "RAWDISK E2E: PASS"
