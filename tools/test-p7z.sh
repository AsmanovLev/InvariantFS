#!/bin/bash
# test-p7z.sh — the p7z containerpack (tools/codecpacks/p7z.codecpack,
# algo 23) end-to-end (persistent regression). WP16a containerpack ABI +
# WP16b seekable (map) flow over 7z archives whose members are all STORED
# (Copy method, `7zz a -t7z -m0=Copy`); compressed/encrypted archives are
# declined by design (bit-exactness is impossible without re-encoding).
#
#   fixtures (bin/7zz builds the real ones; inline python3 for the
#   hand-crafted solid Copy archive — 7zz never groups Copy members into a
#   multi-substream folder, so that parse path needs a hand-built file,
#   cross-validated by `7zz t`):
#     stored.7z  — Copy members: alpha.c (real busybox text), beta.bin
#       (real x86-64 ELF), empty.dat (0 bytes), deep/nested/leaf.txt
#       (deep path), rand.bin (9 MiB + 12345 seeded random — crosses the
#       pack's 8 MiB streaming windows). Default 7zz header: LZMA-encoded
#       (kEncodedHeader), decoded through $P7Z_7ZZ by the helper.
#     hsolid.7z  — hand-crafted SOLID Copy: one folder, two substreams,
#       plain (uncompressed) header. Also works with NO 7zz at all.
#     lzma2.7z   — the 7zz default (LZMA2): the pack must DECLINE.
#     enc.7z     — Copy + 7zAES (-p -mhe=on): DECLINE.
#     garbage.7z — 7z magic + seeded random junk (bad header CRC): DECLINE.
#     nomagic.7z — random junk without the magic (.7z ext still sniffs):
#       DECLINE.
#     trunc.7z   — stored.7z cut at 2 MiB (header lost): DECLINE.
#     nohdr.7z   — `-mhc=off` Copy (pack-level only: works with 7zz absent).
#     dents.7z   — directory entries (0-length dir members), pack level.
#     tjunk.7z   — stored.7z + 777 bytes of trailing junk, pack level.
#   pack-level round-trip (enumerate/extract/strip/rebuild/map + memcmp)
#   runs on every accept fixture BEFORE the FS legs (the pack is its own
#   first arbiter); declines are exit-3-checked at pack level too.
#   mkfs -> cp -> INVFS_CODECPACKS=$REPO/tools/codecpacks invf-sweep
#   (expect "p7z (codecpack)" for stored/hsolid, silence for the declines)
#   -> CONTAINER{23,1} stamps -> members batch (text -> cls=7, ELF ->
#   cls=8) -> verify --deep -> sha256 bit-exact (containers AND direct
#   member reads) -> member table verbatim -> RANGED reads through the
#   MRMP splice -> pack-ABSENT reads still bit-exact -> sweep 2 stores
#   the members (no re-decomposition) -> sweep 3 idle -> delete cascade
#   kills every "!"-sibling -> fsck clean -> survivors bit-exact.
#   Admission legs: INVFS_ARC_BYTES=1M -> GENERIC_MEMLIMIT{23,1} (the
#   whole-file ARC budget fires), and INVFS_DEC_MEM_LIMIT=64K -> same
#   stamp through the pack's estimate command. Both bit-exact.
#
# 7zz RESOLUTION: the pack needs a real 7zz to decode the DEFAULT
# (LZMA-compressed) 7z header; it resolves $P7Z_7ZZ -> a sibling of the
# helper named "7zz" -> PATH. This test exports P7Z_7ZZ=$B/7zz. Without
# it, encoded-header archives decline cleanly (the -mhc=off fixture still
# decomposes) — covered by the pack-level degradation leg.
#
# Run from the repo root after `make`:  bash tools/test-p7z.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
PACK=$REPO/tools/codecpacks/p7z.codecpack
WORK=/dev/shm/wp16p7z
IMG=wp16p7z.img
IMGMEM=wp16p7z-mem.img
IMGWS=wp16p7z-ws.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
export P7Z_7ZZ=$B/7zz                            # the encoded-header decode
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/nopacks" "$WORK/fsrc"
cd /dev/shm
rm -f "$IMG" "$IMGMEM" "$IMGWS"

