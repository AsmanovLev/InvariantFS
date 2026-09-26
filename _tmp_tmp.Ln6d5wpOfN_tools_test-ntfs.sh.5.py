import struct, sys
p = sys.argv[1]
img = bytearray(open(p, "rb").read())
bps, cs, recsz = 512, 4096, 1024
def unfix(rec):
    uo = struct.unpack_from("<H", rec, 4)[0]
    un = struct.unpack_from("<H", rec, 6)[0]
    usn = rec[uo:uo+2]
    r = bytearray(rec)
    for i in range(len(rec)//bps):
        t = (i+1)*bps-2
        assert r[t:t+2] == usn
        r[t:t+2] = rec[uo+2+2*i:uo+4+2*i]
    return bytes(r), uo, un, usn
def refix(r, uo, un, usn):
    r = bytearray(r)
    for i in range(len(r)//bps):
        t = (i+1)*bps-2
        r[uo+2+2*i] = r[t]; r[uo+3+2*i] = r[t+1]
        r[t] = usn[0]; r[t+1] = usn[1]
    return r
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
rec0, _, _, _ = unfix(img[4*cs:4*cs+recsz])
aoff = struct.unpack_from("<H", rec0, 20)[0]
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
# find tiny.txt's record
target = None
for rn in range(16, mreal//recsz):
    raw = bytes(img[recoff(rn):recoff(rn)+recsz])
    if raw[:4] != b"FILE": continue
    rec, uo, un, usn = unfix(raw)
    if not (struct.unpack_from("<H", rec, 22)[0] & 1): continue
    aoff = struct.unpack_from("<H", rec, 20)[0]
    while True:
        t = struct.unpack_from("<I", rec, aoff)[0]
        if t == 0xFFFFFFFF: break
        al = struct.unpack_from("<I", rec, aoff+4)[0]
        if t == 0x30:
            co = struct.unpack_from("<H", rec, aoff+20)[0]; c = rec[aoff+co:]
            if c[66:66+c[64]*2].decode("utf-16le") == "tiny.txt":
                target = (rn, uo, un, usn)
        aoff += al
    if target: break
assert target, "tiny.txt record not found"
rn, uo, un, usn = target
rec, _, _, _ = unfix(img[recoff(rn):recoff(rn)+recsz])
# rebuild the record: keep every attribute up to the resident $DATA,
# regrow $DATA so the END marker lands at 1016 (>= recsz-16, the old
# decline window), keep everything else identical
aoff = struct.unpack_from("<H", rec, 20)[0]
out = bytearray()
data_attr = None
while True:
    t = struct.unpack_from("<I", rec, aoff)[0]
    if t == 0xFFFFFFFF: break
    al = struct.unpack_from("<I", rec, aoff+4)[0]
    if t == 0x80 and not rec[aoff+8]:
        data_attr = bytearray(rec[aoff:aoff+al])
        break
    out += rec[aoff:aoff+al]
    aoff += al
assert data_attr is not None, "tiny.txt has no resident $DATA"
first_off = struct.unpack_from("<H", rec, 20)[0]
# content sits at coff within the attr; pick a new content length so that
# first_off + len(kept) + new_alen == 1016, alen 8-aligned
coff = struct.unpack_from("<H", data_attr, 20)[0]
new_alen = 1016 - first_off - len(out)
assert new_alen >= coff + 8 and new_alen % 8 == 0, new_alen
new_clen = new_alen - coff
payload = (b"TAILEND-REGRESSION " * (new_clen // 19 + 1))[:new_clen]
na = bytearray(data_attr[:coff])
struct.pack_into("<I", na, 4, new_alen)       # attribute length
struct.pack_into("<I", na, 16, new_clen)      # content size
na += payload
out += na
newrec = bytearray(rec[:first_off]) + out
newrec += struct.pack("<I", 0xFFFFFFFF)       # END at 1016
newrec += bytes(recsz - len(newrec))
struct.pack_into("<I", newrec, 24, len(newrec))   # used size (incl. tail)
img[recoff(rn):recoff(rn)+recsz] = refix(newrec, uo, un, usn)
open(p, "wb").write(bytes(img))
print("  fs-tailend.ntfs: tiny.txt record %d, END marker at %d" % (rn, 1016))
