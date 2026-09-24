#!/bin/bash
# test-exercarve.sh — WP14b M2: exe-as-container carving end-to-end
# (persistent regression).
#
#   Synthetic exes (valid ELF64 head, e_machine=62 + code filler + media):
#   (a) game.exe  = ELF + 8KB code + real JPEG (PIL, ~100KB) + real PNG
#       (~93KB) + tail -> sweep carves it: "exe media -> JXL (2 parts)",
#       main = CONTAINER{EXER}, exr0 = CODEC{JXL}, exr1 = GENERIC{ZSTD}
#       (PNG is carved but stored ZSTD -- djxl re-encodes PNGs, so no
#       transcode can be bit-exact while PNGR is Windows-only, WP12(c));
#   (b) fake.exe  = ELF + structurally valid but corrupt fake JPEG
#       (marker walk accepts it, cjxl refuses) -> member skipped, nothing
#       carvable -> binary batch path (BATCHED_BIN) or generic;
#   (c) tiny.exe  = ELF + 2KB JPEG (below the 16 KiB member minimum)
#       -> never carved -> batch/generic.
#   mkfs -> cp -> sweep -> verify --deep -> invf-cat sha256 bit-exact ->
#   class stamps -> re-sweep idempotent -> delete the exe (vol_unlink:
#   the exrN siblings die with it) -> delete the batched ones -> sweep GC
#   reclaims dead batches -> fsck clean -> survivors bit-exact.
#   Negative leg: fresh image, INVFS_DEC_MEM_LIMIT=128K -> the carve's
#   decode working set exceeds the limit -> GENERIC_MEMLIMIT{EXER}, no
#   carve, still bit-exact; re-sweep without the limit upgrades (WP10 §2).
#
# Run from the repo root after `make`:  bash tools/test-exercarve.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
WORK=/dev/shm/wp14exr
IMG=wp14exr.img
IMGNEG=wp14exr-neg.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGNEG"

echo "== tools =="
command -v cjxl >/dev/null || { echo "FAIL: cjxl not installed"; exit 1; }
command -v djxl >/dev/null || { echo "FAIL: djxl not installed"; exit 1; }

echo "== mkfs =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null

echo "== generate fixtures =="
python3 - <<'PY'
import os
import numpy as np
from PIL import Image

rng = np.random.default_rng(7)
d = "/dev/shm/wp14exr/orig"

def elf_head():
    # minimal but valid ELF64 little-endian header, e_machine=62 (x86-64)
    h = bytearray(64)
    h[0:4] = b"\x7fELF"
    h[4] = 2; h[5] = 1                       # 64-bit, LE
    h[6] = 1                                 # EV_CURRENT
    h[16:18] = (2).to_bytes(2, "little")     # ET_EXEC
    h[18:20] = (62).to_bytes(2, "little")    # EM_X86_64
    h[20:24] = (1).to_bytes(4, "little")     # version
    return bytes(h)

def photo_jpg(path, w, h, sigma, quality=85):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(
        path, "JPEG", quality=quality)

def photo_png(path, w, h, sigma):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(path, "PNG")

# deterministic code filler: no FF D8 FF / PNG signature in it
def code(n, seed):
    r = np.random.default_rng(seed)
    b = bytearray(r.integers(0, 256, n, dtype=np.uint8))
    for i in range(len(b) - 8):
        if b[i] == 0xFF and b[i+1] == 0xD8 and b[i+2] == 0xFF:
            b[i+1] = 0x11
        if b[i:i+8] == b"\x89PNG\r\n\x1a\n":
            b[i] = 0x88
    return bytes(b)

# --- (a) game.exe: ELF + 8KB code + real JPEG + real PNG + tail ---
photo_jpg(os.path.join(d, "m.jpg"), 640, 480, 14)
photo_png(os.path.join(d, "m.png"), 256, 192, 8)
jpg = open(os.path.join(d, "m.jpg"), "rb").read()
png = open(os.path.join(d, "m.png"), "rb").read()
assert len(jpg) >= 64 * 1024 and len(png) >= 16 * 1024
exe = elf_head() + code(8192, 1) + jpg + png + code(4096, 2)
open(os.path.join(d, "game.exe"), "wb").write(exe)
print("game.exe: %d bytes (jpeg %d @%d, png %d @%d)"
      % (len(exe), len(jpg), 64 + 8192, len(png), 64 + 8192 + len(jpg)))

