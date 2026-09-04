#!/bin/bash
# test-containerpack.sh — WP16a containerpack ABI + WP16b ABI v1.1
# (seekable containers / DEFER_ENOSPC / profiles) end-to-end (persistent
# regression).
#
#   fixtures (inline python, stdlib only): SPLT containers for the
#   tools/codecpacks/splt_test pack ("SPLT" + u32 n + n u64 lengths +
#   concatenated payloads). splt_test carries a `map` command (ABI v1.1),
#   so its containers are SEEKABLE: reads splice from the !mbrmap map +
#   recipe + member siblings and never exec the pack:
#     multi.splt — 4 members: text (PPMd-batchable), EMPTY (0 bytes), a
#       real x86-64 ELF (ZSTD/BCJ-batchable), random (stays generic).
#     nest.splt  — member 0 is itself a valid SPLT (nested container:
#       decomposed by a LATER sweep, read back through two map splices).
#     neg.splt   — corrupt table (lengths under-count the payload): the
#       FS-side map VALIDATION fires (the container does not partition).
#   mkfs -> cp -> INVFS_CODECPACKS=$REPO/tools/codecpacks invf-sweep
#   (expect "splt_test (codecpack)" lines + "!*: N parts -> PPMd/ZSTD
#   batch") -> class stamps CONTAINER{40,1} / TEXT / BATCHED_BIN ->
#   verify --deep -> sha256 bit-exact (containers AND direct member reads)
#   -> RANGED reads (mid-member / cross-boundary / whole-by-windows)
#   -> pack-ABSENT reads still bit-exact (the map is self-describing)
#   -> sweep 2 decomposes the nested member -> nested pack-absent read
#   -> sweep 3 idle -> vol_unlink cascade kills every "!"-sibling incl.
#   !mbrmap -> fsck clean -> survivors bit-exact.
#
#   Separate-image legs:
#     map-deleted: a seekable container whose !mbrmap sibling is unlinked
#       falls back to the pack's rebuild exec (pack present) / fails LOUDLY
#       (pack absent).
#     nomap pack (map-less splt, algo 41, materialized in $WORK): the WP16a
#       flow -- rebuild-exec guard (neg.splt "rebuild guard refused"),
#       exec reads, pack-absent read fails LOUDLY.
#     corrupt map (SPLT_CORRUPT_MAP=guard / =hole): the sweep refuses,
#       abandons to generic, purges every sibling, no pack stamp, fsck
#       clean.
#     DEFER_ENOSPC: a tight image stamps cls=9{algo,gen} and stays RAW;
#     freeing space and re-sweeping processes the file.
#     INVFS_PROFILE: the sweep logs the profile + generic zstd level.
#   Admission leg: fresh image, INVFS_DEC_MEM_LIMIT=64K -> GENERIC_
#   MEMLIMIT{40,1}, no decomposition, bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-containerpack.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
WORK=/dev/shm/wp16cpack
IMG=wp16cpack.img
IMGMEM=wp16cpack-mem.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/nopacks"
cd /dev/shm
rm -f "$IMG" "$IMGMEM"

echo "== tools =="
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }

echo "== generate fixtures =="
python3 - "$WORK/orig" <<'PY'
import os
import random
import struct
import sys

def splt(members):
    b = b"SPLT" + struct.pack("<I", len(members))
    for m in members:
        b += struct.pack("<Q", len(m))
    return b + b"".join(members)

d = sys.argv[1]
rnd = random.Random(16)

# text member: a real C source (content-sniffs as text; the member name
# "chunk0" carries no extension on purpose)
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

