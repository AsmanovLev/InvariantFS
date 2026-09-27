#!/bin/bash
# test-nlink-audit.sh — WP118: nlink vs dirent fan-in.
#
# The blind spot both verification tools had (WP111b, commit 1771a0d): a
# rootfs with 7 names on disk reported 5. Two pairs of names pointed at one
# inode id each -- a name had landed on an inode it did not belong to, and
# the file that used to live under the older name was silently overwritten.
# `invf-verify --deep` walked live INODES, so it visited each id once and
# had no way to see the second name; `invf-fsck` printed OK.
#
# The invariant that sees it is not "inode ids must be unique" -- hardlinks
# share ids on purpose, so that check is false on every correct volume that
# uses `ln`. It is ACCOUNTING: for each live inode, the number of names that
# resolve to it must equal its nlink.
#
# Legs:
#   A  POSITIVE CONTROL. A volume built through FUSE with real hardlinks
#      (several names on one inode, plus a cross-directory link) must be
#      reported CLEAN by invf-fsck (exit 0) and by invf-verify --deep. If
#      this leg fails, the check is wrong and nothing else here matters.
#   B  NEGATIVE CONTROL. The aliasing is injected at the dirent level --
#      exactly the on-disk state the WP111b bug left, which the fixed write
#      path can no longer produce by itself. invf-fsck must exit non-zero,
#      say DAMAGED, and name the inode id with its discrepancy;
#      invf-verify --deep must not claim "0 corrupt".
#   C  Removing the intruder name puts the accounting back and both tools
#      return to OK -- the verdict tracks the volume, not a latch.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-nlink-audit.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/nlinkaudit
IMG=nlinkaudit.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT"
cd /dev/shm
rm -f "$IMG" "$IMG.fixed"

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {
    $B/invf-fuse "$IMG" "$MNT" -o attr_t=0 2>"$WORK/fuse.log"
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

echo "== WP118: nlink vs dirent fan-in (hardlink control + alias detection) =="

# ---------------------------------------------------------------- leg A
echo "-- leg A: POSITIVE CONTROL, a volume with real hardlinks --"
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "mkfs failed"; }

head -c 200000 /dev/urandom >"$WORK/a.bin"
head -c 150000 /dev/urandom >"$WORK/b.bin"

mnt_up
cp "$WORK/a.bin" "$MNT/a.bin"
cp "$WORK/b.bin" "$MNT/b.bin"
mkdir "$MNT/sub"
cp "$WORK/a.bin" "$MNT/sub/c.bin"
ln "$MNT/a.bin" "$MNT/a-link.bin"
ln "$MNT/a.bin" "$MNT/a-link2.bin"
ln "$MNT/b.bin" "$MNT/sub/b-in-subdir.bin"
[ "$(stat -c %h "$MNT/a.bin")" = "3" ] || fail "a.bin nlink != 3 (three names)"
[ "$(stat -c %h "$MNT/sub/b-in-subdir.bin")" = "2" ] \
    || fail "cross-directory link: b nlink != 2"
mnt_down

# 7 names (6 files + the sub/ directory) over 3 inodes: THREE names on
# a.bin, two on b.bin, one on sub/c.bin. A hardlink is NOT a fault, and
# saying so here is the whole point -- a check of the form "no two names
# may share an inode id" fails on this very volume.
AUDIT=$($B/invf-v3inode "$IMG" nlink audit 2>/dev/null) \
    || { echo "$AUDIT"; fail "the audit rejected a volume with hardlinks"; }
echo "$AUDIT" | grep -q "verdict=OK" \
    || { echo "$AUDIT"; fail "hardlink volume did not audit OK"; }
echo "$AUDIT" | grep -q "names=7 inodes=3" \
    || { echo "$AUDIT"; fail "expected 7 names over 3 inodes"; }
echo "  audit: $AUDIT"

FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero on a clean hardlink volume"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck did not say OK"; }
echo "$FSCK" | grep -q "nlink/fan-in:  ok" \
    || { echo "$FSCK"; fail "fsck did not report the fan-in accounting as ok"; }
echo "  fsck: OK ($(echo "$FSCK" | sed -n 's/^  names\/inodes: *//p'))"

DEEP=$($B/invf-verify "$IMG" --deep 2>/dev/null) \
    || { echo "$DEEP"; fail "verify --deep exited nonzero on a clean hardlink volume"; }
echo "$DEEP" | grep -q "nlink/fan-in: ok" \
    || { echo "$DEEP"; fail "verify --deep did not report the fan-in as ok"; }
echo "  verify --deep: $(echo "$DEEP" | tail -1)"

# Keep a pristine copy: this is the "fixed" side of the before/after pair.
cp "$IMG" "$IMG.fixed"

