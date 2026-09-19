#!/bin/bash
# test-meta-v3-dirent.sh — WP-M6 e2e: the metadata-v3 dirent tree + namespace.
#
# A VOLF_V3 volume must expose a real namespace through FUSE: create files
# and directories, look them up by name (getattr/stat), readdir in sorted
# order, unlink removes, rename moves (files and directories), and every one
# of those survives unmount/remount. v3 nodes are empty (content/recipe
# blobs are WP-M8), so files are created with touch.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-dirent.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3dirent
IMG=metav3dirent.img
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

# list_sorted <dir> : print the directory's entries, sorted, space-separated
list_sorted() {
    python3 - "$1" <<'PY'
import os, sys
print(" ".join(sorted(os.listdir(sys.argv[1]))))
PY
}

# expect_list <dir> <expected space-separated sorted entries> <label>
expect_list() {
    local got
    got=$(list_sorted "$1")
    [ "$got" = "$2" ] || fail "$3: readdir gave [$got], want [$2]"
    echo "  readdir $3: [$got]"
}

echo "== WP-M6: v3 dirent tree + namespace (create/lookup/readdir/unlink/rename) =="

# --- mkfs v3 -------------------------------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"

mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }

# --- create files and directories ---------------------------------------
mkdir -p "$MNT/dir/sub"
touch "$MNT/zeta" "$MNT/alpha" "$MNT/mid"
touch "$MNT/dir/b.txt" "$MNT/dir/a.txt" "$MNT/dir/sub/deep"
echo "created: 2 dirs (dir/sub) + 6 files"

# A large directory (page-boundary crossing) must read back complete and
# ordered -- the v2 dir-hash replacement this key range scan is for.
python3 - "$MNT/big" <<'PY' || fail "large directory leg failed"
import os, sys
d = sys.argv[1]
os.mkdir(d)
for i in range(1000):
    open(os.path.join(d, "f%04d" % i), "w").close()
names = sorted(os.listdir(d))
assert len(names) == 1000, "readdir returned %d of 1000" % len(names)
assert names[0] == "f0000" and names[-1] == "f0999", names[:2] + names[-2:]
print("  large dir: 1000 entries read back complete and ordered")
PY

# --- readdir is ordered (sorted by name) --------------------------------
expect_list "$MNT" "alpha big dir mid zeta" "root"
expect_list "$MNT/dir" "a.txt b.txt sub" "dir"
expect_list "$MNT/dir/sub" "deep" "dir/sub"

# --- lookup by name (getattr / stat) ------------------------------------
[ -f "$MNT/alpha" ]  || fail "lookup: alpha is not a regular file"
[ -d "$MNT/dir" ]    || fail "lookup: dir is not a directory"
[ -d "$MNT/dir/sub" ]|| fail "lookup: dir/sub is not a directory"
[ -f "$MNT/dir/a.txt" ] || fail "lookup: dir/a.txt is not a regular file"
[ ! -e "$MNT/nope" ] || fail "lookup: nonexistent name resolved"
SZ=$(stat -c %s "$MNT/alpha")
[ "$SZ" = "0" ] || fail "lookup: alpha size $SZ, want 0"
echo "lookup: files/dirs resolve by name, empty file size 0"

# --- unlink removes ------------------------------------------------------
rm "$MNT/mid"
[ ! -e "$MNT/mid" ] || fail "unlink: mid still present"
expect_list "$MNT" "alpha big dir zeta" "root after unlink"
echo "unlink: mid removed"

# --- rename a file (same directory and cross-directory) ------------------
mv "$MNT/alpha" "$MNT/omega"
[ ! -e "$MNT/alpha" ] || fail "rename: alpha survived the move"
[ -f "$MNT/omega" ] || fail "rename: omega missing"
mv "$MNT/dir/b.txt" "$MNT/omega"     # POSIX replace an existing file
[ ! -e "$MNT/dir/b.txt" ] || fail "rename replace: source survived"
[ -f "$MNT/omega" ] || fail "rename replace: target missing"
expect_list "$MNT" "big dir omega zeta" "root after renames"
echo "rename: file moved and replacement target handled"

# --- rename a directory (children move with it) -------------------------
mv "$MNT/dir" "$MNT/renamed"
[ ! -e "$MNT/dir" ] || fail "dir rename: old name survived"
[ -d "$MNT/renamed/sub" ] || fail "dir rename: child dir lost"
[ -f "$MNT/renamed/a.txt" ] || fail "dir rename: child file lost"
expect_list "$MNT/renamed" "a.txt sub" "renamed"
echo "rename: directory moved with its children"

# --- remount: everything persists ---------------------------------------
mnt_down
mnt_up
expect_list "$MNT" "big omega renamed zeta" "root after remount"
expect_list "$MNT/renamed" "a.txt sub" "renamed after remount"
expect_list "$MNT/renamed/sub" "deep" "renamed/sub after remount"
[ -f "$MNT/renamed/sub/deep" ] || fail "remount: nested file lost"
[ -f "$MNT/omega" ] || fail "remount: renamed file lost"
[ "$(ls "$MNT/big" | wc -l)" = "1000" ] || fail "remount: big dir incomplete"
echo "remount: namespace identical"

# --- create more after remount (the id counter must not collide) --------
touch "$MNT/after-remount"
[ -f "$MNT/after-remount" ] || fail "post-remount create failed"
expect_list "$MNT" "after-remount big omega renamed zeta" "root after post-remount create"
echo "remount: post-remount create has a fresh inode"

mnt_down

# --- offline sanity: fsck accepts the base tree -------------------------
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "fsck: OK"

echo "ALL META-V3 DIRENT LEGS PASS"
