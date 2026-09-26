import random, sys
random.Random(99).randbytes(1)   # seed once
rnd = random.Random(99)
open(sys.argv[1] + "/filler.bin", "wb").write(rnd.randbytes(120 * 1024 * 1024))
