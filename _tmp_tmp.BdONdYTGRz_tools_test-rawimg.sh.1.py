import os
import struct
import sys

def dicom_meta(ts):
    v = ts.encode('ascii')
    if len(v) % 2:
        v += b'\0'
    return struct.pack('<HH', 0x0002, 0x0010) + b'UI' + \
        struct.pack('<H', len(v)) + v

def dicom_elem(g, e, vr, val, explicit):
    if explicit:
        if vr in (b'OB', b'OW', b'OF', b'SQ', b'UT', b'UN'):
            return struct.pack('<HH', g, e) + vr + b'\0\0' + \
                struct.pack('<I', len(val)) + val
        return struct.pack('<HH', g, e) + vr + struct.pack('<H', len(val)) + val
    return struct.pack('<HH', g, e) + struct.pack('<I', len(val)) + val

def dicom_us(g, e, v, explicit):
    return dicom_elem(g, e, b'US', struct.pack('<H', v), explicit)

def make_dicom(path, w, h, bits, signed, photo, frames,
               ts='1.2.840.10008.1.2.1', comment=b''):
    explicit = ts != '1.2.840.10008.1.2'
    b = bytearray(b'\0' * 128 + b'DICM')
    b += dicom_meta(ts)
    b += dicom_us(0x0028, 0x0002, 1, explicit)
    b += dicom_elem(0x0028, 0x0004, b'CS', photo.encode() + b' ', explicit)
    b += dicom_us(0x0028, 0x0010, h, explicit)
    b += dicom_us(0x0028, 0x0011, w, explicit)
    b += dicom_us(0x0028, 0x0100, bits, explicit)
    b += dicom_us(0x0028, 0x0103, signed, explicit)
    if frames > 1:
        b += dicom_elem(0x0028, 0x0008, b'IS', str(frames).encode() + b' ',
                        explicit)
    if comment:
        b += dicom_elem(0x0028, 0x9000, b'LO', comment, explicit)
    npix = w * h * frames
    if bits == 8:
        pix = bytes((i * 7 + 3) & 0xFF for i in range(npix))
        vr = b'OB'
    else:
        pix = b''.join(struct.pack('<H', (i * 977 + 11) & 0xFFFF)
                       for i in range(npix))
        vr = b'OW'
    b += dicom_elem(0x7FE0, 0x0010, vr, pix, explicit)
    b += b'TAILMARKER'              # junk after the pixel element
    open(path, 'wb').write(bytes(b))

def make_dicom_encapsulated(path):
    b = bytearray(b'\0' * 128 + b'DICM')
    b += dicom_meta('1.2.840.10008.1.2.4.50')   # JPEG baseline
    b += dicom_us(0x0028, 0x0010, 8, True)
    b += dicom_us(0x0028, 0x0011, 8, True)
    b += dicom_us(0x0028, 0x0100, 8, True)
    b += struct.pack('<HH', 0x7FE0, 0x0010) + b'OB' + b'\0\0' + \
        struct.pack('<I', 0xFFFFFFFF)           # undefined length
    b += struct.pack('<HHI', 0xFFFE, 0xE000, 16) + b'\xFF\xD8\xFF\xD9' + \
        b'\0' * 12
    b += struct.pack('<HHI', 0xFFFE, 0xE0DD, 0)
    open(path, 'wb').write(bytes(b))

def make_pgm(path, w, h, bits=8):
    maxval = 255 if bits == 8 else 65535
    hdr = b'P5\n# synthetic fixture\n%d %d\n%d\n' % (w, h, maxval)
    if bits == 8:
        data = bytes((x * 3 + y * 11) & 0xFF for y in range(h)
                     for x in range(w))
    else:
        data = b''.join(struct.pack('>H', (x * 513 + y * 37) & 0xFFFF)
                        for y in range(h) for x in range(w))
    open(path, 'wb').write(hdr + data)

def make_ppm(path, w, h):
    hdr = b'P6\n%d %d\n255\n' % (w, h)
    data = bytes(((x * 5 + y * 3 + c * 41) & 0xFF)
                 for y in range(h) for x in range(w) for c in range(3))
    open(path, 'wb').write(hdr + data)

def make_bmp24(path, w, h):
    row_raw = w * 3
    pad = (4 - row_raw % 4) % 4
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(((x * 7 + y * 11) & 0xFF, (x + y * 5) & 0xFF,
                          (x * 3 + y) & 0xFF))        # BGR on disk
        row += bytes((0xAA, 0xBB, 0xCC)[:pad])        # non-zero padding
        rows.append(bytes(row))
    rows.reverse()                                    # bottom-up
    pix = b''.join(rows)
    hdr = b'BM' + struct.pack('<IHHI', 54 + len(pix), 0, 0, 54)
    hdr += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(pix),
                       2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + pix)

def make_tiff(path, w, h, bits=8, en='<'):
    pixlen = w * h * (bits // 8)
    n = 9
    pix_off = 8 + 2 + n * 12 + 4
    if bits == 8:
        pix = bytes((i * 13 + 7) & 0xFF for i in range(w * h))
    else:
        pix = b''.join(struct.pack(en + 'H', (i * 259 + 5) & 0xFFFF)
                       for i in range(w * h))

    def ent(tag, typ, cnt, val):
        if typ == 3:
            v = struct.pack(en + 'H', val) + b'\0\0'
        else:
            v = struct.pack(en + 'I', val)
        return struct.pack(en + 'HHI', tag, typ, cnt) + v

    ifd = struct.pack(en + 'H', n)
    ifd += ent(256, 4, 1, w)
    ifd += ent(257, 4, 1, h)
    ifd += ent(258, 3, 1, bits)
    ifd += ent(259, 3, 1, 1)
    ifd += ent(262, 3, 1, 1)                          # BlackIsZero
    ifd += ent(273, 4, 1, pix_off)
    ifd += ent(277, 3, 1, 1)
    ifd += ent(278, 4, 1, h)
    ifd += ent(279, 4, 1, pixlen)
    ifd += struct.pack(en + 'I', 0)
    bom = b'II' if en == '<' else b'MM'
    open(path, 'wb').write(bom + struct.pack(en + 'HI', 42, 8) + ifd + pix)

d = sys.argv[1]
make_dicom(os.path.join(d, 'mono8.dcm'), 64, 48, 8, 0, 'MONOCHROME2', 1)
make_dicom(os.path.join(d, 'mono16s.dcm'), 32, 24, 16, 1, 'MONOCHROME1', 1,
           ts='1.2.840.10008.1.2', comment=b'implicit VR LE')
make_dicom(os.path.join(d, 'multi16.dcm'), 16, 16, 16, 0, 'MONOCHROME2', 3)
make_dicom_encapsulated(os.path.join(d, 'encap.dcm'))
make_pgm(os.path.join(d, 'gray8.pgm'), 61, 43)
make_pgm(os.path.join(d, 'gray16.pgm'), 33, 21, bits=16)
make_ppm(os.path.join(d, 'rgb.ppm'), 51, 37)
make_bmp24(os.path.join(d, 'photo.bmp'), 61, 40)
make_tiff(os.path.join(d, 'gray.tif'), 50, 30)
make_tiff(os.path.join(d, 'gray16be.tif'), 25, 17, bits=16, en='>')
for f in sorted(os.listdir(d)):
    print(' ', f, os.path.getsize(os.path.join(d, f)))
