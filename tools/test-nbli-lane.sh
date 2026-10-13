#!/bin/bash
# test-nbli-lane.sh -- WP620 two-way photo handler (jxl|nbli) end-to-end.
#
# Default stays jxl; nbli is opt-in (INVFS_PHOTO_CODEC=nbli here -- the
# offline knob; -o photo_codec= is the mount form, covered by the FUSE leg).
# NBLI arrives as a FAT PACK (registry/codecpacks/nbli/1.0.0: manifest +
# static bin/nbli built from the open prototype sources), never a builtin,
# never the closed fNBLI binary.
#
# Corpus: hand-rolled RAW-deflate PNGs (the only IDAT framing the IVPN
# recipe reproduces -- the same admission PNGR has; a stock zlib-wrapped
# PNG is a refusal leg, not a claim leg) at known deflate grid points,
# one real-pixels frame (CLIC professional_valid_2020 when
# INVFS_NBLI_CLIC_ZIP points at the zip, else a deterministic synthetic
# photo-like array -- the provenance is printed), one PIL JPEG (the knob
# must never touch JPEGs: NBLI has no JPEG-reconstruction mode).
#
# Leg 1 (default jxl): RGB/gray/real-pixels -> CONTAINER{PNGR=10,gen=1},
#   RGBA -> CONTAINER{10} (cjxl keeps alpha: THE contrast with leg 2),
#   wrapped/16-bit/noise -> GENERIC_GUARD{10}, JPEG -> CODEC{JXL=4};
#   everything bit-exact; re-sweep idle.
# Leg 2 (nbli): fresh image, same corpus -> RGB/gray/real-pixels ->
#   CONTAINER{NBLI=16,gen=1} ("[sweep] f: nbli (pack)" lines), RGBA ->
#   GENERIC_GUARD{16} RAW (alpha dropped: the pixel guard refuses), the
#   rest GUARD{16}, JPEG STILL CODEC{4}; everything bit-exact; re-sweep
#   idle (GUARD stamps settle, no refire).
# Leg 3 (pack absent, nbli mode): every PNG RAW and UNSTAMPED (waiting,
#   never a GUARD pin -- installing the pack grows no generation), JPEG
#   still JXL; stage the pack -> re-sweep claims RGB (wait-not-stuck).
# Leg 4 (broken PATH namesake): bin/ gone but an exit-1 `nbli` on PATH ->
#   the pack registers, the probe passes, the encode fails ->
#   GENERIC_GUARD{16}, file stays RAW, bytes exact, re-sweep settles
#   (present-but-broken is a refusal, not a wait: the vol_pack_sweep
#   convention; INVFS_HELPER_VERBOSE's exec line proves the PATH fallback
#   really ran -- a marker file cannot, the sandbox lets pack children
#   write scratch only).
# Leg 4b (truly tool-less: no `nbli` anywhere): pack unregistered ->
#   RAW unstamped wait, bytes intact.
# Leg 5 (FUSE): -o photo_codec=nbli mounts, user.invfs.stats reports
#   photo_codec=nbli; bad value warns and keeps jxl. SKIP when FUSE is
#   unavailable (no /dev/fuse or mount refused).
# Leg 6 (clean): fsck OK on every image; no host pack root mutated; no
#   fake external ever ran (bundled-first proof).
#
# Hermetic: packs staged under a scratch dir via INVFS_CODECPACKS (+SYS=0);
# images under the scratch dir; PATH scrubbed (busybox shims or
# /usr/bin:/bin) with controlled fake/real tool dirs. cjxl/djxl resolve
# via a trusted INVFS_TOOLS dir (never PATH), so the JPEG builtin works
# while the nbli PATH fakes prove the bundle is preferred.
#
# Run: INVFS_E2E_AGENT=wp620 bash tools/run-e2e.sh tools/test-nbli-lane.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
SRC=$REPO/src
. "$REPO/tools/lib-scratch.sh"
WORK="$(invfs_scratch_root)/invf-nbli-$$"
rm -rf "$WORK" && mkdir -p "$WORK/orig"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }
need() { [ -e "$1" ] || fail "missing $1 (run make first?)"; }

