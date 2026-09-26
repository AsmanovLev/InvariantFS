import os, random, sys
d = sys.argv[1]
rnd = random.Random(7)
open(os.path.join(d, "big.bin"), "wb").write(rnd.randbytes(6 * 1024 * 1024))
open(os.path.join(d, "big2.bin"), "wb").write(rnd.randbytes(2 * 1024 * 1024))
open(os.path.join(d, "head1m.bin"), "wb").write(rnd.randbytes(1024 * 1024))
open(os.path.join(d, "head512k.bin"), "wb").write(rnd.randbytes(512 * 1024))
open(os.path.join(d, "head256k.bin"), "wb").write(rnd.randbytes(256 * 1024))
open(os.path.join(d, "hl.txt"), "wb").write(b"hardlinked content\n" * 100)
open(os.path.join(d, "frag1k.bin"), "wb").write(rnd.randbytes(1024))
open(os.path.join(d, "frag4k.bin"), "wb").write(rnd.randbytes(4096))
open(os.path.join(d, "empty.bin"), "wb").write(b"")
# the refused text file that merely carries an .ext4 name
open(os.path.join(d, "plain.ext4"), "wb").write(
    b"/* not a filesystem, just text with an .ext4 name */\n" * 1000)
