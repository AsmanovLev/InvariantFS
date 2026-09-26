import struct, sys
blk = open(sys.argv[1], 'rb').read(0x1000)
off = 0x9D0
assert blk[off:off+4] == b'RT30', "RT30 magic missing"
s0, s1, delta, seq = struct.unpack_from('<QQQQ', blk, off + 0xC)
print(s0, s1, delta, seq)
