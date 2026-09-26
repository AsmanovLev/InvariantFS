import random, sys
w = sys.argv[1]
r = random.Random(5)
blk = r.randbytes(4096)
with open(w + "/src/frag.bin", "wb") as f:
    for _ in range(2560):
        f.write(blk)
# sparse.bin pristine copy: 16 MB, real data only at two 16 KB windows
p1 = r.randbytes(16384)
p2 = r.randbytes(16384)
open(w + "/src/sparse-p1.bin", "wb").write(p1)
open(w + "/src/sparse-p2.bin", "wb").write(p2)
buf = bytearray(16 * 1024 * 1024)
buf[100*4096 : 100*4096+16384] = p1
buf[3000*4096 : 3000*4096+16384] = p2
open(w + "/src/sparse.bin", "wb").write(bytes(buf))
