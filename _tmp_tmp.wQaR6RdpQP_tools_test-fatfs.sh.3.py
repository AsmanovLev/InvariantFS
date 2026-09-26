import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
bps = 1 << b[108]; clus = bps << b[109]
fat_off = struct.unpack_from('<I', b, 80)[0]*bps
heap = struct.unpack_from('<I', b, 88)[0]*bps
nclu = struct.unpack_from('<I', b, 92)[0]
root = struct.unpack_from('<I', b, 96)[0]
def ent(c): return struct.unpack_from('<I', b, fat_off + 4*c)[0]
def setent(c, v): struct.pack_into('<I', b, fat_off + 4*c, v)
def coff(c): return heap + (c-2)*clus
chain = []; c = root
while c < 0xFFFFFFF8: chain.append(c); c = ent(c)
data = b''.join(bytes(b[coff(c):coff(c)+clus]) for c in chain)
def dir_img_off(byteoff):
    return coff(chain[byteoff // clus]) + (byteoff % clus)
bmp_fc = bmp_len = None
frag = None
i = 0
while i+32 <= len(data) and data[i] != 0:
    t = data[i]
    if t == 0x81:   # allocation bitmap
        bmp_fc = struct.unpack_from('<I', data, i+20)[0]
        bmp_len = struct.unpack_from('<Q', data, i+24)[0]
    if t == 0x85:
        ns = data[i+1]
        sx = data[i+32:i+64]
        name = b''
        for j in range(2, ns+1):
            e = data[i+32*j:i+32*j+32]
            if e[0] == 0xC1: name += e[2:32]
        if name.decode('utf-16-le', 'replace').rstrip('\0') == 'frag.bin':
            frag = (i, ns, sx[1], struct.unpack_from('<Q', sx, 8)[0],
                    struct.unpack_from('<I', sx, 20)[0])
        i += 32*(1+ns)
    else:
        i += 32
assert frag and bmp_fc, "fixtures not found"
fi, ns, flags, valid, fc = frag
if not (flags & 2):
    print("  exfat.img: frag.bin already chain-mode, leaving it alone")
    sys.exit(0)
need = (valid + clus - 1)//clus
old = [fc + k for k in range(need)]
bmp = bytearray(b[coff(bmp_fc):coff(bmp_fc)+bmp_len])
def bit(c): return (bmp[(c-2)//8] >> ((c-2) % 8)) & 1
def setbit(c, v):
    o = (c-2)//8; m = 1 << ((c-2) % 8)
    bmp[o] = (bmp[o] | m) if v else (bmp[o] & ~m)
used_old = set(old)
frees = [c for c in range(2, nclu+2) if c not in used_old and not bit(c)]
stride = max(1, len(frees)//need)
targets = [frees[(k*stride) % len(frees)] for k in range(need)]
assert len(set(targets)) == need
for s, d in zip(old, targets):
    b[coff(d):coff(d)+clus] = b[coff(s):coff(s)+clus]
for k in range(need-1): setent(targets[k], targets[k+1])
setent(targets[-1], 0xFFFFFFFF)
for c in old: setent(c, 0)
for c in targets: setbit(c, 1)
for c in old: setbit(c, 0)
b[coff(bmp_fc):coff(bmp_fc)+bmp_len] = bytes(bmp)
so = dir_img_off(fi)
sx_off = so + 32
assert b[sx_off] == 0xC0
b[sx_off+1] = flags & ~0x02                    # NoFatChain clear: chain mode
struct.pack_into('<I', b, sx_off+20, targets[0])
cs = 0                                          # entry-set checksum: rotate
for k, byte in enumerate(b[so:so + 32*(1+ns)]): # right 1 + add, skipping the
    if k in (2, 3): continue                    # checksum field itself
    cs = ((cs << 15) | (cs >> 1)) & 0xFFFF
    cs = (cs + byte) & 0xFFFF
struct.pack_into('<H', b, so+2, cs)
open(p, 'wb').write(bytes(b))
print("  exfat.img: frag.bin scattered to %d chain-mode runs (checksum %#06x)"
      % (need, cs))
