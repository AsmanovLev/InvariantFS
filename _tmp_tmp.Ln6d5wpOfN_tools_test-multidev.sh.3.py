import os, random
random.seed(2626)
d = "/dev/shm/wp25multi/orig"
open(os.path.join(d, "coldA.bin"), "wb").write(random.randbytes(2*1024*1024))
open(os.path.join(d, "coldB.bin"), "wb").write(random.randbytes(5*1024*1024))
