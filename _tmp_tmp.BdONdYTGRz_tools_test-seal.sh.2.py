import os, random
random.seed(21)
d = "/dev/shm/wp20seal/orig"
WORDS = ("secondary modifications change stripe membership int void "
         "static char while for return\n").split()
out = []; n = 0
while n < 80_000:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "new.txt"), "w").write(" ".join(out)[:80_000])
out = []; n = 0
while n < 95_000:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "m.md"), "w").write(" ".join(out)[:95_000])  # overwrite
os.unlink(os.path.join(d, "s.sh"))                                # delete
