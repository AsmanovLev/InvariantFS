#!/bin/bash
# test-fatpacks.sh -- WP619 fat-pack proof: bundled-first + external fallback.
#
# The ape/wavpack registry packs were manifest-only (thin): with no OS tool
# they never even admitted files. Their 1.1.0 releases bundle statically
# built payloads (registry/codecpacks/{ape,wavpack}/1.1.0, built via
# registry/tools/build-payloads.sh); the engine resolves <pack>/bin/<tool>
# BEFORE PATH, so the bundle is preferred and the external is fallback.
#
# Leg 1 (bundled preferred): packs WITH bin/, PATH holding BROKEN fakes
#   (touch a marker, exit 1). A WAV sweep must still land CODEC{algo,gen2}
#   with byte-identical reads, and the markers must be ABSENT (the fakes
#   never ran — the bundle did).
# Leg 2 (external fallback, payload absent): packs WITHOUT bin/ (manifests
#   only), PATH holding the REAL payload binaries (same bytes, different
#   path). Same CODEC stamps + identical bytes — fallback is byte-capable.
# Leg 3 (disaster: no externals at all): packs WITH bin/, PATH scrubbed of
#   every audio tool. Same CODEC stamps + identical bytes. Then the same
#   sweep with THIN 1.0.0 manifests must NOT claim the files (they wait
#   RAW/generic) — the contrast that proves fat matters.
# Leg 4 (decode fallback): leg-1 images re-read with bin/ removed and the
#   real tools on PATH — identical bytes through the fallback decode path.
# Leg 5 (clean): fsck OK on every image; no host pack root mutated.
#
# Hermetic: packs staged under a scratch dir via INVFS_CODECPACKS (+SYS=0
# so the host's /usr/lib/invfs/codecpacks never leaks in); images under
# the scratch dir; offline only (mkfs/cp/sweep/cat/fsck — no FUSE).
# flacr has no engine legs: algo 7 is a builtin container entry the engine
# never lets a pack claim (codec.c pack_register), so its fat payload is
# registry/manager-level only (pins + install/verify, covered by the
# registry's own make test).
#
# Requires: built tree (bin/invf-{mkfs,cp,sweep,cat,fsck}), the registry
# submodule, and built 1.1.0 payloads (registry/.../1.1.0/bin). The last
# is NOT built here (upstream builds need network + minutes); when absent
# legs 1-4 SKIP loudly instead of failing.
#
# Run: INVFS_E2E_AGENT=wp619 bash tools/run-e2e.sh tools/test-fatpacks.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
SRC=$REPO/src
. "$REPO/tools/lib-scratch.sh"
WORK="$(invfs_scratch_root)/invf-fatpacks-$$"
rm -rf "$WORK" && mkdir -p "$WORK"
cd "$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }
need() { [ -e "$1" ] || fail "missing $1 (run make first?)"; }

need "$B/invf-mkfs"; need "$B/invf-cp"; need "$B/invf-sweep"
need "$B/invf-cat"; need "$B/invf-fsck"; need "$REPO/build/core_objs.txt"

REG="$REPO/registry/codecpacks"
FAT_OK=1
for p in ape wavpack; do
    case "$p" in
        ape) tools="mac" ;;
        wavpack) tools="wavpack wvunpack" ;;
    esac
    for t in $tools; do
        [ -x "$REG/$p/1.1.0/bin/$t" ] || FAT_OK=0
    done
    [ -f "$REG/$p/1.1.0/manifest" ] || FAT_OK=0
done
if [ "$FAT_OK" != 1 ]; then
    echo "SKIP: 1.1.0 payloads not built (run registry/tools/build-payloads.sh build first)"
    echo "legs 1-4 need bin/mac + bin/flac + bin/wavpack + bin/wvunpack; nothing asserted."
    rm -rf "$WORK"
    exit 0
fi

