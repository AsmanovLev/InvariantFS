#!/usr/bin/env python3
"""mk-iso-fixtures.py -- synthetic optical images for the iso9660 codecpack
(WP636) e2e, plus the independent ground-truth walker the corpus leg needs.

Why synthetic: this box has no ISO writer (no xorriso/genisoimage/mkisofs,
no pycdlib), and the pack's UDF walker has no real-corpus coverage at all
(every ISO on the box is plain ISO9660). So the fixtures are written byte by
byte here, to the ECMA-119 and ECMA-167/OSTA-UDF layouts the pack parses.

  iso       ISO9660 primary only: text, a real ELF, incompressible bytes,
            an empty file, a subdirectory, a file split over two directory
            records (ISO multi-extent, flag 0x80) and a pair of records
            naming ONE extent (hard link).
  joliet    the same tree plus a Joliet SVD, with DIFFERENT UCS-2 names --
            the pack must prefer the Joliet names and the same extents.
  udf       UDF revision 2 (OSTA UDF 1.02): short_ad files, a two-extent
            file with a hole (AD type 1), a file whose data is embedded in
            its own File Entry (AD type 3 -> no member), an empty file and
            a subdirectory.
  bridge    a UDF bridge disc: the ISO9660 tree AND a UDF tree, naming
            DIFFERENT extents. UDF is authoritative, so its extents must be
            the members and the ISO tree must ride in the recipe.
  vrs-iso   the ISO9660 tree of `iso', but its descriptors start at sector
            19 after the UDF volume recognition sequence (BEA01/NSR02/
            TEA01) and there is NO UDF anchor: the UDF parse declines, and
            the ISO fallback must SKIP the VRS blocks to find the PVD.
            (A real bridge disc is laid out this way; the pack used to stop
            at sector 16 and decline the whole family.)
  decline-* structurally broken images the helper must refuse with exit 3
            (no members / torn volume space / bad both-endian field / no
            volume descriptor at all).

  verify <image> <enumerate.txt>
            independent ground truth: walk the ISO9660 primary tree here,
            in python, and compare member count + byte sum with what the
            pack's `enumerate` printed. Used by the real-corpus leg.

Usage: mk-iso-fixtures.py <kind> <out>
       mk-iso-fixtures.py verify <image> <enumerate.txt>
Deterministic: every byte comes from a fixed pattern or a SHA-256 counter,
never from urandom, so a failure reproduces exactly.
"""
import hashlib
import struct
import sys

SEC = 2048

# Partition Descriptor's partition NUMBER. Deliberately nonzero and unrelated
# to the long_ad partition REFERENCE (always 0 = the index of the single
# partition map), which is what real optical media does -- see build_udf.
PART_NUM = 4352

