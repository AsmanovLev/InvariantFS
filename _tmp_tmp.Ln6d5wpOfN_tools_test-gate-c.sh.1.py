import random, sys, os
d = sys.argv[1]
rnd = random.Random(42)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(256):
    path = os.path.join(d, 'fill%03d.txt' % k)
    with open(path, 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab)
            f.write(w)
            f.write(b' ')
            n += len(w) + 1
print("  fixtures: 256 filler files (256 KB each)")
