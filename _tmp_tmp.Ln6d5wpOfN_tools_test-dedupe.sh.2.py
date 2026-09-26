import os, random
random.seed(777)
d = "/dev/shm/wp12dedupe/orig2"
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def textfile(name, size):
    out, n = [], 0
    while n < size:
        w = random.choice(WORDS)
        out.append(w)
        n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
textfile("one.txt", 40_000)
T = open(os.path.join(d, "one.txt")).read()   # two identical texts
open(os.path.join(d, "two.txt", ), "w").write(T)
textfile("notes.md", 25_000)
open(os.path.join(d, "blob.bin"), "wb").write(random.randbytes(96*1024))
for f in sorted(os.listdir(d)):
    print(" ", f, os.path.getsize(os.path.join(d, f)), "bytes")
