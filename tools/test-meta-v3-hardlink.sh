#!/bin/bash
# test-meta-v3-hardlink.sh — WP-M17 e2e: hardlinks via the inode nlink field.
#
# A VOLF_V3 volume must let many (parent, name) dirents share one inode row.
# `ln` inserts a second dirent and increments nlink; `unlink` drops the dirent
# and decrements, and only the last unlink deletes the row (and, through
# vol_v3_inode_delete, its xattr keys). getattr exposes nlink. Everything must
# survive unmount/remount. v3 nodes are still empty (recipes are WP-M8), so
# the "content" that must stay alive is the shared inode row + its xattrs.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-hardlink.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3hardlink
IMG=metav3hardlink.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT"
cd /dev/shm
rm -f "$IMG"

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

nlink_of() { stat -c %h "$1"; }

echo "== WP-M17: v3 hardlinks (link/nlink/unlink-at-zero/xattr cascade/remount) =="

# --- mkfs v3 -------------------------------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"

mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }

# --- first namespace object, then calibrate its engine inode id ----------
# The id allocator hands ids out sequentially from 2 on a fresh volume, but
# don't hardcode: scan the offline inode tree for the single row and remember
# it, so the "last unlink freed the inode" check is exact.
touch "$MNT/orig"
mnt_down
FID=""
for id in $(seq 2 16); do
    if $B/invf-v3inode "$IMG" get "$id" >/dev/null 2>&1; then
        [ -z "$FID" ] || fail "calibration: several inode rows present"
        FID=$id
    fi
done
[ -n "$FID" ] || fail "calibration: no inode row for orig"
echo "calibration: orig is engine inode $FID"
mnt_up

# --- two names share one inode; nlink == 2 -------------------------------
ln "$MNT/orig" "$MNT/alias"
[ "$(nlink_of "$MNT/orig")" = "2" ] || fail "orig nlink after link != 2"
[ "$(nlink_of "$MNT/alias")" = "2" ] || fail "alias nlink after link != 2"
python3 - "$MNT" <<'PY' || fail "link did not share the inode"
import os, sys
M = sys.argv[1]
os.setxattr(M + "/orig", "user.tag", b"shared")
assert os.getxattr(M + "/alias", "user.tag") == b"shared", \
    "xattr set on orig not visible through alias: not a shared inode"
assert sorted(os.listxattr(M + "/alias")) == ["user.tag"], "alias xattr list"
print("  link: nlink==2, xattr on orig is visible through alias")
PY

# --- a third link, then drop the middle name -----------------------------
ln "$MNT/orig" "$MNT/third"
[ "$(nlink_of "$MNT/orig")" = "3" ] || fail "orig nlink after 2nd link != 3"
[ "$(nlink_of "$MNT/third")" = "3" ] || fail "third nlink after 2nd link != 3"
rm "$MNT/orig"
[ ! -e "$MNT/orig" ] || fail "unlink: orig still present"
[ "$(nlink_of "$MNT/alias")" = "2" ] || fail "alias nlink after rm orig != 2"
[ "$(nlink_of "$MNT/third")" = "2" ] || fail "third nlink after rm orig != 2"
python3 - "$MNT" <<'PY' || fail "survivors lost the shared xattr"
import os, sys
M = sys.argv[1]
for n in ("alias", "third"):
    assert os.getxattr(M + "/" + n, "user.tag") == b"shared", \
        "%s lost the shared xattr" % n
print("  three links: rm middle -> nlink 2, xattr intact on both survivors")
PY

# --- remount: nlink and the shared xattr persist -------------------------
mnt_down
mnt_up
[ "$(nlink_of "$MNT/alias")" = "2" ] || fail "alias nlink after remount != 2"
[ "$(nlink_of "$MNT/third")" = "2" ] || fail "third nlink after remount != 2"
python3 - "$MNT" <<'PY' || fail "hardlink state lost across remount"
import os, sys
M = sys.argv[1]
for n in ("alias", "third"):
    assert os.getxattr(M + "/" + n, "user.tag") == b"shared", \
        "%s xattr lost on remount" % n
print("  remount: nlink==2 and the shared xattr survive both names")
PY

# --- unlink down to the last name, then the last ------------------------
rm "$MNT/third"
[ "$(nlink_of "$MNT/alias")" = "1" ] || fail "alias nlink after rm third != 1"
python3 - "$MNT" <<'PY' || fail "last survivor lost its xattr"
import os, sys
M = sys.argv[1]
assert os.getxattr(M + "/alias", "user.tag") == b"shared", "xattr lost at nlink 1"
print("  unlink: nlink 1, survivor and its xattr still intact")
PY

rm "$MNT/alias"
[ ! -e "$MNT/alias" ] || fail "last unlink: alias still present"
# a second unlink of the dead name must fail, not corrupt a count
if rm "$MNT/alias" 2>/dev/null; then fail "double unlink of a dead name succeeded"; fi
# a fresh file recreated at the name must be clean (no inherited xattr)
touch "$MNT/alias"
python3 - "$MNT" <<'PY' || fail "recreated name inherited dead xattrs"
import os, sys
assert os.listxattr(sys.argv[1] + "/alias") == [], "recreated file has xattrs"
print("  last unlink: name gone, second unlink refused, recreated name clean")
PY
rm "$MNT/alias"

# --- directories cannot be hardlinked (EPERM) ----------------------------
mkdir "$MNT/d"
if ln "$MNT/d" "$MNT/d2" 2>/dev/null; then fail "hardlink of a directory succeeded"; fi
[ ! -e "$MNT/d2" ] || fail "failed dir link left a stray name"
echo "  dirs: hardlink refused (EPERM), no stray dirent"

# --- cross-directory link ------------------------------------------------
mkdir "$MNT/sub"
touch "$MNT/sub/f"
ln "$MNT/sub/f" "$MNT/sub/g"
[ "$(nlink_of "$MNT/sub/f")" = "2" ] || fail "cross-dir link: f nlink != 2"
[ "$(nlink_of "$MNT/sub/g")" = "2" ] || fail "cross-dir link: g nlink != 2"
echo "  cross-dir link: two names in sub/ share one inode"

# --- rename overwrite drops one link, the other name survives ------------
# The victim is hardlinked (nlink 2); renaming a new file onto one of its
# names must decrement the victim's count, not retire its row.
touch "$MNT/sub/victim"
ln "$MNT/sub/victim" "$MNT/sub/victim2"
touch "$MNT/sub/src"
[ "$(nlink_of "$MNT/sub/victim")" = "2" ] || fail "victim nlink before overwrite != 2"
mv "$MNT/sub/src" "$MNT/sub/victim"
[ ! -e "$MNT/sub/src" ] || fail "rename overwrite: source still present"
[ -e "$MNT/sub/victim" ] || fail "rename overwrite: target missing"
[ "$(nlink_of "$MNT/sub/victim")" = "1" ] || fail "rename overwrite: victim nlink != 1"
[ -e "$MNT/sub/victim2" ] || fail "rename overwrite freed the victim's other link"
echo "  rename overwrite: victim link count dropped, other name intact"

mnt_down

# --- offline sanity: fsck clean, and the last unlink freed the inode -----
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "fsck: OK"
if $B/invf-v3inode "$IMG" get "$FID" >/dev/null 2>&1; then
    fail "engine inode $FID still present after its last name was unlinked"
fi
echo "inode $FID: gone after nlink reached 0 (xattr cascade ran)"

echo "ALL META-V3 HARDLINK LEGS PASS"
