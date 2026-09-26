import os, random
random.seed(2929)
d = "/dev/shm/wp19heat/orig2"
os.makedirs(d, exist_ok=True)
WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa\n").split()
def tf(name, size):
    n, out = 0, []
    while n < size:
        w = random.choice(WORDS)
        out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
tf("w.txt", 5000); tf("c.txt", 5000); tf("init.txt", 3000)