# ELF member: a real x86-64 binary, >= 4 KB (binary-family magic gate)
elf = None
for p in ("/usr/bin/passwd", "/usr/bin/gpg", "/bin/ls", "/usr/bin/ls",
          "/bin/bash", "/usr/bin/bash"):
    if os.path.isfile(p) and not os.path.islink(p):
        with open(p, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF" and h[18] == 62 and \
           os.path.getsize(p) > 65536:
            elf = open(p, "rb").read()
            break
assert elf, "no x86-64 ELF fixture found"

multi = splt([text, b"", elf, rnd.randbytes(32768)])
open(os.path.join(d, "multi.splt"), "wb").write(multi)
open(os.path.join(d, "member0.bin"), "wb").write(text)   # for direct reads
open(os.path.join(d, "member2.bin"), "wb").write(elf)

# the member layout, for the ranged-read legs (offsets in the ORIGINAL)
data_off = 8 + 4 * 8
m0_len = len(text)
m2_off = data_off + m0_len          # member 1 is empty: m0 end == m2 start
with open(os.path.join(d, "multi.layout"), "w") as f:
    f.write("size=%d data_off=%d m0_len=%d m2_off=%d m2_len=%d\n"
            % (len(multi), data_off, m0_len, m2_off, len(elf)))

inner = splt([b"inner member alpha\n" * 500, b"inner member beta\n" * 700])
nest = splt([inner, b"outer tail\n" * 300])
open(os.path.join(d, "nest.splt"), "wb").write(nest)

# negative: the table announces 5 payload bytes but 10 are present --
# enumerate/extract/strip succeed; for the seekable pack the FS-side MAP
# VALIDATION refuses (recipe+members cannot partition the container), for
# a map-less pack the rebuild guard refuses (the nomap leg below).
open(os.path.join(d, "neg.splt"), "wb").write(
    b"SPLT" + struct.pack("<I", 1) + struct.pack("<Q", 5) + b"0123456789")

print("  multi.splt", len(multi), "(4 members: text %d, empty, elf %d, rand 32768)"
      % (len(text), len(elf)))
print("  nest.splt ", len(nest), "(member 0 = inner SPLT of %d bytes)" % len(inner))
print("  neg.splt  ", 26, "(corrupt table)")
PY
. "$WORK/orig/multi.layout"   # size data_off m0_len m2_off m2_len

# class-stamp reader + a ranged-read harness (no stock CLI does ranged
# reads); built from the repo objects like test-rawimg.sh's helpers
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in multi.splt nest.splt neg.splt; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 (containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "multi.splt: splt_test (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: multi.splt not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "nest.splt: splt_test (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: nest.splt not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "splt_test: neg.splt: map does not partition" "$WORK/sweep1.log" \
    || { echo "FAIL: neg.splt map validation did not fire"; cat "$WORK/sweep1.log"; exit 1; }
# members batch in the SAME run their container decomposed (WP14b pattern)
grep -q "multi.splt!\*: 1 parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text member not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "multi.splt!\*: 1 parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|refused|partition|parts -> " "$WORK/sweep1.log"

echo "== sibling set (incl. the WP16b member map) =="
N=$($B/invf-ls "$IMG" | grep -c "multi\.splt!" || true)
echo "multi.splt!* names: $N"
[ "$N" -eq 6 ] || { echo "FAIL: want 6 (4 members + member table + map)"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep "multi\.splt!mbr0001-chunk1" | grep -q "0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep -q "multi\.splt!mbrt" \
    || { echo "FAIL: member table sibling missing"; exit 1; }
$B/invf-ls "$IMG" | grep -q "multi\.splt!mbrmap" \
    || { echo "FAIL: member map sibling missing"; exit 1; }
if $B/invf-ls "$IMG" | grep -q "neg\.splt!"; then
    echo "FAIL: map validation refusal left siblings behind"; exit 1
fi
echo "4 members + table + map present; empty member is 0 bytes; neg.splt clean"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" multi.splt)
echo "  multi.splt: $C"
[ "$C" = "cls=3 algo=40 gen=1" ] || { echo "FAIL: want CONTAINER{SPLT=40,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" nest.splt)
echo "  nest.splt: $C"
[ "$C" = "cls=3 algo=40 gen=1" ] || { echo "FAIL: want CONTAINER{SPLT=40,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "multi.splt!mbr0000-chunk0")
echo "  member0 (text): $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "multi.splt!mbr0002-chunk2")
echo "  member2 (elf): $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "multi.splt!mbr0003-chunk3")
echo "  member3 (rand): $C"
[ "$C" = "none" ] || { echo "FAIL: random member should stay unclassified"; exit 1; }
C=$("$WORK/classof" "$IMG" neg.splt)
echo "  neg.splt: $C"
case "$C" in *algo=40*) echo "FAIL: neg.splt carries a pack stamp" ; exit 1;; esac

echo "== verify --deep (reads every container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in multi.splt nest.splt neg.splt; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
# members are real inodes: read them directly (0 = PPMd batch slice,
# 2 = ZSTD_BCJ batch slice -- rebuilds flow through batched storage)
$B/invf-cat "$IMG" "multi.splt!mbr0000-chunk0" "$WORK/out/m0" >/dev/null
$B/invf-cat "$IMG" "multi.splt!mbr0002-chunk2" "$WORK/out/m2" >/dev/null
cmp -s "$WORK/orig/member0.bin" "$WORK/out/m0" || { echo "MISMATCH member0"; ok=0; }
cmp -s "$WORK/orig/member2.bin" "$WORK/out/m2" || { echo "MISMATCH member2"; ok=0; }
[ "$ok" = 1 ] || exit 1
echo "containers and members bit-exact"

echo "== ranged reads (the WP16b local splice) =="
# reference slices from the pristine original
rng() {  # rng <off> <len> <tag>
    dd if="$WORK/orig/multi.splt" of="$WORK/out/ref.$3" bs=1 skip="$1" count="$2" 2>/dev/null
    "$WORK/rngread" "$IMG" multi.splt "$1" "$2" "$WORK/out/got.$3" >/dev/null
    cmp -s "$WORK/out/ref.$3" "$WORK/out/got.$3" \
        || { echo "FAIL: range $3 (off=$1 len=$2) mismatch"; exit 1; }
}
rng $((m2_off + 4096)) 65536 midmember      # strictly inside member 2 (ELF)
rng $((data_off - 16)) 64 rec2mbr           # recipe -> member 0 boundary
rng $((data_off + m0_len - 32)) 64 mbr2mbr  # member 0 -> member 2 boundary
rng $((size - 1000)) 1000 tail              # the container tail
rng 0 64 head                               # the recipe head
"$WORK/rngread" "$IMG" multi.splt 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
"$WORK/rngread" "$IMG" multi.splt $((size + 4096)) 100 "$WORK/out/eof.rng" >/dev/null
[ -s "$WORK/out/eof.rng" ] && { echo "FAIL: read past EOF returned bytes"; exit 1; }
echo "mid-member / cross-boundary / tail / head / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
# a fresh process with an empty pack dir: algo 40 has no registry entry,
# and STILL every byte comes back, without any pack exec -- that is the
# point of a seekable container
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" multi.splt "$WORK/out/absent" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" multi.splt \
    $((m2_off + 4096)) 65536 "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" multi.splt 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent ranged whole read mismatch"; exit 1; }
echo "pack-absent: whole read + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: nested member decomposes, nothing else re-fires =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
grep -q "nest.splt!mbr0000-chunk0: splt_test (codecpack)" "$WORK/sweep2.log" \
    || { echo "FAIL: nested SPLT member not decomposed"; cat "$WORK/sweep2.log"; exit 1; }
if grep -q "multi.splt: splt_test\|nest.splt: splt_test\|guard refused\|partition" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/sweep2.log"; exit 1
fi
grep -E "codecpack" "$WORK/sweep2.log"
$B/invf-ls "$IMG" | grep -q "nest\.splt!mbr0000-chunk0!mbrmap" \
    || { echo "FAIL: nested member map sibling missing"; exit 1; }

echo "== nested read-back (a map splice through a map splice) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify2.log"
grep -q " 0 corrupt," "$WORK/verify2.log" || { echo "FAIL: corrupt after nesting"; exit 1; }
$B/invf-cat "$IMG" nest.splt "$WORK/out/nest2.splt" >/dev/null
cmp -s "$WORK/orig/nest.splt" "$WORK/out/nest2.splt" \
    || { echo "FAIL: nested container not bit-exact"; exit 1; }
$B/invf-cat "$IMG" multi.splt "$WORK/out/multi2.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/multi2.splt" \
    || { echo "FAIL: multi.splt drifted"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" nest.splt "$WORK/out/nest3.splt" >/dev/null
cmp -s "$WORK/orig/nest.splt" "$WORK/out/nest3.splt" \
    || { echo "FAIL: pack-absent NESTED read not bit-exact"; exit 1; }
echo "nested splice bit-exact (pack present AND pack absent)"

echo "== sweep #3: idle =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1 || { cat "$WORK/sweep3.log"; exit 1; }
if grep -q "codecpack\|guard refused\|partition" "$WORK/sweep3.log"; then
    echo "FAIL: sweep 3 re-fired a containerpack"; cat "$WORK/sweep3.log"; exit 1
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
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/cbrm" "$IMG" multi.splt nest.splt
if $B/invf-ls "$IMG" | grep -q "splt!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + maps + nested grandchildren deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivor bit-exact =="
$B/invf-cat "$IMG" neg.splt "$WORK/out/neg2.splt" >/dev/null
cmp -s "$WORK/orig/neg.splt" "$WORK/out/neg2.splt" \
    || { echo "FAIL: neg.splt drifted after deletes"; exit 1; }

echo "== map-deleted fallback (exec when the map is gone) =="
IMGMD=wp16cpack-md.img
rm -f "$IMGMD"
$B/invf-mkfs "$IMGMD" 0.2 >/dev/null
$B/invf-cp "$IMGMD" "$WORK/orig/multi.splt" multi.splt >/dev/null
$B/invf-sweep "$IMGMD" > "$WORK/sweepmd.log" 2>&1 || { cat "$WORK/sweepmd.log"; exit 1; }
grep -q "multi.splt: splt_test (codecpack)" "$WORK/sweepmd.log" \
    || { echo "FAIL: map-deleted leg: not decomposed"; exit 1; }
"$WORK/cbrm" "$IMGMD" "multi.splt!mbrmap"
$B/invf-ls "$IMGMD" | grep -q "multi\.splt!mbrmap" \
    && { echo "FAIL: !mbrmap sibling not deleted"; exit 1; }
# pack present + no map: the WP16a whole-file rebuild exec answers
$B/invf-cat "$IMGMD" multi.splt "$WORK/out/md.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/md.splt" \
    || { echo "FAIL: map-deleted exec fallback not bit-exact"; exit 1; }
# a ranged read falls back the same way (through the whole-file ARC divert)
"$WORK/rngread" "$IMGMD" multi.splt $((m2_off + 4096)) 65536 "$WORK/out/md.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/md.rng" \
    || { echo "FAIL: map-deleted ranged read mismatch"; exit 1; }
# pack absent AND no map: LOUD failure (the WP16a semantics, unchanged)
if INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMGMD" multi.splt \
        "$WORK/out/md.absent" 2> "$WORK/mdabsent.err"; then
    echo "FAIL: pack-absent map-less read SUCCEEDED (silent 1:1 break)"; exit 1
fi
grep -q "codecpack" "$WORK/mdabsent.err" \
    || { echo "FAIL: no loud pack-absent diagnostic"; cat "$WORK/mdabsent.err"; exit 1; }
echo "map deleted: exec fallback exact (pack present), LOUD (pack absent)"

echo "== map-less pack (WP16a flow, incl. the exec guard on neg.splt) =="
IMGNM=wp16cpack-nm.img
rm -f "$IMGNM"
mkdir -p "$WORK/packs-nomap/splt_nomap.codecpack"
ln -s "$REPO/tools/codecpacks/splt_test.codecpack/splt.py" \
    "$WORK/packs-nomap/splt_nomap.codecpack/splt.py"
cat > "$WORK/packs-nomap/splt_nomap.codecpack/manifest" <<EOF
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
$B/invf-mkfs "$IMGNM" 0.2 >/dev/null
$B/invf-cp "$IMGNM" "$WORK/orig/multi.splt" multi.splt >/dev/null
$B/invf-cp "$IMGNM" "$WORK/orig/neg.splt" neg.splt >/dev/null
INVFS_CODECPACKS=$WORK/packs-nomap $B/invf-sweep "$IMGNM" > "$WORK/sweepnm.log" 2>&1 \
    || { cat "$WORK/sweepnm.log"; exit 1; }
grep -q "multi.splt: splt_nomap (codecpack)" "$WORK/sweepnm.log" \
    || { echo "FAIL: map-less pack did not decompose multi.splt"; cat "$WORK/sweepnm.log"; exit 1; }
grep -q "splt_nomap: neg.splt: rebuild guard refused" "$WORK/sweepnm.log" \
    || { echo "FAIL: map-less pack: neg.splt exec guard did not fire"; cat "$WORK/sweepnm.log"; exit 1; }
if $B/invf-ls "$IMGNM" | grep -q "multi\.splt!mbrmap"; then
    echo "FAIL: a map-less pack must not produce a !mbrmap sibling"; exit 1
fi
C=$("$WORK/classof" "$IMGNM" multi.splt)
echo "  multi.splt (nomap): $C"
[ "$C" = "cls=3 algo=41 gen=1" ] || { echo "FAIL: want CONTAINER{SPLT_NOMAP=41,1}"; exit 1; }
# pack present (the NOMAP dir): the WP16a exec-rebuild read path answers
INVFS_CODECPACKS=$WORK/packs-nomap $B/invf-cat "$IMGNM" multi.splt "$WORK/out/nm.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/nm.splt" \
    || { echo "FAIL: map-less exec read not bit-exact"; exit 1; }
if INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMGNM" multi.splt \
        "$WORK/out/nm.absent" 2> "$WORK/nmabsent.err"; then
    echo "FAIL: map-less pack-absent read SUCCEEDED (silent 1:1 break)"; exit 1
fi
grep -q "codecpack" "$WORK/nmabsent.err" \
    || { echo "FAIL: map-less pack-absent: no loud diagnostic"; cat "$WORK/nmabsent.err"; exit 1; }
echo "map-less pack: exec guard fires, exec reads exact, pack-absent LOUD"

echo "== corrupt map: the sweep's guard refuses (guard + hole) =="
for mode in guard hole; do
    IMGC=wp16cpack-corrupt-$mode.img
    rm -f "$IMGC"
    $B/invf-mkfs "$IMGC" 0.2 >/dev/null
    $B/invf-cp "$IMGC" "$WORK/orig/multi.splt" multi.splt >/dev/null
    SPLT_CORRUPT_MAP=$mode $B/invf-sweep "$IMGC" > "$WORK/sweepc-$mode.log" 2>&1 \
        || { cat "$WORK/sweepc-$mode.log"; exit 1; }
    if [ "$mode" = guard ]; then
        grep -q "splt_test: multi.splt: map guard refused" "$WORK/sweepc-$mode.log" \
            || { echo "FAIL: corrupt map ($mode) did not trip the read-back guard"; cat "$WORK/sweepc-$mode.log"; exit 1; }
    else
        grep -q "splt_test: multi.splt: map does not partition" "$WORK/sweepc-$mode.log" \
            || { echo "FAIL: corrupt map ($mode) did not trip validation"; cat "$WORK/sweepc-$mode.log"; exit 1; }
    fi
    # refused: falls to generic UNSTAMPED by the pack, no siblings survive
    if $B/invf-ls "$IMGC" | grep -q "multi\.splt!"; then
        echo "FAIL: corrupt map ($mode) left siblings behind"; $B/invf-ls "$IMGC"; exit 1
    fi
    C=$("$WORK/classof" "$IMGC" multi.splt)
    echo "  multi.splt (corrupt $mode): $C"
    case "$C" in *algo=40*|*cls=3*) echo "FAIL: corrupt map ($mode) got a pack/container stamp"; exit 1;; esac
    $B/invf-cat "$IMGC" multi.splt "$WORK/out/corrupt-$mode.splt" >/dev/null
    cmp -s "$WORK/orig/multi.splt" "$WORK/out/corrupt-$mode.splt" \
        || { echo "FAIL: corrupt map ($mode): content not bit-exact"; exit 1; }
    grep -q " 0 corrupt," <($B/invf-verify "$IMGC" --deep) \
        || { echo "FAIL: corrupt map ($mode): verify --deep not clean"; exit 1; }
    grep -q "orphans:      0" <($B/invf-fsck "$IMGC") \
        || { echo "FAIL: corrupt map ($mode): rollback leaked blocks"; exit 1; }
done
echo "corrupt map: guard refused, rolled back to RAW, generic store, fsck clean"

echo "== DEFER_ENOSPC (cls=9): tight image defers, freed space re-arms =="
IMGT=wp16cpack-tight.img
rm -f "$IMGT"
python3 - "$WORK/orig" <<'PY'
import random, sys
rnd = random.Random(99)
# incompressible filler: big enough that free space drops under the
# DEFER_ENOSPC admission (64 MiB margin + half the member total) once the
# filler and the container are both in
open(sys.argv[1] + "/filler.bin", "wb").write(rnd.randbytes(120 * 1024 * 1024))
PY
$B/invf-mkfs "$IMGT" 0.2 >/dev/null
$B/invf-cp "$IMGT" "$WORK/orig/filler.bin" filler.bin >/dev/null
$B/invf-cp "$IMGT" "$WORK/orig/multi.splt" multi.splt >/dev/null
$B/invf-sweep "$IMGT" > "$WORK/sweept1.log" 2>&1 || { cat "$WORK/sweept1.log"; exit 1; }
if grep -q "splt_test (codecpack)" "$WORK/sweept1.log"; then
    echo "FAIL: decomposition ran on a volume too tight for it"; exit 1
fi
C=$("$WORK/classof" "$IMGT" multi.splt)
echo "  multi.splt (tight): $C"
[ "$C" = "cls=9 algo=40 gen=1" ] || { echo "FAIL: want DEFER_ENOSPC{SPLT=40,1}"; exit 1; }
# the stamp is a WAIT, not a downgrade: the file is still RAW and bit-exact
$B/invf-cat "$IMGT" multi.splt "$WORK/out/tight1.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/tight1.splt" \
    || { echo "FAIL: DEFER_ENOSPC file not bit-exact"; exit 1; }
# re-sweeping while still tight re-stamps and stays RAW (idempotent)
$B/invf-sweep "$IMGT" > "$WORK/sweept2.log" 2>&1 || { cat "$WORK/sweept2.log"; exit 1; }
if grep -q "splt_test (codecpack)" "$WORK/sweept2.log"; then
    echo "FAIL: decomposition ran on the still-tight re-sweep"; exit 1
fi
C=$("$WORK/classof" "$IMGT" multi.splt)
[ "$C" = "cls=9 algo=40 gen=1" ] || { echo "FAIL: DEFER_ENOSPC re-stamp drifted"; exit 1; }
# free space appears: the filler goes, the NEXT sweep decomposes
"$WORK/cbrm" "$IMGT" filler.bin
$B/invf-sweep "$IMGT" > "$WORK/sweept3.log" 2>&1 || { cat "$WORK/sweept3.log"; exit 1; }
grep -q "multi.splt: splt_test (codecpack)" "$WORK/sweept3.log" \
    || { echo "FAIL: freed space did not re-arm the decomposition"; cat "$WORK/sweept3.log"; exit 1; }
C=$("$WORK/classof" "$IMGT" multi.splt)
echo "  multi.splt (after free): $C"
[ "$C" = "cls=3 algo=40 gen=1" ] || { echo "FAIL: want CONTAINER{SPLT=40,1} after free"; exit 1; }
$B/invf-ls "$IMGT" | grep -q "multi\.splt!mbrmap" \
    || { echo "FAIL: re-armed decomposition has no member map"; exit 1; }
$B/invf-cat "$IMGT" multi.splt "$WORK/out/tight3.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/tight3.splt" \
    || { echo "FAIL: re-armed container not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMGT" multi.splt \
    $((m2_off + 4096)) 65536 "$WORK/out/tight3.rng" >/dev/null
cmp -s "$WORK/out/ref.midmember" "$WORK/out/tight3.rng" \
    || { echo "FAIL: re-armed container pack-absent ranged read mismatch"; exit 1; }
echo "DEFER_ENOSPC: cls=9 wait RAW -> re-sweep after free decomposes"

echo "== INVFS_PROFILE =="
IMGP=wp16cpack-prof.img
rm -f "$IMGP"
$B/invf-mkfs "$IMGP" 0.2 >/dev/null
$B/invf-cp "$IMGP" "$WORK/orig/member0.bin" member0.bin >/dev/null
INVFS_PROFILE=fast $B/invf-sweep "$IMGP" > "$WORK/sweepp.log" 2>&1 \
    || { cat "$WORK/sweepp.log"; exit 1; }
grep -q "profile: fast (generic zstd level 6)" "$WORK/sweepp.log" \
    || { echo "FAIL: no fast-profile log line"; cat "$WORK/sweepp.log"; exit 1; }
$B/invf-cat "$IMGP" member0.bin "$WORK/out/prof.bin" >/dev/null
cmp -s "$WORK/orig/member0.bin" "$WORK/out/prof.bin" \
    || { echo "FAIL: fast-profile sweep not bit-exact"; exit 1; }
INVFS_PROFILE=dense $B/invf-sweep "$IMGP" > "$WORK/sweepp2.log" 2>&1 \
    || { cat "$WORK/sweepp2.log"; exit 1; }
grep -q "profile: dense (generic zstd level 19)" "$WORK/sweepp2.log" \
    || { echo "FAIL: no dense-profile log line"; cat "$WORK/sweepp2.log"; exit 1; }
# WP19: turbo is a real profile now (verbatim store), so the rejection leg
# uses a genuinely unknown name
INVFS_PROFILE=ludicrous $B/invf-sweep "$IMGP" > "$WORK/sweepp3.log" 2>&1 \
    || { cat "$WORK/sweepp3.log"; exit 1; }
grep -q "is not a profile; using balanced" "$WORK/sweepp3.log" \
    || { echo "FAIL: invalid profile not rejected to the default"; cat "$WORK/sweepp3.log"; exit 1; }
grep -q "profile: balanced (generic zstd level 19)" "$WORK/sweepp3.log" \
    || { echo "FAIL: invalid profile did not report the default"; cat "$WORK/sweepp3.log"; exit 1; }
# the default (env unset) prints no profile line at all: byte-identical
# logs are how the pre-WP16b suite stays green
if grep -q "^profile:" "$WORK/sweep1.log"; then
    echo "FAIL: default sweep gained a profile line"; exit 1
fi
# the effective profile is published to pack subprocesses (vol_open setenv):
# a codec pack whose helper records the env it runs under proves it.
# WP12d: that observation works by writing $WORK/profseen OUTSIDE the pack
# scratch dir — exactly what the Landlock pack sandbox denies. This leg's
# subject is env propagation, not sandboxing, so it opts out via
# INVFS_PACK_SANDBOX=0 (tools/test-sandbox.sh owns the sandbox coverage).
mkdir -p "$WORK/packs-prof/prop.codecpack"
cat > "$WORK/packs-prof/prop.codecpack/enc" <<EOF
#!/bin/sh
echo "\${INVFS_PROFILE:-unset}" > "$WORK/profseen"
cp "\$1" "\$2"
EOF
chmod +x "$WORK/packs-prof/prop.codecpack/enc"
cat > "$WORK/packs-prof/prop.codecpack/manifest" <<EOF
name = prop
algo = 46
caps = external
sniff.magic = 50524F50
encode = sh {pack}/enc {in} {out}
decode = sh {pack}/enc {in} {out}
EOF
python3 -c "open('$WORK/orig/prop.bin','wb').write(b'PROP' + bytes(100000))"
$B/invf-cp "$IMGP" "$WORK/orig/prop.bin" prop.bin >/dev/null
rm -f "$WORK/profseen"
INVFS_PACK_SANDBOX=0 INVFS_CODECPACKS=$WORK/packs-prof INVFS_PROFILE=dense $B/invf-sweep "$IMGP" \
    > "$WORK/sweepp4.log" 2>&1 || { cat "$WORK/sweepp4.log"; exit 1; }
[ "$(cat "$WORK/profseen" 2>/dev/null)" = "dense" ] \
    || { echo "FAIL: pack exec saw INVFS_PROFILE='$(cat "$WORK/profseen" 2>/dev/null)', want dense"; exit 1; }
echo "profiles: fast=6 / dense=19 logged, invalid -> balanced, default silent"
echo "profile env reaches pack execs (pack saw INVFS_PROFILE=dense)"

echo "== admission leg: INVFS_DEC_MEM_LIMIT=64K =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/multi.splt" multi.splt >/dev/null
# the ABI default working set (sum of member usizes + container size)
# exceeds 64K -> policy refusal before any extract; GENERIC_MEMLIMIT{40,1}
INVFS_DEC_MEM_LIMIT=64K $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "splt_test (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 64K decode-memory limit"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" multi.splt)
echo "  multi.splt (memlimit): $C"
[ "$C" = "cls=5 algo=40 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{SPLT=40,1}"; exit 1; }
$B/invf-cat "$IMGMEM" multi.splt "$WORK/out/multimem.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/multimem.splt" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "CONTAINERPACK E2E: PASS"