# --- (b) fake.exe: corrupt pseudo-JPEG (marker walk ok, cjxl refuses) ---
b = bytearray()
b += b"\xff\xd8"                                    # SOI
b += b"\xff\xe0" + (16).to_bytes(2, "big") + \
     b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"  # APP0
b += b"\xff\xdb" + (67).to_bytes(2, "big") + b"\x00" + bytes([8] * 64)
b += b"\xff\xc0" + (17).to_bytes(2, "big") + b"\x08" + \
     (16).to_bytes(2, "big") + (16).to_bytes(2, "big") + \
     b"\x03\x01\x22\x00\x02\x11\x00\x03\x11\x00"      # SOF0 (saw_sof)
b += b"\xff\xc4" + (20).to_bytes(2, "big") + b"\x00" + bytes(17)
b += b"\xff\xda" + (12).to_bytes(2, "big") + \
     b"\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00"      # SOS
b += b"\x11" * 68000                                 # garbage entropy
b += b"\xff\xd9"                                     # EOI
exe = elf_head() + code(8192, 3) + bytes(b) + code(4096, 4)
open(os.path.join(d, "fake.exe"), "wb").write(exe)
print("fake.exe: %d bytes (fake-jpeg %d)" % (len(exe), len(b)))

# --- (c) tiny.exe: 2KB JPEG, below the 16 KiB member minimum ---
photo_jpg(os.path.join(d, "t.jpg"), 128, 128, 20)
t = open(os.path.join(d, "t.jpg"), "rb").read()
assert 1024 < len(t) < 16 * 1024, len(t)
exe = elf_head() + code(8192, 5) + t + code(4096, 6)
open(os.path.join(d, "tiny.exe"), "wb").write(exe)
print("tiny.exe: %d bytes (jpeg %d)" % (len(exe), len(t)))

os.unlink(os.path.join(d, "m.jpg"))
os.unlink(os.path.join(d, "m.png"))
os.unlink(os.path.join(d, "t.jpg"))
PY

# class-stamp reader (no stock tool prints invfs.class; the conbatch pattern)
cat > "$WORK/classof.c" <<'C'
/* usage: classof <image> <name> -> "cls=<n> algo=<n> gen=<n>" or "none" */
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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== import =="
for f in game.exe fake.exe tiny.exe; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
grep -E "exe media|batch|sweep done" "$WORK/sweep1.log" || true
EXR_LINES=$(grep -c "exe media -> JXL" "$WORK/sweep1.log" || true)
[ "$EXR_LINES" -eq 1 ] || { echo "FAIL: expected exactly 1 carve line"; exit 1; }
grep -q "game.exe: exe media -> JXL (2 parts)" "$WORK/sweep1.log" \
    || { echo "FAIL: game.exe not carved as 2 parts"; cat "$WORK/sweep1.log"; exit 1; }
if grep -q "fake.exe: exe media\|tiny.exe: exe media" "$WORK/sweep1.log"; then
    echo "FAIL: fake/tiny must not carve"; exit 1
fi

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== bit-exact =="
ok=1
for f in game.exe fake.exe tiny.exe; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    [ "$a" = "$b" ] || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all 3 files bit-exact"

