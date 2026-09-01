#!/bin/bash
# test-sweepboot.sh — WP23 sweepboot e2e (persistent regression).
# No VM boot needed: the maintenance boot's engine-side steps are
# exercised against a loop-file image exactly as tools/sweepboot-init.sh
# issues them.
#
#   Leg 0: sweepboot-init.sh is syntax-clean (bash -n; the initramfs
#          runs busybox sh, and bash -n is the available proxy)
#   Leg 1: rootfs-ish image: /usr/lib/invfs/codecpacks fixture (the
#          splt_test pack) + corpus imported; invf-sweep --extract-packs
#          materializes the packs OUT of the unmounted volume, bit-exact
#   Leg 2: loadable: a sweep with INVFS_CODECPACKS=<extracted> picks the
#          packs up (the SPLT containers decompose -- "codecpack" lines)
#   Leg 3: self-hosting across a sweep: the pack files are now PPMd batch
#          members; re-extract (bit-exact again) and a second sweep with
#          the RE-extracted packs decomposes the nested container member
#   Leg 4: /.invfs/codecpacks precedence over /usr/lib/invfs/codecpacks
#   Leg 5: no-packs volume: extraction is a clean empty dir, rc 0 (the
#          sweepboot script falls back to builtin codecs)
#   Leg 6: the script's remaining engine invocation -- invf-sweep --seal
#          (the maintenance pass) -- runs on the image; volume bit-exact,
#          fsck + verify --deep clean
#
# Run via the global e2e lock:  bash tools/run-e2e.sh tools/test-sweepboot.sh
# Uses /dev/shm like the other soak scripts. NOTE: blkio treats /dev/*
# paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp23sweepboot
IMG=wp23sweepboot.img
IMG2=wp23sweepboot-b.img
IMG3=wp23sweepboot-c.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMG2" "$IMG3"

fail() { echo "FAIL: $*" >&2; exit 1; }

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

deep_ok() { $B/invf-verify "$1" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify --deep not clean: $1"; }

echo "== [0] sweepboot-init.sh syntax (bash -n) =="
bash -n "$REPO/tools/sweepboot-init.sh" || fail "sweepboot-init.sh syntax"
bash -n "$REPO/vm/initramfs/init" || fail "vm/initramfs/init syntax"
grep -q "invfs.sweepboot" "$REPO/vm/initramfs/init" \
    || fail "/init lost the invfs.sweepboot branch"
echo "  scripts parse; /init carries the invfs.sweepboot branch"

echo
echo "== [1] extract-packs materializes the on-volume packs, bit-exact =="
command -v python3 >/dev/null || fail "python3 required (splt_test helper)"

# the rootfs-ish fixture tree: the pack the volume hosts for itself,
# plus a small corpus the sweep has something to chew on
mkdir -p "$WORK/vroot/usr/lib/invfs/codecpacks/splt_test.codecpack"
cp "$REPO/tools/codecpacks/splt_test.codecpack/manifest" \
   "$REPO/tools/codecpacks/splt_test.codecpack/splt.py" \
   "$WORK/vroot/usr/lib/invfs/codecpacks/splt_test.codecpack/"
python3 - "$WORK/orig" <<'PY'
import random, struct, sys
d = sys.argv[1]
def splt(members):
    b = b"SPLT" + struct.pack("<I", len(members))
    for m in members: b += struct.pack("<Q", len(m))
    return b + b"".join(members)
words = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
open(d + '/demo.splt', 'wb').write(
    splt([(words * 300)[:20000], (words * 500)[:30000]]))
inner = splt([b"inner member alpha\n" * 500, b"inner member beta\n" * 700])
open(d + '/nest.splt', 'wb').write(splt([inner, b"outer tail\n" * 300]))
open(d + '/notes.txt', 'wb').write((words * 900)[:60000])
rnd = random.Random(5)
open(d + '/rand.bin', 'wb').write(rnd.randbytes(40000))
PY

$B/invf-mkfs "$IMG" 0.2 >/dev/null
$B/invf-import "$IMG" "$WORK/vroot" >/dev/null
for f in demo.splt nest.splt notes.txt rand.bin; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
# the exact argv sweepboot-init.sh issues (step 2)
X=$($B/invf-sweep "$IMG" --extract-packs "$WORK/packs") || fail "--extract-packs rc"
[ "$X" = "$WORK/packs" ] || fail "payload line is not the dir: '$X'"
[ -f "$WORK/packs/splt_test.codecpack/manifest" ] \
    || fail "manifest not materialized"
[ -f "$WORK/packs/splt_test.codecpack/splt.py" ] \
    || fail "splt.py not materialized"
diff -r "$WORK/vroot/usr/lib/invfs/codecpacks" "$WORK/packs" \
    || fail "extracted tree differs from the fixture"
echo "  packs extracted from the UNMOUNTED volume, bit-exact"

echo
echo "== [2] extracted packs are loadable (a sweep picks them up) =="
INVFS_CODECPACKS="$WORK/packs" $B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; fail "sweep with extracted packs failed"; }
grep -q "demo.splt: splt_test (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: demo.splt not decomposed by the extracted pack"
         cat "$WORK/sweep1.log"; exit 1; }
