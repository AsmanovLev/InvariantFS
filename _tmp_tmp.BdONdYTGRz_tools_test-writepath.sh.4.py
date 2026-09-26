import mmap, os, sys
MNT = sys.argv[1]
fd = os.open(MNT + '/mm.bin', os.O_RDWR | os.O_CREAT)
os.ftruncate(fd, 5 * 1024 * 1024)
mm = mmap.mmap(fd, 5 * 1024 * 1024)     # PROT_WRITE|READ, MAP_SHARED
pat = os.urandom(1000)
mm[1234567:1234567 + 1000] = pat
mm[4 * 1024 * 1024 + 13:4 * 1024 * 1024 + 16] = b'END'
mm.flush()                              # msync MS_SYNC
mm.close(); os.close(fd)
data = open(MNT + '/mm.bin', 'rb').read()
assert data[1234567:1234567 + 1000] == pat, "mmap write lost"
assert data[4 * 1024 * 1024 + 13:4 * 1024 * 1024 + 16] == b'END'
print("  mmap write+msync persisted (RAW file)")