echo "== tools =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
[ -x "$B/7zz" ] || { echo "FAIL: $B/7zz not executable"; exit 1; }
echo "  cc, python3, bin/7zz present"

echo "== build the pack (the -Wall -Wextra -Werror gate) =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/p7z" "$PACK/p7z.c" \
    || { echo "FAIL: pack build failed"; exit 1; }
echo "  bin/p7z built, -Wall -Wextra -Werror clean"

echo "== generate fixtures =="
python3 - "$WORK" <<'PY'
import os
import random
import struct
import sys
import zlib

w = sys.argv[1]
fsrc = os.path.join(w, "fsrc")
orig = os.path.join(w, "orig")
rnd = random.Random(42)

# --- member content -------------------------------------------------------
texts = []
for root, _dirs, files in os.walk("/home/user/InvariantFS/tools/busybox-src"):
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                texts.append(open(p, "rb").read())
        if len(texts) >= 2:
            break
    if len(texts) >= 2:
        break
assert len(texts) == 2, "need two busybox .c fixtures"
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

open(os.path.join(fsrc, "alpha.c"), "wb").write(texts[0])
open(os.path.join(fsrc, "beta.bin"), "wb").write(elf)
open(os.path.join(fsrc, "empty.dat"), "wb").write(b"")
os.makedirs(os.path.join(fsrc, "deep/nested"))
open(os.path.join(fsrc, "deep/nested/leaf.txt"), "wb").write(texts[1])
# 9 MiB + 12345 of seeded random: crosses the pack's 8 MiB streaming
# windows in extract/strip/rebuild, and stays unclassified in the FS.
open(os.path.join(fsrc, "rand.bin"), "wb").write(
    rnd.randbytes(9 * 1024 * 1024 + 12345))

# sname -> member source path, for the direct-read legs (sname = basename)
with open(os.path.join(orig, "members.list"), "w") as f:
    f.write("alpha.c\talpha.c\n")
    f.write("beta.bin\tbeta.bin\n")
    f.write("empty.dat\tempty.dat\n")
    f.write("leaf.txt\tdeep/nested/leaf.txt\n")
    f.write("rand.bin\trand.bin\n")

# --- hsolid.7z: hand-crafted SOLID Copy (1 folder, 2 substreams), plain
#     header. 7zz never groups Copy members solidly, so the multi-substream
#     parse path needs this; `7zz t` cross-validates below. ---------------
def vu(v):
    # 7z UINT64: k leading 1-bits in the first byte; the remaining (7-k)
    # bits are the HIGH part; the next k bytes are the LOW part (LE).
    for k in range(0, 8):
        if v < (1 << (7 + 7 * k)):
            if k == 0:
                return bytes([v])
            lo = v & ((1 << (8 * k)) - 1)
            hi = (v >> (8 * k)) & ((1 << (7 - k)) - 1)
            return bytes([((0xFF << (8 - k)) & 0xFF) | hi]) + \
                lo.to_bytes(k, "little")
    return b"\xff" + v.to_bytes(8, "little")

payload_a = b"alpha solid member\n" * 1000          # text-ish, 19000 B
payload_b = bytes(range(256)) * 40                  # 10240 B, binary-ish
data = payload_a + payload_b
names = "".join(n + "\0" for n in ("sa.txt", "sb.bin")).encode("utf-16-le")

def digests(vals):
    return b"\x01" + b"".join(struct.pack("<I", v) for v in vals)

folder = vu(1) + bytes([0x01]) + bytes([0x00])      # 1 coder, id {00} Copy
unpack = (vu(0x0B) + vu(1) + b"\x00" + folder +
          vu(0x0C) + vu(len(data)) +
          vu(0x0A) + digests([zlib.crc32(data)]) + vu(0x00))