echo "== ranged reads (the mount path: whole-file divert + ARC) =="
cat > "$WORK/rread.c" <<'C'
/* usage: rread <image> <name> <off> <len> <file>  -> write [off,+len) to <file> */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"
int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id, off;
    long len;
    uint8_t *buf;
    int got;
    FILE *f;

    if (argc < 6) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    off = strtoull(argv[3], NULL, 10);
    len = strtol(argv[4], NULL, 10);
    buf = malloc((size_t)len);
    got = vol_read_range(v, id, off, (size_t)len, buf);
    f = got > 0 ? fopen(argv[5], "wb") : NULL;
    if (f && fwrite(buf, 1, (size_t)got, f) == (size_t)got && fclose(f) == 0) {
        vol_close(v);
        return 0;
    }
    return 1;
}
C
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/rread" "$WORK/rread.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread
# windows: glue head | inside the JPEG member | across the JPEG/PNG gap |
# inside the PNG member | the tail. game.exe = 64B elf + 8K code + jpg@8256
# (107056B) + png@115312 (93574B) + 4K tail
for spec in "0 4096" "9000 50000" "110000 20000" "150000 40000" "200000 12982"; do
    set -- $spec
    "$WORK/rread" "$IMG" game.exe "$1" "$2" "$WORK/out/win.bin"
    dd if="$WORK/orig/game.exe" of="$WORK/out/win.ref" bs=1 skip="$1" count="$2" 2>/dev/null
    cmp -s "$WORK/out/win.bin" "$WORK/out/win.ref" \
        || { echo "FAIL: ranged read off=$1 len=$2"; exit 1; }
done
echo "5 ranged windows bit-exact (glue, JPEG, gap, PNG, tail)"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" game.exe)
echo "  game.exe: $C"
[ "$C" = "cls=3 algo=15 gen=1" ] || { echo "FAIL: game.exe: want CONTAINER{EXER}"; exit 1; }
C=$("$WORK/classof" "$IMG" "game.exe!exr0")
echo "  game.exe!exr0: $C"
[ "$C" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: exr0: want CODEC{JXL,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "game.exe!exr1")
echo "  game.exe!exr1: $C"
[ "$C" = "cls=4 algo=1 gen=0" ] || { echo "FAIL: exr1: want GENERIC{ZSTD} (PNG is carved, not transcoded)"; exit 1; }
for f in fake.exe tiny.exe; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    case "$C" in
    cls=8\ algo=1*\ gen=1|cls=8\ algo=14\ gen=1) ;;   # BATCHED_BIN (x64 -> BCJ)
    cls=4\ algo=1\ gen=0) ;;                          # GENERIC (batch declined)
    *) echo "FAIL: $f: want BATCHED_BIN or GENERIC, got $C"; exit 1;;
    esac
done
# a carved part is a real sibling inode and reads back as the bare member
$B/invf-ls "$IMG" | grep "game.exe!"
NEXR=$($B/invf-ls "$IMG" | grep -c "game.exe!exr" || true)
[ "$NEXR" -eq 2 ] || { echo "FAIL: expected 2 exr siblings, got $NEXR"; exit 1; }

echo "== re-sweep is idempotent =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1
if grep -q "exe media" "$WORK/sweep2.log"; then
    echo "FAIL: re-sweep re-carved"; cat "$WORK/sweep2.log"; exit 1
fi
echo "no re-carves"

echo "== ratio =="
grep "blocks:" "$WORK/verify1.log"
$B/invf-stats "$IMG" | grep -E "logical bytes|est. ratio"

echo "== delete the carved exe (vol_unlink: siblings must die) =="
cat > "$WORK/exrm.c" <<'C'
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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/exrm" "$WORK/exrm.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/exrm" "$IMG" game.exe
if $B/invf-ls "$IMG" | grep -q "game.exe"; then
    echo "FAIL: game.exe (or a sibling) survived the delete"; exit 1
fi
echo "carved exe + both parts deleted"

echo "== delete the batched exes; GC reclaims the dead batches =="
"$WORK/exrm" "$IMG" fake.exe tiny.exe
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1
GC_LINE=$(grep "text gc:" "$WORK/sweep3.log" || true)
echo "${GC_LINE:-no gc line}"
echo "$GC_LINE" | grep -q "text gc: [1-9]" || { echo "FAIL: GC reclaimed nothing"; exit 1; }

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== abort path: INVFS_FAIL_CHILD=2 kills the second part write =="
IMGAB=wp14exr-ab.img
rm -f "$IMGAB"
$B/invf-mkfs "$IMGAB" 0.2 >/dev/null
$B/invf-cp "$IMGAB" "$WORK/orig/game.exe" game.exe >/dev/null
INVFS_FAIL_CHILD=2 $B/invf-sweep "$IMGAB" > "$WORK/sweep-ab.log" 2>&1 \
    || { cat "$WORK/sweep-ab.log"; exit 1; }
