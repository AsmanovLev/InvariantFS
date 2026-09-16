#!/bin/bash
# test-jxl.sh — JPEG -> JXL lossless-recompression end-to-end (persistent),
# WP16e edition: the lane is PACK-OWNED now (tools/codecpacks/jxl.codecpack,
# algo 4 overrides the builtin EXTERNAL placeholder). The builtin volume.c
# cjxl branch is gone; the sweep flows through the WP13 generic pack path
# (vol_pack_sweep) and the sweep log line is "<name>: jxl (codecpack)".
#
#   helper build (jxlest SOF estimator, C11/libc, the rawdisk convention:
#   compiled at test time into $WORK/bin, put on PATH) -> mkfs -> 5
#   photo-ish JPEGs (gradient+noise, ~50KB..2MB, q85) + 1 tiny JPEG (PIL)
#   -> invf-cp -> invf-sweep (expect "jxl (codecpack)" lines) ->
#   class stamps CODEC{4,1} -> invf-verify --deep -> invf-cat bit-exact
#   (sha256).
#   Negative: fresh image, INVFS_DEC_MEM_LIMIT=256K -> the photo JPEGs (raw
#   estimate w*h*3 = 518KB..7MB > 256K, computed by the pack's estimate
#   command) fall back to generic ZSTD with a GENERIC_MEMLIMIT{4,1} stamp
#   and still read back bit-exactly; the tiny one (128x128 = 48KB raw) is
#   admitted even there and serves as control.
#   Retry (WP12(b)): re-sweep the negative image WITHOUT the limit -> the
#   five MEMLIMIT-stamped photos upgrade to JXL through vol_jxl_retry ->
#   vol_pack_sweep (class CODEC{4,1}) and stay bit-exact; tiny.jpg is
#   already JXL and must not be re-processed.
#   Pack-proof: fresh image, INVFS_CODECPACKS without the jxl pack ->
#   JPEGs stay RAW and UNSTAMPED even though cjxl/djxl sit on PATH (the
#   placeholder's PACKONLY defer); restore the pack -> they transcode.
#   Guard: fake cjxl via INVFS_TOOLS (writes garbage, exits 0) -> the
#   decode-back memcmp guard refuses -> GENERIC_GUARD{4,1}, generic
#   storage, bit-exact; a re-sweep does NOT refire (same generation).
#   Tool-absent: pack loaded but cjxl unresolvable (stripped PATH) ->
#   RAW, unstamped; restore PATH -> next sweep transcodes.
#   WP12(d) RLIMIT_AS: a fixture pack (algo 41, dec_mem=8MiB -> child cap
#   max(2*dec_mem,256MB) = 256MB) whose encode helper memhog allocates
#   4 GiB -> the child dies on the ceiling -> sweep treats it as a tool
#   failure (GENERIC_GUARD{41,1}), the file lands generic, FS clean.
#
# Run from the repo root after `make`:  bash tools/test-jxl.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
PACK=$REPO/tools/codecpacks/jxl.codecpack
WORK=/dev/shm/wp11jxl
trap 'rm -rf "$WORK" /dev/shm/wp11jxl*.img' EXIT
IMG=wp11jxl.img
IMGNEG=wp11jxl-neg.img
IMGPA=wp11jxl-pa.img
IMGG=wp11jxl-guard.img
IMGT=wp11jxl-tools.img
IMGHOG=wp11jxl-hog.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" \
    "$WORK/binonly" "$WORK/packs" "$WORK/nopacks" "$WORK/faketools" \
    "$WORK/hogpacks/hog.codecpack"
ln -s "$PACK" "$WORK/packs/jxl.codecpack"          # isolated pack dir (the rawdisk pattern)
cd /dev/shm
rm -f "$IMG" "$IMGNEG" "$IMGPA" "$IMGG" "$IMGT" "$IMGHOG"

