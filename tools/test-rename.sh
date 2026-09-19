#!/bin/bash
# test-rename.sh — WP65 e2e: rename through a mounted FUSE volume.
#
#   Leg A (renameat2 flags): RENAME_NOREPLACE onto a live destination
#   returns EEXIST and moves/clobbers nothing; onto a free name it
#   succeeds; RENAME_EXCHANGE / RENAME_WHITEOUT return EOPNOTSUPP (not
#   EINVAL). fsck stays clean.
#
#   Leg B (live sweep checkpoint): a bare offline invf-sweep leaves a
#   live CKP0 checkpoint. After remount, a rename through the mount must
#   REALIZE that checkpoint (the point of no return) and succeed
#   bit-exact; afterwards no checkpoint survives (invf-rollback reports
#   "no checkpoint") and fsck/verify are clean.
#
# Mirrors the test-writepath.sh mount conventions. Run from the repo
# root after `make`; the e2e runner serialises /dev/shm state.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WORK=/dev/shm/wp65rename
IMG=wp65-rename.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {
    $B/invf-fuse "$IMG" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    cat "$WORK/fuse.log" >&2
    fail "mount of $IMG never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 100); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 100); do
        pgrep -f "invf-fuse $IMG" >/dev/null || return 0
        sleep 0.1
    done
    fail "invf-fuse daemon did not exit after unmount"
}

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
}
trap cleanup EXIT

# renameat2 driver: prints the errno (0 on success) and always exits 0.
cat > "$WORK/rn.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    errno = 0;
    if (renameat2(AT_FDCWD, argv[1], AT_FDCWD, argv[2],
                  (unsigned)strtoul(argv[3], NULL, 0)) == 0)
        printf("0\n");
    else
        printf("%d\n", errno);
    return 0;
}
EOF
cc -O2 -o "$WORK/rn" "$WORK/rn.c" || fail "rn helper build"
RN="$WORK/rn"

echo "== leg A: renameat2 flags through the mount =="
$B/invf-mkfs "$IMG" 0.0625 >/dev/null
mnt_up
printf 'AAAA' > "$MNT/a"
printf 'BBBB' > "$MNT/b"
# NOREPLACE onto a live file: EEXIST, both untouched
[ "$($RN "$MNT/a" "$MNT/b" 1)" = "17" ] || fail "NOREPLACE existing not EEXIST"
[ "$(cat "$MNT/a")" = "AAAA" ] || fail "NOREPLACE moved the source"
[ "$(cat "$MNT/b")" = "BBBB" ] || fail "NOREPLACE clobbered the destination"
# NOREPLACE onto a free name: succeeds, bit-exact
[ "$($RN "$MNT/a" "$MNT/c" 1)" = "0" ] || fail "NOREPLACE free failed"
[ "$(cat "$MNT/c")" = "AAAA" ] || fail "NOREPLACE free not bit-exact"
[ -e "$MNT/a" ] && fail "NOREPLACE left the source behind"
# EXCHANGE / WHITEOUT: EOPNOTSUPP (was EINVAL)
[ "$($RN "$MNT/c" "$MNT/b" 2)" = "95" ] || fail "RENAME_EXCHANGE not EOPNOTSUPP"
[ "$($RN "$MNT/c" "$MNT/b" 4)" = "95" ] || fail "RENAME_WHITEOUT not EOPNOTSUPP"
mnt_down
$B/invf-fsck "$IMG" >"$WORK/fsck-a.log" 2>&1 || true
grep -q "^OK$" "$WORK/fsck-a.log" || { cat "$WORK/fsck-a.log"; fail "fsck A not OK"; }
echo "   NOREPLACE=EEXIST+no-clobber, free=ok, EXCHANGE/WHITEOUT=EOPNOTSUPP, fsck OK"

echo "== leg B: rename under a live sweep checkpoint realizes it =="
# a bare sweep leaves a live CKP0 (its rollback net)
$B/invf-sweep "$IMG" >"$WORK/sweep.log" 2>&1 || { cat "$WORK/sweep.log"; fail "sweep"; }
mnt_up
printf 'checkpoint-data' > "$MNT/old"
sync
mv "$MNT/old" "$MNT/new" || fail "rename under a live checkpoint failed"
[ -e "$MNT/old" ] && fail "source name survived the rename"
[ "$(cat "$MNT/new")" = "checkpoint-data" ] || fail "renamed content mismatch"
mnt_down
# no checkpoint survives: invf-rollback rc=1 == "no checkpoint"
set +e
$B/invf-rollback "$IMG" >"$WORK/rb.log" 2>&1
RB=$?
set -e
[ $RB -eq 1 ] || { cat "$WORK/rb.log"; fail "checkpoint still live (rollback rc=$RB)"; }
$B/invf-fsck "$IMG" >"$WORK/fsck-b.log" 2>&1 || true
grep -q "^OK$" "$WORK/fsck-b.log" || { cat "$WORK/fsck-b.log"; fail "fsck B not OK"; }
$B/invf-verify "$IMG" --deep >"$WORK/verify-b.log" 2>&1 || { cat "$WORK/verify-b.log"; fail "verify B"; }
grep -q " 0 corrupt," "$WORK/verify-b.log" || fail "verify B reported corrupt"
echo "   rename realized the checkpoint, bit-exact, none left, fsck/verify clean"

echo "RENAME E2E: PASS"
