# hand-scatter FRAG.BIN's chain: copy each cluster to a strided free
# cluster and re-link (both FAT copies). fsck-clean genuine fragmentation.
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
spc = b[13]; res = struct.unpack_from('<H', b, 14)[0]; nf = b[16]
fsz = struct.unpack_from('<I', b, 36)[0]
first = res + nf*fsz
clus = spc*512
def ent(c, k=0): return struct.unpack_from('<I', b, res*512 + k*fsz*512 + 4*c)[0] & 0x0FFFFFFF
def setent(c, v):
    for k in range(nf):
        o = res*512 + k*fsz*512 + 4*c
        old = struct.unpack_from('<I', b, o)[0]
        struct.pack_into('<I', b, o, (old & 0xF0000000) | v)
def coff(c): return (first + (c-2)*spc)*512
root = struct.unpack_from('<I', b, 44)[0]
chain = []; c = root
while c < 0x0FFFFFF8: chain.append(c); c = ent(c)
data = b''.join(bytes(b[coff(c):coff(c)+clus]) for c in chain)
fc = sz = diroff = None
for i in range(0, len(data), 32):
    e = data[i:i+32]
    if e[0] == 0: break
    if e[0] == 0xE5 or e[11] == 0x0F: continue
    if e[0:8].decode('ascii', 'replace').strip() == 'FRAG':
        fc = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        sz = struct.unpack_from('<I', e, 28)[0]
        diroff = coff(chain[i // clus]) + (i % clus)
        break
assert fc is not None, "FRAG.BIN not found"
need = (sz + clus - 1)//clus
cl = [fc]
for _ in range(need-1): cl.append(ent(cl[-1]))
nclu = (len(b)//512 - first)//spc
used = set(cl)
frees = [c for c in range(2, nclu+2) if c not in used and ent(c) == 0]
stride = max(1, len(frees)//need)
targets = [frees[(i*stride) % len(frees)] for i in range(need)]
assert len(set(targets)) == need
for s, d in zip(cl, targets):
    b[coff(d):coff(d)+clus] = b[coff(s):coff(s)+clus]
for i in range(need-1): setent(targets[i], targets[i+1])
setent(targets[-1], 0x0FFFFFFF)
for c in cl: setent(c, 0)
struct.pack_into('<H', b, diroff+20, (targets[0] >> 16) & 0xFFFF)
struct.pack_into('<H', b, diroff+26, targets[0] & 0xFFFF)
open(p, 'wb').write(bytes(b))
print("  fat32.img: FRAG.BIN scattered to %d single-cluster runs" % need)