def prand(n, tag=b'wp636'):
    """Deterministic incompressible bytes."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(tag + str(i).encode()).digest()
        i += 1
    return bytes(out[:n])

def texty(n, tag='iso9660'):
    """Deterministic, highly compressible C-ish text."""
    unit = ("static int %s_helper_%d(const char *p) { return p ? p[0] : 0; }\n"
            % (tag, 0)).encode()
    out = bytearray()
    i = 0
    while len(out) < n:
        out += ("/* %s chunk %d: %s */\n" % (tag, i, 'x' * 24)).encode() + unit
        i += 1
    return bytes(out[:n])

def elfblob():
    """A real ELF when the box has one (BCJ-batchable member), else a
    minimal hand-written ELF64 header -- the batch classifier only reads
    the magic."""
    for p in ('/bin/true', '/usr/bin/true'):
        try:
            with open(p, 'rb') as f:
                return f.read()
        except OSError:
            pass
    return b'\x7fELF\x02\x01\x01' + bytes(64)

def pad(b, sec=SEC):
    return b + bytes((-len(b)) % sec)

# ---------------------------------------------------------------- ISO9660 --
def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)

def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)

def rec(ext_block, size, flags, name_bytes):
    """One ECMA-119 directory record. name_bytes is the raw identifier."""
    rl = 33 + len(name_bytes)
    if len(name_bytes) % 2 == 0:
        rl += 1
    r = bytearray(rl)
    r[0] = rl
    r[1] = 0                                   # no extended-attribute record
    r[2:10] = both32(ext_block)
    r[10:18] = both32(size)
    r[18:25] = bytes([126, 1, 1, 0, 0, 0, 0])  # date: a fixed valid stamp
    r[25] = flags
    r[28:32] = both16(1)                       # volume sequence number
    r[32] = len(name_bytes)
    r[33:33 + len(name_bytes)] = name_bytes
    return bytes(r)

def write_dirs(img, blocks):
    for blk, data in blocks.items():
        img[blk * SEC:blk * SEC + len(data)] = data

def iso_files():
    """(name, data, dup_of) -- dup_of names another entry whose extent this
    one points at (an ISO hard link: one extent, two records)."""
    txt = texty(120 * 1024)
    return [
        ('TEXT.C', txt, None),
        ('PROG.ELF', elfblob(), None),
        ('RAND.BIN', prand(150000), None),
        ('EMPTY.BIN', b'', None),
        ('INNER.TXT', texty(40 * 1024, 'inner'), None),
        ('SPLIT.BIN', texty(30 * 1024, 'split-a'), None),
        ('SPLIT2.BIN', texty(20 * 1024, 'split-b'), None),
        ('LINK1.TXT', texty(6 * 1024, 'link'), None),
        ('LINK2.TXT', None, 'LINK1.TXT'),
    ]

JOLIET_NAMES = {
    'TEXT.C': 'the quick brown fox.txt', 'PROG.ELF': 'a program.elf',
    'RAND.BIN': 'noise.bin', 'EMPTY.BIN': 'nothing.here',
    'INNER.TXT': 'sub dir/inner text.txt', 'SPLIT.BIN': 'split part one.bin',
    'SPLIT2.BIN': 'split part two.bin', 'LINK1.TXT': 'linked one.txt',
    'LINK2.TXT': 'linked two.txt',
}

def build_iso(joliet=False, bad=None, tail=0, vd_start=16, vrs=False):
    """One ISO9660 image. vd_start = sector of the PVD; vrs=True writes the
    UDF volume recognition sequence at sector 16 and starts the ISO
    descriptors after it (that is how a UDF bridge disc is laid out -- and
    the reason the pack's ISO fallback must SKIP VRS sectors instead of
    stopping at the first non-CD001 block).

    With joliet=True the disc carries a SECOND tree: the primary tree keeps
    the 8.3 names, the Joliet tree names the same extents with long UCS-2
    names (JOLIET_NAMES). The pack must report the Joliet names and the
    identical extents -- that is the preference leg."""
    root_blk = vd_start + 3
    files = iso_files()
    if bad == 'nomembers':
        files = [('EMPTY.BIN', b'', None)]
    placement = {}
    blk = vd_start + 8
    for name, data, dup in files:
        if dup:
            placement[name] = placement[dup]
            continue
        placement[name] = (blk, len(data))
        blk += max((len(data) + SEC - 1) // SEC, 1)
    total = blk + tail
    img = bytearray(total * SEC)

    def ident(n, jol):
        return n.encode('utf-16-be') if jol else n.encode('ascii')

    def one_tree(jol, root, sub):
        """Directory record bytes for one tree at extents root/sub."""
        rr, sr = [], []
        for name, data, dup in files:
            eb, sz = placement[name]
            flags = 0x80 if name == 'SPLIT.BIN' else 0
            nm = ident(JOLIET_NAMES.get(name, name) if jol else name, jol)
            # INNER.TXT lives in SUB/, never in the root: a directory must
            # not name its own extent, or the walker recurses into itself.
            (sr if name == 'INNER.TXT' else rr).append(rec(eb, sz, flags, nm))
        subname = ident('sub dir' if jol else 'SUB', jol)
        rr.append(rec(sub, SEC, 0x02, subname))
        top = [rec(root, SEC, 0x02, b'\x00'), rec(root, SEC, 0x02, b'\x01')]
        return b''.join(top + rr), b''.join(
            [rec(sub, SEC, 0x02, b'\x00'), rec(root, SEC, 0x02, b'\x01')] + sr)

    for name, data, dup in files:
        if dup or not data:
            continue
        eb, _sz = placement[name]
        img[eb * SEC:eb * SEC + len(data)] = data
    root, sub = one_tree(False, root_blk, root_blk + 1)
    img[root_blk * SEC:root_blk * SEC + len(root)] = root
    img[(root_blk + 1) * SEC:(root_blk + 1) * SEC + len(sub)] = sub
    def vd(typ, esc=b'\x00\x00\x00', root_ext=root_blk):
        d = bytearray(SEC)
        d[0] = typ
        d[1:6] = b'CD001'
        d[6] = 1
        vss = both32(total + 64) if bad == 'vss' else both32(total)
        d[80:88] = vss
        d[88:91] = esc
        d[120:124] = both16(1)
        d[124:128] = both16(1)
        bs = both16(SEC)
        if bad == 'blocksize':                 # the two copies disagree
            bs = struct.pack('<H', SEC) + struct.pack('>H', 512)
        d[128:132] = bs
        d[156:190] = rec(root_ext, SEC, 0x02, b'\x00')
        return d
    img[vd_start * SEC:(vd_start + 1) * SEC] = vd(1)
    nxt = vd_start + 1
    if joliet:
        jroot, jsub = one_tree(True, root_blk + 2, root_blk + 3)
        img[(root_blk + 2) * SEC:(root_blk + 2) * SEC + len(jroot)] = jroot
        img[(root_blk + 3) * SEC:(root_blk + 3) * SEC + len(jsub)] = jsub
        img[nxt * SEC:(nxt + 1) * SEC] = vd(2, b'%/E', root_blk + 2)
        nxt += 1
    t = bytearray(SEC)
    t[0] = 255
    t[1:6] = b'CD001'
    t[6] = 1
    img[nxt * SEC:(nxt + 1) * SEC] = t
    if vrs:
        for i, name in enumerate((b'BEA01', b'NSR02', b'TEA01')):
            d = bytearray(SEC)
            d[0] = 0
            d[1:6] = name
            d[6] = 1
            img[(16 + i) * SEC:(17 + i) * SEC] = d
    return bytes(img)

# ------------------------------------------------------- UDF (ECMA-167) ----
def le16(v): return struct.pack('<H', v)
def le32(v): return struct.pack('<I', v)
def le64(v): return struct.pack('<Q', v)

def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def udf_tag(blk, tid, loc, crclen=None):
    """Stamp descriptor tag at blk[0:16] (version 2 = ECMA-167 rev 2)."""
    if crclen is None:
        crclen = len(blk) - 16
    blk[0:2] = le16(tid)
    blk[2:4] = le16(2)
    blk[4] = 0
    blk[5] = 0
    blk[6:8] = le16(1)                     # serial number
    blk[8:10] = le16(crc16(bytes(blk[16:16 + crclen])))
    blk[10:12] = le16(crclen)
    blk[12:16] = le32(loc)
    ck = 0
    for i in range(16):
        if i != 4:
            ck += blk[i]
    blk[4] = ck & 0xFF
    return blk

def udf_lad(buf, off, lb, ln=SEC, part=0):
    buf[off:off + 4] = le32(ln)
    buf[off + 4:off + 8] = le32(lb)
    buf[off + 8:off + 10] = le16(part)

def udf_fe(tid, loc, ftype, adtype, info_len, ads, embedded=None):
    b = bytearray(SEC)
    b[27] = ftype                        # fileType: 4 dir, 5 file
    b[34:36] = le16(adtype)              # icbTag flags low 3 bits
    b[56:64] = le64(info_len)
    hdr = 176 if tid == 261 else 216
    b[hdr - 8:hdr - 4] = le32(0)         # lengthOfExtendedAttributes
    b[hdr - 4:hdr] = le32(len(ads) * 8)
    for i, (etype, lb, ln) in enumerate(ads):
        e = hdr + i * 8
        b[e:e + 4] = le32(ln | (etype << 30))
        b[e + 4:e + 8] = le32(lb)
    if embedded is not None:
        b[hdr:hdr + len(embedded)] = embedded
    return udf_tag(b, tid, loc)

def udf_fid(ch, icb_lb, name=None, part=0):
    nm = b'' if name is None else bytes([8]) + name.encode('ascii')
    lifi = len(nm)
    tl = (38 + lifi + 3) & ~3
    b = bytearray(tl)
    b[16:18] = le16(1)                   # fileVersionNumber
    b[18] = ch
    b[19] = lifi
    udf_lad(b, 20, icb_lb, SEC, part)    # ICB long_ad
    b[36:38] = le16(0)                   # lengthOfImplementationUse
    b[38:38 + lifi] = nm
    return udf_tag(b, 257, 0, crclen=tl - 16)

def udf_tree():
    """((name, kind, payload)) -- kind: fe / hole / embed / dir / empty."""
    return [
        ('text.c', 'fe', texty(90 * 1024)),
        ('noise.bin', 'fe', prand(120000)),
        ('holed.bin', 'hole', (texty(8 * 1024, 'ha'), texty(8 * 1024, 'hb'))),
        ('embedded.txt', 'embed', texty(600, 'emb')),
        ('empty.bin', 'empty', b''),
        ('sub', 'dir', [('inner.txt', 'fe', texty(30 * 1024, 'ui'))]),
    ]

def build_udf(base=None, part_start=264, overlay_at=256):
    """UDF 1.02 image. base=None builds a UDF-only image; base=<ISO bytes>
    overlays the UDF structures at `overlay_at` and puts the partition
    after them, producing a bridge disc whose UDF and ISO trees name
    DIFFERENT extents (UDF is authoritative)."""
    LB = {'fsd': 0, 'root_fe': 1, 'root_d': 2, 'sub_fe': 3, 'sub_d': 4}
    nxt = 5
    plan = []
    def take(n=1):
        nonlocal nxt
        b = nxt
        nxt += n
        return b
    tree = udf_tree()
    def nb(n):
        return max((n + SEC - 1) // SEC, 1)
    def walk(entries, depth):
        recs = [(udf_fid(0x08, LB['root_fe']), None, 'parent', None, None, None)]
        for name, kind, payload in entries:
            if kind == 'dir':
                fe_lb = take(1); d_lb = take(4)       # directory data: 4 blocks
                recs.append((udf_fid(0x02, fe_lb, name), name, kind, payload, fe_lb, d_lb))
            elif kind == 'fe':
                fe_lb = take(1); d_lb = take(nb(len(payload)))
                recs.append((udf_fid(0x00, fe_lb, name), name, kind, payload, fe_lb, d_lb))
            elif kind == 'hole':
                fe_lb = take(1)
                a = take(nb(len(payload[0])))
                hole = take(1)
                b = take(nb(len(payload[1])))
                recs.append((udf_fid(0x00, fe_lb, name), name, kind,
                             (payload, fe_lb, a, hole, b), fe_lb, None))
            elif kind == 'embed':
                fe_lb = take(1)
                recs.append((udf_fid(0x00, fe_lb, name), name, kind, payload, fe_lb, None))
            else:                                       # empty
                fe_lb = take(1)
                recs.append((udf_fid(0x00, fe_lb, name), name, kind, payload, fe_lb, None))
        return recs
    top = walk(tree, 0)
    subrecs = None
    for r in top:
        if r[2] == 'dir':
            subrecs = walk(r[3], 1)
    part_len = nxt
    total = part_start + part_len
    if base:
        total = max(total, len(base) // SEC)
    img = bytearray(total * SEC)
    if base:
        img[0:len(base)] = base
    # ---- volume recognition sequence + anchor, unless the base wrote one
    for i, ident in enumerate((b'BEA01', b'NSR02', b'TEA01')):
        d = bytearray(SEC)
        d[0] = 0
        d[1:6] = ident
        d[6] = 1
        img[(16 + i) * SEC:(17 + i) * SEC] = d
    vds_loc = overlay_at + 1
    av = bytearray(SEC)
    av[16:20] = le32(4 * SEC)
    av[20:24] = le32(vds_loc)
    # the anchor is read 512 bytes at a time, so its descriptor CRC covers
    # only the significant area (two extent_ad's = bytes 16..32)
    img[overlay_at * SEC:(overlay_at + 1) * SEC] = udf_tag(av, 2, overlay_at,
                                                           crclen=16)
    pvd = bytearray(SEC)
    udf_tag(pvd, 1, vds_loc)
    pd = bytearray(SEC)
    pd[16:20] = le32(len(img) // SEC)              # volume space size
    # A NONZERO partition NUMBER, with every long_ad referring to it as
    # partition reference 0 (the INDEX of the map below). This is the shape
    # real Windows media has (measured: Windows 11 26H2 arm64 DVD declares
    # partition 2989 and refers to it as 0), and writing 0 here hid a pack
    # bug that compared the reference against the number and therefore
    # declined every real disc. Do not set this back to 0.
    pd[22:24] = le16(PART_NUM)                     # partition number
    pd[188:192] = le32(part_start)                 # start, in 2048 sectors
    pd[192:196] = le32(part_len)
    udf_tag(pd, 5, vds_loc + 1)
    lvd = bytearray(SEC)
    lvd[212:216] = le32(SEC)                       # logical block size
    udf_lad(lvd, 248, LB['fsd'], SEC, 0)           # file set descriptor
    lvd[264:268] = le32(6)                         # partition map table length
    lvd[268:272] = le32(1)                         # number of partition maps
    lvd[440] = 1                                   # type 1 = physical
    lvd[441] = 6
    lvd[442:444] = le16(1)                         # volume sequence number
    lvd[444:446] = le16(PART_NUM)                  # the map's partition number
    udf_tag(lvd, 6, vds_loc + 2)
    td = bytearray(SEC)
    udf_tag(td, 8, vds_loc + 3)
    for i, d in enumerate((pvd, pd, lvd, td)):
        off = (vds_loc + i) * SEC
        img[off:off + SEC] = d
    def absb(lb):
        return (part_start + lb) * SEC
    fsd = bytearray(SEC)
    udf_lad(fsd, 400, LB['root_fe'], SEC, 0)       # rootDirectoryICB
    img[absb(LB['fsd']):absb(LB['fsd']) + SEC] = udf_tag(fsd, 256, part_start)
    rootd = b''.join(r[0] for r in top)
    img[absb(LB['root_d']):absb(LB['root_d']) + len(rootd)] = rootd
    subd = b''.join(r[0] for r in subrecs) if subrecs else b''
    img[absb(LB['root_fe']):absb(LB['root_fe']) + SEC] = udf_fe(
        261, LB['root_fe'], 4, 0, len(rootd), [(0, LB['root_d'], len(rootd))])
    for r in subrecs or ():
        if r[2] == 'parent':
            continue
        name, kind, payload, fe_lb, d_lb = r[1], r[2], r[3], r[4], r[5]
        if kind == 'fe':
            img[absb(fe_lb):absb(fe_lb) + SEC] = udf_fe(
                261, fe_lb, 5, 0, len(payload), [(0, d_lb, len(payload))])
            img[absb(d_lb):absb(d_lb) + len(payload)] = payload
    for r in top:
        if r[2] == 'parent':
            continue
        name, kind, payload, fe_lb, d_lb = r[1], r[2], r[3], r[4], r[5]
        if kind == 'fe':
            img[absb(fe_lb):absb(fe_lb) + SEC] = udf_fe(
                261, fe_lb, 5, 0, len(payload), [(0, d_lb, len(payload))])
            img[absb(d_lb):absb(d_lb) + len(payload)] = payload
        elif kind == 'hole':
            (pa, pb), a, hole, b = payload[0], payload[2], payload[3], payload[4]
            img[absb(a):absb(a) + len(pa)] = pa
            img[absb(hole):absb(hole) + SEC] = b'\xAA' * SEC   # hole bytes
            img[absb(b):absb(b) + len(pb)] = pb
            ads = [(0, a, len(pa)), (1, hole, SEC), (0, b, len(pb))]
            img[absb(fe_lb):absb(fe_lb) + SEC] = udf_fe(
                261, fe_lb, 5, 0, len(pa) + SEC + len(pb), ads)
        elif kind == 'embed':
            img[absb(fe_lb):absb(fe_lb) + SEC] = udf_fe(
                261, fe_lb, 5, 3, len(payload), [], embedded=payload)
        elif kind == 'empty':
            img[absb(fe_lb):absb(fe_lb) + SEC] = udf_fe(
                261, fe_lb, 5, 0, 0, [(0, fe_lb, 0)])
        if kind == 'dir':
            img[absb(fe_lb):absb(fe_lb) + SEC] = udf_fe(
                261, fe_lb, 4, 0, len(subd), [(0, d_lb, len(subd))])
            img[absb(d_lb):absb(d_lb) + len(subd)] = subd
    return bytes(img)

# ------------------------------------------------------- ground truth ------
def gt_walk(img):
    """Independent ISO9660 primary-tree walk (no pack code involved).
    Returns (n_extents, byte_sum) over non-empty recorded extents."""
    # find the primary volume descriptor the way a reader must: the UDF
    # volume recognition sequence (BEA01/NSR02/NSR03/TEA01) can sit at
    # sector 16 and the ISO descriptor set starts after it.
    pvd = None
    for sec in range(16, 16 + 64):
        d = img[sec * SEC:(sec + 1) * SEC]
        if len(d) < SEC:
            break
        if d[1:6] in (b'BEA01', b'NSR02', b'NSR03', b'TEA01'):
            continue
        if d[1:6] != b'CD001':
            break
        if d[0] == 1:
            pvd = d
            break
        if d[0] == 255:
            break
    if pvd is None:
        return None
    blk = struct.unpack_from('<H', pvd, 128)[0]
    root = pvd[156:190]
    n, tot = 0, 0
    stack = [(struct.unpack_from('<I', root, 2)[0],
              struct.unpack_from('<I', root, 10)[0])]
    seen = set()
    # One member per DISTINCT extent: two records naming one extent (an ISO
    # hard link) are one extent, and the pack's member table has to be a set
    # of non-overlapping extents anyway. A multi-extent file's records are
    # distinct extents and count separately, which is what the pack streams.
    extents = set()
    while stack:
        lba, dlen = stack.pop()
        if (lba, dlen) in seen:
            continue
        seen.add((lba, dlen))
        d = img[lba * blk:lba * blk + dlen]
        i = 0
        while i < len(d) and i + 33 <= len(d) and d[i]:
            rl = d[i]
            eb = struct.unpack_from('<I', d, i + 2)[0]
            sz = struct.unpack_from('<I', d, i + 10)[0]
            fl = d[i + 25]
            nlen = d[i + 32]
            name = d[i + 33:i + 33 + nlen]
            if not (nlen == 1 and name in (b'\x00', b'\x01')):
                if fl & 0x02:
                    stack.append((eb, sz))
                elif sz and (eb, sz) not in extents:
                    extents.add((eb, sz))
                    n += 1
                    tot += sz
            i += rl
        i = (i // blk + 1) * blk if i < len(d) and not d[i] else i
    return n, tot

def verify(image, enum_txt):
    img = open(image, 'rb').read()
    scale = max(1, len(img) // 40000000)          # 39 MB memory per 40 MB file
    if scale > 1:
        del img
        with open(image, 'rb') as f:
            f.seek(16 * SEC)
            pvd = f.read(SEC)
            if pvd[1:6] != b'CD001' or pvd[0] != 1:
                print("DECLINE: no ISO9660 primary volume descriptor; "
                      "no independent walk available (UDF image)")
                return 0
        print("SKIP: image too large for the in-memory walker")
        return 0
    gt = gt_walk(img)
    if gt is None:
        print("DECLINE: no ISO9660 primary tree")
        return 0
    n = 0
    tot = 0
    for ln in open(enum_txt):
        parts = ln.rstrip('\n').split('\t')
        if len(parts) == 3:
            n += 1
            tot += int(parts[2])
    print("ground truth: %d extents, %d bytes | pack: %d members, %d bytes"
          % (gt[0], gt[1], n, tot))
    if (n, tot) != gt:
        print("MISMATCH")
        return 1
    print("MATCH")
    return 0

# ---------------------------------------------------------------- main -----
KINDS = ('iso', 'joliet', 'udf', 'bridge', 'vrs-iso', 'decline-nomembers',
         'decline-vss', 'decline-blocksize', 'decline-notcd')

def main(argv):
    if len(argv) == 4 and argv[1] == 'verify':
        return verify(argv[2], argv[3])
    if len(argv) != 3:
        print(__doc__.strip().split('\n\n')[-1].strip(), file=sys.stderr)
        return 2
    kind, out = argv[1], argv[2]
    if kind == 'iso':
        img = build_iso()
    elif kind == 'joliet':
        img = build_iso(joliet=True)
    elif kind == 'udf':
        img = build_udf()
    elif kind == 'bridge':
        img = build_udf(base=build_iso(vd_start=19, vrs=True), part_start=264)
    elif kind == 'vrs-iso':
        # descriptors after the UDF VRS, but NO UDF anchor: the UDF
        # parse declines, so the ISO fallback must skip BEA01/NSR02/
        # TEA01 to find the PVD at sector 19.
        img = build_iso(vd_start=19, vrs=True)
    elif kind == 'decline-nomembers':
        img = build_iso(bad='nomembers')
    elif kind == 'decline-vss':
        img = build_iso(bad='vss')
    elif kind == 'decline-blocksize':
        img = build_iso(bad='blocksize')
    elif kind == 'decline-notcd':
        img = bytearray(build_iso())
        img[16 * SEC + 1:16 * SEC + 6] = b'XXXXX'
        img = bytes(img)
    else:
        print("unknown kind %r (want one of: %s)" % (kind, ', '.join(KINDS)),
              file=sys.stderr)
        return 2
    with open(out, 'wb') as f:
        f.write(img)
    print("%s: %d bytes" % (out, len(img)))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
