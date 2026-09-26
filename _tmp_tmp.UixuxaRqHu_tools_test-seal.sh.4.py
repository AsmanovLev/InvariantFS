import os, random
random.seed(23)
d = "/dev/shm/wp20seal/origc"
os.makedirs(d, exist_ok=True)
# semi-compressible binaries: generic ZSTD -> own shadow segments,
# enough of them that several seal stripes are occupied
for i in range(6):
    data = bytearray()
    while len(data) < 400_000:
        data += bytes([random.randrange(4)]) * 64
        data += os.urandom(32)
    open(os.path.join(d, f"g{i}.bin"), "wb").write(bytes(data))
open(os.path.join(d, "note.txt"), "w").write("a text file for the batch\n" * 4000)
