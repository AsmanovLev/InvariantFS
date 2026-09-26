import os, random
random.seed(2525)
d = "/dev/shm/wp25multi/orig"
WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa "
         "lambda mu nu xi omicron pi rho sigma tau\n").split()
def tf(name, size):
    n, out = 0, []
    while n < size:
        w = random.choice(WORDS)
        out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
tf("notes.txt", 12000)
tf("readme.txt", 4000)
open(os.path.join(d, "rnd.bin"), "wb").write(random.randbytes(300*1024))
open(os.path.join(d, "hot.bin"), "wb").write(random.randbytes(1500*1024))
