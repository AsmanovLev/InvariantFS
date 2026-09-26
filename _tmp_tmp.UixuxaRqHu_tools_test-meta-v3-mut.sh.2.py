import hashlib, struct, sys
f = open(sys.argv[1], 'rb')
blk = f.read(0x1000)
off = 0x9D0
delta = struct.unpack_from('<Q', blk, off + 0x1C)[0]
h = hashlib.sha256()
if delta:
    f.seek(delta * 4096)
    h.update(f.read(128 * 1024))
print(h.hexdigest())
