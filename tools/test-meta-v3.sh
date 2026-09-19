#!/bin/bash
# test-meta-v3.sh — WP-M1 e2e leg 0: the metadata-v3 on-disk skeleton.
#
# INVFS_V3=1 mkfs writes format_version=3 + VOLF_V3, a zeroed root area and
# the RT30 root-area descriptor (seq=0, empty root slots, no delta). The
# mount presents an empty namespace and closes CLEAN; fsck accepts it.
#
# This leg freezes the v3 on-disk interface (design-meta-v3.md §12) before
# the WP-M2 engine exists. It is not a data-path test: nothing is written
# through the v3 namespace yet.
#
# Run from the repo root after `make`:  bash tools/run-e2e.sh tools/test-meta-v3.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3
IMG=metav3.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {
    $B/invf-fuse "$IMG" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    cat "$WORK/fuse.log" >&2 || true
    fail "mount of $IMG never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $IMG" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
}
trap cleanup EXIT

echo "== leg 0: v3 format skeleton (mkfs/open/close/fsck) =="

# --- mkfs v3 -------------------------------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.2 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"

# --- on-disk marker: format_version=3, VOLF_V3, RT30 descriptor ----------
python3 - "$IMG" <<'PY' || fail "v3 on-disk marker/RT30 descriptor wrong"
import struct, sys
blk = open(sys.argv[1], 'rb').read(4096)
assert len(blk) == 4096, "short block 0"
fmt = blk[0x90]
flags = struct.unpack_from('<I', blk, 0x88)[0]
assert fmt == 3, "format_version=%d, want 3" % fmt
assert flags & 0x10, "VOLF_V3 not set (vol_flags=0x%08x)" % flags
off = 0x9D0
magic = blk[off:off+4]
assert magic == b'RT30', "RT30 magic %r" % magic
ver, page_size = struct.unpack_from('<II', blk, off+4)
slot0, slot1, delta, seq = struct.unpack_from('<QQQQ', blk, off+0xC)
assert ver == 1, "RT30 version %d" % ver
assert page_size == 4096, "RT30 page_size %d" % page_size
assert slot0 == 0 and slot1 == 0, "root slots not empty: %d/%d" % (slot0, slot1)
assert delta == 0, "delta_pba not empty: %d" % delta
assert seq == 0, "seq not 0: %d" % seq
print("on-disk: format_version=3 VOLF_V3 set RT30 version=1 page_size=%d "
      "slots empty seq=0" % page_size)
PY

# --- offline open: empty namespace --------------------------------------
OUT=$($B/invf-ls "$IMG" 2>"$WORK/ls.log") \
    || { cat "$WORK/ls.log"; fail "offline invf-ls failed on v3 volume"; }
echo "$OUT" | grep -q "0 file(s)" \
    || { echo "$OUT"; fail "offline v3 namespace is not empty"; }

# --- FUSE mount: empty root, clean unmount ------------------------------
mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }
LS=$(ls -A "$MNT") || { cat "$WORK/fuse.log"; fail "cannot list v3 mount"; }
[ -z "$LS" ] || { echo "unexpected entries: $LS"; fail "v3 root is not empty"; }
echo "mounted: root lists empty"
mnt_down   # daemon must exit on unmount (blocks otherwise)
# after a clean unmount the superblock must still read CLEAN (0xCA)
python3 - "$IMG" <<'PY' || fail "volume not CLEAN after unmount"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X after unmount, want CLEAN" % blk[0x18]
print("unmounted: superblock CLEAN")
PY

# --- fsck accepts and reports clean -------------------------------------
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "$FSCK" | grep -q "format:       v3" \
    || { echo "$FSCK"; fail "fsck did not report v3"; }

echo "leg 0 OK: v3 skeleton round-trips (mkfs -> mount -> empty -> clean -> fsck)"
echo "ALL META-V3 LEGS PASS"
