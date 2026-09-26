import os, sys, tempfile
MNT = sys.argv[1]
fd, actual_path = tempfile.mkstemp(prefix='opt-test.', dir=MNT + '/tmp')
os.write(fd, b"ram mode works")
os.close(fd)
with open(actual_path, 'rb') as f:
    assert f.read() == b"ram mode works"
print("  tmp_area=ram mount option works")
