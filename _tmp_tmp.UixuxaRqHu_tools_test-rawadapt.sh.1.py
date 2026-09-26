import random, sys
d = sys.argv[1]
rnd = random.Random(23)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(64):
    with open('%s/fat%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
for k in range(40):
    with open('%s/small%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 16384:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
words = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
probe = (words * 4000)[:262144]                      # exactly 256KB
for t in ('low', 'mid', 'high', 'koff', 'turbo', 'oscB', 'oscC'):
    open('%s/probe-%s.txt' % (d, t), 'wb').write(probe)
fat = bytearray()
while len(fat) < 1048576:
    w = rnd.choice(vocab); fat += w + b' '
open('%s/fat1m.bin' % d, 'wb').write(bytes(fat))     # the leg-6a crossing write
print("  fixtures: 64 fat + 40 small fillers, 7 probe payloads, 1MB fat")
