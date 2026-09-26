import os, random
random.seed(1919)
d = "/dev/shm/wp19heat/orig"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t heat tier\n").split()
for i in range(30):
    n, out = 0, []
    size = 3000 + (i % 7) * 900
    while n < size:
        w = random.choice(WORDS)
        out.append(w); n += len(w) + 1
    open(os.path.join(d, "t%02d.txt" % i), "w").write(" ".join(out)[:size])
open(os.path.join(d, "blob1.bin"), "wb").write(random.randbytes(100*1024))
open(os.path.join(d, "blob2.bin"), "wb").write(random.randbytes(100*1024))
print("tree:", len(os.listdir(d)), "files")
