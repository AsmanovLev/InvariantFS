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
