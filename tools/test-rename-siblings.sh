#!/bin/bash
# test-rename-siblings.sh — a container's members follow it on rename.
#
# WHAT THIS SUITE EXISTS FOR (leg5 soak, 2026-11-26)
#
# Container members are minted as "<container>!<tag>" and the READ path derives
# the member name from the container's CURRENT name (`vol_read.c`: the TARR /
# FLACR part lookup builds "%s!part%u" from the live record name). So a rename
# that moved the container and left its members behind did two things at once:
#
#   * the container stopped decoding — every read EIOs, while its row and its
#     bytes are still allocated (a SILENT loss until somebody reads it);
#   * the leftovers became internal names with no parent, i.e. the GHOST the
#     power-loss soak reported (`t.tar!part0` on the volume, never written; the
#     parent had been renamed to r13.bin by an ordinary `mv` in UP mode).
#
# Measured before the fix, on a HEALTHY device with no fault injection at all:
# `mv t.tar x.tar` succeeded, `invf-cat x.tar` returned different bytes than the
# original, `invf-verify --deep` reported `CORRUPT: x.tar`, and `t.tar!part0`
# stayed behind. That is a plain rename, which is why the fix belongs in
# vol_rename and not in the recovery ladder.
#
# LEGS
#   1  in-place rename of a TARR container: new name reads byte-identical, the
#      OLD member name is gone, the NEW one is on the volume, offline cat
#      matches the host bytes, verify --deep 0 corrupt.
#   2  cross-directory move: members move to the destination directory.
#   3  a DIRECTORY rename needs no cascade (members are entries inside it) and
#      must keep working — the control that the fix did not touch that path.
#   4  a plain file rename is unchanged (no members to move).
#   5  fsck clean at the end; no `!`-shaped name is left without its parent.
#
# Run from the repo root after `make`:  bash tools/test-rename-siblings.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=${INVFS_BIN:-$REPO/bin}
WORK=/dev/shm/rensib
IMG=rensib.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT" "$WORK/d"
cd /dev/shm
rm -f "$IMG"

. "$REPO/tools/fsck-clean.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 60); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount never appeared"
}
mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 100); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $IMG" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}
sha() { sha256sum < "$1" | cut -d' ' -f1; }
onvol() { $B/invf-ls "$IMG" 2>/dev/null | awk '{print $NF}'; }

echo "== fixture: one TARR container + a plain file =="
python3 - <<'PY'
import os, random
random.seed(4711)
d = "/dev/shm/rensib/d"
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "big.bin"), "wb").write(random.randbytes(300*1024))
open(os.path.join(d, "plain.txt"), "w").write("plain file, no members\n")
PY
(cd "$WORK/d" && tar cf t.tar big.bin)
H_TAR=$(sha "$WORK/d/t.tar")

$B/invf-mkfs "$IMG" 0.5 >/dev/null
$B/invf-cp "$IMG" "$WORK/d/t.tar" t.tar >/dev/null
$B/invf-cp "$IMG" "$WORK/d/plain.txt" plain.txt >/dev/null
$B/invf-sweep "$IMG" >/dev/null 2>&1 || fail "sweep failed"

onvol | grep -qx 't.tar!part0' || fail "fixture: the container was not decomposed (no t.tar!part0)"
echo "  fixture ok: t.tar!part0 present"

echo "== leg 1: in-place rename of a container =="
mnt_up "$IMG"
[ "$(sha "$MNT/t.tar")" = "$H_TAR" ] || fail "leg1: the container does not read before the rename"
mv "$MNT/t.tar" "$MNT/x.tar" || fail "leg1: mv failed"
[ "$(sha "$MNT/x.tar")" = "$H_TAR" ] || fail "leg1: the renamed container does not read back byte-identical"
[ ! -e "$MNT/t.tar" ] || fail "leg1: the old name is still there"
mnt_down

onvol | grep -qx 't.tar!part0' && fail "leg1: the OLD member name survived the rename (the leg5 GHOST)"
onvol | grep -qx 'x.tar!part0' || fail "leg1: the member did not move with the container"
[ "$($B/invf-cat "$IMG" x.tar | sha256sum | cut -d' ' -f1)" = "$H_TAR" ] \
    || fail "leg1: offline read of the renamed container differs"
echo "  renamed in place: member moved, bytes identical"

echo "== leg 2: cross-directory move =="
mnt_up "$IMG"
mkdir "$MNT/sub" || fail "leg2: mkdir failed"
mv "$MNT/x.tar" "$MNT/sub/y.tar" || fail "leg2: mv into a subdirectory failed"
[ "$(sha "$MNT/sub/y.tar")" = "$H_TAR" ] || fail "leg2: the moved container does not read back"
mnt_down
onvol | grep -q '^sub/y.tar!part0$' || fail "leg2: the member did not follow into sub/"
onvol | grep -qx 'x.tar!part0' && fail "leg2: a stale member was left in the old directory"
[ "$($B/invf-cat "$IMG" sub/y.tar | sha256sum | cut -d' ' -f1)" = "$H_TAR" ] \
    || fail "leg2: offline read after the cross-directory move differs"
echo "  moved across directories: member followed"

echo "== leg 3: renaming a DIRECTORY needs no cascade =="
mnt_up "$IMG"
mv "$MNT/sub" "$MNT/sub2" || fail "leg3: directory rename failed"
[ "$(sha "$MNT/sub2/y.tar")" = "$H_TAR" ] || fail "leg3: the container inside the renamed directory broke"
mnt_down
onvol | grep -q '^sub2/y.tar!part0$' || fail "leg3: the member did not follow its directory"
echo "  directory rename: container inside still reads"

echo "== leg 4: a plain file rename is unchanged =="
mnt_up "$IMG"
mv "$MNT/plain.txt" "$MNT/plain2.txt" || fail "leg4: mv of a plain file failed"
cmp -s "$MNT/plain2.txt" "$WORK/d/plain.txt" || fail "leg4: the plain file's bytes changed"
mnt_down
echo "  plain file: renamed, bytes identical"

echo "== leg 5: fsck + verify =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
fsck_clean "$WORK/fsck.log" || fail "fsck not clean"
$B/invf-verify "$IMG" --deep > "$WORK/verify.log" 2>&1 || { tail -5 "$WORK/verify.log"; fail "verify --deep exited non-zero"; }
grep -qiE '0 corrupt' "$WORK/verify.log" || { grep -i corrupt "$WORK/verify.log" | head -3; fail "verify --deep reported corruption"; }
echo "  fsck clean, verify --deep 0 corrupt"

echo "PASS: container members follow the container on rename — in place, across"
echo "      directories, and inside a renamed directory; verify --deep clear"
