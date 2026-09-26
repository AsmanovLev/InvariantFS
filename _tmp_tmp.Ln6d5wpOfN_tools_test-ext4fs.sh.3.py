import struct, sys
d = open(sys.argv[2], "rb").read()
bino = int(sys.argv[3])
(n,) = struct.unpack_from("<I", d, 4)
runs = []          # big.bin member runs (off, len)
boundary = None    # first RECIPE -> MEMBER transition
prev_kind = None
size = 64 * 1024 * 1024
last_end_kind = None
for i in range(n):
    off, ln, kind, idx, so = struct.unpack_from("<QQBIQ", d, 8 + i * 29)
    if kind == 1 and idx == bino:
        runs.append((off, ln))
    if prev_kind == 0 and kind == 1 and boundary is None:
        boundary = (off - 16, 64)
    prev_kind = kind
runs.sort()
big = max(runs, key=lambda r: r[1])        # the merged contiguous head run
mid = (big[0] + 4096, min(65536, big[1] - 4096))
print("mid %d %d" % mid)
print("boundary %d %d" % boundary)
print("tail %d %d" % (size - 1000, 1000))
print("head %d %d" % (0, 8192))
