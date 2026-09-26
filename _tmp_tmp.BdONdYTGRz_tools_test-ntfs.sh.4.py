import struct, sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
bps, cs, recsz = 512, 4096, 1024
def fixup(rec):
    uo = struct.unpack_from("<H", rec, 4)[0]
    un = struct.unpack_from("<H", rec, 6)[0]
    usn = rec[uo:uo+2]
    r = bytearray(rec)
    for i in range(len(rec)//bps):
        t = (i+1)*bps-2
        assert r[t:t+2] == usn
        r[t:t+2] = rec[uo+2+2*i:uo+4+2*i]
    return bytes(r)
def runlist(buf, off):
    runs = []; pos = off; lcn = 0
    while True:
        h = buf[pos]; pos += 1
        if h == 0: break
        ll = h & 0xF; ol = h >> 4
        rl = int.from_bytes(buf[pos:pos+ll], "little"); pos += ll
        dl = int.from_bytes(buf[pos:pos+ol], "little", signed=True); pos += ol
        lcn += dl; runs.append((lcn, rl))
    return runs
rec0 = fixup(img[4*cs:4*cs+recsz])
aoff = struct.unpack_from("<H", rec0, 20)[0]
mruns = None; mreal = None
while True:
    t = struct.unpack_from("<I", rec0, aoff)[0]
    if t == 0xFFFFFFFF: break
    al = struct.unpack_from("<I", rec0, aoff+4)[0]
    if t == 0x80:
        ro = struct.unpack_from("<H", rec0, aoff+32)[0]
        mruns = runlist(rec0, aoff+ro); mreal = struct.unpack_from("<Q", rec0, aoff+48)[0]
    aoff += al
def recoff(rn):
    off = rn*recsz; vcn = off//cs; w = off % cs; pos = 0
    for lcn, rl in mruns:
        if pos <= vcn < pos+rl: return (lcn+vcn-pos)*cs+w
        pos += rl
    raise ValueError
n = 0
for rn in range(16, mreal//recsz):
    raw = bytes(img[recoff(rn):recoff(rn)+recsz])
    if raw[:4] != b"FILE": continue
    flags = struct.unpack_from("<H", raw, 22)[0]   # raw flags: below the
    if flags & 1: continue                          # first trailer
    img[recoff(rn) + 510] ^= 0xFF                   # break its USN trailer
    n += 1
    if n == 2: break
assert n >= 2, "want >=2 free records to corrupt"
open(p, "wb").write(bytes(img))
print("  fs-freefix.ntfs: %d free records' trailers broken" % n)
