import mmap, os, sys
MNT, WORK = sys.argv[1], sys.argv[2]
ref = bytearray(open(WORK + '/ref/swept.txt', 'rb').read())
fd = os.open(MNT + '/swept.txt', os.O_RDWR)
st = os.fstat(fd)
mm = mmap.mmap(fd, st.st_size)
pat1 = os.urandom(5000)
mm[77777:77777 + 5000] = pat1
ref[77777:77777 + 5000] = pat1
pat2 = os.urandom(200000)
mm[1500000:1500000 + 200000] = pat2     # spans 64K segments
ref[1500000:1500000 + 200000] = pat2
mm.flush()
mm.close(); os.close(fd)
got = open(MNT + '/swept.txt', 'rb').read()
assert got == bytes(ref), "swept-file mmap write mismatch"
open(WORK + '/ref/swept.txt', 'wb').write(bytes(ref))
print("  mmap write+msync persisted (swept file, materialized)")
