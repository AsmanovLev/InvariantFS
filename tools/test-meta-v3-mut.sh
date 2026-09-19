#!/bin/bash
# test-meta-v3-mut.sh — WP-M12 e2e: v3 metadata mutations go to the delta.
#
# A VOLF_V3 volume must route create/unlink/rename/setattr/xattr through the
# WP-M10 delta append instead of a base B+-tree rewrite, so the base root
# stays immutable between folds (design-meta-v3.md §1/§4/§13). The WP-M11
# overlay (delta first, then base) must make every mutation visible
# immediately and after a remount.
#
#   leg 0  mkfs v3;
#   leg 1  seed content so the base tree has a non-empty, stable root, then
#          capture the RT30 root slots/seq and a hash of the active delta
#          segment (baseline);
#   leg 2  mount and mutate: create/mkdir/rmdir/unlink/rename/chmod/chown/
#          utimens/setxattr/removexattr; assert the mount reads reflect every
#          change (through the overlay);
#   leg 3  unmount: the base root slots/seq are byte-identical to the
#          baseline (no base rewrite) while the delta segment changed
#          (the mutations appended);
#   leg 4  remount: every change replays from the delta; then truncate a
#          content file bit-exactly (recipe tier) and confirm size/bytes;
#   leg 5  fsck clean and the superblock CLEAN.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-mut.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3mut
IMG=metav3mut.img
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

# rtslot <img> : print "slot0 slot1 delta_pba seq" from RT30 (block 0).
rtslot() {
    python3 - "$1" <<'PY'
import struct, sys
blk = open(sys.argv[1], 'rb').read(0x1000)
off = 0x9D0
assert blk[off:off+4] == b'RT30', "RT30 magic missing"
s0, s1, delta, seq = struct.unpack_from('<QQQQ', blk, off + 0xC)
print(s0, s1, delta, seq)
PY
}

# deltahash <img> : sha256 of the active 128 KiB delta segment (empty pba ->
# hash of the empty string). A metadata-only mutation must change this while
# leaving the RT30 root slots/seq untouched.
deltahash() {
    python3 - "$1" <<'PY'
import hashlib, struct, sys
f = open(sys.argv[1], 'rb')
blk = f.read(0x1000)
off = 0x9D0
delta = struct.unpack_from('<Q', blk, off + 0x1C)[0]
h = hashlib.sha256()
if delta:
    f.seek(delta * 4096)
    h.update(f.read(128 * 1024))
print(h.hexdigest())
PY
}

echo "== leg 0: mkfs v3 =="
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"
echo "mkfs: v3 metadata skeleton"

echo "== leg 1: seed content (non-empty, stable base root) =="
mnt_up
python3 - "$MNT/seed" <<'PY' || fail "seed content write"
import sys
open(sys.argv[1], 'wb').write(bytes((i * 7 + 3) & 0xFF for i in range(9000)))
PY
[ "$(stat -c %s "$MNT/seed")" = 9000 ] || fail "seed size wrong after write"
mnt_down
read -r S0 S1 SDELTA SSEQ < <(rtslot "$IMG")
[ "$S0" -gt 0 ] || fail "seed did not publish a base root (slot0=$S0)"
BEFORE_HASH=$(deltahash "$IMG")
export BEFORE_S0="$S0" BEFORE_S1="$S1" BEFORE_SEQ="$SSEQ" BEFORE_HASH="$BEFORE_HASH"
echo "baseline: base root slots $S0/$S1 seq $SSEQ; delta hash ${BEFORE_HASH:0:16}"

echo "== leg 2: metadata mutations append to the delta =="
mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }

touch "$MNT/alpha" "$MNT/beta" "$MNT/doomed"
mkdir "$MNT/dir"
mkdir "$MNT/tmpdir" && rmdir "$MNT/tmpdir" || fail "rmdir failed"
chmod 640 "$MNT/alpha" || fail "chmod failed"
chown "$(id -u):$(id -g)" "$MNT/alpha" || fail "chown failed"
touch -d '2020-01-02 03:04:05' "$MNT/alpha" || fail "utimens failed"
python3 - "$MNT/alpha" <<'PY' || fail "xattr set/remove"
import os, sys
a = sys.argv[1]
os.setxattr(a, "user.keep", b"kept")
os.setxattr(a, "user.gone", b"gone")
os.removexattr(a, "user.gone")
assert os.getxattr(a, "user.keep") == b"kept", "kept xattr mismatch"
try:
    os.getxattr(a, "user.gone")
