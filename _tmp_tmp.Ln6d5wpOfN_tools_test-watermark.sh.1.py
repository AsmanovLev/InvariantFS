import random, sys
d = sys.argv[1]
rnd = random.Random(26)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(64):
    with open('%s/fat%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
print("  fixtures: 64 fat fillers (256KB each)")