sub = (vu(0x0D) + vu(2) +
       vu(0x09) + vu(len(payload_a)) +
       vu(0x0A) + digests([zlib.crc32(payload_a), zlib.crc32(payload_b)]) +
       vu(0x00))
pack = (vu(0x06) + vu(0) + vu(1) + vu(0x09) + vu(len(data)) + vu(0x00))
streams = pack + vu(0x07) + unpack + vu(0x08) + sub + vu(0x00)
files = (vu(0x05) + vu(2) +
         vu(0x11) + vu(len(names) + 1) + b"\x00" + names +
         vu(0x00))
header = vu(0x01) + vu(0x04) + streams + files + vu(0x00)
start = struct.pack("<QQL", len(data), len(header), zlib.crc32(header))
sig = b"7z\xBC\xAF\x27\x1C" + bytes([0, 4]) + \
    struct.pack("<I", zlib.crc32(start)) + start
assert len(sig) == 32
open(os.path.join(orig, "hsolid.7z"), "wb").write(sig + data + header)
open(os.path.join(orig, "hsolid.m0"), "wb").write(payload_a)
open(os.path.join(orig, "hsolid.m1"), "wb").write(payload_b)

# --- decline fixtures -------------------------------------------------------
open(os.path.join(orig, "garbage.7z"), "wb").write(
    b"7z\xBC\xAF\x27\x1C" + rnd.randbytes(100000))  # magic + junk
open(os.path.join(orig, "nomagic.7z"), "wb").write(rnd.randbytes(100000))
print("  member content + hsolid.7z + garbage fixtures generated")
PY

# 7zz-built fixtures (member paths relative -> deep/nested/leaf.txt stays a
# deep path inside the archive)
(
    cd "$WORK/fsrc"
    $B/7zz a -t7z -m0=Copy "$WORK/orig/stored.7z" \
        alpha.c beta.bin empty.dat deep/nested/leaf.txt rand.bin >/dev/null
    $B/7zz a -t7z "$WORK/orig/lzma2.7z" alpha.c beta.bin >/dev/null
    $B/7zz a -t7z -m0=Copy -psecret -mhe=on "$WORK/orig/enc.7z" alpha.c >/dev/null
    $B/7zz a -t7z -m0=Copy -mhc=off "$WORK/orig/nohdr.7z" alpha.c empty.dat >/dev/null
    $B/7zz a -t7z -m0=Copy "$WORK/orig/dents.7z" deep >/dev/null
)
# truncated: the 7z header rides at the END of the archive, so any cut loses it
head -c 2097152 "$WORK/orig/stored.7z" > "$WORK/orig/trunc.7z"
# trailing junk: a valid archive prefix with garbage appended
cat "$WORK/orig/stored.7z" > "$WORK/orig/tjunk.7z"
head -c 777 /dev/urandom >> "$WORK/orig/tjunk.7z"
# the hand-crafted solid archive must be a REAL 7z: 7zz integrity check
$B/7zz t "$WORK/orig/hsolid.7z" >/dev/null \
    || { echo "FAIL: 7zz refuses the hand-crafted hsolid.7z"; exit 1; }
$B/7zz l -slt "$WORK/orig/hsolid.7z" | grep -q "^Solid = +" \
    || { echo "FAIL: hsolid.7z is not solid"; exit 1; }
ls -la "$WORK/orig" | grep -E "7z$|members.list"

