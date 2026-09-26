import random, sys
rnd = random.Random(99)
# incompressible filler: big enough that free space drops under the
# DEFER_ENOSPC admission (64 MiB margin + half the member total) once the
# filler and the container are both in
open(sys.argv[1] + "/filler.bin", "wb").write(rnd.randbytes(120 * 1024 * 1024))
