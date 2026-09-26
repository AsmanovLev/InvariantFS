import os
d = "/dev/shm/wp19heat/orig3"
os.makedirs(d, exist_ok=True)
# compressible BINARY payloads: the generic floor is what profiles steer,
# and a .txt would defer into the PPMd batch accumulator before reaching it
payload = bytes(range(256)) * 32
for name in ("p1.bin", "p2.bin", "p3.bin", "p4.bin"):
    open(os.path.join(d, name), "wb").write(payload)