echo "== pack-level round-trip (the pack is its own first arbiter) =="
P=$PACK/bin/p7z
ok=1
for f in stored.7z hsolid.7z nohdr.7z dents.7z tjunk.7z; do
    rm -rf "$WORK/pk" && mkdir -p "$WORK/pk/mbr"
    "$P" enumerate "$WORK/orig/$f" "$WORK/pk/table" \
        || { echo "FAIL: enumerate $f"; ok=0; continue; }
    [ -s "$WORK/pk/table" ] || { echo "FAIL: $f empty table"; ok=0; }
    while IFS=$'\t' read -r idx sn sz; do
        "$P" extract "$WORK/orig/$f" "$idx" "$WORK/pk/mbr/$idx" \
            || { echo "FAIL: extract $f idx=$idx"; ok=0; }
        [ "$(stat -c%s "$WORK/pk/mbr/$idx")" = "$sz" ] \
            || { echo "FAIL: $f idx=$idx size drift"; ok=0; }
    done < "$WORK/pk/table"
    "$P" strip "$WORK/orig/$f" "$WORK/pk/recipe" || { echo "FAIL: strip $f"; ok=0; }
    "$P" map "$WORK/orig/$f" "$WORK/pk/map" || { echo "FAIL: map $f"; ok=0; }
    "$P" rebuild "$WORK/pk/recipe" "$WORK/pk/mbr" "$WORK/pk/out" \
        || { echo "FAIL: rebuild $f"; ok=0; }
    cmp -s "$WORK/orig/$f" "$WORK/pk/out" \
        || { echo "FAIL: $f rebuild not bit-exact"; ok=0; }
    "$P" estimate "$WORK/orig/$f" > "$WORK/pk/est" || { echo "FAIL: estimate $f"; ok=0; }
    E=$(cat "$WORK/pk/est")
    [ "$E" -gt "$(stat -c%s "$WORK/orig/$f")" ] \
        || { echo "FAIL: $f estimate $E <= archive size"; ok=0; }
    echo "  $f: $(wc -l < "$WORK/pk/table") members, enumerate/extract/strip/map/rebuild/estimate OK, rebuild bit-exact"
done
# member content spot checks through the pack itself
rm -rf "$WORK/pk" && mkdir -p "$WORK/pk/mbr"
"$P" enumerate "$WORK/orig/stored.7z" "$WORK/pk/table"
while IFS=$'\t' read -r idx sn sz; do
    "$P" extract "$WORK/orig/stored.7z" "$idx" "$WORK/pk/mbr/$idx"
    src=$(awk -F'\t' -v s="$sn" '$1 == s {print $2}' "$WORK/orig/members.list")
    cmp -s "$WORK/fsrc/$src" "$WORK/pk/mbr/$idx" \
        || { echo "FAIL: member $sn content mismatch"; ok=0; }
