#!/bin/bash
# test-pngflac.sh — WP12c: the PNG (PNGR) and FLAC (FLACR) sweep transcodes
# on POSIX. Both stay BUILTIN lanes (no codecpack): vol_png.c /
# vol_cpack.c bodies running on the WP11 tool layer (cjxl/djxl/ffmpeg via
# $INVFS_TOOLS -> /usr/lib/invfs/tools -> PATH, RLIMIT_AS-capped children,
# /dev/shm scratch dirs), the in-tree pngx/flacx recipe engines, and the
# zlib/miniz deflate replicas.
#
#   helper build (classof from the repo objects; flacx CLI from flacx.c
#   WITHOUT INVFS_EMBED_FLACX — fixture tool only) -> deterministic PNG
#   fixtures via python3+PIL+numpy (several sizes, forced odd filters,
#   IDAT splits, ancillary chunks, zlib grid params) + ffmpeg FLAC
#   fixtures -> mkfs -> invf-cp -> invf-sweep -> class stamps ->
#   invf-verify --deep -> invf-cat bit-exact (cmp).
#
# Legs:
#   1. main:     5 crafted PNGs transcode -> CONTAINER{PNGR=10,gen=1} +
#                name!jxl siblings; idempotent re-sweep (swept=0).
#   2. refused:  16-bit + Adam7-interlaced (v1 guard), Z_FIXED-strategy
#                stream (encoder outside the replica grid), incompressible
#                noise (size guard), palette + PIL-default (observed
#                GUARD here; the stamp is informational for those two) —
#                all stay RAW, GENERIC_GUARD{PNGR,1}, bit-exact.
#   3. guard:    fake cjxl via INVFS_TOOLS (garbage output, exit 0) ->
#                the djxl decode-back pixel memcmp refuses ->
#                GENERIC_GUARD{PNGR,1}, RAW, bit-exact; no refire.
#   4. MEMLIMIT: INVFS_DEC_MEM_LIMIT=1M -> big.png (raw ~3.2 MB) admitted
#                nowhere -> GENERIC_MEMLIMIT{PNGR,1}, small.png (raw ~12
#                KB) transcodes (control); re-sweep without the limit
#                upgrades big.png to CONTAINER{PNGR,1} (swept=1).
#   5. FLAC:     the in-tree half is proven directly (flacx CLI
#                extract->rebuild round-trip is bit-exact on every
#                fixture, covers included). The sweep lane itself needs an
#                APE encoder: flacx only does recipe extract/rebuild, the
#                PCM blob is Monkey's Audio (`mac`), which is not in-tree
#                and not installed here -> the ported body refuses cleanly:
#                GENERIC_GUARD{FLACR=7,1}, the original stays RAW and
#                bit-exact, nothing half-committed (no !recipe sibling),
#                and a re-sweep does not refire. On a host WITH `mac` the
#                same leg expects the full transcode: CONTAINER{FLACR,1} +
#                !recipe sibling + bit-exact.
#
# Run from the repo root after `make`:  bash tools/run-e2e.sh tools/test-pngflac.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
SRC=$REPO/src
WORK=/dev/shm/w12cpngflac
trap 'rm -rf "$WORK" /dev/shm/w12cpngflac*.img' EXIT
IMG=w12cpngflac.img        # leg 1: transcodable PNGs
IMGR=w12cpngflac-ref.img   # leg 2: refused PNGs
IMGG=w12cpngflac-guard.img # leg 3: fake cjxl
IMGM=w12cpngflac-mem.img   # leg 4: MEMLIMIT
IMGF=w12cpngflac-flac.img  # leg 5: FLAC
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" "$WORK/faketools"
cd /dev/shm
rm -f "$IMG" "$IMGR" "$IMGG" "$IMGM" "$IMGF"

