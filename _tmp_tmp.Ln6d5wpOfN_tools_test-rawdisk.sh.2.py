import struct
import sys

def parse_recipe(path):
    b = open(path, "rb").read()
    assert b[:4] == b"RDR1", "bad recipe magic"
    size = struct.unpack_from("<Q", b, 4)[0]
    nmem = struct.unpack_from("<I", b, 12)[0]
    pos = 16
    mem = []
    for _ in range(nmem):
        mem.append(struct.unpack_from("<IQQ", b, pos))
        pos += 20
    nrng = struct.unpack_from("<I", b, pos)[0]
    pos += 4
    ranges = []
    for _ in range(nrng):
        off, ln = struct.unpack_from("<QQ", b, pos)
        ranges.append((off, ln, pos + 16))
        pos += 16 + ln
    assert pos == len(b), "recipe trailing garbage"
    return size, mem, ranges, b

def check(img, recipe, map_path):
    orig = open(img, "rb").read()
    size, mem, ranges, rblob = parse_recipe(recipe)
    assert size == len(orig)
    mb = open(map_path, "rb").read()
    assert mb[:4] == b"MRMP"
    count = struct.unpack_from("<I", mb, 4)[0]
    assert len(mb) == 8 + count * 29
    pos = 0
    n_rec = n_mem = 0
    for i in range(count):
        off, ln, kind, idx, src = struct.unpack_from("<QQBIQ", mb, 8 + i * 29)
        assert off == pos and ln > 0, "map does not partition"
        if kind == 0:
            assert rblob[src:src + ln] == orig[off:off + ln], "RECIPE bytes"
            n_rec += 1
        else:
            m = [m for m in mem if m[0] == idx]
            assert m and src + ln <= m[0][2], "MEMBER bounds"
            n_mem += 1
        pos += ln
    assert pos == len(orig)
    print("  %s: %d entries (%d RECIPE + %d MEMBER), partition + recipe bytes OK"
          % (img.split("/")[-1], count, n_rec, n_mem))

w = sys.argv[1]
check(w + "/orig/disk-mbr.img", w + "/out/recipe.a", w + "/out/map.a")
check(w + "/orig/disk-gpt.img", w + "/out/recipe.b", w + "/out/map.b")
