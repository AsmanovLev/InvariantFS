#!/bin/bash
# test-sbin-shims.sh -- /sbin front-ends (mkfs.invfs/fsck.invfs/mount.invfs) e2e.
#
# Leg 1 (mkfs): mkfs.invfs builds a bootable-shaped image; plain invf-fsck
#   says OK on it.
# Leg 2 (clean check): fsck.invfs (bare, -a, -p, -n, -f) on a clean volume
#   exits 0. -V exits 0.
# Leg 3 (uncorrectable): an RT30 descriptor with a stale CRC is DAMAGED
#   under -a and exits 4 (fsck(8) "errors left uncorrected"), never 0.
# Leg 4 (operational): garbage file exits 8; no args / bad flag /
#   --discard-reachable exit 16.
# Leg 5 (mkfs refusals): -L/-U refused (no label field in format v0);
#   non-numeric size refused. -V exits 0.
# Leg 6 (mount fake): mount.invfs -f prints the daemon command, mounts
#   nothing, exits 0. Bare usage exits 1.
# Leg 7 (mount real, needs /dev/fuse): file visible through the mount,
#   fusermount -u / umount clean. Skipped without /dev/fuse.
#
# Hermetic: images under /dev/shm (relative paths; blkio treats /dev/*
# as raw devices). No root needed except the optional leg 7 mount, which
# FUSE allows for the mounting uid.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
S=$REPO/tools/sbin
WORK=/dev/shm/sbshim
IMG=sbin-shim.img
rm -rf "$WORK" && mkdir -p "$WORK/src"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }
RC=0

echo "== [1] mkfs.invfs builds an image =="
echo "hello invariantfs" > "$WORK/src/note.txt"
"$S/mkfs.invfs" "$IMG" 1 >/dev/null || fail "mkfs.invfs rc=$?"
$B/invf-import "$IMG" "$WORK/src" >/dev/null
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fresh image not clean"

echo "== [2] fsck.invfs on a clean volume =="
"$S/fsck.invfs" "$IMG" >/dev/null || fail "bare check rc=$?"
"$S/fsck.invfs" -a "$IMG" >/dev/null || fail "-a rc=$?"
"$S/fsck.invfs" -p "$IMG" >/dev/null || fail "-p rc=$?"
"$S/fsck.invfs" -n "$IMG" >/dev/null || fail "-n rc=$?"
"$S/fsck.invfs" -f "$IMG" >/dev/null || fail "-f rc=$?"
"$S/fsck.invfs" -V >/dev/null || fail "-V rc=$?"

echo "== [3] stale RT30 crc -> DAMAGED, exit 4 =="
cp "$IMG" "$WORK/dmg.img"
python3 - "$WORK/dmg.img" <<'PYEOF'
import sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(0x9D0 + 0x24)
    b = f.read(1)
    f.seek(0x9D0 + 0x24)
    f.write(bytes([b[0] ^ 0x01]))
print("flipped RT30 seq byte: CRC now stale")
PYEOF
"$S/fsck.invfs" -a "$WORK/dmg.img" > "$WORK/dmg.txt" 2>&1 || RC=$?
[ "$RC" -eq 4 ] || fail "stale RT30: exit $RC, want 4 (output: $(cat "$WORK/dmg.txt"))"
grep -q "^DAMAGED$" "$WORK/dmg.txt" || fail "stale RT30: no DAMAGED verdict"
echo "stale RT30: DAMAGED, exit 4"

echo "== [4] operational and usage errors =="
head -c 100000 /dev/urandom > "$WORK/garbage.bin"
"$S/fsck.invfs" "$WORK/garbage.bin" >/dev/null 2>&1 || RC=$?
[ "$RC" -eq 8 ] || fail "garbage file: exit $RC, want 8"
"$S/fsck.invfs" >/dev/null 2>&1 || RC=$?
[ "$RC" -eq 16 ] || fail "no args: exit $RC, want 16"
"$S/fsck.invfs" --bogus "$IMG" >/dev/null 2>&1 || RC=$?
[ "$RC" -eq 16 ] || fail "bad flag: exit $RC, want 16"
"$S/fsck.invfs" --discard-reachable "$IMG" >/dev/null 2>&1 || RC=$?
[ "$RC" -eq 16 ] || fail "discard-reachable: exit $RC, want 16"

echo "== [5] mkfs.invfs refusals =="
"$S/mkfs.invfs" -L data "$WORK/lbl.img" 1 >/dev/null 2>&1 || RC=$?
[ "$RC" -ne 0 ] || fail "-L accepted (no label field exists)"
"$S/mkfs.invfs" -U 1234 "$WORK/lbl.img" 1 >/dev/null 2>&1 || RC=$?
[ "$RC" -ne 0 ] || fail "-U accepted (UUID is generated)"
"$S/mkfs.invfs" "$WORK/lbl.img" lots >/dev/null 2>&1 || RC=$?
[ "$RC" -ne 0 ] || fail "non-numeric size accepted"
"$S/mkfs.invfs" -V >/dev/null || fail "mkfs -V rc=$?"

echo "== [6] mount.invfs -f prints, mounts nothing =="
"$S/mount.invfs" -f "$IMG" "$WORK/mnt" > "$WORK/fake.txt" 2>&1 || fail "-f rc=$?"
grep -q "invf-fuse" "$WORK/fake.txt" || fail "-f: no daemon command printed"
[ ! -e "$WORK/mnt" ] || fail "-f created the mountpoint"
"$S/mount.invfs" "$IMG" >/dev/null 2>&1 || RC=$?
[ "$RC" -ne 0 ] || fail "mount.invfs without dir rc=0"

echo "== [7] mount.invfs real mount =="
if [ ! -e /dev/fuse ]; then
    echo "SKIP: no /dev/fuse on this host"
else
    mkdir -p "$WORK/mnt"
    "$S/mount.invfs" "$IMG" "$WORK/mnt" || fail "mount.invfs rc=$?"
    grep -q "hello invariantfs" "$WORK/mnt/note.txt" || fail "file not visible through mount"
    echo "mounted, file reads back"
    umount "$WORK/mnt" 2>/dev/null || fusermount -u "$WORK/mnt" 2>/dev/null \
        || fail "unmount failed"
    echo "unmounted clean"
fi

echo "PASS: sbin shims"
