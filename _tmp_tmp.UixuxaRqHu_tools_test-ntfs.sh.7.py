import struct, sys
w = sys.argv[1]
img = open(w + "/fs.ntfs", "rb").read()
mapb = open(w + "/map", "rb").read()
recipe = open(w + "/recipe", "rb").read()
assert mapb[:4] == b"MRMP"
n, = struct.unpack_from("<I", mapb, 4)
assert len(mapb) == 8 + n * 29, "map blob size"
pos = 0
members = {}
frag_runs = 0
first_member = None
r2m = m2r = m2m = None
prev = None
ents = []
for i in range(n):
    off, ln, kind, idx, so = struct.unpack_from("<QQBIQ", mapb, 8 + i*29)
    ents.append((off, ln, kind, idx, so))
    assert off == pos and ln > 0, "partition broken at %d" % i
    if kind == 0:
        assert idx == 0 and so + ln <= len(recipe)
        assert recipe[so:so+ln] == img[off:off+ln], "recipe range %d lies" % i
    else:
        members.setdefault(idx, 0)
        members[idx] += ln
        mbr = open(w + "/mbr/%d" % idx, "rb").read()
        assert so + ln <= len(mbr)
        assert mbr[so:so+ln] == img[off:off+ln], "member range %d lies" % i
    pos += ln
assert pos == len(img), "partition does not cover the image"
# member disk bytes <= usize (sparse member: far less)
tbl = {}
for line in open(w + "/table"):
    i_, _n, u_ = line.split("\t")
    tbl[int(i_)] = int(u_)
for idx, b in members.items():
    assert b <= tbl[idx], "member %d map bytes over usize" % idx
# frag.bin must be multi-run
frag_idx = next(i_ for i_, n_, u_ in
                (l.split("\t") for l in open(w + "/table")) if n_ == "frag.bin")
frag_idx = int(frag_idx)
frag_ents = [e for e in ents if e[2] == 1 and e[3] == frag_idx]
assert len(frag_ents) >= 2, "frag.bin is not multi-run"
# the sparse member must have exactly 2 real runs of 16 KB
sp_idx = int(next(i_ for i_, n_, u_ in
                  (l.split("\t") for l in open(w + "/table")) if n_ == "sparse.bin"))
sp_ents = [e for e in ents if e[2] == 1 and e[3] == sp_idx]
assert len(sp_ents) == 2 and all(e[1] == 16384 for e in sp_ents), "sparse runs"
print("  partition exact: %d entries; frag.bin runs: %d; sparse real runs: %d"
      % (n, len(frag_ents), len(sp_ents)))
# ranged-read offsets for the FS legs
def emit(name, off, ln):
    print_off = "%s_OFF=%d %s_LEN=%d" % (name, off, name, ln)
    return print_off
out = []
# mid-member: inside frag's first run (clamped to stay strictly inside)
e = frag_ents[0]
mo = e[0] + max(0, e[1]//2 - 32768)
ml = min(65536, e[1] - (mo - e[0]))
out.append(emit("MIDM", mo, ml))
# recipe->member boundary: first RECIPE followed by MEMBER
for i in range(n - 1):
    if ents[i][2] == 0 and ents[i+1][2] == 1:
        b = ents[i+1][0]
        out.append(emit("R2M", b - 32, 64))
        break
# member->recipe boundary
for i in range(n - 1):
    if ents[i][2] == 1 and ents[i+1][2] == 0:
        b = ents[i+1][0]
        out.append(emit("M2R", b - 32, 64))
        break
# member->member boundary (two members' runs adjacent)
for i in range(n - 1):
    if ents[i][2] == 1 and ents[i+1][2] == 1 and ents[i][3] != ents[i+1][3]:
        b = ents[i+1][0]
        out.append(emit("M2M", b - 32, 64))
        break
# sparse member: read across its hole (content window 95K..130K crosses
# the first real run into the big hole): served via member bytes
out.append(emit("SPARSE", 0, 0))   # placeholder; the sparse member's DISK
                                   # offsets are not contiguous — ranged
                                   # reads target the image, not content
out.append(emit("TAIL", len(img) - 1000, 1000))
out.append(emit("HEAD", 0, 64))
open(w + "/layout", "w").write("\n".join(out) + "\n")
print("  layout:", " ".join(x.split()[0] for x in out if "OFF" in x))
