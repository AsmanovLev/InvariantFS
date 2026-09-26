import struct, sys
img, mode = sys.argv[1], sys.argv[2]
P = 4096

def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return crc ^ 0xFFFFFFFF

def page_crc(page):
    return crc32c(bytes(page[:16]) + bytes(page[20:]))

def seal(page):
    struct.pack_into('<I', page, 16, page_crc(page))
    return bytes(page)

def leaf(gen, entries):
    body = bytearray()
    for k, v in entries:
        body += struct.pack('<H', len(k)) + k
        body += struct.pack('<H', len(v)) + v
    p = bytearray(P)
    p[0:4] = b'BPG3'
    struct.pack_into('<Q', p, 4, gen)
    struct.pack_into('<H', p, 12, 0)              # INVFS_PAGE_LEVEL_LEAF
    struct.pack_into('<H', p, 14, len(entries))
    p[20:20 + len(body)] = body
    return bytearray(seal(p))

def internal(gen, children):
    body = bytearray()
    for k, pba, crc, cgen, flags in children:
        body += struct.pack('<H', len(k)) + k
        body += struct.pack('<Q', pba) + struct.pack('<I', crc)
        body += struct.pack('<Q', cgen) + struct.pack('<I', flags)
    p = bytearray(P)
    p[0:4] = b'BPG3'
    struct.pack_into('<Q', p, 4, gen)
    struct.pack_into('<H', p, 12, 1)              # internal
    struct.pack_into('<H', p, 14, len(children))
    p[20:20 + len(body)] = body
    return bytearray(seal(p))

with open(img, 'r+b') as f:
    blk = f.read(P)
    total = struct.unpack_from('<Q', blk, 0x20)[0]
    mapper_pba = struct.unpack_from('<Q', blk, 0x98)[0]
    mapper_blocks = struct.unpack_from('<I', blk, 0xA0)[0]
    root_pba = mapper_pba + mapper_blocks       # first reserved root-area page
    l0, l1 = root_pba + 1, root_pba + 2
    assert l1 < total, "root area past end of volume"

    la = leaf(1, [(b'alpha', b'one')])
    ca = struct.unpack_from('<I', la, 16)[0]
    lb = leaf(1, [(b'beta', b'two'), (b'gamma', b'three')])
    cb = struct.unpack_from('<I', lb, 16)[0]
    root = internal(2, [(b'alpha', l0, ca, 1, 1),
                        (b'beta',  l1, cb, 1, 1)])

    f.seek(l0 * P); f.write(la)
    f.seek(l1 * P); f.write(lb)
    f.seek(root_pba * P); f.write(root)

    if mode == 'badpage':
        f.seek(l0 * P + 300)
        b = f.read(1)
        f.seek(l0 * P + 300)
        f.write(bytes([b[0] ^ 0x01]))
        print("corrupted leaf page pba %d" % l0)

    rt = bytearray(48)
    rt[0:4] = b'RT30'
    struct.pack_into('<I', rt, 4, 1)             # version
    struct.pack_into('<I', rt, 8, 4096)          # page_size
    struct.pack_into('<Q', rt, 0xC, root_pba)    # root_slot[0]
    struct.pack_into('<Q', rt, 0x24, 3)          # seq
    struct.pack_into('<I', rt, 0x2C, crc32c(bytes(rt[:0x2C])))
    if mode == 'badrt30':
        rt[0x24] ^= 0x01                         # seq byte: CRC now stale
        print("corrupted RT30 descriptor")
    f.seek(0x9D0); f.write(rt)
print("crafted %s: root_pba=%d leaves=%d,%d" % (mode, root_pba, l0, l1))
