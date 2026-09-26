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
