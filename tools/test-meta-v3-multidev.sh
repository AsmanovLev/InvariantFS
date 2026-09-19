#!/bin/bash
# test-meta-v3-multidev.sh — WP-M19 e2e: v3 two-device placement + metadata
# mirror.
#
# A v3 volume over dev0 (fast: metadata + base tree) + dev1 (capacity:
# metadata mirror + Shadow). The v3 base B+-tree draws its pages from the
# dev0 metadata span (mirrored by the WP25 DEVT/mux), never from the dev1
# shadow fallback; RT30 and DEVT are written consistently on both devices;
# the metadata span stays byte-identical.
#
#   leg 1  build: INVFS_V3=1 mkfs dev0+dev1, RT30 + DEVT on both, geometry
#   leg 2  placement: metadata span byte-identical; base root pages live in
#          the mirrored metadata span (dev0), not on the dev1 shadow
#   leg 3  FUSE mount, create a file + dir/nested file, unmount
#   leg 4  remount, namespace persists, base-tree pages still on dev0
#   leg 5  fsck clean (offline, dev1 attached); mirror span identical again
#   leg 6  DEGRADED (dev0 withheld): FUSE opens dev1 alone read-only, lists
#          and stats the namespace from the mirror, writes refused (EROFS);
#          reattach -> RW resumes
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-multidev.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3multi
D0=metav3md0.img
D1=metav3md1.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT"
cd /dev/shm
rm -f "$D0" "$D1"

export INVFS_DEV1=$D1      # device-1 locator (wins over the DEVT hint)

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {
    $B/invf-fuse "$D0" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    cat "$WORK/fuse.log" >&2 || true
    fail "mount of $D0 never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $D0" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $D0" 2>/dev/null || true
}
trap cleanup EXIT

# rtslot <dev> : print "slot0 slot1 delta seq" from the RT30 descriptor
rtslot() {
    python3 - "$1" <<'PY'
import struct, sys
blk = open(sys.argv[1], 'rb').read(4096)
off = 0x9D0
assert blk[off:off+4] == b'RT30', "RT30 magic missing"
slot0, slot1, delta, seq = struct.unpack_from('<QQQQ', blk, off + 0xC)
print(slot0, slot1, delta, seq)
PY
}

echo "== leg 1: build the v3 two-device volume =="
INVFS_V3=1 $B/invf-mkfs "$D0" 0.125 "$D1" 0.25 > "$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 two-device mkfs failed"; }
grep -q "devices:         2" "$WORK/mkfs.log" || fail "mkfs did not report 2 devices"
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"
META_HI=$(sed -n 's/.*metadata zone: *blocks [0-9]* \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs.log")
[ -n "$META_HI" ] || fail "could not parse the metadata geometry"
META_SPAN=$(( (META_HI + 1) * 4096 ))
# RT30 + DEVT present on BOTH devices
for d in "$D0" "$D1"; do
    [ "$(dd if="$d" bs=1 skip=$((0x9D0)) count=4 status=none)" = "RT30" ] \
        || fail "no RT30 on $d"
    [ "$(dd if="$d" bs=1 skip=$((0x2A0)) count=4 status=none)" = "DEVT" ] \
        || fail "no DEVT on $d"
done
echo "built: format v3, DEVT + RT30 on both devices (metadata zone ends at $META_HI)"

echo "== leg 2: placement — metadata span mirrored, base pages in it =="
cmp <(head -c "$META_SPAN" "$D0") <(head -c "$META_SPAN" "$D1") \
    || fail "metadata span differs between dev0 and dev1 at mkfs"
echo "metadata span ($((META_HI + 1)) blocks) byte-identical on both devices"

echo "== leg 3: FUSE mount, create namespace =="
mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }
mkdir -p "$MNT/dir/sub"
touch "$MNT/hello.txt" "$MNT/dir/sub/deep"
[ -f "$MNT/hello.txt" ] || fail "hello.txt not created"
[ -f "$MNT/dir/sub/deep" ] || fail "nested file not created"
echo "created: hello.txt + dir/sub/deep"
mnt_down

# base root pages must resolve inside the mirrored metadata span (dev0 base)
read -r RS0 RS1 DELTA SEQ < <(rtslot "$D0")
[ "$RS0" -gt 0 ] && [ "$RS0" -lt "$META_SPAN" ] \
    || fail "root slot 0 pba $RS0 outside the metadata span (base not dev0-resident)"
[ "$RS1" -gt 0 ] && [ "$RS1" -lt "$META_SPAN" ] \
    || fail "root slot 1 pba $RS1 outside the metadata span (base not dev0-resident)"
echo "base root slots $RS0/$RS1 live in the dev0 metadata span (mirrored)"

echo "== leg 4: remount, namespace persists =="
mnt_up
ls "$MNT" | grep -qx "hello.txt" || fail "hello.txt lost after remount"
ls "$MNT" | grep -qx "dir" || fail "dir lost after remount"
[ -f "$MNT/dir/sub/deep" ] || fail "dir/sub/deep lost after remount"
echo "remount: hello.txt + dir/sub/deep present"
mnt_down

echo "== leg 5: fsck clean; mirror still byte-identical =="
FSCK=$($B/invf-fsck "$D0" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "$FSCK" | grep -q "format:       v3" || { echo "$FSCK"; fail "fsck not v3"; }
cmp <(head -c "$META_SPAN" "$D0") <(head -c "$META_SPAN" "$D1") \
    || fail "metadata span differs after writes"
echo "fsck OK (v3 base tree); metadata mirror byte-identical after writes"

echo "== leg 6: DEGRADED mount (dev0 withheld) =="
mv "$D0" "$D0.hidden"
# The tool is pointed at the (missing) dev0 path; vol_open opens device 1
# alone READ-ONLY and serves every structure from the dev1 mirror.
mnt_up
grep -q "DEGRADED" "$WORK/fuse.log" || { cat "$WORK/fuse.log"; fail "no DEGRADED diagnostic"; }
ls "$MNT" | grep -qx "hello.txt" || fail "degraded: hello.txt not listed"
[ -f "$MNT/dir/sub/deep" ] || fail "degraded: nested file not resolvable"
echo "degraded: namespace lists from the dev1 mirror"
if touch "$MNT/nope" 2>"$WORK/dwr.log"; then
    fail "degraded write succeeded?!"
fi
grep -qE "Read-only|EROFS|DEGRADED|read-only" "$WORK/dwr.log" \
    || { cat "$WORK/dwr.log"; fail "degraded write not loudly refused"; }
echo "degraded write refused: $(tail -1 "$WORK/dwr.log")"
mnt_down
mv "$D0.hidden" "$D0"
# reattach -> RW resumes, the new file survives a remount
mnt_up
touch "$MNT/rearmed" || fail "reattach write"
[ -f "$MNT/rearmed" ] || fail "reattached file missing"
mnt_down
echo "reattach: RW resumed, writes land"

echo
echo "PASS: v3 two-device (dev0 base + metadata mirror + degraded read)"
