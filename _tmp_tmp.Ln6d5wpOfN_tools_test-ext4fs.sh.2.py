import os, struct, sys
img, mapf, rcpt, mdir = sys.argv[1:5]
orig = open(img, "rb").read()
recipe = open(rcpt, "rb").read()
d = open(mapf, "rb").read()
assert d[:4] == b"MRMP", "map magic"
(n,) = struct.unpack_from("<I", d, 4)
assert len(d) == 8 + n * 29, "map blob length"
out = bytearray(len(orig))
pos = 0
nmem = 0
for i in range(n):
    off, ln, kind, idx, so = struct.unpack_from("<QQBIQ", d, 8 + i * 29)
    assert off == pos and ln > 0, "map must partition contiguously"
    if kind == 0:
        assert idx == 0 and so + ln <= len(recipe), "RECIPE bounds"
        out[off:off+ln] = recipe[so:so+ln]
    else:
        mp = os.path.join(mdir, "%d" % idx)
        m = open(mp, "rb").read()
        assert so + ln <= len(m), "MEMBER bounds"
        out[off:off+ln] = m[so:so+ln]
        nmem += 1
    pos += ln
assert pos == len(orig), "map must cover the whole image"
assert bytes(out) == orig, "map-splice mismatch"
print("    map: %d entries (%d member runs), splice bit-exact" % (n, nmem))