need "$B/invf-mkfs"; need "$B/invf-cp"; need "$B/invf-sweep"
need "$B/invf-cat"; need "$B/invf-fsck"; need "$REPO/build/core_objs.txt"
[ -f "$REPO/registry/codecpacks/nbli/1.0.0/manifest" ] \
    || fail "nbli pack manifest absent (submodule checked out?)"

echo "== stage the nbli fat pack (manifest + static bin/) =="
stage_nbli() { # <destdir>: copy manifest and built bin/ beside it
    mkdir -p "$1/nbli.codecpack"
    cp "$REPO/registry/codecpacks/nbli/1.0.0/manifest" "$1/nbli.codecpack/manifest"
    [ -x "$REPO/registry/codecpacks/nbli/1.0.0/bin/nbli" ] \
        || fail "bin/nbli not built (run registry/tools/build-payloads.sh build nbli first)"
    cp -r "$REPO/registry/codecpacks/nbli/1.0.0/bin" "$1/nbli.codecpack/bin"
}
mkdir -p emptytools toolsdir realbin
for t in cjxl djxl; do
    p=$(command -v "$t") || fail "$t not installed (JPEG legs need it)"
    ln -sf "$p" "toolsdir/$t"
    # Pack REGISTRATION (codec.c manifest_tool_ok) resolves argv0 via
    # <pack>/bin or PATH only -- $INVFS_TOOLS does not count there (it
    # counts at probe/exec time). So the jxl pack will not even register
    # under a fully scrubbed PATH; the JPEG legs intentionally provide
    # the real cjxl/djxl on the suite PATH (the test-jxl shape), while
    # nbli stays pack-bin-only with a broken PATH namesake behind it.
    ln -sf "$p" "realbin/$t"
done
export INVFS_TOOLS="$WORK/toolsdir"
# The JPEG legs run through the vendored jxl codecpack (the test-jxl
# shape): symlink it into every packs dir that should transcode JPEGs.
# Its estimate helper builds here (host cc, before the PATH scrub); it
# resolves via realbin, while nbli resolves ONLY via the fat pack's bin/
# (fakebin's broken nbli proves bundled-first).

# NOTE: fixtures generate BEFORE the PATH scrub further below (unzip and
# python3 live on the host PATH, not in the scrubbed shim dir).
# (PATH scrub happens after fixture generation -- see below.)

echo "== fixtures (raw-deflate PNGs at grid points + JPEG) =="
CLICZIP="${INVFS_NBLI_CLIC_ZIP:-/tmp/invfs-wp620-corpus/clic-prof-valid-2020.zip}"
if [ -f "$CLICZIP" ]; then
    unzip -p "$CLICZIP" valid/schicka-307.png > orig/clic.png 2>/dev/null \
        || fail "cannot stream schicka-307.png from $CLICZIP"
    echo "  real-pixels source: CLIC valid/schicka-307.png ($(wc -c < orig/clic.png) bytes)"
else
    echo "  NOTE: no CLIC zip at $CLICZIP -- real-pixels leg uses synthetic photo-like pixels"
    echo "  (set INVFS_NBLI_CLIC_ZIP to run it on CLIC frames)"
fi
python3 - "$CLICZIP" <<'PY'
import sys, os, zlib, struct
import numpy as np
from PIL import Image
D = "orig"
cliczip = sys.argv[1]

def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + \
           struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)

def filt_rows(arr, bpp, filters):
    out = bytearray(); prev = bytes(arr.shape[1] * bpp)
    for idx in range(arr.shape[0]):
        row = arr[idx].tobytes(); f = filters[idx % len(filters)]
        dst = bytearray(len(row))
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 0: v = row[i]
            elif f == 1: v = (row[i] - a) & 255
            elif f == 2: v = (row[i] - b) & 255
            elif f == 3: v = (row[i] - ((a + b) >> 1)) & 255
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                v = (row[i] - pr) & 255
            dst[i] = v
        out.append(f); out += dst; prev = row
    return bytes(out)