except OSError:
    pass
else:
    raise AssertionError("removed xattr still present")
PY
mv "$MNT/beta" "$MNT/gamma" || fail "rename failed"
rm -f "$MNT/doomed" || fail "unlink failed"
echo "mutated: create 3, mkdir+rmdir, chmod/chown/utimens, xattr set+remove, rename, unlink"

# every change must be visible through the overlay on the live mount
[ -f "$MNT/alpha" ] || fail "alpha missing"
[ -d "$MNT/dir" ] || fail "dir missing"
[ -f "$MNT/gamma" ] || fail "renamed gamma missing"
[ ! -e "$MNT/beta" ] || fail "renamed source beta still present"
[ ! -e "$MNT/doomed" ] || fail "unlinked doomed still present"
[ ! -e "$MNT/tmpdir" ] || fail "rmdir'd tmpdir still present"
[ "$(stat -c %a "$MNT/alpha")" = 640 ] || fail "chmod not visible"
WANT_T=$(date -d '2020-01-02 03:04:05' +%s)
[ "$(stat -c %Y "$MNT/alpha")" = "$WANT_T" ] || fail "utimens not visible"
python3 - "$MNT/alpha" <<'PY' || fail "xattr not visible"
import os, sys
a = sys.argv[1]
assert os.getxattr(a, "user.keep") == b"kept"
assert "user.gone" not in os.listxattr(a)
PY
echo "live reads: create/rmdir/rename/chmod/utimens/xattr all reflected"

echo "== leg 3: base root unchanged; delta grew =="
mnt_down
read -r N0 N1 NDELTA NSEQ < <(rtslot "$IMG")
[ "$N0" = "$BEFORE_S0" ] && [ "$N1" = "$BEFORE_S1" ] && [ "$NSEQ" = "$BEFORE_SEQ" ] \
    || fail "base root moved by a metadata mutation (before $BEFORE_S0/$BEFORE_S1 seq $BEFORE_SEQ, after $N0/$N1 seq $NSEQ)"
AFTER_HASH=$(deltahash "$IMG")
[ "$AFTER_HASH" != "$BEFORE_HASH" ] \
    || fail "delta segment did not change; mutations did not append"
echo "base root unchanged ($N0/$N1 seq $NSEQ); delta hash ${AFTER_HASH:0:16} (grew)"

echo "== leg 4: remount replays the delta; truncate stays bit-exact =="
mnt_up
[ -f "$MNT/alpha" ] || fail "remount: alpha lost"
[ -d "$MNT/dir" ] || fail "remount: dir lost"
[ -f "$MNT/gamma" ] || fail "remount: gamma lost"
[ ! -e "$MNT/beta" ] || fail "remount: beta resurrected"
[ ! -e "$MNT/doomed" ] || fail "remount: doomed resurrected"
[ "$(stat -c %a "$MNT/alpha")" = 640 ] || fail "remount: mode lost"
[ "$(stat -c %Y "$MNT/alpha")" = "$WANT_T" ] || fail "remount: mtime lost"
python3 - "$MNT/alpha" <<'PY' || fail "remount: xattr lost"
import os, sys
a = sys.argv[1]
assert os.getxattr(a, "user.keep") == b"kept"
assert "user.gone" not in os.listxattr(a)
PY
echo "remount: namespace + attrs + xattr replayed identically"

# truncate-size: shrink a content file; the bytes must be bit-exact
python3 - "$MNT/seed" <<'PY' || fail "truncate-size bit-exactness"
import os, sys
p = sys.argv[1]
orig = bytes((i * 7 + 3) & 0xFF for i in range(9000))
assert open(p, "rb").read() == orig, "seed bytes not bit-exact before truncate"
os.truncate(p, 3000)
assert os.path.getsize(p) == 3000, "truncate did not shrink to 3000"
assert open(p, "rb").read() == orig[:3000], "truncate shrank to wrong bytes"
PY
echo "truncate: 9000 -> 3000 bytes bit-exact"
mnt_down

echo "== leg 5: fsck clean =="
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "$FSCK" | grep -q "format:       v3" || { echo "$FSCK"; fail "fsck not v3"; }
python3 - "$IMG" <<'PY' || fail "volume not CLEAN at the end"
import sys
blk = open(sys.argv[1], 'rb').read(0x20)
assert blk[0x18] == 0xCA, "state=0x%02X, want CLEAN" % blk[0x18]
print("state: CLEAN")
PY

echo "ALL META-V3 MUTATION LEGS PASS"