done < "$WORK/pk/table"
"$P" enumerate "$WORK/orig/hsolid.7z" "$WORK/pk/table3"
"$P" extract "$WORK/orig/hsolid.7z" 0 "$WORK/pk/m0"
"$P" extract "$WORK/orig/hsolid.7z" 1 "$WORK/pk/m1"
cmp -s "$WORK/orig/hsolid.m0" "$WORK/pk/m0" || { echo "FAIL: hsolid member0"; ok=0; }
cmp -s "$WORK/orig/hsolid.m1" "$WORK/pk/m1" || { echo "FAIL: hsolid member1"; ok=0; }
[ "$(cat "$WORK/pk/table3")" = "0	sa.txt	19000
1	sb.bin	10240" ] || { echo "FAIL: hsolid table: $(cat "$WORK/pk/table3")"; ok=0; }
echo "  member contents exact (incl. the hand-crafted solid substreams)"
# the MRMP map must be a valid partition whose sources reproduce the bytes
"$P" map "$WORK/orig/stored.7z" "$WORK/pk/map"
python3 - "$WORK/orig/stored.7z" "$WORK/pk/map" <<'PY'
import struct, sys
arch = open(sys.argv[1], "rb").read()
m = open(sys.argv[2], "rb").read()
assert m[:4] == b"MRMP", "bad map magic"
count, = struct.unpack("<I", m[4:8])
assert len(m) == 8 + count * 29, "bad map length"
pos = 0
for i in range(count):
    o = 8 + i * 29
    orig_off, ln = struct.unpack("<QQ", m[o:o + 16])
    kind = m[o + 16]
    idx, = struct.unpack("<I", m[o + 17:o + 21])
    src_off, = struct.unpack("<Q", m[o + 21:o + 29])
    assert orig_off == pos, "map does not partition"
    assert ln > 0, "zero-length map entry"
    if kind == 0:
        assert idx == 0, "RECIPE with nonzero idx"
    else:
        assert kind == 1, "unknown kind"
    pos += ln
assert pos == len(arch), "map short of the container"
print("  stored.7z MRMP: %d entries, partitions [0, %d) exactly" % (count, pos))
PY
# declines at pack level (exit 3, never 0/1)
for f in lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    "$P" enumerate "$WORK/orig/$f" "$WORK/pk.table" 2>/dev/null \
        && { echo "FAIL: $f was NOT declined"; ok=0; }
    rc=$?
    [ "$rc" = 3 ] || { echo "FAIL: $f decline rc=$rc, want 3"; ok=0; }
    echo "  $f: declined (rc=3)"
done
# degradation leg: WITHOUT 7zz the default (encoded-header) archive declines
# cleanly and the plain-header one still works
if env -u P7Z_7ZZ "$P" enumerate "$WORK/orig/stored.7z" "$WORK/pk.table" 2>/dev/null; then
    echo "FAIL: encoded-header archive decomposed without 7zz"; ok=0
else
    echo "  stored.7z without 7zz: clean decline (encoded header)"
fi
env -u P7Z_7ZZ "$P" enumerate "$WORK/orig/nohdr.7z" "$WORK/pk.table" \
    || { echo "FAIL: -mhc=off archive needs no 7zz"; ok=0; }
echo "  nohdr.7z without 7zz: decomposes (plain header)"
[ "$ok" = 1 ] || exit 1

# fixture layout for the ranged-read legs: parse the pack's own MRMP map
"$P" map "$WORK/orig/stored.7z" "$WORK/orig/stored.map"
python3 - "$WORK/orig" <<'PY'
import struct, sys, os
d = sys.argv[1]
arch = open(os.path.join(d, "stored.7z"), "rb").read()
m = open(os.path.join(d, "stored.map"), "rb").read()
count, = struct.unpack("<I", m[4:8])
ents = []
for i in range(count):
    o = 8 + i * 29
    orig_off, ln = struct.unpack("<QQ", m[o:o + 16])
    kind = m[o + 16]
    idx, = struct.unpack("<I", m[o + 17:o + 21])
    ents.append((orig_off, ln, kind, idx))
members = [e for e in ents if e[2] == 1]
recipes = [e for e in ents if e[2] == 0]
rand = max(members, key=lambda e: e[1])     # the 9 MiB member
with open(os.path.join(d, "stored.layout"), "w") as f:
    f.write("size=%d mbr0_off=%d bnd_off=%d midrand_off=%d hdr_off=%d\n" % (
        len(arch),
        members[0][0],
        members[1][0],                      # member|member (or mbr|recipe) edge
        rand[0] + 4 * 1024 * 1024,          # 4 MiB into rand
        recipes[-1][0]))                    # the 7z header area (recipe)
print("  layout: size=%d, %d member extents, %d recipe ranges"
      % (len(arch), len(members), len(recipes)))
PY
. "$WORK/orig/stored.layout"   # size mbr0_off bnd_off midrand_off hdr_off
"$P" enumerate "$WORK/orig/stored.7z" "$WORK/orig/stored.table"

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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o"
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in stored.7z hsolid.7z lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 (p7z containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
for f in stored.7z hsolid.7z; do
    grep -q "$f: p7z (codecpack)" "$WORK/sweep1.log" \
        || { echo "FAIL: $f not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
done
for f in lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    if grep -q "$f: p7z (codecpack)" "$WORK/sweep1.log"; then
        echo "FAIL: $f was decomposed (must be declined)"; cat "$WORK/sweep1.log"; exit 1
    fi
done
# members batch in the SAME run their container decomposed (WP14b pattern)
grep -q "stored.7z!\*: 2 parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text members not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "stored.7z!\*: 1 parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|parts -> " "$WORK/sweep1.log"

echo "== sibling set (members + table + map) =="
NROWS=$(wc -l < "$WORK/orig/stored.table")
N=$($B/invf-ls "$IMG" | grep -c "stored\.7z!" || true)
echo "  stored.7z!* names: $N (want $NROWS members + mbrt + mbrmap)"
[ "$N" -eq "$((NROWS + 2))" ] || { echo "FAIL: stored.7z sibling count"; $B/invf-ls "$IMG"; exit 1; }
N=$($B/invf-ls "$IMG" | grep -c "hsolid\.7z!" || true)
echo "  hsolid.7z!* names: $N (want 2 + mbrt + mbrmap)"
[ "$N" -eq 4 ] || { echo "FAIL: hsolid.7z sibling count"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep -q "stored\.7z!mbrt" || { echo "FAIL: member table missing"; exit 1; }
$B/invf-ls "$IMG" | grep -q "stored\.7z!mbrmap" || { echo "FAIL: member map missing"; exit 1; }
# every announced member exists with its announced size; the map from
# idx -> sibling name rides the members.list relpath for content refs
while IFS=$'\t' read -r idx sn sz; do
    mbr=$(printf "stored.7z!mbr%04d-%s" "$idx" "$sn")
    $B/invf-ls "$IMG" | grep "$mbr" | grep -q "$sz bytes" \
        || { echo "FAIL: member $mbr not $sz bytes"; $B/invf-ls "$IMG"; exit 1; }
done < "$WORK/orig/stored.table"
$B/invf-ls "$IMG" | grep "stored\.7z!mbr....-empty\.dat" | grep -q "0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; exit 1; }
for f in lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    if $B/invf-ls "$IMG" | grep -q "$f!"; then
        echo "FAIL: declined $f gained siblings"; $B/invf-ls "$IMG"; exit 1
    fi
done
echo "members + table + map present with announced sizes; declines clean"

echo "== class stamps =="
for f in stored.7z hsolid.7z; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=3 algo=23 gen=1" ] || { echo "FAIL: $f: want CONTAINER{p7z=23,1}"; exit 1; }
done
while IFS=$'\t' read -r idx sn sz; do
    mbr=$(printf "stored.7z!mbr%04d-%s" "$idx" "$sn")
    C=$("$WORK/classof" "$IMG" "$mbr")
    echo "  $mbr: $C"
    case "$sn" in
        alpha.c|leaf.txt)
            [ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: text member $sn: want TEXT{PPMD,1}"; exit 1; } ;;
        beta.bin)
            [ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: ELF member: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; } ;;
        rand.bin|empty.dat)
            [ "$C" = "none" ] || { echo "FAIL: $sn should be unclassified after sweep 1"; exit 1; } ;;
    esac
done < "$WORK/orig/stored.table"
C=$("$WORK/classof" "$IMG" "hsolid.7z!mbr0000-sa.txt")
echo "  hsolid.7z!mbr0000-sa.txt: $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: solid text member: want TEXT{PPMD,1}"; exit 1; }
for f in lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    case "$C" in *algo=23*) echo "FAIL: declined $f carries a pack stamp"; exit 1;; esac
done