echo "== fixtures (deterministic 1 s stereo tones) =="
python3 - <<'PY'
import wave, struct, math
for name, f1, f2 in (("a.wav", 440.0, 550.0), ("w.wav", 330.0, 495.0)):
    w = wave.open(name, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(b"".join(
        struct.pack("<hh", int(8000 * math.sin(6.283185307179586 * f1 * i / 44100)),
                   int(6000 * math.sin(6.283185307179586 * f2 * i / 44100)))
        for i in range(44100)))
    w.close()
PY
ls -la a.wav w.wav

echo "== classof helper (repo-objects link, the test-pngflac pattern) =="
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

# stage_pack <pack> <destdir> [thin]: copy the 1.1.0 manifest (+thin: the
# 1.0.0 one) and, unless thin, the built bin/ payload beside it.
stage_pack() {
    local p="$1" dest="$2" ver="1.1.0"
    [ "${3:-}" = thin ] && ver="1.0.0"
    mkdir -p "$dest/$p.codecpack"
    cp "$REG/$p/$ver/manifest" "$dest/$p.codecpack/manifest"
    if [ "$ver" = "1.1.0" ] && [ "${FAT_BIN:-1}" = 1 ]; then
        cp -r "$REG/$p/$ver/bin" "$dest/$p.codecpack/bin"
    fi
}

# sweep_run <img> <packsdir> <pathvalue>: one offline sweep under a
# controlled PATH (empty trusted-tools dir so only packsdir/bin and PATH
# can resolve anything).
sweep_run() {
    mkdir -p emptytools
    INVFS_CODECPACKS="$1" INVFS_CODECPACKS_SYS=0 INVFS_TOOLS="$WORK/emptytools" \
        env PATH="$2" "$B/invf-sweep" "$3"
}

echo "== tool PATHs (scrubbed: host audio tools must never leak in) =="
BB="$REPO/bin/busybox-static"
if [ -x "$BB" ]; then
    mkdir -p bbpath
    for u in sh ls cp mv rm mkdir cat chmod touch env; do ln -sf "$BB" "bbpath/$u"; done
    SAFEPATH="$WORK/bbpath"
else
    SAFEPATH="/usr/bin:/bin"
fi
for t in mac flac wavpack wvunpack cjxl djxl ffmpeg; do
    if PATH="$SAFEPATH" command -v "$t" >/dev/null 2>&1; then
        fail "scrubbed PATH leaks host tool $t ($(PATH="$SAFEPATH" command -v "$t"))"
    fi
done
echo "scrubbed PATH clean: $SAFEPATH"

echo "== leg 1-3 per pack (one pack per image: every fat pack sniffs"
echo "   RIFF, so co-installed packs would first-hit every WAV) =="
mkdir -p fakebin realbin
for t in mac wavpack wvunpack; do
    # builtin-only marker (no external touch: must fire even under the
    # helper env scrub): proves the fake RAN if the bundle is bypassed.
    printf '#!/bin/sh\n: >> "$MARKDIR/marker-%s"\nexit 1\n' "$t" > "fakebin/$t"
    chmod +x "fakebin/$t"
done
export MARKDIR="$WORK"
cp "$REG/ape/1.1.0/bin/mac" realbin/mac
cp "$REG/wavpack/1.1.0/bin/wavpack" realbin/wavpack
cp "$REG/wavpack/1.1.0/bin/wvunpack" realbin/wvunpack
pack_tools() { case "$1" in ape) echo mac;; wavpack) echo "wavpack wvunpack";; esac; }

# pack_legs <pack> <wav> <algo>: legs 1-3 for one pack. Each leg exports
# its own INVFS_CODECPACKS (reads decode through the pack trampoline;
# without it the read falls back to the builtin strict-resolution lane).
pack_legs() {
    local p="$1" wav="$2" algo="$3" t
    echo "-- leg 1 ($p): bundled preferred over broken externals --"
    mkdir -p "packs1-$p"
    stage_pack "$p" "packs1-$p"
    export INVFS_CODECPACKS="$WORK/packs1-$p" INVFS_CODECPACKS_SYS=0
    export INVFS_TOOLS="$WORK/emptytools"
    "$B/invf-mkfs" "leg1-$p.img" 1 >/dev/null
    "$B/invf-cp" "leg1-$p.img" "$wav" tune.wav || fail "leg1-$p cp"
    sweep_run "$WORK/packs1-$p" "$WORK/fakebin:$SAFEPATH" "leg1-$p.img" \
        > "sweep1-$p.log" 2>&1 || fail "leg1-$p sweep rc=$?"
    [ "$(./classof "leg1-$p.img" tune.wav)" = "cls=2 algo=$algo gen=2" ] \
        || fail "leg1-$p stamp: $(./classof "leg1-$p.img" tune.wav) (want cls=2 algo=$algo gen=2)"
    env PATH="$WORK/fakebin:$SAFEPATH" "$B/invf-cat" "leg1-$p.img" tune.wav > "got1-$p.wav" \
        || fail "leg1-$p cat"
    cmp "$wav" "got1-$p.wav" || fail "leg1-$p bytes differ"
    for t in $(pack_tools "$p"); do
        [ ! -e "$WORK/marker-$t" ] || fail "leg1-$p: fake external $t RAN (bundle not preferred)"
    done
    echo "leg1-$p OK: CODEC{$algo,2}, bytes identical, broken PATH tools never ran"
    echo "-- leg 2 ($p): external fallback with payload absent --"
    mkdir -p "packs2-$p"
    FAT_BIN=0 stage_pack "$p" "packs2-$p"
    unset FAT_BIN
    [ ! -e "packs2-$p/$p.codecpack/bin" ] || fail "leg2-$p: bin/ should be absent"
    export INVFS_CODECPACKS="$WORK/packs2-$p"
    "$B/invf-mkfs" "leg2-$p.img" 1 >/dev/null
    "$B/invf-cp" "leg2-$p.img" "$wav" tune.wav || fail "leg2-$p cp"
    sweep_run "$WORK/packs2-$p" "$WORK/realbin:$SAFEPATH" "leg2-$p.img" \
        > "sweep2-$p.log" 2>&1 || fail "leg2-$p sweep rc=$?"
    [ "$(./classof "leg2-$p.img" tune.wav)" = "cls=2 algo=$algo gen=2" ] \
        || fail "leg2-$p stamp: $(./classof "leg2-$p.img" tune.wav)"
    env PATH="$WORK/realbin:$SAFEPATH" "$B/invf-cat" "leg2-$p.img" tune.wav > "got2-$p.wav" \
        || fail "leg2-$p cat"
    cmp "$wav" "got2-$p.wav" || fail "leg2-$p bytes differ"
    echo "leg2-$p OK: no bin/, PATH tools carry the pack — same stamp, same bytes"
    echo "-- leg 3 ($p): disaster (no externals at all) --"
    mkdir -p "packs3-$p"
    stage_pack "$p" "packs3-$p"
    export INVFS_CODECPACKS="$WORK/packs3-$p"
    "$B/invf-mkfs" "leg3-$p.img" 1 >/dev/null
    "$B/invf-cp" "leg3-$p.img" "$wav" tune.wav || fail "leg3-$p cp"
    sweep_run "$WORK/packs3-$p" "$SAFEPATH" "leg3-$p.img" \
        > "sweep3-$p.log" 2>&1 || fail "leg3-$p sweep rc=$?"
    [ "$(./classof "leg3-$p.img" tune.wav)" = "cls=2 algo=$algo gen=2" ] \
        || fail "leg3-$p stamp: $(./classof "leg3-$p.img" tune.wav)"
    env PATH="$SAFEPATH" "$B/invf-cat" "leg3-$p.img" tune.wav > "got3-$p.wav" \
        || fail "leg3-$p cat"
    cmp "$wav" "got3-$p.wav" || fail "leg3-$p bytes differ"
    echo "leg3-$p OK: fat claims with zero externals"
}
pack_legs ape a.wav 3
pack_legs wavpack w.wav 6