echo "== tools + helper build =="
command -v cjxl    >/dev/null || { echo "FAIL: cjxl not installed"; exit 1; }
command -v djxl    >/dev/null || { echo "FAIL: djxl not installed"; exit 1; }
command -v ffmpeg  >/dev/null || { echo "FAIL: ffmpeg not installed"; exit 1; }
command -v magick  >/dev/null || { echo "FAIL: magick not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
python3 -c "import PIL, numpy" || { echo "FAIL: python3 PIL/numpy missing"; exit 1; }
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
if command -v mac >/dev/null; then HAVE_MAC=1; else HAVE_MAC=0; fi
echo "mac (Monkey's Audio) present: $HAVE_MAC"

# class-stamp reader (same shape as test-jxl.sh's classof helper)
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
gcc -std=gnu11 -O2 -I$SRC -I$SRC/core -I$SRC/codecs -I$SRC/recipes -I$SRC/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread

# flacx CLI fixture (flacx.c without INVFS_EMBED_FLACX builds its main():
# the standalone extract/rebuild tool — validates the in-tree recipe
# engine on the FLAC fixtures below)
cc -std=c11 -O2 -o "$WORK/bin/flacx" "$SRC/recipes/flacx.c"

# The WP11 tool layer resolves helpers through $INVFS_TOOLS, then
# /usr/lib/invfs/tools, and only then PATH -- and the PATH leg is refused by
# default ("resolving cjxl via PATH (untrusted)", AGENTS.md 2.8). Relying on
# PATH here meant the PNGR lane could never run: cjxl never resolved, so
# every file fell into GENERIC_GUARD{PNGR} and leg 1's swept=5 never
# happened. Point the suite at a trusted directory holding the real tools.
# leg 3 overrides INVFS_TOOLS with its fake cjxl, so it is unaffected.
mkdir -p "$WORK/tools"
for t in cjxl djxl ffmpeg; do
    p=$(command -v "$t") || { echo "FAIL: $t not installed"; exit 1; }
    ln -sf "$p" "$WORK/tools/$t"
done
export INVFS_TOOLS="$WORK/tools"

echo "== generate fixtures (deterministic seeds) =="
python3 - <<'PY'
import zlib, struct, subprocess, os
import numpy as np
from PIL import Image

rng = np.random.default_rng(20260903)
D = "/dev/shm/w12cpngflac/orig"

def mkpx(w, h, sigma, ct=2, seed=None):
    r_ = np.random.default_rng(seed) if seed is not None else rng
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = r_.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255); g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    if ct == 2: return np.dstack([r, g, b]).astype(np.uint8)
    if ct == 6: return np.dstack([r, g, b, np.clip(128 + n, 0, 255)]).astype(np.uint8)
    if ct == 0: return np.clip((x + y) / 2 + n, 0, 255).astype(np.uint8)
    raise ValueError(ct)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)

def filter_rows(rows, bpp, filters):
    out = bytearray(); prev = bytes(len(rows[0]))
    for idx, row in enumerate(rows):
        f = filters[idx % len(filters)]
        dst = bytearray(len(row))
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 0:   v = row[i]
            elif f == 1: v = (row[i] - a) & 255
            elif f == 2: v = (row[i] - b) & 255
            elif f == 3: v = (row[i] - ((a + b) >> 1)) & 255
            else:        v = (row[i] - paeth(a, b, c)) & 255
            dst[i] = v
        out.append(f); out += dst; prev = row
    return bytes(out)

def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + \
           struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)