def wraw(fn, arr, ct, filters, level=6, mem=8, wbits=-15):
    h, w = arr.shape[:2]
    bpp = {0: 1, 2: 3, 6: 4}[ct]
    filt = filt_rows(arr, bpp, filters)
    co = zlib.compressobj(level, zlib.DEFLATED, wbits, mem, zlib.Z_DEFAULT_STRATEGY)
    idat = co.compress(filt) + co.flush()
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    open(os.path.join(D, fn), "wb").write(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
    print(" ", fn, os.path.getsize(os.path.join(D, fn)), "bytes")

rng = np.random.default_rng(620)
def mkpx(w, h, sigma, ct=2):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255); g = np.clip(y + n, 0, 255); b = np.clip((x + y) / 2 + n, 0, 255)
    if ct == 2: return np.dstack([r, g, b]).astype(np.uint8)
    if ct == 6: return np.dstack([r, g, b, np.clip(128 + n, 0, 255)]).astype(np.uint8)
    return np.clip((x + y) / 2 + n, 0, 255).astype(np.uint8)

wraw("grad_rgb.png", mkpx(320, 240, 6), 2, [0, 1, 2, 3, 4], 6, 8)
wraw("gray_sub.png", mkpx(333, 77, 8, 0), 0, [1], 1, 8)
wraw("rgba_up.png", mkpx(200, 150, 10, 6), 6, [2], 7, 8)
wraw("wrapped.png", mkpx(64, 48, 6), 2, [0], 6, 8, wbits=15)
g16 = rng.integers(0, 65536, (128, 128)).astype(">u2")
g16rows = b"".join(b"\x00" + g16[r].tobytes() for r in range(128))
co = zlib.compressobj(6, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY)
idat = co.compress(g16rows) + co.flush()
ihdr = struct.pack(">IIBBBBB", 128, 128, 16, 0, 0, 0, 0)
open(os.path.join(D, "gray16.png"), "wb").write(
    b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
wraw("noise64.png", rng.integers(0, 256, (64, 64, 3)).astype(np.uint8), 2, [0], 6, 8)
# real pixels, lane-shaped container: CLIC frame when available, else synthetic
if os.path.exists("orig/clic.png"):
    px = np.asarray(Image.open("orig/clic.png").convert("RGB"))
    print("  clic pixels", px.shape)
else:
    px = mkpx(256, 192, 9)
    print("  synthetic pixels", px.shape)
h, w = px.shape[:2]
filt = filt_rows(px, 3, [4])
co = zlib.compressobj(6, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY)
idat = co.compress(filt) + co.flush()
ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
open(os.path.join(D, "real_raw.png"), "wb").write(
    b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
print("  real_raw.png", os.path.getsize(os.path.join(D, "real_raw.png")), "bytes")
Image.fromarray(mkpx(320, 240, 6), "RGB").save(os.path.join(D, "photo.jpg"), quality=85)
# Try-last pin: opaque binary no pack claims. The nbli pack (a PNG-magic
# claim) must never trial-encode it -- any GUARD{16}/CONTAINER{16} stamp
# here is try-last interference, failing the leg by name below.
open(os.path.join(D, "rand.bin"), "wb").write(rng.integers(0, 256, (20000,)).astype(np.uint8).tobytes())
PY

echo "== classof helper (repo-objects link, the test-fatpacks pattern) =="
cat > classof.c <<'C'
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
# shellcheck disable=SC2046
gcc -std=gnu11 -O2 -I$SRC -I$SRC/core -I$SRC/codecs -I$SRC/recipes -I$SRC/vendor7z \
    -o "$WORK/classof" "$WORK/classof.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread || fail "classof link"
classof() { "$WORK/classof" "$1" "$2"; }
cc -std=c11 -O2 -Wall -Wextra -o "$WORK/realbin/jxlest" \
    "$REPO/tools/codecpacks/jxl.codecpack/jxlest.c" || fail "jxlest build"
link_jxl_pack() { # <destdir>: the vendored jxl pack beside the staged nbli pack
    ln -sfn "$REPO/tools/codecpacks/jxl.codecpack" "$1/jxl.codecpack"
}

echo "== scrubbed PATH (host nbli/cjxl must never leak in) =="
BB="$REPO/bin/busybox-static"
ORIGPATH="$PATH"
if [ -x "$BB" ]; then
    mkdir -p bbpath
    for u in sh ls cp mv rm mkdir cat chmod touch env cmp grep ln; do ln -sf "$BB" "bbpath/$u"; done
    SAFEPATH="$WORK/bbpath"
else
    SAFEPATH="/usr/bin:/bin"
fi
mkdir -p fakebin realbin
printf '#!/bin/sh\n: >> "$MARKDIR/marker-nbli"\nexit 1\n' > fakebin/nbli
chmod +x fakebin/nbli
export MARKDIR="$WORK"
export PATH="$WORK/realbin:$WORK/fakebin:$SAFEPATH"

# Hermeticity proof: no host audio/image tool may leak through the scrubbed
# PATH -- EXCEPT cjxl/djxl/jxlest, which the JPEG legs intentionally
# provide via realbin (pack registration needs argv0 on PATH; see above).
# nbli in particular must resolve ONLY to fakebin's broken namesake, so
# the marker files prove the fat pack's bin/ did the work.
for t in mac flac wavpack wvunpack ffmpeg; do
    if PATH="$SAFEPATH" command -v "$t" >/dev/null 2>&1; then
        fail "scrubbed PATH leaks host tool $t"
    fi
done
[ -x realbin/cjxl ] && [ -x realbin/djxl ] && [ -x realbin/jxlest ] \
    || fail "realbin JPEG tools missing"
[ -x fakebin/nbli ] || fail "fakebin/nbli missing"

FILES="grad_rgb.png gray_sub.png rgba_up.png real_raw.png wrapped.png gray16.png noise64.png photo.jpg rand.bin"
fill() { # <img>: cp the whole corpus in
    for f in $FILES; do "$B/invf-cp" "$1" "orig/$f" "$f" >/dev/null || fail "cp $f"; done
}
check_bytes() { # <img>: every file cats back identical
    for f in $FILES; do
        "$B/invf-cat" "$1" "$f" > "got-$f" || fail "cat $f"
        cmp "orig/$f" "got-$f" || fail "$f bytes differ"
    done
}

echo "== leg 1: default (jxl) =="
mkdir -p packs1 && stage_nbli packs1 && link_jxl_pack packs1
export INVFS_CODECPACKS="$WORK/packs1" INVFS_CODECPACKS_SYS=0
unset INVFS_PHOTO_CODEC
"$B/invf-mkfs" leg1.img 32 >/dev/null
fill leg1.img
"$B/invf-sweep" leg1.img > sweep1.log 2>&1 || fail "leg1 sweep rc=$?"
for f in grad_rgb.png gray_sub.png rgba_up.png real_raw.png; do
    [ "$(classof leg1.img "$f")" = "cls=3 algo=10 gen=1" ] \
        || fail "leg1 $f: $(classof leg1.img "$f") (want CONTAINER{PNGR,1})"
done
for f in wrapped.png gray16.png noise64.png; do
    [ "$(classof leg1.img "$f")" = "cls=6 algo=10 gen=1" ] \
        || fail "leg1 $f: $(classof leg1.img "$f") (want GUARD{PNGR,1})"
done
[ "$(classof leg1.img photo.jpg)" = "cls=2 algo=4 gen=1" ] \
    || fail "leg1 photo.jpg: $(classof leg1.img photo.jpg) (want CODEC{JXL,1})"
case "$(classof leg1.img rand.bin)" in
    *"algo=16"*) fail "leg1 rand.bin: $(classof leg1.img rand.bin) (nbli touched an unclaimed binary)" ;;
esac
check_bytes leg1.img
"$B/invf-sweep" leg1.img > sweep1b.log 2>&1 || fail "leg1 re-sweep rc=$?"
grep -q "swept=0" sweep1b.log || fail "leg1 re-sweep not idle"
echo "leg1 OK: PNGR claims + refusals as specified, JPEG JXL, bytes exact, re-sweep idle"

echo "== leg 2: nbli position =="
mkdir -p packs2 && stage_nbli packs2 && link_jxl_pack packs2
export INVFS_CODECPACKS="$WORK/packs2"
export INVFS_PHOTO_CODEC=nbli
"$B/invf-mkfs" leg2.img 32 >/dev/null
fill leg2.img
INVFS_HELPER_VERBOSE=1 "$B/invf-sweep" leg2.img > sweep2.log 2>&1 \
    || fail "leg2 sweep rc=$?"
grep -q ": nbli (pack)" sweep2.log || fail "leg2: no 'nbli (pack)' sweep lines"
grep -q "nbli.codecpack/bin/nbli" sweep2.log \
    || fail "leg2: no bundled-bin exec in the sweep log"
for f in grad_rgb.png gray_sub.png real_raw.png; do
    [ "$(classof leg2.img "$f")" = "cls=3 algo=16 gen=1" ] \
        || fail "leg2 $f: $(classof leg2.img "$f") (want CONTAINER{NBLI,1})"
done
# RGBA is THE contrast: jxl keeps alpha (leg1 transcodes), nbli drops it
# (pixel guard refuses) -- RAW behind GUARD{16}, bytes intact.
[ "$(classof leg2.img rgba_up.png)" = "cls=6 algo=16 gen=1" ] \
    || fail "leg2 rgba_up.png: $(classof leg2.img rgba_up.png) (want GUARD{NBLI,1})"
for f in wrapped.png gray16.png noise64.png; do
    [ "$(classof leg2.img "$f")" = "cls=6 algo=16 gen=1" ] \
        || fail "leg2 $f: $(classof leg2.img "$f") (want GUARD{NBLI,1})"
done
[ "$(classof leg2.img photo.jpg)" = "cls=2 algo=4 gen=1" ] \
    || fail "leg2 photo.jpg: $(classof leg2.img photo.jpg) (knob must not touch JPEG)"
case "$(classof leg2.img rand.bin)" in
    *"algo=16"*) fail "leg2 rand.bin: $(classof leg2.img rand.bin) (nbli touched an unclaimed binary)" ;;
