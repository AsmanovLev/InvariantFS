import os, random
random.seed(24)
d = "/dev/shm/wp24rocp/v1"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
text_file("a.c", 120_000)
text_file("h.py", 45_000)
payload = bytearray()
pat = bytes(range(64)) * 4 + b"\x00" * 128 + os.urandom(64)
while len(payload) < 300_000:
    payload += pat
    payload += bytes([random.randrange(256)]) * 32
h = bytearray(64)
h[0:4] = b"\x7fELF"; h[18] = 62   # EM_X86_64
open(os.path.join(d, "bin_x64"), "wb").write(bytes(h) + bytes(payload[:300_000]))
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(150_000))
print("v1 corpus:", *sorted(os.listdir(d)))