grep -q "nest.splt: splt_test (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: nest.splt not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "sweep done" "$WORK/sweep1.log" || fail "sweep did not complete"
echo "  extracted pack drove: demo.splt + nest.splt decomposition"

echo
echo "== [3] self-hosting across a sweep (packs now PPMd-batched) =="
# the pack files themselves were batched by the sweep above; re-extract
# (the engine decodes the batches in-process) and sweep AGAIN with the
# re-extracted copy: the nested member decomposes only if the packs are
# still loadable
$B/invf-sweep "$IMG" --extract-packs "$WORK/packs2" >/dev/null \
    || fail "re-extract from the swept volume failed"
diff -r "$WORK/vroot/usr/lib/invfs/codecpacks" "$WORK/packs2" \
    || fail "re-extracted tree differs (batch decode broke bytes)"
INVFS_CODECPACKS="$WORK/packs2" $B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; fail "second sweep failed"; }
grep -q "nest.splt!mbr0000-chunk0: splt_test (codecpack)" "$WORK/sweep2.log" \
    || { echo "FAIL: nested member not decomposed by the re-extracted pack"
         cat "$WORK/sweep2.log"; exit 1; }
echo "  packs read back out of PPMd batches; nested member decomposed"

echo
echo "== [4] /.invfs/codecpacks precedence =="
mkdir -p "$WORK/vroot2/.invfs/codecpacks/marker.codecpack" \
         "$WORK/vroot2/usr/lib/invfs/codecpacks/other.codecpack"
printf 'name = marker\nalgo = 47\ncaps = external\nsniff.ext = mrk\nencode = cat {in} {out}\ndecode = cat {in} {out}\n' \
    > "$WORK/vroot2/.invfs/codecpacks/marker.codecpack/manifest"
printf 'name = other\nalgo = 48\ncaps = external\nsniff.ext = oth\nencode = cat {in} {out}\ndecode = cat {in} {out}\n' \
    > "$WORK/vroot2/usr/lib/invfs/codecpacks/other.codecpack/manifest"
$B/invf-mkfs "$IMG2" 0.2 >/dev/null
$B/invf-import "$IMG2" "$WORK/vroot2" >/dev/null
$B/invf-sweep "$IMG2" --extract-packs "$WORK/packs3" >/dev/null \
    || fail "extract with dual pack roots failed"
[ -f "$WORK/packs3/marker.codecpack/manifest" ] \
    || fail ".invfs/codecpacks not picked"
[ ! -e "$WORK/packs3/other.codecpack" ] \
    || fail "usr/lib root won over .invfs (precedence inverted)"
echo "  /.invfs/codecpacks wins; the system root is the fallback"

echo
echo "== [5] no-packs volume: clean empty extraction =="
$B/invf-mkfs "$IMG3" 0.2 >/dev/null
$B/invf-cp "$IMG3" "$WORK/orig/notes.txt" notes.txt >/dev/null
X=$($B/invf-sweep "$IMG3" --extract-packs "$WORK/packs4") \
    || fail "extract on a pack-less volume must succeed"
[ "$X" = "$WORK/packs4" ] || fail "payload line is not the dir"
[ -d "$WORK/packs4" ] && [ -z "$(ls -A "$WORK/packs4")" ] \
    || fail "pack-less extraction not an empty dir"
echo "  empty dir, rc 0 -- the sweepboot script falls back to builtins"

echo
echo "== [6] the maintenance pass itself: invf-sweep --seal =="
# the exact argv sweepboot-init.sh issues (step 3) on the swept image
INVFS_CODECPACKS="$WORK/packs2" $B/invf-sweep "$IMG" --seal > "$WORK/seal.log" 2>&1 \
    || { cat "$WORK/seal.log"; fail "sweep --seal failed"; }
grep -q "\[seal\]" "$WORK/seal.log" || fail "no seal report"
# everything still bit-exact, volume clean
for f in demo.splt nest.splt notes.txt rand.bin; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    cmp "$WORK/orig/$f" "$WORK/out/$f" || fail "$f not bit-exact post-sweepboot"
done
fsck_ok "$IMG"
deep_ok "$IMG"
fsck_ok "$IMG2"
fsck_ok "$IMG3"
echo "  sweep --seal done; corpus bit-exact; fsck + verify --deep clean"

rm -f "$IMG" "$IMG2" "$IMG3"
echo
echo "SWEEPBOOT E2E: PASS"