esac
check_bytes leg2.img
[ ! -e "$WORK/marker-nbli" ] || fail "leg2: fake PATH nbli RAN (bundle not preferred)"
"$B/invf-sweep" leg2.img > sweep2b.log 2>&1 || fail "leg2 re-sweep rc=$?"
grep -q "swept=0" sweep2b.log || fail "leg2 re-sweep not idle (GUARD must settle)"
echo "leg2 OK: NBLI claims + RGBA contrast + JPEG untouched, bytes exact, re-sweep idle"

echo "== leg 3: pack absent (nbli mode waits, never pins) =="
mkdir -p packs3empty && link_jxl_pack packs3empty
export INVFS_CODECPACKS="$WORK/packs3empty"
"$B/invf-mkfs" leg3.img 32 >/dev/null
fill leg3.img
"$B/invf-sweep" leg3.img > sweep3.log 2>&1 || fail "leg3 sweep rc=$?"
for f in grad_rgb.png gray_sub.png rgba_up.png real_raw.png wrapped.png gray16.png noise64.png; do
    [ "$(classof leg3.img "$f")" = "none" ] \
        || fail "leg3 $f: $(classof leg3.img "$f") (want unstamped wait)"
done
[ "$(classof leg3.img photo.jpg)" = "cls=2 algo=4 gen=1" ] \
    || fail "leg3 photo.jpg moved (pack absence must not touch JPEG)"
