import random, struct, sys
d = sys.argv[1]
def splt(members):
    b = b"SPLT" + struct.pack("<I", len(members))
    for m in members: b += struct.pack("<Q", len(m))
    return b + b"".join(members)
words = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
open(d + '/demo.splt', 'wb').write(
    splt([(words * 300)[:20000], (words * 500)[:30000]]))
inner = splt([b"inner member alpha\n" * 500, b"inner member beta\n" * 700])
open(d + '/nest.splt', 'wb').write(splt([inner, b"outer tail\n" * 300]))
open(d + '/notes.txt', 'wb').write((words * 900)[:60000])
rnd = random.Random(5)
open(d + '/rand.bin', 'wb').write(rnd.randbytes(40000))
