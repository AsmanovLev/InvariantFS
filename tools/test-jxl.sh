#!/bin/bash
# test-jxl.sh — WP11 JPEG -> JXL lossless-recompression end-to-end (persistent).
#
#   mkfs -> 5 photo-ish JPEGs (gradient+noise, ~50KB..2MB, q85) + 1 tiny JPEG
#   (PIL) -> invf-cp -> invf-sweep (expect "JPEG -> JXL" lines) ->
#   invf-verify --deep -> invf-cat bit-exact vs originals (sha256) ->
#   class stamps CODEC{JXL} via an inline vol_get_class helper.
#   Negative: fresh image, INVFS_DEC_MEM_LIMIT=256K -> the photo JPEGs (raw
#   estimate w*h*3 = 518KB..7MB > 256K) fall back to generic ZSTD with a
#   GENERIC_MEMLIMIT{JXL} stamp and still read back bit-exactly; the tiny
#   one (128x128 = 48KB raw) is admitted even there and serves as control.
#   Retry (WP12(b)): re-sweep the negative image WITHOUT the limit -> the
#   five MEMLIMIT-stamped photos upgrade to JXL (class CODEC{JXL,1}) and
#   stay bit-exact; tiny.jpg is already JXL and must not be re-processed.
#
# Run from the repo root after `make`:  bash tools/test-jxl.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp11jxl
IMG=wp11jxl.img
IMGNEG=wp11jxl-neg.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGNEG"

echo "== tools =="
command -v cjxl >/dev/null || { echo "FAIL: cjxl not installed"; exit 1; }
command -v djxl >/dev/null || { echo "FAIL: djxl not installed"; exit 1; }

echo "== generate JPEGs =="
python3 - <<'PY'
import os
import numpy as np
from PIL import Image

rng = np.random.default_rng(11)
d = "/dev/shm/wp11jxl/orig"

def photo(name, w, h, sigma, quality=85):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    im = Image.fromarray(np.dstack([r, g, b]).astype(np.uint8))
    im.save(os.path.join(d, name), "JPEG", quality=quality)

# ~50KB..2MB at q85, rising size and noise
photo("p1.jpg",  480,  360, 14)
photo("p2.jpg",  640,  480, 18)
photo("p3.jpg", 1024,  768, 16)
photo("p4.jpg", 1400, 1050, 14)
photo("p5.jpg", 1920, 1280, 12)
# tiny but still a valid baseline JPEG
photo("tiny.jpg", 128, 128, 20)
for f in sorted(os.listdir(d)):
    print(" ", f, os.path.getsize(os.path.join(d, f)), "bytes")
PY

FILES=$(cd "$WORK/orig" && ls)
echo "== mkfs =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null

echo "== import =="
for f in $FILES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

# class-stamp reader (no stock tool prints invfs.class; built from the repo
# objects like test-textzone.sh's tzrm helper)
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

echo "== sweep =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
JXL_LINES=$(grep -c "JPEG -> JXL (lossless)" "$WORK/sweep1.log" || true)
echo "JPEG->JXL lines: $JXL_LINES"
[ "$JXL_LINES" -eq 6 ] || { echo "FAIL: expected 6 JPEG->JXL lines"; cat "$WORK/sweep1.log"; exit 1; }

echo "== class stamps =="
for f in $FILES; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: $f: want CODEC{JXL,1}"; exit 1; }
done

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== cat bit-exact =="
ok=1
for f in $FILES; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$FILES" | wc -w) files bit-exact"

echo "== negative: INVFS_DEC_MEM_LIMIT=256K =="
$B/invf-mkfs "$IMGNEG" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGNEG" "$WORK/orig/$f" "$f" >/dev/null
done
INVFS_DEC_MEM_LIMIT=256K $B/invf-sweep "$IMGNEG" > "$WORK/sweep-neg.log" 2>&1 \
    || { cat "$WORK/sweep-neg.log"; exit 1; }
# the control (tiny.jpg) transcodes even here; the five photos must not
if grep "JPEG -> JXL" "$WORK/sweep-neg.log" | grep -q "p[0-9]\.jpg"; then
    echo "FAIL: a photo JPEG transcoded under a 256K decode-memory limit"
    grep "JPEG -> JXL" "$WORK/sweep-neg.log"; exit 1
fi
# raw estimate w*h*3 > 256K for the five photos; the tiny one (128x128 = 48KB)
# is admitted even here -- it is the control that admission, not blunt
# refusal, is what gates the codec
for f in p1.jpg p2.jpg p3.jpg p4.jpg p5.jpg; do
    C=$("$WORK/classof" "$IMGNEG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=5 algo=4 gen=1" ] || { echo "FAIL: $f: want GENERIC_MEMLIMIT{JXL,1}"; exit 1; }
done
CT=$("$WORK/classof" "$IMGNEG" tiny.jpg)
echo "  tiny.jpg: $CT (control: admitted at 256K)"
[ "$CT" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: tiny.jpg should still transcode"; exit 1; }
# generic fallback is still bit-exact
ok=1
for f in $FILES; do
    $B/invf-cat "$IMGNEG" "$f" "$WORK/out/$f.neg" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.neg" || { echo "MISMATCH neg $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
$B/invf-verify "$IMGNEG" --deep | tail -1

echo "== upgrade retry: re-sweep IMGNEG without the limit (WP12(b)) =="
# the five photos carry GENERIC_MEMLIMIT{JXL,1} over generic ZSTD storage;
# with the limit gone the class predicate re-arms and the retry must run
# the full cjxl + djxl-guard flow even though the zone is no longer RAW
$B/invf-sweep "$IMGNEG" > "$WORK/sweep-upg.log" 2>&1 \
    || { cat "$WORK/sweep-upg.log"; exit 1; }
UPG_LINES=$(grep -c "JPEG -> JXL (lossless)" "$WORK/sweep-upg.log" || true)
echo "JPEG->JXL upgrade lines: $UPG_LINES"
[ "$UPG_LINES" -eq 5 ] || { echo "FAIL: expected 5 upgrades, tiny.jpg must NOT re-process"; \
    grep "JPEG" "$WORK/sweep-upg.log"; exit 1; }
for f in p1.jpg p2.jpg p3.jpg p4.jpg p5.jpg; do
    C=$("$WORK/classof" "$IMGNEG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: $f: want CODEC{JXL,1} after upgrade"; exit 1; }
done
CT=$("$WORK/classof" "$IMGNEG" tiny.jpg)
echo "  tiny.jpg: $CT (already JXL: untouched)"
[ "$CT" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: tiny.jpg changed"; exit 1; }
# bit-exact after the upgrade
ok=1
for f in $FILES; do
    $B/invf-cat "$IMGNEG" "$f" "$WORK/out/$f.upg" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.upg" || { echo "MISMATCH upg $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$FILES" | wc -w) files bit-exact after upgrade"
# idempotence: another sweep transcodes nothing
$B/invf-sweep "$IMGNEG" > "$WORK/sweep-upg2.log" 2>&1 \
    || { cat "$WORK/sweep-upg2.log"; exit 1; }
if grep -q "JPEG -> JXL" "$WORK/sweep-upg2.log"; then
    echo "FAIL: third sweep re-transcoded"; grep "JPEG" "$WORK/sweep-upg2.log"; exit 1
fi
$B/invf-verify "$IMGNEG" --deep | tail -1

echo "JXL E2E: PASS"
