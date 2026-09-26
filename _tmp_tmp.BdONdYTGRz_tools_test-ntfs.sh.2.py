import sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
img[4*4096 + 5*1024 + 510] ^= 0xFF      # record 5 sector-0 USN trailer
open(p, "wb").write(bytes(img))
