#!/bin/bash
# test-meta-inode.sh — WP-M5 e2e: the metadata-v3 inode tree.
#
# A v3 volume must be writable enough to create a regular file's inode and
# stat it, and that inode must survive unmount/remount. WP-M6 (dirents) is
# not landed yet, so there is no name -> inode resolution on a v3 volume and
# this leg drives the inode tree by id with invf-vnode (the offline
# counterpart of a FUSE create/stat). A real FUSE mount/unmount round-trip
# is included to prove a populated v3 base tree still mounts cleanly.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-inode.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metavnode
IMG=metavnode.img
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

# get_ok <img> <id> <expected substring of the "found"/"stat" lines>
get_ok() {
    local out
    out=$("$B/invf-vnode" "$1" get "$2") \
        || { echo "$out" >&2; fail "inode $2 absent (want present)"; }
    echo "$out" | grep -q "$3" \
        || { echo "$out" >&2; fail "inode $2 fields: missing '$3'"; }
}

# get_absent <img> <id>
get_absent() {
    if "$B/invf-vnode" "$1" get "$2" >"$WORK/absent.out" 2>&1; then
        cat "$WORK/absent.out" >&2
        fail "inode $2 present (want absent)"
    fi
}

echo "== WP-M5: v3 inode tree (create -> stat -> remount) =="

# --- mkfs v3 -------------------------------------------------------------
$B/invf-mkfs "$IMG" 0.3 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "mkfs failed"; }
grep -q "format: v0 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v0 format"

# --- create a regular file's inode and stat it ---------------------------
$B/invf-vnode "$IMG" put 42 0 640 1000 1000 0 >"$WORK/put.log" 2>&1 \
    || { cat "$WORK/put.log"; fail "put inode 42 failed"; }
get_ok "$IMG" 42 "type=0 mode=0640 uid=1000 gid=1000 nlink=1 size=0"
echo "created: inode 42 (REG 0640 uid=1000 gid=1000) stat ok"

# --- FUSE round-trip: a populated base tree still mounts cleanly ---------
mnt_up
grep -q "format v0" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v0 open path"; }
LS=$(ls -A "$MNT") || { cat "$WORK/fuse.log"; fail "cannot list v3 mount"; }
[ -z "$LS" ] || { echo "unexpected entries: $LS"; fail "v3 root not empty (no dirents yet)"; }
mnt_down
get_ok "$IMG" 42 "type=0 mode=0640 uid=1000 gid=1000 nlink=1 size=0"
echo "remount: inode 42 survived a FUSE mount/unmount round-trip"

# --- setattr persists (mode/uid/gid/size) --------------------------------
$B/invf-vnode "$IMG" put 42 0 600 1001 1002 1234 >/dev/null 2>&1 \
    || fail "setattr put inode 42 failed"
get_ok "$IMG" 42 "type=0 mode=0600 uid=1001 gid=1002 nlink=1 size=1234"
echo "setattr: mode/uid/gid/size updated in the row"

# --- a second and third inode (different types) + physical delete --------
$B/invf-vnode "$IMG" put 43 2 777 0 0 0 >/dev/null 2>&1 \
    || fail "put symlink inode 43 failed"
$B/invf-vnode "$IMG" put 44 1 755 0 0 0 >/dev/null 2>&1 \
    || fail "put dir inode 44 failed"
get_ok "$IMG" 43 "type=2 mode=0777"
get_ok "$IMG" 44 "type=1 mode=0755"
$B/invf-vnode "$IMG" del 43 >/dev/null 2>&1 || fail "delete inode 43 failed"
get_absent "$IMG" 43
get_ok "$IMG" 42 "mode=0600 uid=1001 gid=1002 nlink=1 size=1234"
get_ok "$IMG" 44 "type=1 mode=0755"
echo "tree: 3 inodes created, 43 deleted, survivors intact"

# --- offline reopen + fsck sanity, then a final remount -------------------
get_ok "$IMG" 42 "mode=0600 uid=1001 gid=1002 nlink=1 size=1234"
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
mnt_up
mnt_down
get_ok "$IMG" 42 "mode=0600 uid=1001 gid=1002 nlink=1 size=1234"
get_ok "$IMG" 44 "type=1 mode=0755"

# after all round-trips the superblock must still read CLEAN
python3 - "$IMG" <<'PY' || fail "volume not CLEAN at the end"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN after all round-trips")
PY

echo "ALL META INODE LEGS PASS"