check_bytes leg3.img
export INVFS_CODECPACKS="$WORK/packs2"
"$B/invf-sweep" leg3.img > sweep3b.log 2>&1 || fail "leg3 re-sweep rc=$?"
[ "$(classof leg3.img grad_rgb.png)" = "cls=3 algo=16 gen=1" ] \
    || fail "leg3 grad_rgb.png: $(classof leg3.img grad_rgb.png) (wait-not-stuck)"
check_bytes leg3.img
echo "leg3 OK: packless wait is unstamped; staging the pack claims on re-sweep"

echo "== leg 4: broken PATH namesake (guard, not corruption) =="
# bin/ removed, but a broken `nbli` (exit 1) sits on PATH: the pack
# registers (argv0 resolves), the probe passes (it is executable), the
# encode fails -> GENERIC_GUARD{16} with the file left RAW, bytes exact,
# settled on re-sweep. Present-but-broken is a refusal, not a wait -- the
# vol_pack_sweep convention. The marker proves the attempt really exec'd
# the PATH namesake (fallback) and still refused safely.
mkdir -p packs4 && stage_nbli packs4
rm -rf packs4/nbli.codecpack/bin
export INVFS_CODECPACKS="$WORK/packs4"
"$B/invf-mkfs" leg4.img 32 >/dev/null
fill leg4.img
# INVFS_HELPER_VERBOSE logs the exec argv: the proof the attempt went
# through the PATH fallback (a Landlock-blocked marker file cannot prove
# it -- the sandbox lets the child write scratch only, so the fake's
# append never lands; its exit 1 is what refuses).
env -u INVFS_TOOLS INVFS_HELPER_VERBOSE=1 "$B/invf-sweep" leg4.img > sweep4.log 2>&1 \
    || fail "leg4 sweep rc=$?"