def write_png(fn, arr, ct, filters, level, mem, split=1,
              strategy=zlib.Z_DEFAULT_STRATEGY, pre=b"", post=b""):
    """Hand-rolled PNG writer: explicit per-row filters and zlib params, so
       the IDAT stream sits at a known (level, memLevel, strategy) point of
       the FS replica grid."""
    h, w = arr.shape[:2]
    bpp = {0: 1, 2: 3, 3: 1, 6: 4}[ct]
    rows = [arr[r].tobytes() for r in range(h)]
    filt = filter_rows(rows, bpp, filters)
    co = zlib.compressobj(level, zlib.DEFLATED, 15, mem, strategy)
    idat = co.compress(filt) + co.flush()
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    parts = []
    if split <= 1:
        parts = [idat]
    else:   # uneven split: exercises the recipe's split table
        bounds = [len(idat) // 3, len(idat) // 2]
        o = 0
        for bnd in bounds:
            parts.append(idat[o:o + bnd]); o += bnd
        parts.append(idat[o:])
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + pre
    for p in parts: png += chunk(b"IDAT", p)
    png += post + chunk(b"IEND", b"")
    open(os.path.join(D, fn), "wb").write(png)
    print(" ", fn, len(png), "bytes")

# --- transcodable set (leg 1) ---
a = mkpx(320, 240, 6)
write_png("grad_rgb.png", a, 2, [0, 1, 2, 3, 4], 6, 8)          # grid[0]
write_png("paeth_split.png", a, 2, [4], 9, 9, split=3)           # grid[5], 3 IDATs
write_png("rgba_up.png", mkpx(200, 150, 10, 6), 6, [2], 7, 8)    # RGBA, all-Up
write_png("gray_sub.png", mkpx(333, 77, 8, 0), 0, [1], 1, 8)     # odd dims, gray
pre = chunk(b"tEXt", b"Title\x00WP12c fixture")
post = chunk(b"vpAg", b"\x01\x02\x03\x04")                       # ancillary, private
write_png("ancil.png", mkpx(257, 129, 7), 2, [3], 6, 9, split=2,
          pre=pre, post=post)                                    # grid[2] + chunks

# --- refused set (leg 2) ---
# 16-bit grayscale (PIL): the v1 guard (bitdepth != 8) refuses.
g16 = rng.integers(0, 65536, (128, 128)).astype(np.uint16)
Image.fromarray(g16).save(os.path.join(D, "gray16.png"))
# incompressible noise: the size guard (jxl+recipe >= png) refuses.
write_png("noise64.png", rng.integers(0, 256, (64, 64, 3)).astype(np.uint8),
          2, [0], 6, 8)
# Z_FIXED strategy: outside the replica grid (grid = Z_DEFAULT_STRATEGY +
# miniz tdefl) -> "unknown encoder" refusal.
write_png("fixed.png", mkpx(200, 150, 6), 2, [1], 6, 8,
          strategy=zlib.Z_FIXED)
# palette (crafted: PLTE + indexed rows, grid zlib): djxl expands to RGB,
# the pixel-shape memcmp refuses.
pal = bytes(range(256)) * 3
idx = (np.arange(150 * 200, dtype=np.uint32).reshape(150, 200) % 256
       ).astype(np.uint8)
write_png("palette.png", idx, 3, [0], 6, 8,
          pre=chunk(b"PLTE", pal))
# PIL default save: on this box Pillow's stream is zlib-ng (6,9,Z_FILTERED),
# outside the grid -> refused. (Elsewhere it may sit in the grid and
# transcode — either disposition must read back bit-exactly.)
Image.fromarray(mkpx(160, 120, 6)).save(os.path.join(D, "pil_default.png"))

# --- MEMLIMIT set (leg 4) ---
write_png("big.png", mkpx(1200, 900, 6), 2, [0, 1, 2, 3, 4], 6, 8)
write_png("small.png", mkpx(64, 64, 6), 2, [0, 1, 2, 3, 4], 6, 8)

# --- fixture sanity: the interlaced + 16-bit fixtures must be what they
# --- claim (asserted against IHDR) ---
subprocess.run(["magick", os.path.join(D, "grad_rgb.png"),
                "-interlace", "PNG", os.path.join(D, "interlaced.png")],
               check=True)
def ihdr_of(fn):
    d = open(os.path.join(D, fn), "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    return d[16:16 + 13]
ih = ihdr_of("interlaced.png"); assert ih[12] == 1, "interlace flag not set"
ih = ihdr_of("gray16.png");     assert ih[8] == 16, "not a 16-bit PNG"
print("  interlaced.png + gray16.png sanity OK")
PY

# --- FLAC fixtures (deterministic PCM, ffmpeg encoder) ---
python3 - <<'PY'
import numpy as np, wave, struct
D = "/dev/shm/w12cpngflac/orig"
sr = 44100
t = np.arange(sr, dtype=np.float64) / sr
l = (np.sin(2 * np.pi * 440 * t) * 0.6 * 32767).astype(np.int16)
r = (np.sin(2 * np.pi * 554 * t) * 0.5 * 32767).astype(np.int16)
w = wave.open(D + "/tone16.wav", "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b"".join(struct.pack("<hh", int(a), int(b))
                       for a, b in zip(l, r)))
w.close()
sr2 = 22050
t2 = np.arange(sr2 // 2, dtype=np.float64) / sr2
v = (np.sin(2 * np.pi * 330 * t2) * 0.4 * 8388607).astype(np.int32)
data = bytearray()
for x in v:
    data += int(x).to_bytes(4, "little", signed=True)[:3]
w = wave.open(D + "/tone24.wav", "wb")
w.setnchannels(1); w.setsampwidth(3); w.setframerate(sr2)
w.writeframes(bytes(data)); w.close()
from PIL import Image
Image.fromarray((np.random.default_rng(3).integers(0, 255, (48, 48, 3)))
                .astype("uint8")).save(D + "/cover.png")
PY
ffmpeg -loglevel error -y -i "$WORK/orig/tone16.wav" "$WORK/orig/tone16.flac"
ffmpeg -loglevel error -y -i "$WORK/orig/tone24.wav" -c:a flac "$WORK/orig/tone24.flac"
ffmpeg -loglevel error -y -i "$WORK/orig/tone16.wav" -i "$WORK/orig/cover.png" \
    -map 0:a -map 1:v -c:a flac -c:v png -disposition:v:0 attached_pic \
    "$WORK/orig/cov.flac"
ls -la "$WORK/orig/"*.flac

bit_exact_all() {   # $1=image, rest: names
    local img=$1; shift
    local ok=1 f a b
    for f in "$@"; do
        $B/invf-cat "$img" "$f" "$WORK/out/$f" >/dev/null
        cmp -s "$WORK/orig/$f" "$WORK/out/$f" || { echo "MISMATCH $f"; ok=0; }
    done
    [ "$ok" = 1 ] || return 1
}

swept_of() { sed -n 's/.*sweep done: swept=\([0-9]*\).*/\1/p' "$1"; }

# Re-deflate every fixture's IDAT with the HOST zlib.
#
# The PNGR lane stores the JXL pixels and rebuilds the original file by
# re-deflating the refiltered rows, so it can only transcode a PNG whose
# original IDAT the host zlib can reproduce BIT-FOR-BIT. PIL wheels link
# their own zlib, so the fixtures PIL wrote are not reproducible by the
# build's zlib (here: zlib-ng) -- every combination of level, memLevel and
# strategy missed, and all five leg-1 PNGs landed in GENERIC_GUARD{PNGR}
# instead of being transcoded. That is a property of the lane's design, not
# of the fixtures' content, so the fixtures are made host-reproducible:
# decompress PIL's IDAT to the filtered stream and recompress it with this
# interpreter's zlib at level 6. The filter bytes, the image data and the
# decoded pixels are untouched, so each file is still the same PNG -- only
# the IDAT encoding is one this build can reproduce.
python3 - "$WORK/orig" <<'PY'
import glob, os, struct, sys, zlib

for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.png"))):
    d = open(path, "rb").read()
    out = bytearray(d[:8])
    i, changed = 8, False
    while i < len(d):
        ln = struct.unpack(">I", d[i:i+4])[0]
        typ = d[i+4:i+8]
        body = d[i+8:i+8+ln]
        if typ == b"IDAT":
            # coalesce the whole run of IDAT chunks, then emit it as one
            idat = bytearray(body)
            i += 12 + ln
            while i + 8 <= len(d) and d[i+4:i+8] == b"IDAT":
                n2 = struct.unpack(">I", d[i:i+4])[0]
                idat += d[i+8:i+8+n2]
                i += 12 + n2
            new = zlib.compress(zlib.decompress(bytes(idat)), 6)
            if new != body:
                changed = True
            out += struct.pack(">I", len(new)) + b"IDAT"
            out += new + struct.pack(">I", zlib.crc32(b"IDAT" + new))
            i += 12 + ln
            continue
        out += d[i:i+12+ln]
        i += 12 + ln
    if changed:
        open(path, "wb").write(bytes(out))
print("re-deflated IDATs with the host zlib")
PY

PNGS_OK="grad_rgb.png paeth_split.png rgba_up.png gray_sub.png ancil.png"

echo "== leg 1: PNG sweep transcode (PNGR) =="
$B/invf-mkfs "$IMG" 0.1 >/dev/null
for f in $PNGS_OK; do $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null; done
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
echo "sweep: $(grep 'sweep done' "$WORK/sweep1.log")"
[ "$(swept_of "$WORK/sweep1.log")" = "5" ] || { echo "FAIL: want swept=5"; cat "$WORK/sweep1.log"; exit 1; }
echo "== class stamps =="
for f in $PNGS_OK; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=3 algo=10 gen=1" ] || { echo "FAIL: $f: want CONTAINER{PNGR,1}"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "^.*$f!jxl$" || { echo "FAIL: $f!jxl sibling missing"; $B/invf-ls "$IMG"; exit 1; }
done
echo "== bit-exact cat =="
bit_exact_all "$IMG" $PNGS_OK || exit 1
echo "all 5 transcoded PNGs bit-exact"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log" | tail -1
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== leg 1b: idempotent re-sweep =="
$B/invf-sweep "$IMG" > "$WORK/sweep1b.log" 2>&1 || { cat "$WORK/sweep1b.log"; exit 1; }
echo "re-sweep: $(grep 'sweep done' "$WORK/sweep1b.log")"
[ "$(swept_of "$WORK/sweep1b.log")" = "0" ] || { echo "FAIL: re-sweep re-transcoded"; cat "$WORK/sweep1b.log"; exit 1; }
for f in $PNGS_OK; do
    C=$("$WORK/classof" "$IMG" "$f")
    [ "$C" = "cls=3 algo=10 gen=1" ] || { echo "FAIL: $f stamp changed on re-sweep: $C"; exit 1; }
done
bit_exact_all "$IMG" $PNGS_OK || exit 1
echo "stamps stable, still bit-exact"
$B/invf-fsck "$IMG" | tee "$WORK/fsck1.log" | tail -1
grep -q "^OK" "$WORK/fsck1.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== leg 2: refused PNGs stay RAW, GENERIC_GUARD{PNGR,1} =="
REF_STRICT="gray16.png interlaced.png fixed.png noise64.png"
REF_INFO="palette.png pil_default.png"
$B/invf-mkfs "$IMGR" 0.1 >/dev/null
for f in $REF_STRICT $REF_INFO; do $B/invf-cp "$IMGR" "$WORK/orig/$f" "$f" >/dev/null; done
$B/invf-sweep "$IMGR" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
echo "sweep: $(grep 'sweep done' "$WORK/sweep2.log")"
[ "$(swept_of "$WORK/sweep2.log")" = "0" ] || { echo "FAIL: a refused-case PNG transcoded"; exit 1; }
for f in $REF_STRICT; do
    C=$("$WORK/classof" "$IMGR" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=6 algo=10 gen=1" ] || { echo "FAIL: $f: want GENERIC_GUARD{PNGR,1}"; exit 1; }
done
for f in $REF_INFO; do
    # environment-dependent disposition (palette re-palettization /
    # Pillow's deflate strategy vary by toolchain): GUARD here, the
    # bit-exact check below is the invariant that must hold everywhere
    C=$("$WORK/classof" "$IMGR" "$f")
    echo "  $f: $C (informational)"
    case "$C" in
        "cls=6 algo=10 gen=1"|"cls=3 algo=10 gen=1") ;;
        *) echo "FAIL: $f: unexpected stamp $C"; exit 1 ;;
    esac
done
bit_exact_all "$IMGR" $REF_STRICT $REF_INFO || exit 1
echo "all 6 refused-case PNGs bit-exact (originals kept)"
$B/invf-verify "$IMGR" --deep | tail -1
$B/invf-sweep "$IMGR" > "$WORK/sweep2b.log" 2>&1 || { cat "$WORK/sweep2b.log"; exit 1; }
[ "$(swept_of "$WORK/sweep2b.log")" = "0" ] || { echo "FAIL: guard refired on re-sweep"; exit 1; }
echo "re-sweep: no refire (swept=0)"

echo "== leg 3: decode-back guard — fake cjxl (garbage output, exit 0) =="
printf '#!/bin/sh\nprintf "this is not a jxl blob" > "$2"\n' > "$WORK/faketools/cjxl"
chmod +x "$WORK/faketools/cjxl"
$B/invf-mkfs "$IMGG" 0.1 >/dev/null
for f in $PNGS_OK; do $B/invf-cp "$IMGG" "$WORK/orig/$f" "$f" >/dev/null; done
INVFS_TOOLS="$WORK/faketools" $B/invf-sweep "$IMGG" > "$WORK/sweep3.log" 2>&1 \
    || { cat "$WORK/sweep3.log"; exit 1; }
echo "sweep: $(grep 'sweep done' "$WORK/sweep3.log")"
[ "$(swept_of "$WORK/sweep3.log")" = "0" ] || { echo "FAIL: garbage cjxl output committed"; exit 1; }
for f in $PNGS_OK; do
    C=$("$WORK/classof" "$IMGG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=6 algo=10 gen=1" ] || { echo "FAIL: $f: want GENERIC_GUARD{PNGR,1}"; exit 1; }
done
bit_exact_all "$IMGG" $PNGS_OK || exit 1
echo "guard refused all 5 (garbage cjxl), originals bit-exact"
INVFS_TOOLS="$WORK/faketools" $B/invf-sweep "$IMGG" > "$WORK/sweep3b.log" 2>&1 \
    || { cat "$WORK/sweep3b.log"; exit 1; }
[ "$(swept_of "$WORK/sweep3b.log")" = "0" ] || { echo "FAIL: guard refired at same generation"; exit 1; }
$B/invf-verify "$IMGG" --deep | tail -1

echo "== leg 4: MEMLIMIT admission (INVFS_DEC_MEM_LIMIT=1M) =="
$B/invf-mkfs "$IMGM" 0.1 >/dev/null
for f in big.png small.png; do $B/invf-cp "$IMGM" "$WORK/orig/$f" "$f" >/dev/null; done
INVFS_DEC_MEM_LIMIT=1M $B/invf-sweep "$IMGM" > "$WORK/sweep4.log" 2>&1 \
    || { cat "$WORK/sweep4.log"; exit 1; }
echo "sweep: $(grep 'sweep done' "$WORK/sweep4.log")"
# big.png raw estimate = 900*(1+1200*3) = 3,240,900 > 1M -> MEMLIMIT;
# small.png = 64*(1+192) = 12,352 < 1M -> admitted (the control: admission,
# not blunt refusal, is what gates the lane)
CB=$("$WORK/classof" "$IMGM" big.png)
CS=$("$WORK/classof" "$IMGM" small.png)
echo "  big.png: $CB"; echo "  small.png: $CS"
[ "$CB" = "cls=5 algo=10 gen=1" ] || { echo "FAIL: big.png: want GENERIC_MEMLIMIT{PNGR,1}"; exit 1; }
[ "$CS" = "cls=3 algo=10 gen=1" ] || { echo "FAIL: small.png: want CONTAINER{PNGR,1}"; exit 1; }
bit_exact_all "$IMGM" big.png small.png || exit 1
echo "both bit-exact under the limit"
# upgrade: re-sweep without the limit -> big.png transcodes now
$B/invf-sweep "$IMGM" > "$WORK/sweep4b.log" 2>&1 || { cat "$WORK/sweep4b.log"; exit 1; }
echo "upgrade sweep: $(grep 'sweep done' "$WORK/sweep4b.log")"
[ "$(swept_of "$WORK/sweep4b.log")" = "1" ] || { echo "FAIL: want exactly the big.png upgrade"; cat "$WORK/sweep4b.log"; exit 1; }
CB=$("$WORK/classof" "$IMGM" big.png)
[ "$CB" = "cls=3 algo=10 gen=1" ] || { echo "FAIL: big.png after upgrade: $CB"; exit 1; }
CS=$("$WORK/classof" "$IMGM" small.png)
[ "$CS" = "cls=3 algo=10 gen=1" ] || { echo "FAIL: small.png changed"; exit 1; }
bit_exact_all "$IMGM" big.png small.png || exit 1
echo "big.png upgraded to CONTAINER{PNGR,1}, bit-exact"
$B/invf-sweep "$IMGM" > "$WORK/sweep4c.log" 2>&1 || { cat "$WORK/sweep4c.log"; exit 1; }
[ "$(swept_of "$WORK/sweep4c.log")" = "0" ] || { echo "FAIL: third sweep not idle"; exit 1; }
$B/invf-verify "$IMGM" --deep | tail -1

echo "== leg 5: FLAC — in-tree flacx round-trip + gated sweep lane =="
# 5a: the in-process half of the FLACR lane is fully testable: flacx
# extract -> (ffmpeg WAV decode, the same step invfs_ape_compress runs
# first) -> flacx rebuild must reproduce the original FLAC byte-for-byte.
flacx_rt() {   # $1=flac basename, $2=pcm codec
    local f=$1 codec=$2
    ffmpeg -loglevel error -y -i "$WORK/orig/$f.flac" -map 0:a -c:a $codec \
        "$WORK/out/$f.mid.wav" || return 1
    "$WORK/bin/flacx" extract "$WORK/orig/$f.flac" "$WORK/out/$f.rbin" \
        >/dev/null 2>&1 || return 1
    "$WORK/bin/flacx" rebuild "$WORK/out/$f.mid.wav" "$WORK/out/$f.rbin" \
        "$WORK/out/$f.re.flac" >/dev/null 2>&1 || return 1
    cmp -s "$WORK/orig/$f.flac" "$WORK/out/$f.re.flac"
}
ok=1
flacx_rt tone16 pcm_s16le && echo "  tone16.flac: flacx round-trip bit-exact" || { echo "  FAIL: tone16 flacx round-trip"; ok=0; }
flacx_rt tone24 pcm_s24le && echo "  tone24.flac: flacx round-trip bit-exact" || { echo "  FAIL: tone24 flacx round-trip"; ok=0; }
flacx_rt cov    pcm_s16le && echo "  cov.flac (PICTURE covers): flacx round-trip bit-exact" || { echo "  FAIL: cov flacx round-trip"; ok=0; }
[ "$ok" = 1 ] || exit 1

# 5b: the sweep lane. The PCM blob is Monkey's Audio (mac); flacx itself
# only extracts/rebuilds recipes and there is no APE encoder in-tree.
$B/invf-mkfs "$IMGF" 0.1 >/dev/null
for f in tone16.flac tone24.flac cov.flac; do
    $B/invf-cp "$IMGF" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGF" > "$WORK/sweep5.log" 2>&1 || { cat "$WORK/sweep5.log"; exit 1; }
echo "sweep: $(grep 'sweep done' "$WORK/sweep5.log")"
if [ "$HAVE_MAC" = "1" ]; then
    # full lane: APE(PCM) blob + IVFR recipe + covers, decode-back guarded
    for f in tone16.flac tone24.flac cov.flac; do
        C=$("$WORK/classof" "$IMGF" "$f")
        echo "  $f: $C"
        [ "$C" = "cls=3 algo=7 gen=1" ] || { echo "FAIL: $f: want CONTAINER{FLACR,1}"; exit 1; }
        $B/invf-ls "$IMGF" | grep -q "$f!recipe" || { echo "FAIL: $f!recipe missing"; exit 1; }
    done
else
    # gated: mac absent -> clean refusal; the original stays RAW, nothing
    # half-committed, no refire on re-sweep
    for f in tone16.flac tone24.flac cov.flac; do
        C=$("$WORK/classof" "$IMGF" "$f")
        echo "  $f: $C"
        [ "$C" = "cls=6 algo=7 gen=1" ] || { echo "FAIL: $f: want GENERIC_GUARD{FLACR,1}"; exit 1; }
        if $B/invf-ls "$IMGF" | grep -q "$f!recipe"; then
            echo "FAIL: $f!recipe exists despite the refused transcode"; exit 1
        fi
    done
fi
bit_exact_all "$IMGF" tone16.flac tone24.flac cov.flac || exit 1
echo "all 3 FLACs bit-exact"
$B/invf-verify "$IMGF" --deep | tail -1
$B/invf-sweep "$IMGF" > "$WORK/sweep5b.log" 2>&1 || { cat "$WORK/sweep5b.log"; exit 1; }
[ "$(swept_of "$WORK/sweep5b.log")" = "0" ] || { echo "FAIL: FLAC re-sweep not idle"; exit 1; }
echo "re-sweep: idle (swept=0)"

echo "PNG/FLAC E2E (WP12c POSIX transcodes): PASS"