# ---------------------------------------------------------------- leg B
echo "-- leg B: NEGATIVE CONTROL, an aliased name on a live inode --"
# a.bin is inode id 2 on a fresh volume, but do not hardcode it: ask.
AID=$($B/invf-ls "$IMG" 2>/dev/null | sed -n 's/.*inode \([0-9]*\)  a\.bin$/\1/p')
[ -n "$AID" ] || fail "could not determine a.bin's engine inode id"
echo "  a.bin is engine inode $AID"

# The corruption the WP111b bug left on disk: a second NAME on an inode
# whose nlink was not bumped. `dirent put` deliberately does not touch the
# link count -- doing the accounting is the caller's job, and the failure
# being reproduced is precisely a caller that forgot.
$B/invf-v3inode "$IMG" dirent put 1 intruder.bin "$AID" >/dev/null \
    || fail "could not inject the aliased dirent"
echo "  injected: dirent (1, intruder.bin) -> inode $AID, nlink untouched"

set +e
AUDIT=$($B/invf-v3inode "$IMG" nlink audit 2>/dev/null); ARC=$?
set -e
[ "$ARC" -ne 0 ] || { echo "$AUDIT"; fail "the audit passed an aliased volume"; }
echo "$AUDIT" | grep -q "verdict=MISMATCH" \
    || { echo "$AUDIT"; fail "no MISMATCH verdict"; }
# a.bin carries three legitimate names, so its row says nlink 3 and the
# injected name makes four: the discrepancy is (3, 4), by inode id.
echo "$AUDIT" | grep -q "id=$AID nlink=3 fanin=4" \
    || { echo "$AUDIT"; fail "the offender is not named with its discrepancy"; }
echo "  audit: $AUDIT"

set +e
FSCK=$($B/invf-fsck "$IMG" 2>&1); FSCK_RC=$?
set -e
[ "$FSCK_RC" -ne 0 ] || { echo "$FSCK"; fail "fsck exited 0 on an aliased volume"; }
echo "$FSCK" | grep -q "^DAMAGED$" || { echo "$FSCK"; fail "fsck did not say DAMAGED"; }
echo "$FSCK" | grep -q "nlink/fan-in:  1 inode(s) DO NOT BALANCE" \
    || { echo "$FSCK"; fail "fsck did not report the imbalance"; }
# the name in the message is whichever name the walk reached first on that
# inode -- all three legitimate ones are equally true -- so assert on the
# inode id and the two numbers, which are the finding.
echo "$FSCK" | grep -q "nlink/fan-in: inode $AID (\S*): nlink 3, 4 name(s) resolve to it" \
    || { echo "$FSCK"; fail "fsck did not name inode $AID with its discrepancy"; }
echo "  fsck: exit $FSCK_RC, $(echo "$FSCK" | tail -1)"

set +e
DEEP=$($B/invf-verify "$IMG" --deep 2>/dev/null); DEEP_RC=$?
set -e
[ "$DEEP_RC" -ne 0 ] || { echo "$DEEP"; fail "verify --deep exited 0 on an aliased volume"; }
echo "$DEEP" | grep -q " 0 corrupt," \
    && { echo "$DEEP"; fail "verify --deep still reports 0 corrupt"; }
echo "$DEEP" | grep -q "CORRUPT: inode $AID" \
    || { echo "$DEEP"; fail "verify --deep did not name inode $AID"; }
echo "  verify --deep: exit $DEEP_RC, $(echo "$DEEP" | tail -1)"

# -f must not pretend to have fixed it: the intruder is not decidable from
# the volume, and a quiet OK after -f would be the WP111b lie all over again.
set +e
FIXED=$($B/invf-fsck "$IMG" -f 2>&1); FIXED_RC=$?
set -e
[ "$FIXED_RC" -ne 0 ] || { echo "$FIXED"; fail "fsck -f claimed to have repaired the aliasing"; }
echo "$FIXED" | grep -q "^DAMAGED$" || { echo "$FIXED"; fail "fsck -f reported OK"; }
echo "  fsck -f: exit $FIXED_RC, $(echo "$FIXED" | tail -1) (not repairable, and says so)"

# ---------------------------------------------------------------- leg C
echo "-- leg C: removing the intruder restores the verdict --"
$B/invf-v3inode "$IMG" dirent del 1 intruder.bin >/dev/null \
    || fail "could not remove the aliased dirent"
$B/invf-v3inode "$IMG" nlink audit >/dev/null 2>&1 \
    || fail "the audit still fails after the intruder is gone"
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck is not OK again"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck did not return to OK"; }
DEEP=$($B/invf-verify "$IMG" --deep 2>/dev/null) || fail "verify --deep is not clean again"
echo "  back to clean: fsck OK, verify --deep $(echo "$DEEP" | tail -1)"

# the before/after pair, stated as the two images they are
cmp -s "$IMG" "$IMG.fixed" >/dev/null 2>&1 \
    && echo "  note: the repaired volume is byte-identical to the clean one"
rm -f "$IMG.fixed"

echo "ALL NLINK-AUDIT LEGS PASS"
