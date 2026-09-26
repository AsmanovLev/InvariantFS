import struct
import sys


def member_ref(path):
    d = open(path, "rb").read()
    be32 = lambda o: struct.unpack(">I", d[o:o + 4])[0]
    be64 = lambda o: struct.unpack(">Q", d[o:o + 8])[0]
    assert d[:4] == b"QFI\xfb"
    cs = 1 << be32(20)
    l1_size, l1_off = be32(36), be64(40)
    l2_per = cs // 8
    out = []
    for i in range(l1_size):
        e = be64(l1_off + 8 * i)
        if not e:
            continue
        l2o = e & 0x3fffffffffffffff
        for j in range(l2_per):
            le = be64(l2o + 8 * j)
            if not le or (le & (1 << 62)) or (le & 1):
                continue
            off = le & 0x3ffffffffffffffe
            out.append(d[off:off + cs])
    return b"".join(out)


for name in ("qgen", "qzero"):
    ref = member_ref(sys.argv[1] + "/%s.qcow2" % name)
    open(sys.argv[1] + "/member-%s.ref" % name, "wb").write(ref)
    print("  %s.qcow2: member reference %d bytes" % (name, len(ref)))