echo "== tools + helper build (cc -std=c11 -O2 -Wall -Wextra) =="
command -v cjxl >/dev/null || { echo "FAIL: cjxl not installed"; exit 1; }
command -v djxl >/dev/null || { echo "FAIL: djxl not installed"; exit 1; }
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
cc -std=c11 -O2 -Wall -Wextra -o "$WORK/bin/jxlest" "$PACK/jxlest.c"
echo "jxlest compiled: $($WORK/bin/jxlest estimate /dev/null 2>/dev/null || echo x) bytes for /dev/null (0 = unknown geometry)"
cp "$WORK/bin/jxlest" "$WORK/binonly/jxlest"
export PATH="$WORK/bin:$PATH"                 # jxlest resolves by name
export INVFS_CODECPACKS="$WORK/packs"         # the sweep AND the reads

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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_meta_merge,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs,vol_tier}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep (the jxl codecpack owns the lane) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
JXL_LINES=$(grep -c ": jxl (codecpack)" "$WORK/sweep1.log" || true)
echo "jxl (codecpack) lines: $JXL_LINES"
[ "$JXL_LINES" -eq 6 ] || { echo "FAIL: expected 6 pack transcodes"; cat "$WORK/sweep1.log"; exit 1; }
if grep -q "JPEG -> JXL (lossless)" "$WORK/sweep1.log"; then
    echo "FAIL: the builtin JXL rc-7 line appeared -- the lane must be pack-owned"
    exit 1
fi

echo "== class stamps =="
for f in $FILES; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: $f: want CODEC{JXL,1}"; exit 1; }
done

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== cat bit-exact (reads route through the pack decode trampoline) =="
ok=1
for f in $FILES; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$FILES" | wc -w) files bit-exact"

echo "== pack-proof: INVFS_CODECPACKS without the jxl pack =="
# cjxl/djxl stay on PATH the whole time: if anything but the pack could
# still transcode JPEGs (the retired builtin branch), it would fire here.
$B/invf-mkfs "$IMGPA" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGPA" "$WORK/orig/$f" "$f" >/dev/null
done
INVFS_CODECPACKS="$WORK/nopacks" $B/invf-sweep "$IMGPA" > "$WORK/sweep-pa.log" 2>&1 \
    || { cat "$WORK/sweep-pa.log"; exit 1; }
if grep -q "codecpack\|JPEG -> JXL" "$WORK/sweep-pa.log"; then
    echo "FAIL: a JPEG transcoded with the pack hidden"; cat "$WORK/sweep-pa.log"; exit 1
fi
for f in $FILES; do
    C=$(INVFS_CODECPACKS="$WORK/nopacks" "$WORK/classof" "$IMGPA" "$f")
    echo "  $f: $C"
    [ "$C" = "none" ] || { echo "FAIL: $f: want unstamped RAW wait, got $C"; exit 1; }
    $B/invf-cat "$IMGPA" "$f" "$WORK/out/$f.pa" >/dev/null   # RAW: no pack needed
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.pa" || { echo "FAIL: $f unreadable RAW"; exit 1; }
done
echo "pack hidden -> all 6 wait RAW + unstamped (bit-exact)"
# restore the pack: the very next sweep transcodes them
$B/invf-sweep "$IMGPA" > "$WORK/sweep-pa2.log" 2>&1 || { cat "$WORK/sweep-pa2.log"; exit 1; }
PA_LINES=$(grep -c ": jxl (codecpack)" "$WORK/sweep-pa2.log" || true)
echo "after restore: $PA_LINES pack transcodes"
[ "$PA_LINES" -eq 6 ] || { echo "FAIL: pack restore did not pick the files up"; exit 1; }
C=$("$WORK/classof" "$IMGPA" p1.jpg)
[ "$C" = "cls=2 algo=4 gen=1" ] || { echo "FAIL: p1.jpg after restore: $C"; exit 1; }

echo "== negative: INVFS_DEC_MEM_LIMIT=256K (the pack's estimate command gates) =="
$B/invf-mkfs "$IMGNEG" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGNEG" "$WORK/orig/$f" "$f" >/dev/null
done
INVFS_DEC_MEM_LIMIT=256K $B/invf-sweep "$IMGNEG" > "$WORK/sweep-neg.log" 2>&1 \
    || { cat "$WORK/sweep-neg.log"; exit 1; }
