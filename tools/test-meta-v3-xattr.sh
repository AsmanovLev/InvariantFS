#!/bin/bash
# test-meta-v3-xattr.sh — WP-M7 e2e: the metadata-v3 xattr tree.
#
# A VOLF_V3 volume must expose named xattrs through the normal FUSE xattr
# operations, stored in the base B+-tree (key 0x03||inode||name_len||name)
# rather than the v2 INO2 TLV area: set/get round-trip, a multi-xattr
# listxattr that is name-sorted, removexattr, per-inode isolation, values
# larger than one 4 KiB base page (chunked across tree records), overwrite in
# both directions (inline <-> chunked), an empty value, and persistence across
# unmount/remount. Unlinking a file must drop its xattr keys (the
# vol_v3_inode_delete cascade) and leave fsck clean.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-xattr.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3xattr
IMG=metav3xattr.img
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

echo "== WP-M7: v3 xattr tree (set/get/list/remove, large values, persist) =="

# --- mkfs v3 -------------------------------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.5 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
grep -q "format: v3 metadata skeleton" "$WORK/mkfs.log" \
    || fail "mkfs did not report the v3 format"

mnt_up
grep -q "format v3" "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log"; fail "mount did not take the v3 open path"; }

touch "$MNT/a" "$MNT/b"
mkdir "$MNT/xd"
echo "created: 2 files + 1 dir"

# --- small-value round-trip, multi-xattr list, remove, isolation --------
python3 - "$MNT" <<'PY' || fail "small/large xattr round-trip"
import os, sys
M = sys.argv[1]
a, b = M + "/a", M + "/b"

os.setxattr(a, "user.small", b"hello")
assert os.getxattr(a, "user.small") == b"hello", "small get mismatch"

os.setxattr(a, "user.alpha", b"1")
os.setxattr(a, "user.zeta", b"2")
os.setxattr(a, "user.beta", b"3")
names = os.listxattr(a)
want = ["user.alpha", "user.beta", "user.small", "user.zeta"]
assert names == want, "listxattr order %r, want %r" % (names, want)

# per-inode isolation: b must not see a's xattrs
assert os.listxattr(b) == [], "b sees foreign xattrs: %r" % os.listxattr(b)
os.setxattr(b, "user.onlyb", b"B")
assert os.getxattr(b, "user.onlyb") == b"B"
assert "user.onlyb" not in os.listxattr(a), "xattr leaked across inodes"

# empty value
os.setxattr(a, "user.empty", b"")
assert os.getxattr(a, "user.empty") == b"", "empty get mismatch"

# absent name is ENODATA
try:
    os.getxattr(a, "user.nope")
except OSError as e:
    assert e.errno == 61, "absent get errno %d, want ENODATA(61)" % e.errno
else:
    raise AssertionError("absent getxattr succeeded")

# remove one and confirm it disappears
os.removexattr(a, "user.zeta")
assert "user.zeta" not in os.listxattr(a), "removed xattr still listed"
try:
    os.removexattr(a, "user.zeta")
except OSError as e:
    assert e.errno in (61, 93), "re-remove errno %d" % e.errno
else:
    raise AssertionError("re-removexattr succeeded")

# many xattrs: force leaf splits; list must stay sorted and complete
for i in range(60):
    os.setxattr(a, "user.n%02d" % i, b"v%d" % i)
got = [n for n in os.listxattr(a) if n.startswith("user.n")]
want = ["user.n%02d" % i for i in range(60)]
assert got == want, "many-xattr list mismatch (%d entries)" % len(got)
for i in range(60):
    assert os.getxattr(a, "user.n%02d" % i) == b"v%d" % i
print("  small values + sorted list + per-inode isolation + empty + remove OK")
print("  many xattrs (60) survived splits, list sorted and complete")
PY

# --- values larger than one 4 KiB base page (chunked records) -----------
python3 - "$MNT" <<'PY' || fail "large-value xattr round-trip"
import os, sys
M = sys.argv[1]
a = M + "/a"

big = bytes((i * 7 + 3) & 0xFF for i in range(9000))   # > one page
assert len(big) > 4096
os.setxattr(a, "user.big", big)
assert os.getxattr(a, "user.big") == big, "9000-byte get mismatch"

huge = bytes((i * 13 + 5) & 0xFF for i in range(12000))  # multiple chunks
os.setxattr(a, "user.huge", huge)
assert os.getxattr(a, "user.huge") == huge, "12000-byte get mismatch"

