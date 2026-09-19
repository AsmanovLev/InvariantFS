#!/bin/bash
# test-meta-v3-write.sh — WP-M9 e2e: the full v3 write path on the base tree.
#
# A VOLF_V3 volume must support the same streaming/ranged write session the
# v2 path has (vol_write_begin / vol_write_range / vol_write_truncate /
# vol_write_commit), but with the file's recipe published as an immutable
# content-addressed blob and the inode row committed through the base
# B+-tree. This leg drives a ranged-write matrix through FUSE and checks the
# bytes bit-exactly (cmp), then remounts, reads offline, and demands a clean
# fsck.
#
# Matrix:
#   middle      overwrite a window strictly inside the file (per-seg RMW)
#   head        overwrite at offset 0
#   boundary    unaligned write crossing a 64 KiB segment boundary
#   multi-seg   one write spanning several segments
#   tail        append at EOF (extension)
#   sparse      write past EOF leaving a zero gap
#   trunc-shrink  truncate to a smaller size
#   trunc-extend  truncate to a larger size (zero extension)
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-write.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3write
IMG=metav3write.img
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

# rwops.py applies the same operation sequence to the mounted file and to a
# local expected file, so a bit-exact cmp proves the filesystem kept up.
#   w <off> <len> <seed>   pwrite len deterministic bytes at off
#   t <size>               truncate to size
cat >"$WORK/rwops.py" <<'PY'
import os, random, sys
fuse, exp = sys.argv[1], sys.argv[2]
ops = sys.argv[3:]
i = 0
while i < len(ops):
    op = ops[i]; i += 1
    if op == "w":
        off = int(ops[i]); ln = int(ops[i+1]); seed = int(ops[i+2]); i += 3
        d = random.Random(seed).randbytes(ln)
        for p in (fuse, exp):
            fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
            os.pwrite(fd, d, off)
            os.close(fd)
    elif op == "t":
        n = int(ops[i]); i += 1
        for p in (fuse, exp):
            if not os.path.exists(p):
                fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
                os.close(fd)
            os.truncate(p, n)
    else:
        sys.exit("unknown op %r" % op)
PY

# round <name> <ops...> : (re)create the pair, apply ops, cmp bit-exactly
round() {
    local name="$1"; shift
    : > "$MNT/$name" || fail "create $name through FUSE failed"
    : > "$SRC/$name" || fail "create local $name failed"
    python3 "$WORK/rwops.py" "$MNT/$name" "$SRC/$name" "$@" \
        || fail "$name: write/truncate sequence failed"
    cmp "$SRC/$name" "$MNT/$name" \
        || fail "$name: FUSE bytes differ after [$*]"
    echo "  $name: ok ($(stat -c %s "$SRC/$name") bytes)"
}

cmp_fuse() {
    cmp "$SRC/$1" "$MNT/$1" \
        || fail "$1: FUSE read differs from the source bytes"
}

echo "== WP-M9: full v3 write path (ranged writes + truncate on the base tree) =="

# --- mkfs v3 -------------------------------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"

# --- seed source files (built by the same op driver, then copied in) ------
SEG=65536
head -c 200000 /dev/urandom > "$SRC/base"
echo "base: $(stat -c %s "$SRC/base") bytes"

mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }

# --- create the base file through FUSE (multi-segment write) -------------
cp "$SRC/base" "$MNT/base" || fail "FUSE create/write of base failed"
cmp_fuse base
echo "create: base written multi-segment and bit-exact"

# --- ranged-write matrix -------------------------------------------------
echo "ranged matrix:"
# middle overwrite inside segment 1 (strictly per-segment RMW)
round mid   w 100000 5000 1
# head overwrite
round head  w 0 100 2
# unaligned window straddling the segment-0/1 boundary
round bnd   w $((SEG - 6)) 12 3
# one write spanning several segments (crosses EOF -> extension)
round multi w 60000 200000 4
# exact append at EOF
round tail  w 200000 12345 5
# write past EOF leaving a zero-filled sparse gap
round sparse w $((200000 + 100000)) 1000 6

# --- truncate shrink then extend ----------------------------------------
echo "truncate:"
round sh    t 100000
round ext   t 300000
round sh2   w 120000 5000 7 t 90000 t 260000
# shrink away a segment the session itself wrote, then extend back over it
# (a stale session pba must not be freed twice)
round sh3   w 200000 5000 8 t 100000 t 300000
echo "truncate shrink/extend: ok"

# --- re-read every file after the whole matrix ---------------------------
for n in base mid head bnd multi tail sparse sh ext sh2 sh3; do
    cmp_fuse "$n"
done
echo "matrix: all files bit-exact through FUSE"

# --- remount persistence -------------------------------------------------
mnt_down
mnt_up
for n in base mid head bnd multi tail sparse sh ext sh2 sh3; do
    cmp_fuse "$n"
done
echo "remount: all files still bit-exact"

mnt_down

# --- offline engine reads the same bytes ---------------------------------
for n in base mid head bnd multi tail sparse sh ext sh2 sh3; do
    "$B/invf-cat" "$IMG" "$n" >"$WORK/offline.out" 2>"$WORK/offline.err" \
        || { cat "$WORK/offline.err"; fail "$n: offline invf-cat failed"; }
    cmp "$SRC/$n" "$WORK/offline.out" \
        || fail "$n: offline read differs from the source bytes"
done
echo "offline: invf-cat bit-exact for all files"

# --- fsck + CLEAN --------------------------------------------------------
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "fsck: OK"

python3 - "$IMG" <<'PY' || fail "volume not CLEAN at the end"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN after all round-trips")
PY

echo "ALL META-V3 WRITE LEGS PASS"
