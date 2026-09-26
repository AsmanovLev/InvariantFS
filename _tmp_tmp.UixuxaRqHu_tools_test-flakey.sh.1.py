import os, random, sys, tarfile, io
d, seed, prof = sys.argv[1], int(sys.argv[2]), sys.argv[3]
random.seed(seed)
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

def fake_bin(name, emachine, size):
    # compressible fabricated binary: ELF magic + e_machine + payload of
    # repeating patterns (sniffs as a binary family, batches well)
    payload = bytearray()
    pat = bytes(range(64)) * 4 + b"\x00" * 128 + random.randbytes(64)
    while len(payload) < size:
        payload += pat
        payload += bytes([random.randrange(256)]) * 32
    h = bytearray(64)
    h[0:4] = b"\x7fELF"; h[18] = emachine & 0xFF; h[19] = emachine >> 8
    open(os.path.join(d, name), "wb").write(bytes(h) + bytes(payload[:size]))

M = 1024 * 1024
if prof == "full":      # ~200MB
    texts = [("a.c", 25*M), ("r.log", 20*M), ("w.py", 15*M), ("m.md", 10*M),
             ("d.json", 8*M), ("s.sh", 4*M)]
    bins  = [("bin_x64", 62, 30*M), ("bin_a64", 183, 25*M), ("bin_pe", 0, 15*M)]
    rand_mb, tar_names = 36, ["a.c", "r.log"]
elif prof == "medium":  # ~100MB
    texts = [("a.c", 14*M), ("r.log", 12*M), ("w.py", 9*M)]
    bins  = [("bin_x64", 62, 20*M), ("bin_a64", 183, 15*M)]
    rand_mb, tar_names = 20, ["a.c", "w.py"]
else:                   # small, ~40MB (soak seed corpus)
    texts = [("a.c", 8*M), ("r.log", 7*M)]
    bins  = [("bin_x64", 62, 12*M)]
    rand_mb, tar_names = 10, ["a.c"]

for name, size in texts: text_file(name, size)
for name, em, size in bins: fake_bin(name, em, size)
# turn bin_pe into a PE: MZ stub with e_lfanew -> "PE\0\0"
if any(n == "bin_pe" for n, _, _ in bins):
    p = bytearray(open(os.path.join(d, "bin_pe"), "rb").read())
    p[0:2] = b"MZ"; p[0x3C:0x40] = (0x40).to_bytes(4, "little")
    p[0x40:0x44] = b"PE\0\0"
    open(os.path.join(d, "bin_pe"), "wb").write(bytes(p))
# incompressible file: stays RAW verbatim
open(os.path.join(d, "rand.bin"), "wb").write(random.randbytes(rand_mb * M))
# a tar of some texts (TARR decomposition + part batching)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as tf:
    for name in tar_names:
        data = open(os.path.join(d, name), "rb").read()
        ti = tarfile.TarInfo("t/" + name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
open(os.path.join(d, "t.tar"), "wb").write(buf.getvalue())
tot = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
print("  corpus[%s]: %d files, %.1f MB" % (prof, len(os.listdir(d)), tot / 1e6))
