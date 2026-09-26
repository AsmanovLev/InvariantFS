import os, random, zlib, struct
random.seed(42)
d = "/dev/shm/wp10tz/orig"
# text files of several families and sizes (1KB..500KB)
specs = [("a.c", 120_000), ("b.c", 3_000), ("h.py", 45_000),
         ("w.py", 250_000), ("r.log", 500_000), ("n.txt", 1_500),
         ("m.md", 90_000), ("s.sh", 12_000), ("d.json", 60_000)]
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
for name, size in specs:
    out = []
    n = 0
    while n < size:
        w = random.choice(WORDS)
        out.append(w)
        n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
# one >4MB text file
with open(os.path.join(d, "big.txt"), "w") as f:
    n = 0
    while n < 5_300_000:
        line = " ".join(random.choices(WORDS, k=16)) + "\n"
        f.write(line)
        n += len(line)
# binaries: /bin/true + a random blob
import shutil
shutil.copy("/bin/true", os.path.join(d, "true.bin"))
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(300_000))
# one PNG (real, tiny, valid)
def chunk(t, data):
    return (struct.pack(">I", len(data)) + t + data +
            struct.pack(">I", zlib.crc32(t + data) & 0xffffffff))
ihdr = struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0)
rows = b"".join(b"\x00" + bytes(random.randrange(256) for _ in range(12))
                for _ in range(4))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
       chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
open(os.path.join(d, "t.png"), "wb").write(png)
print("tree:", *sorted(os.listdir(d)))
