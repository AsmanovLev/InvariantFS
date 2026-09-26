import struct, sys
arch = open(sys.argv[1], "rb").read()
m = open(sys.argv[2], "rb").read()
assert m[:4] == b"MRMP", "bad map magic"
count, = struct.unpack("<I", m[4:8])
assert len(m) == 8 + count * 29, "bad map length"
pos = 0
for i in range(count):
    o = 8 + i * 29
    orig_off, ln = struct.unpack("<QQ", m[o:o + 16])
    kind = m[o + 16]
    idx, = struct.unpack("<I", m[o + 17:o + 21])
    src_off, = struct.unpack("<Q", m[o + 21:o + 29])
    assert orig_off == pos, "map does not partition"
    assert ln > 0, "zero-length map entry"
    if kind == 0:
        assert idx == 0, "RECIPE with nonzero idx"
    else:
        assert kind == 1, "unknown kind"
    pos += ln
assert pos == len(arch), "map short of the container"
print("  stored.7z MRMP: %d entries, partitions [0, %d) exactly" % (count, pos))
