#!/bin/bash
# test-containerpack.sh — WP16a containerpack ABI end-to-end (persistent
# regression).
#
#   fixtures (inline python, stdlib only): SPLT containers for the
#   tools/codecpacks/splt_test pack ("SPLT" + u32 n + n u64 lengths +
#   concatenated payloads):
#     multi.splt — 4 members: text (PPMd-batchable), EMPTY (0 bytes), a
#       real x86-64 ELF (ZSTD/BCJ-batchable), random (stays generic).
#     nest.splt  — member 0 is itself a valid SPLT (nested container:
#       decomposed by a LATER sweep, read back through two rebuilds).
#     neg.splt   — corrupt table (lengths under-count the payload):
#       enumerate/extract/strip succeed, the FS-side rebuild guard fires
#       and the file falls through to generic UNSTAMPED by the pack.
#   mkfs -> cp -> INVFS_CODECPACKS=$REPO/tools/codecpacks invf-sweep
#   (expect "splt_test (codecpack)" lines + "!*: N parts -> PPMd/ZSTD
#   batch") -> class stamps CONTAINER{40,1} / TEXT / BATCHED_BIN ->
#   verify --deep -> invf-cat sha256 bit-exact (containers AND direct
#   !mbr member reads) -> pack-absent read fails LOUDLY -> sweep 2
#   decomposes the nested member (idempotent for the rest) -> sweep 3
#   idle -> vol_unlink cascade kills every "!"-sibling incl. the nested
#   grandchildren -> fsck clean -> survivors bit-exact.
#   Admission leg: fresh image, INVFS_DEC_MEM_LIMIT=64K -> GENERIC_
#   MEMLIMIT{40,1}, no decomposition, bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-containerpack.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
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

inner = splt([b"inner member alpha\n" * 500, b"inner member beta\n" * 700])
nest = splt([inner, b"outer tail\n" * 300])
open(os.path.join(d, "nest.splt"), "wb").write(nest)

# negative: the table announces 5 payload bytes but 10 are present --
# enumerate/extract/strip all succeed, rebuild produces 21 of the 26
# original bytes, the FS-side memcmp guard refuses the decomposition.
open(os.path.join(d, "neg.splt"), "wb").write(
    b"SPLT" + struct.pack("<I", 1) + struct.pack("<Q", 5) + b"0123456789")

print("  multi.splt", len(multi), "(4 members: text %d, empty, elf %d, rand 32768)"
      % (len(text), len(elf)))
print("  nest.splt ", len(nest), "(member 0 = inner SPLT of %d bytes)" % len(inner))
print("  neg.splt  ", 26, "(corrupt table)")
PY

# class-stamp reader (no stock tool prints invfs.class; built from the repo
# objects like test-rawimg.sh's helper)
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
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $REPO/build/obj/{volume,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

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
grep -q "splt_test: neg.splt: rebuild guard refused" "$WORK/sweep1.log" \
    || { echo "FAIL: neg.splt guard did not fire"; cat "$WORK/sweep1.log"; exit 1; }
# members batch in the SAME run their container decomposed (WP14b pattern)
grep -q "multi.splt!\*: 1 parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text member not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "multi.splt!\*: 1 parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|guard refused|parts -> " "$WORK/sweep1.log"

echo "== sibling set =="
N=$($B/invf-ls "$IMG" | grep -c "multi\.splt!" || true)
echo "multi.splt!* names: $N"
[ "$N" -eq 5 ] || { echo "FAIL: want 5 (4 members + member table)"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep "multi\.splt!mbr0001-chunk1" | grep -q "0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep -q "multi\.splt!mbrt" \
    || { echo "FAIL: member table sibling missing"; exit 1; }
if $B/invf-ls "$IMG" | grep -q "neg\.splt!"; then
    echo "FAIL: guard refusal left siblings behind"; exit 1
fi
echo "4 members + table present; empty member is 0 bytes; neg.splt clean"

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

echo "== verify --deep =="
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

echo "== pack-absent read fails LOUDLY =="
# a fresh process with an empty pack dir: algo 40 has no registry entry,
# the read must fail (house semantics: -1 -> EIO at the FUSE boundary,
# "read failed" + exit 1 in invf-cat) -- never serve the recipe as data
if INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" multi.splt \
        "$WORK/out/absent" 2> "$WORK/absent.err"; then
    echo "FAIL: pack-absent read SUCCEEDED (silent 1:1 break)"; exit 1
fi
grep -q "codecpack" "$WORK/absent.err" \
    || { echo "FAIL: no loud pack-absent diagnostic"; cat "$WORK/absent.err"; exit 1; }
cat "$WORK/absent.err"

echo "== sweep #2: nested member decomposes, nothing else re-fires =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
grep -q "nest.splt!mbr0000-chunk0: splt_test (codecpack)" "$WORK/sweep2.log" \
    || { echo "FAIL: nested SPLT member not decomposed"; cat "$WORK/sweep2.log"; exit 1; }
if grep -q "multi.splt: splt_test\|nest.splt: splt_test\|guard refused" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/sweep2.log"; exit 1
fi
grep -E "codecpack" "$WORK/sweep2.log"

echo "== nested read-back (rebuild through a rebuild) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify2.log"
grep -q " 0 corrupt," "$WORK/verify2.log" || { echo "FAIL: corrupt after nesting"; exit 1; }
$B/invf-cat "$IMG" nest.splt "$WORK/out/nest2.splt" >/dev/null
cmp -s "$WORK/orig/nest.splt" "$WORK/out/nest2.splt" \
    || { echo "FAIL: nested container not bit-exact"; exit 1; }
$B/invf-cat "$IMG" multi.splt "$WORK/out/multi2.splt" >/dev/null
cmp -s "$WORK/orig/multi.splt" "$WORK/out/multi2.splt" \
    || { echo "FAIL: multi.splt drifted"; exit 1; }
echo "nested rebuild bit-exact"

echo "== sweep #3: idle =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1 || { cat "$WORK/sweep3.log"; exit 1; }
if grep -q "codecpack\|guard refused" "$WORK/sweep3.log"; then
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
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $REPO/build/obj/{volume,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/cbrm" "$IMG" multi.splt nest.splt
if $B/invf-ls "$IMG" | grep -q "splt!"; then
    echo "FAIL: !mbr siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + nested grandchildren deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivor bit-exact =="
$B/invf-cat "$IMG" neg.splt "$WORK/out/neg2.splt" >/dev/null
cmp -s "$WORK/orig/neg.splt" "$WORK/out/neg2.splt" \
    || { echo "FAIL: neg.splt drifted after deletes"; exit 1; }

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
