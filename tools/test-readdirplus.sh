#!/bin/bash
# test-readdirplus.sh -- readdirplus attrs e2e (daily-driver scan speed).
#
# The daemon fills each readdir entry's stat inline (FUSE_FILL_DIR_PLUS)
# from the same helpers getattr uses, so scanners pay zero LOOKUP+GETATTR
# round trips per entry. Legs assert CORRECTNESS (the mapping must be
# what getattr would answer -- reg/dir/symlink types, mode/uid/size):
# the round-trip elimination itself is structural (proven by handler
# tracing: 0 getattrs over `ls -l` + `find` on 200 files) and timing
# lives outside CI because TCG-adjacent hosts make it flaky.
#
# Leg 1: fixture (reg with known bytes/mode, subdir, symlink) via
#   offline import; stat(1) through the mount matches expectations.
# Leg 2: ls -lR output names every entry with the right type letter.
# Leg 3: fsck clean (readdir never mutates).
#
# Uses /dev/shm (tmpfs) like the other suites; RELATIVE image paths
# (blkio treats /dev/* as raw devices). Needs a FUSE mount.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/rdplus
IMG=rdplus.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/src/sub"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }
mnt_up() {
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $1 never appeared"
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
fsck_ok() { $B/invf-fsck "$1" | grep -q "^OK$" || fail "fsck not clean: $1"; }
cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
}
trap cleanup EXIT

printf 'twelve bytes' > "$WORK/src/a.txt"
chmod 640 "$WORK/src/a.txt"
printf 'sub contents here' > "$WORK/src/sub/b.txt"
ln -s a.txt "$WORK/src/link"
$B/invf-mkfs "$IMG" 64 >/dev/null
$B/invf-import "$IMG" "$WORK/src" >/dev/null
mnt_up "$IMG"

echo "== [1] stat through the mount =="
[ "$(stat -c '%s %a %F' "$MNT/a.txt")" = "12 640 regular file" ] \
    || fail "leg1: stat a.txt: $(stat -c '%s %a %F' "$MNT/a.txt")"
[ "$(stat -c '%F' "$MNT/sub")" = "directory" ] \
    || fail "leg1: sub is not a directory"
[ "$(stat -c '%F' "$MNT/link")" = "symbolic link" ] \
    || fail "leg1: link is not a symlink"
[ "$(readlink "$MNT/link")" = "a.txt" ] || fail "leg1: link target"

echo "== [2] ls -lR sees every entry with the right type letter =="
ls -lR "$MNT" > "$WORK/ls.txt"
grep -q "^-rw-r-----.*a.txt" "$WORK/ls.txt" || fail "leg2: a.txt line"
grep -q "^d.* sub$" "$WORK/ls.txt" || fail "leg2: sub line"
grep -q "^l.* link -> a.txt" "$WORK/ls.txt" || fail "leg2: link line"
[ "$(find "$MNT" -type f | wc -l)" = "2" ] || fail "leg2: find file count"

mnt_down
echo "== [3] fsck clean =="
fsck_ok "$IMG"

rm -f "$IMG"
echo
echo "READDIRPLUS E2E: PASS"