# overwrite large -> small, then small -> large (chunk keys must be dropped)
os.setxattr(a, "user.big", b"tiny")
assert os.getxattr(a, "user.big") == b"tiny", "large->small mismatch"
os.setxattr(a, "user.big", big[::-1])
assert os.getxattr(a, "user.big") == big[::-1], "small->large mismatch"

# a value of exactly one page is a boundary case
page = bytes(range(256)) * 16   # 4096 bytes
os.setxattr(a, "user.page", page)
assert os.getxattr(a, "user.page") == page, "4096-byte get mismatch"

assert os.listxattr(a).count("user.big") == 1, "large name listed twice"
print("  large values: 9000/12000/4096 bytes, overwrite both directions OK")

# many chunked values: force leaf splits while ~1 KiB chunk records are
# interleaved with small ones (the WP-M3 splitter stress case)
stress = {}
b = M + "/b"
for i in range(40):
    n = 1500 + (i * 97) % 4000
    stress["user.s%02d" % i] = bytes((i + j) & 0xFF for j in range(n))
    os.setxattr(b, "user.s%02d" % i, stress["user.s%02d" % i])
for name, want in stress.items():
    got = os.getxattr(b, name)
    assert got == want, "stress %s: %d bytes != %d" % (name, len(got), len(want))
got = [n for n in os.listxattr(b) if n.startswith("user.s")]
assert got == ["user.s%02d" % i for i in range(40)], "stress list mismatch"
print("  stress: 40 chunked values (1.5-5.5 KB) survived interleaved splits")
PY

# --- persistence across unmount/remount ---------------------------------
mnt_down
mnt_up
python3 - "$MNT" <<'PY' || fail "xattrs did not persist across remount"
import os, sys
M = sys.argv[1]
a, b = M + "/a", M + "/b"

assert os.getxattr(a, "user.small") == b"hello", "small lost on remount"
assert os.getxattr(a, "user.empty") == b"", "empty lost on remount"
assert os.getxattr(b, "user.onlyb") == b"B", "b xattr lost on remount"
assert "user.onlyb" not in os.listxattr(a), "xattr crossed inodes on remount"

big = bytes((i * 7 + 3) & 0xFF for i in range(9000))
huge = bytes((i * 13 + 5) & 0xFF for i in range(12000))
page = bytes(range(256)) * 16
assert os.getxattr(a, "user.big") == big[::-1], "large (reversed) lost"
assert os.getxattr(a, "user.huge") == huge, "huge lost on remount"
assert os.getxattr(a, "user.page") == page, "page-sized lost on remount"

got = [n for n in os.listxattr(a) if n.startswith("user.n")]
assert got == ["user.n%02d" % i for i in range(60)], "many lost on remount"
for i in range(60):
    assert os.getxattr(a, "user.n%02d" % i) == b"v%d" % i
for i in (0, 7, 19, 39):
    n = 1500 + (i * 97) % 4000
    want = bytes((i + j) & 0xFF for j in range(n))
    assert os.getxattr(b, "user.s%02d" % i) == want, "stress lost on remount"
print("  remount: all values (small/empty/large/12000/many/stress) identical")
PY

# --- unlink drops the dying inode's xattr keys (cascade) ----------------
python3 - "$MNT" <<'PY' || fail "unlink/xattr cascade setup"
import os, sys
M = sys.argv[1]
p = M + "/dying"
open(p, "w").close()
os.setxattr(p, "user.doomed", b"x" * 5000)
os.setxattr(p, "user.also", b"y")
assert os.getxattr(p, "user.doomed") == b"x" * 5000
print("  cascade: set 2 xattrs (one 5000 bytes) on a file about to die")
PY
rm "$MNT/dying"
[ ! -e "$MNT/dying" ] || fail "unlink: dying still present"

# a fresh file created afterwards must not inherit them
touch "$MNT/fresh"
python3 - "$MNT" <<'PY' || fail "new file inherited dead xattrs"
import os, sys
assert os.listxattr(sys.argv[1] + "/fresh") == [], "fresh file has xattrs"
print("  cascade: unlinked file's xattrs gone, new file clean")
PY

mnt_down

# --- offline sanity: fsck accepts the base tree (no stranded keys) ------
FSCK=$($B/invf-fsck "$IMG" 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK"; }
echo "fsck: OK"

echo "ALL META-V3 XATTR LEGS PASS"