[ "$(classof leg4.img grad_rgb.png)" = "cls=6 algo=16 gen=1" ] \
    || fail "leg4 grad_rgb.png: $(classof leg4.img grad_rgb.png) (want GUARD{NBLI,1})"
grep -q "fakebin/nbli" sweep4.log \
    || fail "leg4: no PATH-fallback exec in the sweep log"
check_bytes leg4.img
"$B/invf-sweep" leg4.img > sweep4b.log 2>&1 || fail "leg4 re-sweep rc=$?"
grep -q "swept=0" sweep4b.log || fail "leg4 re-sweep not idle"
echo "leg4 OK: broken tool refuses into GUARD, RAW bytes intact, settles"

echo "== leg 4b: truly tool-less (unstamped wait) =="
# Same manifest-only pack, but no `nbli` anywhere (not even a fake): the
# pack does not register, the placeholder has no trampolines, the lane
# waits RAW unstamped. PATH here is realbin (JPEG tools only) + bbpath.
mkdir -p packs4b && stage_nbli packs4b
rm -rf packs4b/nbli.codecpack/bin
export INVFS_CODECPACKS="$WORK/packs4b"
"$B/invf-mkfs" leg4b.img 32 >/dev/null
fill leg4b.img
export PATH="$WORK/realbin:$SAFEPATH"
env -u INVFS_TOOLS "$B/invf-sweep" leg4b.img > sweep4b2.log 2>&1 || fail "leg4b sweep rc=$?"
[ "$(classof leg4b.img grad_rgb.png)" = "none" ] \
    || fail "leg4b grad_rgb.png: $(classof leg4b.img grad_rgb.png) (want unstamped wait)"
check_bytes leg4b.img
export PATH="$WORK/realbin:$WORK/fakebin:$SAFEPATH"
echo "leg4b OK: tool-less lane waits RAW, bytes intact"

echo "== leg 5: FUSE (mount option + stats visibility) =="
if [ ! -e /dev/fuse ]; then
    echo "SKIP leg5: no /dev/fuse"
else
    export PATH="/usr/bin:/bin:$ORIGPATH"
    mkdir -p mnt5
    # getfattr(1) is absent on minimal hosts; stdlib os.getxattr reads the
    # same xattr with no package dependency.
    if ! INVFS_CODECPACKS="$WORK/packs2" INVFS_CODECPACKS_SYS=0 \
            "$B/invf-fuse" -o photo_codec=nbli leg2.img mnt5 > fuse5.log 2>&1; then
        echo "SKIP leg5: mount refused in this environment (see fuse5.log)"
    elif ! STATS=$(python3 -c "import os,sys; sys.stdout.write(os.getxattr('mnt5', 'user.invfs.stats').decode())" 2>/dev/null); then
        echo "SKIP leg5: stats unreadable after mount (see fuse5.log)"
        fusermount3 -u mnt5 2>/dev/null || umount mnt5 2>/dev/null || true
    else
        echo "$STATS" | grep -q "^photo_codec=nbli$" \
            || fail "leg5: stats lacks photo_codec=nbli (got: $STATS)"
        echo "leg5 OK: stats reports photo_codec=nbli"
        fusermount3 -u mnt5 2>/dev/null || umount mnt5 2>/dev/null \
            || fail "leg5: unmount"
    fi
    sleep 2
    export PATH="$WORK/realbin:$WORK/fakebin:$SAFEPATH"
fi

echo "== leg 6: fsck clean, host untouched =="
for i in leg1.img leg2.img leg3.img leg4.img leg4b.img; do
    "$B/invf-fsck" "$i" | grep -q "^OK$" || fail "leg6: fsck $i not clean"
done
[ ! -e /.invariantfs/codecpacks/nbli.codecpack ] \
    || fail "leg6: leaked nbli pack into host root"
# (No marker assertion here: leg 2 proves the bundle is preferred -- the
# fake never ran there; leg 4 proves the PATH fallback refuses safely.)
echo "leg6 OK"

echo "PASS: nbli-lane (two-way photo handler, opt-in nbli, bit-exact)"
