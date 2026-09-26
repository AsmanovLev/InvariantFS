import os, sys
d = sys.argv[1]
rnd = open('/dev/urandom','rb').read
for k in range(1, 5):
    open('%s/f%d.bin' % (d, k), 'wb').write(rnd(32 * 1024 * 1024))
words = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
open('%s/words.txt' % d, 'wb').write((words * 100000)[:8 * 1024 * 1024])
open('%s/small.bin' % d, 'wb').write(rnd(1024 * 1024))
print("  4x32MB incompressible + 8MB compressible + 1MB small")
