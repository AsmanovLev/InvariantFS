import os
import sys
work = sys.argv[1]
block = 65536
common = bytes(((i * 37 + 11) & 0xff) for i in range(block))
unique_a = bytes(((i * 59 + 17) & 0xff) for i in range(block))
unique_b = bytes(((i * 83 + 29) & 0xff) for i in range(block))
with open(os.path.join(work, "a.bin"), "wb") as f:
    f.write(common + common + unique_a)
with open(os.path.join(work, "b.bin"), "wb") as f:
    f.write(common + unique_b)
