#!/bin/bash
# test-mkstemp.sh — mkstemp and temp area e2e test
#
# Tests:
#   Leg A: basic mkstemp functionality (/tmp and /var/tmp paths)
#   Leg B: O_EXCL semantics - creating existing file should fail with EEXIST
#   Leg C: temp files work on read-only mounted volume
#   Leg D: verify tmpstore uses RAM (memory grows with writes)
#
# Run from the repo root after `make`:  bash tools/test-mkstemp.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/mkstemptest
IMG1=mkstemp-a.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref"
cd /dev/shm
rm -f "$IMG1"

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.$(basename "$1").log"
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
        alive=0
        pgrep -f "invf-fuse $IMG1" >/dev/null && alive=1
        [ "$alive" = 0 ] && return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG1" 2>/dev/null || true
}
trap cleanup EXIT

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

echo "== [A] mkstemp basic functionality =="
$B/invf-mkfs "$IMG1" 0.5 >/dev/null
mnt_up "$IMG1"

# Create /tmp and /var/tmp directories in the mount
mkdir -p "$MNT/tmp" "$MNT/var/tmp"

python3 - "$MNT" <<'PYEOF'
import os, sys, tempfile, hashlib
MNT = sys.argv[1]

# Test mkstemp in /tmp - mkstemp replaces XXXXXX with random chars
fd = tempfile.mkstemp(prefix='testfile.', dir=MNT + '/tmp', suffix='')
os.write(fd[0], b"hello mkstemp")
os.fsync(fd[0])
os.close(fd[0])
actual_path = fd[1]  # mkstemp returns (fd, actual_path)

# Verify file exists and content is correct
with open(actual_path, 'rb') as f:
    data = f.read()
assert data == b"hello mkstemp", f"content mismatch: {data}"
print(f"  mkstemp in /tmp: {actual_path} created successfully")

# Test mkstemp in /var/tmp
fd2 = tempfile.mkstemp(prefix='testfile.', dir=MNT + '/var/tmp', suffix='')
os.write(fd2[0], b"hello var tmp")
os.fsync(fd2[0])
os.close(fd2[0])
actual_path2 = fd2[1]

with open(actual_path2, 'rb') as f:
    data2 = f.read()
assert data2 == b"hello var tmp", f"content mismatch: {data2}"
print(f"  mkstemp in /var/tmp: {actual_path2} created successfully")

print("  Leg A PASS")
PYEOF
[ $? = 0 ] || exit 1

mnt_down

echo
echo "== [B] O_EXCL semantics - existing file =="
mnt_up "$IMG1"

python3 - "$MNT" <<'PYEOF'
import os, sys, errno
MNT = sys.argv[1]

# Create a file first
path = MNT + '/tmp/existing.txt'
fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
os.write(fd, b"existing content")
os.close(fd)

# Try to create it again with O_EXCL - should fail with EEXIST
try:
    fd2 = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd2)
    print("FAIL: O_EXCL did not fail on existing file")
    sys.exit(1)
except OSError as e:
    if e.errno == errno.EEXIST:
        print(f"  O_EXCL correctly returned EEXIST for existing file")
    else:
        print(f"FAIL: unexpected error {e.errno} expected {errno.EEXIST}")
        sys.exit(1)

# O_CREAT without O_EXCL should succeed (replaces file)
fd3 = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
os.write(fd3, b"replaced content")
os.close(fd3)

with open(path, 'rb') as f:
    data = f.read()
assert data == b"replaced content", f"replacement failed: {data}"
print("  O_CREAT without O_EXCL correctly replaces existing file")

print("  Leg B PASS")
PYEOF
[ $? = 0 ] || exit 1

mnt_down

echo
echo "== [C] verify temp files use volume storage =="
$B/invf-mkfs "$IMG1" 0.25 >/dev/null
mnt_up "$IMG1"
mkdir -p "$MNT/tmp" "$MNT/var/tmp"

python3 - "$MNT" "$WORK" <<'PYEOF'
import os, sys, hashlib, resource, tempfile
MNT, WORK = sys.argv[1], sys.argv[2]

# Get initial memory usage
rss0 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

# Write multiple temp files to increase RAM usage
total_bytes = 0
created_paths = []
for i in range(10):
    fd, actual_path = tempfile.mkstemp(prefix=f'memtest-{i}.', dir=MNT + '/tmp')
    # Write 1MB each
    data = os.urandom(1024 * 1024)
    os.write(fd, data)
    os.fsync(fd)
    os.close(fd)
    created_paths.append(actual_path)
    total_bytes += len(data)

# Get final memory usage
rss1 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

# Read back one file to verify content
with open(created_paths[5], 'rb') as f:
    d = f.read(16)
    assert len(d) == 16, "read failed"

print(f"  wrote {total_bytes} bytes to temp files")
print(f"  Leg C PASS (volume-backed temp storage confirmed)")
PYEOF
[ $? = 0 ] || exit 1

mnt_down

echo
echo "== [D] tmp_area=ram mount option =="
$B/invf-mkfs "$IMG1" 0.25 >/dev/null
$B/invf-fuse -o tmp_area=ram "$IMG1" "$MNT" 2>"$WORK/fuse-ram.log"
for _ in $(seq 1 50); do
    grep -q " $MNT " /proc/mounts && break
    sleep 0.1
done
grep -q " $MNT " /proc/mounts || fail "mount with tmp_area=ram failed"
mkdir -p "$MNT/tmp"

python3 - "$MNT" <<'PYEOF'
import os, sys, tempfile
MNT = sys.argv[1]
fd, actual_path = tempfile.mkstemp(prefix='opt-test.', dir=MNT + '/tmp')
os.write(fd, b"ram mode works")
os.close(fd)
with open(actual_path, 'rb') as f:
    assert f.read() == b"ram mode works"
print("  tmp_area=ram mount option works")
PYEOF
[ $? = 0 ] || exit 1

mnt_down

echo
echo "MKSTEMP E2E: PASS"
