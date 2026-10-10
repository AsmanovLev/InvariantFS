#!/bin/bash
# test-verify-size.sh — invf-verify backing-size tolerance (leg-8 finding).
#
# A volume is a PREFIX of its backing: mkfs with an explicit smaller size
# on a bigger device (or a device swapped for a larger one) is legal --
# blocks past total_blocks are never allocated. verify must tolerate a
# LARGER backing and refuse only a SMALLER one (truncation). Demanding
# equality false-alarmed prefix volumes as corrupt (rc=1 with 0 corrupt
# files); flakey leg 8 arm C tripped over exactly that.
#
# Hermetic: images under /dev/shm (relative paths); no mount, no daemon.
set -e
set -o pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
B=$REPO/bin
WORK=/dev/shm/wpverify
IMG=wpverify.img

fail() { echo "FAIL: $1" >&2; exit 1; }
rm -rf "$WORK" && mkdir -p "$WORK" && cd "$WORK"

echo "== [1] baseline: exact-size backing verifies =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null || fail "mkfs"
head -c 100000 /dev/urandom > a.bin
$B/invf-cp "$IMG" a.bin a.bin >/dev/null || fail "cp"
$B/invf-verify "$IMG" --deep > verify1.log 2>&1 || fail "baseline verify rc=$?"
grep -q "0 corrupt" verify1.log || fail "baseline not clean"

echo "== [2] larger backing: prefix volume still verifies (the regression) =="
truncate -s 1G "$IMG"
$B/invf-verify "$IMG" --deep > verify2.log 2>&1 || fail "grown-backing verify rc=$?"
grep -q "0 corrupt" verify2.log || fail "grown backing not clean"
$B/invf-cat "$IMG" a.bin a.out >/dev/null || fail "cat after grow"
cmp a.bin a.out || fail "not bit-exact on grown backing"
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean on grown backing"
echo "  0.5G volume on 1G backing: verify clean, bit-exact"

echo "== [3] smaller backing: truncation still refused =="
cp "$IMG" trunc.img
truncate -s 256M trunc.img
if $B/invf-verify trunc.img --deep > verify3.log 2>&1; then
    fail "truncated backing verified (must refuse)"
fi
grep -q "smaller than superblock" verify3.log \
    || { cat verify3.log; fail "truncation refused without saying why"; }
echo "  256M backing under 0.5G volume: refused with a diagnostic"

rm -f "$IMG" trunc.img a.bin a.out verify1.log verify2.log verify3.log
echo
echo "VERIFY-SIZE E2E: PASS"