echo "== verify --deep (reads every container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in stored.7z hsolid.7z lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
# the members are real inodes: read them directly (alpha.c/leaf.txt = PPMd
# batch slices, beta.bin = ZSTD_BCJ batch slice, rand.bin = RAW)
while IFS=$'\t' read -r idx sn sz; do
    mbr=$(printf "stored.7z!mbr%04d-%s" "$idx" "$sn")
    src=$(awk -F'\t' -v s="$sn" '$1 == s {print $2}' "$WORK/orig/members.list")
    $B/invf-cat "$IMG" "$mbr" "$WORK/out/mbr.$sn" >/dev/null
    cmp -s "$WORK/fsrc/$src" "$WORK/out/mbr.$sn" || { echo "MISMATCH member $sn"; ok=0; }
done < "$WORK/orig/stored.table"
$B/invf-cat "$IMG" "hsolid.7z!mbr0000-sa.txt" "$WORK/out/hm0" >/dev/null
$B/invf-cat "$IMG" "hsolid.7z!mbr0001-sb.bin" "$WORK/out/hm1" >/dev/null
cmp -s "$WORK/orig/hsolid.m0" "$WORK/out/hm0" || { echo "MISMATCH hsolid member0"; ok=0; }
cmp -s "$WORK/orig/hsolid.m1" "$WORK/out/hm1" || { echo "MISMATCH hsolid member1"; ok=0; }
[ "$ok" = 1 ] || exit 1
echo "containers and members bit-exact"
# the member table rides verbatim as !mbrt
$B/invf-cat "$IMG" "stored.7z!mbrt" "$WORK/out/mbrt" >/dev/null
cmp -s "$WORK/orig/stored.table" "$WORK/out/mbrt" \
    || { echo "FAIL: mbrt content drifted"; diff "$WORK/orig/stored.table" "$WORK/out/mbrt"; exit 1; }
