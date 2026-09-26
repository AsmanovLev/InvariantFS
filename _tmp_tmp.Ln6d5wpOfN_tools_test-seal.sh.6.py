import os, random
random.seed(24)
d = "/dev/shm/wp20seal/origd"
os.makedirs(d, exist_ok=True)
for i in range(5):
    data = bytearray()
    while len(data) < 350_000:
        data += bytes([random.randrange(4)]) * 64
        data += os.urandom(32)
    open(os.path.join(d, f"d{i}.bin"), "wb").write(bytes(data))
WORDS = ("layer two reed solomon stripe parity cauchy vandermonde\n").split()
out = []; n = 0
while n < 300_000:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "t.txt"), "w").write(" ".join(out)[:300_000])