# the control (tiny.jpg) transcodes even here; the five photos must not
if grep ": jxl (codecpack)" "$WORK/sweep-neg.log" | grep -q "p[0-9]\.jpg"; then
    echo "FAIL: a photo JPEG transcoded under a 256K decode-memory limit"
    grep ": jxl (codecpack)" "$WORK/sweep-neg.log"; exit 1
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
# with the limit gone the class predicate re-arms and vol_jxl_retry runs
# the pack sweep even though the zone is no longer RAW
$B/invf-sweep "$IMGNEG" > "$WORK/sweep-upg.log" 2>&1 \
    || { cat "$WORK/sweep-upg.log"; exit 1; }
UPG_LINES=$(grep -c ": jxl (codecpack)" "$WORK/sweep-upg.log" || true)
echo "jxl (codecpack) upgrade lines: $UPG_LINES"
[ "$UPG_LINES" -eq 5 ] || { echo "FAIL: expected 5 upgrades, tiny.jpg must NOT re-process"; \
    grep "jxl" "$WORK/sweep-upg.log"; exit 1; }
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
if grep -q "codecpack\|JPEG -> JXL" "$WORK/sweep-upg2.log"; then
    echo "FAIL: third sweep re-transcoded"; grep -i "jxl" "$WORK/sweep-upg2.log"; exit 1
fi
$B/invf-verify "$IMGNEG" --deep | tail -1

