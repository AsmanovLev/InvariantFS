import os, sys, tarfile, io
WORK = sys.argv[1]
WORDS = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
text = (WORDS * 40000)[:3000000]
open(WORK + '/ref/swept.txt', 'wb').write(text)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w') as tf:
    for n in ('m1.txt', 'm2.txt', 'm3.txt'):
        d = (WORDS * 900)[:50000]
        ti = tarfile.TarInfo(n); ti.size = len(d)
        tf.addfile(ti, io.BytesIO(d))
open(WORK + '/ref/swept.tar', 'wb').write(buf.getvalue())
rnd = os.urandom(3000000)
open(WORK + '/ref/swept.bin', 'wb').write(rnd)
# PB7 leg fixtures: two identical files for the dedupe share
dup = os.urandom(1000000)
open(WORK + '/ref/dup1.bin', 'wb').write(dup)
open(WORK + '/ref/dup2.bin', 'wb').write(dup)
print("  fixtures staged")
