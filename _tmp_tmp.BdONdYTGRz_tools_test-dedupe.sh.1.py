import os, random
random.seed(4242)
d = "/dev/shm/wp12dedupe/orig"
K = 1024
SHARED = random.randbytes(64*K)          # the shared middle
def w(name, data): open(os.path.join(d, name), "wb").write(data)
# two 164KB files sharing a 64KB middle at offset 65536 (segment grid)
w("a1.bin", random.randbytes(64*K) + SHARED + random.randbytes(36*K))
w("a2.bin", random.randbytes(64*K) + SHARED + random.randbytes(36*K))
# one fully duplicated file (150KB = 3 segments), under two names
D = random.randbytes(150*K)
w("dup1.bin", D)
w("dup2.bin", D)
# one unique file
w("uniq.bin", random.randbytes(96*K))
for f in sorted(os.listdir(d)):
    print(" ", f, os.path.getsize(os.path.join(d, f)), "bytes")

# Control tree: byte-identical to the dedupe tree except every would-be
# shared 64 KB segment has one byte flipped, so the dedupe pass merges
# nothing. Keeping the rest identical keeps the sweep's non-dedupe metadata
# footprint (and compression geometry) the same, so the control image
# captures the metadata allocations that make the naive
# FREE0+FREED==FREE1 identity false on Meta-v3 (WP75 Bug C).
import shutil
c = "/dev/shm/wp12dedupe/origctl"
def copy_flip(src, dst, offs):
    b = bytearray(open(os.path.join(d, src), "rb").read())
    for off in offs:
        b[off] ^= 0xFF
    open(os.path.join(c, dst), "wb").write(bytes(b))
shutil.copy(os.path.join(d, "a1.bin"), os.path.join(c, "a1.bin"))
copy_flip("a2.bin", "a2.bin", [65536])          # break a1/a2 shared middle
shutil.copy(os.path.join(d, "dup1.bin"), os.path.join(c, "dup1.bin"))
copy_flip("dup2.bin", "dup2.bin", [0, 65536, 131072])   # break all 3 dup segments
shutil.copy(os.path.join(d, "uniq.bin"), os.path.join(c, "uniq.bin"))
print("control tree: same geometry, no shared segments")
