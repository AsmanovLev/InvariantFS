import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
res = struct.unpack_from('<H', b, 14)[0]
fsz = struct.unpack_from('<I', b, 36)[0]
for k in range(2):   # cluster 100 is inside BIG.BIN: a self-loop
    struct.pack_into('<I', b, res*512 + k*fsz*512 + 4*100, 100)
open(sys.argv[2], 'wb').write(bytes(b))
