import os, random, tarfile, io
random.seed(18)
d = "/dev/shm/wp18resize/orig"
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
    pat = bytes(range(64)) * 4 + b"\x00" * 128 + os.urandom(64)
    while len(payload) < size:
        payload += pat
        payload += bytes([random.randrange(256)]) * 32
    h = bytearray(64)
    h[0:4] = b"\x7fELF"; h[18] = emachine & 0xFF; h[19] = emachine >> 8
    open(os.path.join(d, name), "wb").write(bytes(h) + bytes(payload[:size]))

text_file("a.c", 1_200_000)
text_file("h.py", 450_000)
text_file("r.log", 5_000_000)
text_file("m.md", 900_000)
text_file("d.json", 600_000)
text_file("w.py", 2_500_000)
fake_bin("bin_x64", 62, 4_000_000)     # EM_X86_64  -> BCJ batch
fake_bin("bin_a64", 183, 3_000_000)    # EM_AARCH64 -> non-BCJ batch
fake_bin("bin_pe", 0, 2_000_000)       # overwritten below: MZ/PE
p = bytearray(open(os.path.join(d, "bin_pe"), "rb").read())
p[0:2] = b"MZ"; p[0x3C:0x40] = (0x40).to_bytes(4, "little")
p[0x40:0x44] = b"PE\0\0"
open(os.path.join(d, "bin_pe"), "wb").write(bytes(p))
# incompressible file: stays in the RAW zone
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(2_560_000))
# a tar of some texts (TARR decomposition + part batching)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as tf:
    for name in ["a.c", "h.py", "r.log"]:
        data = open(os.path.join(d, name), "rb").read()
        ti = tarfile.TarInfo("t/" + name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
open(os.path.join(d, "t.tar"), "wb").write(buf.getvalue())
print("corpus:", *sorted(os.listdir(d)))