echo "member table sibling verbatim"

echo "== ranged reads (the WP16b local splice over stored.7z) =="
rng() {  # rng <off> <len> <tag>
    dd if="$WORK/orig/stored.7z" of="$WORK/out/ref.$3" bs=1 skip="$1" count="$2" 2>/dev/null
    "$WORK/rngread" "$IMG" stored.7z "$1" "$2" "$WORK/out/got.$3" >/dev/null
    cmp -s "$WORK/out/ref.$3" "$WORK/out/got.$3" \
        || { echo "FAIL: range $3 (off=$1 len=$2) mismatch"; exit 1; }
}
rng 0 64 head                              # the 7z signature (recipe)
rng $((mbr0_off - 16)) 64 rec2mbr          # signature -> first member extent
rng $((bnd_off - 32)) 128 mbr2mbr          # member -> member boundary
rng $midrand_off 1048576 midrand           # deep inside the 9 MiB member
rng $((hdr_off - 32)) 96 mbr2hdr           # last member -> header boundary
rng $hdr_off 512 hdr                       # the (packed) 7z header (recipe)
rng $((size - 1000)) 1000 tail             # the archive tail (recipe)
"$WORK/rngread" "$IMG" stored.7z 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/stored.7z" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
"$WORK/rngread" "$IMG" stored.7z $((size + 4096)) 100 "$WORK/out/eof.rng" >/dev/null
[ -s "$WORK/out/eof.rng" ] && { echo "FAIL: read past EOF returned bytes"; exit 1; }
# the hand-crafted solid container splices through its own map too:
# a range crossing the member|member substream boundary inside the one
# solid folder (sa.txt's tail -> sb.bin's head), dd-referenced
dd if="$WORK/orig/hsolid.7z" of="$WORK/out/ref.hsold" bs=1 skip=18968 count=128 2>/dev/null
"$WORK/rngread" "$IMG" hsolid.7z 18968 128 "$WORK/out/hsolid.tail" >/dev/null
cmp -s "$WORK/out/ref.hsold" "$WORK/out/hsolid.tail" \
    || { echo "FAIL: hsolid substream-boundary read mismatch"; exit 1; }
echo "head / boundaries / mid-extent / header / tail / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" stored.7z "$WORK/out/absent" >/dev/null
cmp -s "$WORK/orig/stored.7z" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" stored.7z \
    $midrand_off 1048576 "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midrand" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" stored.7z 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/orig/stored.7z" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent ranged whole read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" hsolid.7z "$WORK/out/absent.hs" >/dev/null