grep -q "INVFS_FAIL_CHILD" "$WORK/sweep-ab.log" || { echo "FAIL: fault hook did not fire"; exit 1; }
if grep -q "exe media" "$WORK/sweep-ab.log"; then
    echo "FAIL: carve completed despite the failed part"; exit 1
fi
# the purge must leave no orphan siblings behind
if $B/invf-ls "$IMGAB" | grep -q "game.exe!"; then
    echo "FAIL: aborted carve left siblings"; $B/invf-ls "$IMGAB"; exit 1
fi
$B/invf-cat "$IMGAB" game.exe "$WORK/out/game.exe.ab" >/dev/null
cmp -s "$WORK/orig/game.exe" "$WORK/out/game.exe.ab" \
    || { echo "MISMATCH after aborted carve"; exit 1; }
$B/invf-fsck "$IMGAB" | tee "$WORK/fsck-ab.log"
grep -q "orphans:      0" "$WORK/fsck-ab.log" || { echo "FAIL: orphans after abort"; exit 1; }
grep -q "^OK" "$WORK/fsck-ab.log" || { echo "FAIL: fsck not OK after abort"; exit 1; }
echo "aborted carve: original intact, no orphans"

echo "== negative admission: INVFS_DEC_MEM_LIMIT=128K =="
$B/invf-mkfs "$IMGNEG" 0.2 >/dev/null
for f in game.exe fake.exe tiny.exe; do
    $B/invf-cp "$IMGNEG" "$WORK/orig/$f" "$f" >/dev/null
done
INVFS_DEC_MEM_LIMIT=128K $B/invf-sweep "$IMGNEG" > "$WORK/sweep-neg.log" 2>&1 \
    || { cat "$WORK/sweep-neg.log"; exit 1; }
if grep -q "exe media" "$WORK/sweep-neg.log"; then
    echo "FAIL: carve fired under a 128K decode-memory limit"; exit 1
fi
C=$("$WORK/classof" "$IMGNEG" game.exe)
echo "  game.exe (neg): $C"
[ "$C" = "cls=5 algo=15 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{EXER,1}"; exit 1; }
# the generic fallback is still bit-exact
ok=1
for f in game.exe fake.exe tiny.exe; do
    $B/invf-cat "$IMGNEG" "$f" "$WORK/out/$f.neg" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.neg" || { echo "MISMATCH neg $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
$B/invf-verify "$IMGNEG" --deep | tail -1

echo "== upgrade retry: re-sweep without the limit =="
$B/invf-sweep "$IMGNEG" > "$WORK/sweep-upg.log" 2>&1 || { cat "$WORK/sweep-upg.log"; exit 1; }
grep -q "game.exe: exe media -> JXL (2 parts)" "$WORK/sweep-upg.log" \
    || { echo "FAIL: MEMLIMIT-stamped exe did not carve once admitted"; cat "$WORK/sweep-upg.log"; exit 1; }
C=$("$WORK/classof" "$IMGNEG" game.exe)
echo "  game.exe (upg): $C"
[ "$C" = "cls=3 algo=15 gen=1" ] || { echo "FAIL: want CONTAINER{EXER} after upgrade"; exit 1; }
$B/invf-cat "$IMGNEG" game.exe "$WORK/out/game.exe.upg" >/dev/null
cmp -s "$WORK/orig/game.exe" "$WORK/out/game.exe.upg" || { echo "MISMATCH upg game.exe"; exit 1; }
$B/invf-verify "$IMGNEG" --deep | tail -1

echo "EXERCARVE E2E: PASS"
