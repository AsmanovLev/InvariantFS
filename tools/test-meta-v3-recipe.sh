#!/bin/bash
# test-meta-v3-recipe.sh — WP-M8 e2e: content-addressed recipe blobs + v3 read.
#
# A VOLF_V3 volume stores a file's AST recipe as an immutable,
# content-addressed blob (key 0x04||BLAKE3-256(recipe)) referenced from the
# inode row. The read path fetches the blob, VERIFIES BLAKE3, then decodes
# the segments with the shared v2 segment decoder. This leg drives the whole
# thing through FUSE: write file content covering the AST shapes, read it
# back bit-exact (cmp), remount and read again, and prove a corrupted recipe
# fails LOUDLY instead of returning wrong bytes.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-recipe.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3recipe
IMG=metav3recipe.img
MNT=$WORK/mnt
SRC=$WORK/src
rm -rf "$WORK" && mkdir -p "$MNT" "$SRC"
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

# cmp_fuse <name> : read $MNT/<name> through FUSE and compare bit-exactly
cmp_fuse() {
    cmp "$SRC/$1" "$MNT/$1" \
        || fail "$1: FUSE read differs from the source bytes"
}

# cmp_offline <name> : read via the offline engine (invf-cat) and compare
cmp_offline() {
    "$B/invf-cat" "$IMG" "$1" >"$WORK/offline.out" 2>"$WORK/offline.err" \
        || { cat "$WORK/offline.err"; fail "$1: offline invf-cat failed"; }
    cmp "$SRC/$1" "$WORK/offline.out" \
        || fail "$1: offline read differs from the source bytes"
}

echo "== WP-M8: content-addressed recipe blobs + verified v3 read path =="

# --- mkfs v3 -------------------------------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"

# --- source files covering the AST shapes -------------------------------
: > "$SRC/empty"
printf 'the quick brown fox jumps over the lazy dog\n' > "$SRC/small"
# exactly one 64 KiB segment (SEGMENT_SIZE) and one-past it
head -c 65536 /dev/urandom > "$SRC/seg1"
{ cat "$SRC/seg1"; printf 'x'; } > "$SRC/seg1plus"
# multi-segment: ~200 KiB of incompressible bytes (raw/LZ4 path)
head -c 200000 /dev/urandom > "$SRC/multi"
# highly compressible, still bit-exact
python3 - "$SRC/compressible" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"AB" * 40000)
PY
# dedup pair: identical bytes -> one recipe blob, two files
cp "$SRC/multi" "$SRC/dup1"
cp "$SRC/multi" "$SRC/dup2"
NAMES="empty small seg1 seg1plus multi compressible dup1 dup2"
echo "sources: $(for n in $NAMES; do printf '%s(%s) ' "$n" "$(stat -c %s "$SRC/$n")"; done)"

# --- write through FUSE and read back identical -------------------------
mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }
for n in $NAMES; do
    cp "$SRC/$n" "$MNT/$n" || fail "FUSE write of $n failed"
    cmp_fuse "$n"
done
echo "write+read: all $(printf '%s\n' $NAMES | wc -l) files bit-exact through FUSE"

# empty file is readable and still zero bytes
[ -f "$MNT/empty" ] && [ ! -s "$MNT/empty" ] \
    || fail "empty file lost its zero-length shape"
echo "empty file: present, size 0"

# dedup: rewrite a file with identical content (same recipe address)
cp "$SRC/multi" "$MNT/dup1"
cmp_fuse dup1
cmp_fuse dup2
echo "dedup: identical recipes re-read identically"

# remount persists everything
mnt_down
mnt_up
for n in $NAMES; do
    cmp_fuse "$n"
done
echo "remount: all files still bit-exact"

mnt_down

# --- offline engine reads the same bytes ---------------------------------
for n in $NAMES; do
    cmp_offline "$n"
done
echo "offline: invf-cat bit-exact for all files"

# --- crafted corruption: a bad recipe must fail loudly -------------------
# Corrupt the recipe VALUE (not the page structure) and re-seal the page so
# only the content-address check can catch it. A BLAKE3 mismatch must refuse
# the read -- never return wrong bytes. The image is restored afterwards so
# the final fsck/CLEAN checks see a clean volume.
cp "$IMG" "$WORK/img.bak"
python3 - "$IMG" <<'PY' || fail "could not locate/corrupt a recipe blob"
import struct, sys
img, size, ver = sys.argv[1], 65537, 1

def crc32c(d):
    poly = 0x82F63B78
    c = 0xFFFFFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (poly & -(c & 1))
    return c ^ 0xFFFFFFFF

def reseal(page):
    # mbuf_page_crc skips the 4-byte checksum field entirely
    page[16:20] = struct.pack("<I", crc32c(bytes(page[:16]) + bytes(page[20:4096])))

data = bytearray(open(img, "rb").read())
needle = struct.pack("<I", ver) + struct.pack("<I", size)
flips = 0
for blk in range(0, len(data), 4096):
    if data[blk:blk+4] != b"BPG3":
        continue
    pos = blk
    while True:
        i = data.find(needle, pos, blk + 4096)
        if i < 0:
            break
        # a real recipe value: [klen=0x21][0x04 key(32)][vlen=0x50][value...]
        if (i - 2 >= blk and data[i-2:i] == b"\x50\x00"
                and i - 35 >= blk and data[i-35] == 0x04):
            data[i + 8] ^= 0xFF        # flip a value byte (num_blocks)
            flips += 1
        pos = i + 1
    page = bytearray(data[blk:blk+4096])
    reseal(page)
    data[blk:blk+4096] = page
assert flips > 0, "no recipe value found to corrupt"
open(img, "wb").write(data)
print("corrupted+resealed %d recipe value(s)" % flips)
PY

if "$B/invf-cat" "$IMG" seg1plus >"$WORK/corrupt.out" 2>"$WORK/corrupt.err"; then
    fail "corrupt recipe read succeeded (must fail loudly)"
fi
[ ! -s "$WORK/corrupt.out" ] || fail "corrupt recipe returned bytes"
grep -qiE "BLAKE3 mismatch|corrupt|mismatch" "$WORK/corrupt.err" \
    || { cat "$WORK/corrupt.err"; fail "corrupt recipe failure was not loud"; }
echo "corruption: BLAKE3-verified recipe refused (no bytes returned)"

# --- restore, then fsck + CLEAN + final remount --------------------------
cp "$WORK/img.bak" "$IMG"
"$B/invf-cat" "$IMG" seg1plus >"$WORK/restored.out" 2>/dev/null \
    || fail "restored image read failed"
cmp "$SRC/seg1plus" "$WORK/restored.out" || fail "restored image not bit-exact"
echo "restore: clean image reads bit-exact"

FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "fsck: OK"

mnt_up
cmp_fuse seg1plus
cmp_fuse multi
mnt_down

python3 - "$IMG" <<'PY' || fail "volume not CLEAN at the end"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN after all round-trips")
PY

echo "ALL META-V3 RECIPE LEGS PASS"