cmp -s "$WORK/orig/hsolid.7z" "$WORK/out/absent.hs" \
    || { echo "FAIL: pack-absent hsolid read not bit-exact"; exit 1; }
echo "pack-absent: whole reads + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: members store through the normal pipeline, no re-fire =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q ": p7z (codecpack)" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) \
    || { echo "FAIL: corrupt after sweep 2"; exit 1; }
for f in stored.7z hsolid.7z; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/s2.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/s2.$f" || { echo "FAIL: $f drifted"; exit 1; }
    C=$("$WORK/classof" "$IMG" "$f")
    [ "$C" = "cls=3 algo=23 gen=1" ] || { echo "FAIL: $f stamp drifted: $C"; exit 1; }
done
echo "containers stable and bit-exact after sweep 2 (idempotent re-sweep)"

echo "== sweep #3: idle =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1 || { cat "$WORK/sweep3.log"; exit 1; }
if grep -q ": p7z (codecpack)" "$WORK/sweep3.log"; then
    echo "FAIL: sweep 3 re-fired the p7z pack"; cat "$WORK/sweep3.log"; exit 1
fi
grep -q " 0 corrupt" <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
echo "no re-decomposition"

echo "== delete cascade (vol_unlink, the FUSE path) =="
"$WORK/cbrm" "$IMG" stored.7z hsolid.7z
if $B/invf-ls "$IMG" | grep -q "7z!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + tables + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivors bit-exact =="
ok=1
for f in lzma2.7z enc.7z garbage.7z nomagic.7z trunc.7z; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/surv.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/surv.$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "declined files bit-exact after the deletes"

echo "== admission leg: INVFS_ARC_BYTES=1M =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/stored.7z" stored.7z >/dev/null
# the container (9+ MiB) exceeds the 1 MiB whole-file ARC budget -> policy
# refusal before strip/extract; GENERIC_MEMLIMIT{23,1}
INVFS_ARC_BYTES=1M $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q ": p7z (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 1M ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" stored.7z)
echo "  stored.7z (arc limit): $C"
[ "$C" = "cls=5 algo=23 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{p7z=23,1}"; exit 1; }
$B/invf-cat "$IMGMEM" stored.7z "$WORK/out/storedmem.7z" >/dev/null
cmp -s "$WORK/orig/stored.7z" "$WORK/out/storedmem.7z" \
    || { echo "FAIL: arc-limit read not bit-exact"; exit 1; }
echo "ARC policy refusal stored generic, bit-exact"

echo "== admission leg: INVFS_DEC_MEM_LIMIT=64K (the pack's estimate) =="
$B/invf-mkfs "$IMGWS" 0.2 >/dev/null
$B/invf-cp "$IMGWS" "$WORK/orig/stored.7z" stored.7z >/dev/null
# the pack's estimate (archive + members + 64 MiB margin) exceeds 64K ->
# policy refusal before strip/extract; GENERIC_MEMLIMIT{23,1}
INVFS_DEC_MEM_LIMIT=64K $B/invf-sweep "$IMGWS" > "$WORK/sweep-ws.log" 2>&1 \
    || { cat "$WORK/sweep-ws.log"; exit 1; }
if grep -q ": p7z (codecpack)" "$WORK/sweep-ws.log"; then
    echo "FAIL: decomposition ran under a 64K decode-memory limit"; exit 1
fi
C=$("$WORK/classof" "$IMGWS" stored.7z)
echo "  stored.7z (dec_mem limit): $C"
[ "$C" = "cls=5 algo=23 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{p7z=23,1}"; exit 1; }
$B/invf-cat "$IMGWS" stored.7z "$WORK/out/storedws.7z" >/dev/null
cmp -s "$WORK/orig/stored.7z" "$WORK/out/storedws.7z" \
    || { echo "FAIL: dec_mem-limit read not bit-exact"; exit 1; }
echo "estimate-driven policy refusal stored generic, bit-exact"

echo "P7Z CONTAINERPACK E2E: PASS"
