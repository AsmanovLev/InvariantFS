import struct, sys
blob = open(sys.argv[1], 'rb').read()
(count,) = struct.unpack_from('<I', blob, 4)
runs = {}
for i in range(count):
    off, ln, kind, idx, src = struct.unpack_from('<QQBIQ', blob, 8 + i*29)
    if kind == 1: runs[idx] = runs.get(idx, 0) + 1
best = max(runs.values()) if runs else 0
assert best > 100, "fragmented fixture lost its fragmentation (best %d)" % best
print("    fragmentation gate: max %d runs in one member" % best)
