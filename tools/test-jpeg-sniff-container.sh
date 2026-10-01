#!/bin/bash
# test-jpeg-sniff-container.sh — a JPEG marker inside a container must not
# claim the container (WP: wp/jpeg-sniff-claims-a-dicom-container).
#
# WHAT THIS ASSERTS, and why it is the LANE and not the bytes:
#
#   The DICOM fixture is built by test-rawimg.sh's own generator
#   (make_dicom_encapsulated). Its pixel-data element is ENCAPSULATED JPEG
#   baseline, and that element literally contains b'\xFF\xD8\xFF\xD9'. The
#   file is a medical container; those bytes are a member of it.
#
#   test-rawimg.sh asserts the DICOM is refused as GENERIC_GUARD{RAWIMG,13}
#   — but only when the jxl pack's estimator is NOT resolvable, because
#   that is the only case in which the jxl pack cannot run. That gate is
#   invisible and ambient: test-jxl.sh compiles jxlest into a PATH dir at
#   test time, so on any host or CI job where test-jxl.sh ran first, or
#   where jxlest is installed, the jxl codecpack is admitted and the DICOM
#   comes back stamped GUARD{JXL,4} instead. Measured, on this tree at
#   5c25cf2: encap.dcm -> cls=6 algo=4 gen=1 with jxlest present,
#   cls=6 algo=13 gen=1 without it. Same binary, same manifests.
#
#   So this script COMPILES jxlest itself. That is what makes the gate
#   deterministic instead of dependent on what else has run on the host.
#
#   The bytes are NOT the assertion here: the lane decision is what
#   regressed, and the bytes are covered by test-rawimg.sh's own
#   verify/cmp legs. This script still checks both files are bit-exact,
#   because a lane that claims a container is worth nothing if it also
#   loses the container.
#
# THE CONTROL ARM (a fix that refuses everything passes the negative and
# breaks the feature): tiny.jpg is an ordinary baseline JPEG, magic at
# offset 0, built by test-jxl.sh's own photo() generator. It MUST still be
# claimed by the JXL lane — cls=2 algo=4 (CODEC{JXL}). If this ever fails,
# the negative below has been "fixed" by disabling the JPEG lane, which is
# not a fix.
#
# Run from the repo root after `make`:
#   bash tools/test-jpeg-sniff-container.sh
# Uses /dev/shm (tmpfs) like the other codecpack suites. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
PACK=$REPO/tools/codecpacks/jxl.codecpack
WORK=/dev/shm/wpjpegcontainer
trap 'rm -rf "$WORK" /dev/shm/wpjpegcontainer*.img' EXIT
IMG=wpjpegcontainer.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin"
cd /dev/shm
rm -f "$IMG"

echo "== tools =="
command -v cjxl >/dev/null || { echo "FAIL: cjxl not installed"; exit 1; }
command -v djxl >/dev/null || { echo "FAIL: djxl not installed"; exit 1; }
command -v cc    >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }

echo "== compile jxlest (the gate this suite has to own) =="
# Without this the jxl pack's probe() fails, the pack is skipped as
# "tools absent", and the defect below is invisible. The point of compiling
# it here is that the test then fails for the same reason whether or not
# test-jxl.sh ever ran.
cc -std=c11 -O2 -Wall -Wextra -o "$WORK/bin/jxlest" "$PACK/jxlest.c"
export PATH="$WORK/bin:$PATH"                 # jxlest resolves by name
command -v jxlest >/dev/null || { echo "FAIL: jxlest did not land on PATH"; exit 1; }

echo "== fixtures =="
# The DICOM comes from test-rawimg.sh's OWN generator, extracted from its
# python heredoc by CONTENT (the opening and closing PY markers), not by
# line number, so editing that suite cannot silently move this one. There
# is one source of truth for the fixture.
awk '/^python3 - "\$WORK\/orig" <<.PY.$/{f=1;next} f&&/^PY$/{exit} f' \
    "$REPO/tools/test-rawimg.sh" > "$WORK/dicomfix.py"
[ -s "$WORK/dicomfix.py" ] || { echo "FAIL: could not extract the fixture generator"; exit 1; }
DCMFIX="$WORK/dicomfix.py" python3 - "$WORK/orig" <<'PY'
import os, sys
outdir = sys.argv[1]
src = open(os.environ['DCMFIX']).read()
g = {'__name__': 'fixture'}
exec(compile(src, 'dicomfix.py', 'exec'), g)
g['make_dicom_encapsulated'](os.path.join(outdir, 'encap.dcm'))
PY

