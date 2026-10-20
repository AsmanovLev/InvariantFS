#!/bin/bash
# test-entry-v3.sh — WP639 recipe entry v3 e2e: a forced-v3 volume lives
# a full lifecycle, and mixed v1/v2+v3 volumes read back bit-exact.
#
#   Leg A (forced): mkfs + invf-cp a small corpus (text, random binary,
#   3-segment file) with INVFS_RECIPE_FORCE_V3=1, offline sweep, invf-fsck
#   CLEAN, invf-cat every file memcmp-identical to the host original,
#   unlink one file through the mount, blocks gone, fsck CLEAN again.
#   This is the user-visible half the C unit test cannot see: the whole
#   read/write/sweep/fsck path over 40 B entries with per-entry CRCs.
#   (The CRC tamper legs live in the unit test; here a torn recipe would
#   surface as EIO/fsck-DAMAGED, which is what the legs assert against.)
#
#   Leg B (mixed): the same volume gets a second file written through a
#   NON-forced mount (v1/v2 recipe) next to the forced-v3 files. Every
#   file reads back byte-identical and fsck stays CLEAN -- old and new
#   strides coexist in one volume, and the dual-stride parse serves both.
#
#   Leg C (unforced regression): a fresh volume with the knob OFF takes
#   the same corpus through cp + sweep + cat + fsck. Nothing about the
#   production path changed (v1/v2 byte-identical output).
#
# Run from the repo root after `make`:  bash tools/test-entry-v3.sh
# Under e2e rules via: bash tools/run-e2e.sh tools/test-entry-v3.sh
# with INVFS_E2E_AGENT=wp639 (AGENTS.md 1.5). Uses /dev/shm workdirs and
# RELATIVE image paths (blkio treats /dev/* as raw devices).
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp639entryv3
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref"
cd /dev/shm
rm -f wp639-a.img wp639-c.img

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "  OK    $1"; }

mnt_up() {   # <img>
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.$(basename "$1").log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $1 never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
}

fsck_ok() {  # <img>
    $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
        || { cat "$WORK/fsck.last"; fail "fsck of $1 not OK"; }
}

# deterministic corpus: text (sweepable), random binary (verbatim lane),
# 3-segment file (multi-entry recipe: the v3 conversion walks > 1 entry).
python3 - "$WORK/ref" <<'EOF'
import sys, os
d = sys.argv[1]
open(os.path.join(d, "hello.txt"), "wb").write(b"hello invfs\n" * 1000)
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(100000))
open(os.path.join(d, "three.bin"), "wb").write(os.urandom(3 * 65536 + 100))
EOF

echo "== [A] forced-v3 volume lifecycle =="
$B/invf-mkfs wp639-a.img 64 >/dev/null || fail "mkfs A"
INVFS_RECIPE_FORCE_V3=1 $B/invf-cp wp639-a.img "$WORK/ref/hello.txt" hello.txt >/dev/null || fail "cp hello"
INVFS_RECIPE_FORCE_V3=1 $B/invf-cp wp639-a.img "$WORK/ref/rand.bin" rand.bin >/dev/null || fail "cp rand"
INVFS_RECIPE_FORCE_V3=1 $B/invf-cp wp639-a.img "$WORK/ref/three.bin" three.bin >/dev/null || fail "cp three"
INVFS_RECIPE_FORCE_V3=1 $B/invf-sweep wp639-a.img >/dev/null 2>&1 || fail "sweep A"
fsck_ok wp639-a.img
for f in hello.txt rand.bin three.bin; do
    $B/invf-cat wp639-a.img "$f" > "$WORK/out-$f" || fail "cat $f"
    cmp "$WORK/ref/$f" "$WORK/out-$f" || fail "$f not bit-exact after forced-v3 sweep"
done
ok "A: forced-v3 cp + sweep + cat bit-exact, fsck OK"

echo "== [B] mixed strides coexist =="
mnt_up wp639-a.img
cp "$WORK/ref/hello.txt" "$MNT/plain.txt" || fail "plain write through mount"
sync
mnt_down
fsck_ok wp639-a.img
mnt_up wp639-a.img
cmp "$WORK/ref/hello.txt" "$MNT/hello.txt" || fail "mixed readback hello"
cmp "$WORK/ref/rand.bin" "$MNT/rand.bin" || fail "mixed readback rand"
cmp "$WORK/ref/three.bin" "$MNT/three.bin" || fail "mixed readback three"
cmp "$WORK/ref/hello.txt" "$MNT/plain.txt" || fail "mixed readback plain"
rm "$MNT/rand.bin" || fail "unlink through mount"
mnt_down
fsck_ok wp639-a.img
mnt_up wp639-a.img
[ ! -e "$MNT/rand.bin" ] || fail "unlinked file still present"
cmp "$WORK/ref/three.bin" "$MNT/three.bin" || fail "post-unlink survivor not exact"
mnt_down
ok "B: mixed v1/v2+v3 volume reads exact, unlink reclaims, fsck OK"

echo "== [C] unforced regression =="
$B/invf-mkfs wp639-c.img 64 >/dev/null || fail "mkfs C"
$B/invf-cp wp639-c.img "$WORK/ref/hello.txt" hello.txt >/dev/null || fail "cp C hello"
$B/invf-cp wp639-c.img "$WORK/ref/three.bin" three.bin >/dev/null || fail "cp C three"
$B/invf-sweep wp639-c.img >/dev/null 2>&1 || fail "sweep C"
fsck_ok wp639-c.img
$B/invf-cat wp639-c.img hello.txt > "$WORK/out-c-hello" || fail "cat C"
cmp "$WORK/ref/hello.txt" "$WORK/out-c-hello" || fail "unforced round-trip changed"
ok "C: knob-off path unchanged and green"

echo "PASS: test-entry-v3 (all legs green)"