echo "== thin contrast (tool-less thin pack waits unclaimed) =="
mkdir -p packs3thin
stage_pack ape packs3thin thin
export INVFS_CODECPACKS="$WORK/packs3thin"
"$B/invf-mkfs" leg3thin.img 1 >/dev/null
"$B/invf-cp" leg3thin.img a.wav tune.wav || fail "leg3thin cp"
sweep_run "$WORK/packs3thin" "$SAFEPATH" leg3thin.img > sweep3thin.log 2>&1 \
    || fail "leg3thin sweep rc=$?"
THINSTAMP="$(./classof leg3thin.img tune.wav)"
case "$THINSTAMP" in
    "cls=2 algo=3 gen="*) fail "leg3thin: thin pack claimed the file without tools ($THINSTAMP)" ;;
    *) echo "leg3thin stamp (unclaimed, as thin behaves tool-less): $THINSTAMP" ;;
esac
"$B/invf-cat" leg3thin.img tune.wav > got3t-a.wav || fail "leg3thin cat"
cmp a.wav got3t-a.wav || fail "leg3thin bytes differ"
echo "thin contrast OK: waits unclaimed, bytes intact"
unset INVFS_CODECPACKS

echo "== leg 4: decode fallback (bin/ removed after encode) =="
for p in ape wavpack; do
    mkdir -p "packs4-$p"
    stage_pack "$p" "packs4-$p"
    rm -rf "packs4-$p/$p.codecpack/bin"
    case "$p" in
        ape) wav=a.wav ;; wavpack) wav=w.wav ;;
    esac
    INVFS_CODECPACKS="$WORK/packs4-$p" INVFS_CODECPACKS_SYS=0 INVFS_TOOLS="$WORK/emptytools" \
        env PATH="$WORK/realbin:$SAFEPATH" "$B/invf-cat" "leg1-$p.img" tune.wav > "got4-$p.wav" \
        || fail "leg4-$p cat"
    cmp "$wav" "got4-$p.wav" || fail "leg4-$p bytes differ"
    echo "leg4-$p OK: fallback decode path byte-identical"
done

echo "== leg 5: fsck clean, host untouched =="
for i in leg1-ape.img leg2-ape.img leg3-ape.img leg1-wavpack.img leg2-wavpack.img leg3-wavpack.img leg3thin.img; do
    "$B/invf-fsck" "$i" | grep -q "^OK$" || fail "leg5: fsck $i not clean"
done
[ ! -e /.invariantfs/codecpacks/ape.codecpack ] \
    || fail "leg5: leaked ape pack into host root"
[ ! -e /.invariantfs/codecpacks/wavpack.codecpack ] \
    || fail "leg5: leaked wavpack pack into host root"
echo "leg5 OK"

echo "PASS: fatpacks (bundled-first + fallback + disaster + thin contrast)"
