import sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
img[4*4096 + 510] ^= 0xFF               # record 0 sector-0 USN trailer
open(p, "wb").write(bytes(img))
