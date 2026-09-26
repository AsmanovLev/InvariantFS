import os, sys, hashlib
MNT = sys.argv[1]
fd = os.open(MNT + '/dup2.bin', os.O_RDWR)
os.pwrite(fd, os.urandom(65536), 262144)    # one full segment, odd phase
os.fsync(fd); os.close(fd)
h = hashlib.sha256(open(MNT + '/dup1.bin', 'rb').read()).hexdigest()
print("  dup1 sha256 after dup2 rewrite:", h[:16])
