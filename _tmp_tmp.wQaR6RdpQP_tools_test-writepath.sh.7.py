import mmap, os, sys
MNT = sys.argv[1]
fd = os.open(MNT + '/victim.bin', os.O_RDWR | os.O_CREAT)
os.ftruncate(fd, 2 * 1024 * 1024)
mm = mmap.mmap(fd, 2 * 1024 * 1024)
mm[0:16] = b'FLUSHED-REGION--'
mm.flush()                              # written back + committed below
mm[1048576:1048592] = b'NEVER-MSYNCED-XX'    # stays dirty in page cache
os.fsync(fd)                            # fsync commits the session (+barrier)
print("  armed (region1 msynced+fsynced, region2 dirty only)")