echo "== guard: fake cjxl via INVFS_TOOLS -> GENERIC_GUARD{4,1} =="
# the pack probe passes (INVFS_TOOLS is a valid tool source), but the exec
# resolves the fake, which "succeeds" with garbage output -- the decode-back
# memcmp guard is what refuses the file
printf '#!/bin/sh\nprintf "this is not a jxl blob" > "$2"\n' > "$WORK/faketools/cjxl"
chmod +x "$WORK/faketools/cjxl"
$B/invf-mkfs "$IMGG" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGG" "$WORK/orig/$f" "$f" >/dev/null
done
INVFS_TOOLS="$WORK/faketools" $B/invf-sweep "$IMGG" > "$WORK/sweep-g.log" 2>&1 \
    || { cat "$WORK/sweep-g.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep-g.log"; then
    echo "FAIL: a JPEG transcoded with a garbage cjxl"; cat "$WORK/sweep-g.log"; exit 1
fi
for f in $FILES; do
    C=$("$WORK/classof" "$IMGG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=6 algo=4 gen=1" ] || { echo "FAIL: $f: want GENERIC_GUARD{JXL,1}"; exit 1; }
    $B/invf-cat "$IMGG" "$f" "$WORK/out/$f.g" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.g" || { echo "FAIL: $f not bit-exact after guard"; exit 1; }
done
echo "all $(echo "$FILES" | wc -w) guard-refused files bit-exact (generic storage)"
# same generation: a re-sweep must NOT refire the attempt (WP10 §2)
INVFS_TOOLS="$WORK/faketools" $B/invf-sweep "$IMGG" > "$WORK/sweep-g2.log" 2>&1 \
    || { cat "$WORK/sweep-g2.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep-g2.log"; then
    echo "FAIL: the guard retry refired at the same generation"; exit 1
fi
$B/invf-verify "$IMGG" --deep | tail -1

echo "== tool-absent: pack loaded, cjxl unresolvable -> RAW, unstamped =="
# $WORK/binonly holds jxlest but no cjxl/djxl; the sweep itself execs nothing
# else from PATH
$B/invf-mkfs "$IMGT" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGT" "$WORK/orig/$f" "$f" >/dev/null
done
PATH="$WORK/binonly" $B/invf-sweep "$IMGT" > "$WORK/sweep-t.log" 2>&1 \
    || { cat "$WORK/sweep-t.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep-t.log"; then
    echo "FAIL: a JPEG transcoded with cjxl absent"; cat "$WORK/sweep-t.log"; exit 1
fi
for f in $FILES; do
    C=$("$WORK/classof" "$IMGT" "$f")
    [ "$C" = "none" ] || { echo "FAIL: $f: want unstamped RAW wait, got $C"; exit 1; }
done
echo "tools absent -> all 6 wait RAW + unstamped"
# the first sweep after the tools return picks them up
$B/invf-sweep "$IMGT" > "$WORK/sweep-t2.log" 2>&1 || { cat "$WORK/sweep-t2.log"; exit 1; }
T_LINES=$(grep -c ": jxl (codecpack)" "$WORK/sweep-t2.log" || true)
echo "after tools return: $T_LINES pack transcodes"
[ "$T_LINES" -eq 6 ] || { echo "FAIL: tool return did not pick the files up"; exit 1; }
ok=1
for f in $FILES; do
    $B/invf-cat "$IMGT" "$f" "$WORK/out/$f.t" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.t" || { echo "MISMATCH tools $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
$B/invf-verify "$IMGT" --deep | tail -1

echo "== WP12(d) RLIMIT_AS: the hog pack's encode dies on the child ceiling =="
# dec_mem=8MiB -> RLIMIT_AS = max(2*dec_mem, 256MB) = 256MB on the exec'd
# child; memhog wants 4 GiB -> allocation fails -> nonzero exit -> the
# sweep treats it as a tool failure (GENERIC_GUARD{41,1}), FS unaffected.
cat > "$WORK/memhog.c" <<'C'
/* memhog.c -- WP12(d) fixture: allocate until RLIMIT_AS bites. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    size_t i;
    (void)argc; (void)argv;
    for (i = 0; i < 128; i++) {          /* 128 x 32 MiB = 4 GiB wanted */
        void *p = malloc(32u << 20);
        if (!p) { fprintf(stderr, "memhog: OOM at %zu x 32MiB\n", i); return 3; }
        memset(p, 1, 32u << 20);
    }
    return 0;                            /* never reached under the cap */
}
C
cc -std=c11 -O2 -Wall -Wextra -o "$WORK/bin/memhog" "$WORK/memhog.c"
cat > "$WORK/hogpacks/hog.codecpack/manifest" <<'M'
# WP12(d) fixture: dec_mem drives the exec child's RLIMIT_AS ceiling.
name = hog
algo = 41
pack_version = 1
caps = wholefile|external
dec_mem = 8388608
generation = 1
sniff.magic = FFD8FF
encode = memhog {in} {out}
decode = cp {in} {out}
M
$B/invf-mkfs "$IMGHOG" 0.2 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGHOG" "$WORK/orig/$f" "$f" >/dev/null
done
INVFS_CODECPACKS="$WORK/hogpacks" $B/invf-sweep "$IMGHOG" > "$WORK/sweep-hog.log" 2>&1 \
    || { cat "$WORK/sweep-hog.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep-hog.log"; then
    echo "FAIL: the hog pack transcoded something"; cat "$WORK/sweep-hog.log"; exit 1
fi
for f in $FILES; do
    C=$(INVFS_CODECPACKS="$WORK/hogpacks" "$WORK/classof" "$IMGHOG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=6 algo=41 gen=1" ] || { echo "FAIL: $f: want GENERIC_GUARD{41,1}"; exit 1; }
    INVFS_CODECPACKS="$WORK/hogpacks" $B/invf-cat "$IMGHOG" "$f" "$WORK/out/$f.h" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.h" || { echo "FAIL: $f not bit-exact (hog)"; exit 1; }
done
echo "all $(echo "$FILES" | wc -w) hog-killed files fell to generic, bit-exact"
$B/invf-verify "$IMGHOG" --deep | tail -1
$B/invf-fsck "$IMGHOG" | tee "$WORK/fsck-hog.log" | tail -1
grep -q "orphans:      0" "$WORK/fsck-hog.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck-hog.log" || { echo "FAIL: fsck not OK after the hog leg"; exit 1; }

echo "JXL E2E (codecpack lane): PASS"