# The control JPEG comes from test-jxl.sh's OWN generator, same content-
# keyed extraction. tiny.jpg there is "tiny but still a valid baseline
# JPEG" — magic FF D8 FF at offset 0.
awk '/^python3 - <<.PY.$/{f=1;next} f&&/^PY$/{exit} f' \
    "$REPO/tools/test-jxl.sh" > "$WORK/jpegfix.py"
[ -s "$WORK/jpegfix.py" ] || { echo "FAIL: could not extract the JPEG generator"; exit 1; }
sed -i "s|^d = \"/dev/shm/wp11jxl/orig\"|d = \"$WORK/orig\"|" "$WORK/jpegfix.py"
sed -i 's/^photo("p[0-9]\.jpg".*$//' "$WORK/jpegfix.py"
sed -i 's/^import numpy as np$/import numpy as np/' "$WORK/jpegfix.py"
python3 "$WORK/jpegfix.py"

for f in encap.dcm tiny.jpg; do
    [ -s "$WORK/orig/$f" ] || { echo "FAIL: missing fixture $f"; exit 1; }
    echo "  $f: $(stat -c %s "$WORK/orig/$f") bytes"
done
# Sanity: the negative fixture really does carry the marker, and NOT at
# offset 0. If this ever reads offset 0 the fixture changed shape and the
# whole premise of this suite has moved.
python3 - "$WORK/orig/encap.dcm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'\xff\xd8\xff')
assert i > 0, 'no JPEG marker found'
assert i != 0, 'marker at offset 0: fixture changed shape'
print('  encap.dcm carries FF D8 FF at offset %d of %d (not 0)' % (i, len(d)))
PY

# class-stamp reader (no stock tool prints invfs.class; built from the repo
# objects, the same helper test-rawimg.sh and test-jxl.sh both build)
cat > "$WORK/classof.c" <<'C'
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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes \
    -I$REPO/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in encap.dcm tiny.jpg; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep =="
INVFS_DEBUG_PACKS=1 $B/invf-sweep "$IMG" > "$WORK/sweep.log" 2>&1 || {
    cat "$WORK/sweep.log"; exit 1; }

echo "== NEGATIVE: the container is not classified by the JPEG codec =="
# The lane assertion, and it is about the algo, not the class. `cls=6` is
# INVFS_CLASS_GENERIC_GUARD -- a refusal -- and its `algo` names WHICH codec
# refused. On this file that must be raw_image (13), never JXL (4). A stamp
# of algo=4 here says "the JPEG codec had an opinion about a DICOM", and
# that opinion is the defect whether or not it was acted on.
#
# Measured on this tree at 5c25cf2, with jxlest resolvable: cls=6 algo=4.
# The bytes were bit-exact in that state, so this is a classification
# defect and not a data-loss one -- but it is the defect, and holding it
# shut is what this suite is for.
C=$("$WORK/classof" "$IMG" encap.dcm)
echo "  encap.dcm: $C"
ALGO=$(printf '%s\n' "$C" | awk '{print $2}')   # field 2 of "cls=6 algo=4 gen=1"
if [ "$ALGO" = "algo=4" ]; then
    echo "FAIL: encap.dcm is classified by algo=4 (INVFS_ALGO_JXL,"
    echo "      src/core/invarifs.h:64). The JPEG codec's only claim on this"
    echo "      file is an FF D8 FF sitting inside its encapsulated"
    echo "      pixel-data element -- a member, not the container."
    grep '^\[packdbg\]' "$WORK/sweep.log" || true
    exit 1
fi
case "$C" in
    cls=2*) echo "FAIL: encap.dcm came back CODEC of some kind: $C"; exit 1 ;;
    *)     echo "  not classified by the JPEG codec (declined: $C)" ;;
esac

echo "== CONTROL: an ordinary JPEG IS still claimed by the JXL lane =="
# The other half. A fix that makes the sniff refuse everything passes the
# negative above and breaks JPEG transcoding; this is what catches that.
CT=$("$WORK/classof" "$IMG" tiny.jpg)
echo "  tiny.jpg: $CT"
[ "$CT" = "cls=2 algo=4 gen=1" ] || {
    echo "FAIL: tiny.jpg should still be CODEC{JXL,1} — the JPEG lane is broken."
    exit 1; }

echo "== bit-exactness (both, still) =="
ok=1
for f in encap.dcm tiny.jpg; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    if cmp -s "$WORK/orig/$f" "$WORK/out/$f"; then
        echo "  $f: bit-exact"
    else
        echo "FAIL: MISMATCH $f — a bit-exactness regression outranks the"
        echo "      lane finding; stop and report."
        ok=0
    fi
done
[ "$ok" = 1 ] || exit 1

$B/invf-verify "$IMG" --deep | tail -1
echo "JPEG-SNIF-CONTAINER E2E: PASS"