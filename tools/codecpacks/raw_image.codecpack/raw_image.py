#!/usr/bin/env python3
"""
raw_image.py — raw_image codecpack helper (InvariantFS WP13).

DICOM / PNM / BMP / TIFF  ->  lossless JXL (cjxl), bit-exact rebuild.

The pack turns the ORIGINAL container into (header, pixels, tail): the
untouched bytes before and after the pixel region, plus the pixel region
normalized to something cjxl reads natively (PGM/PPM, maxval 255/65535).
Byte-exactness never depends on parsing the container perfectly — only on
(pix_off, pix_len) delimiting a self-consistent pixel region and on every
pixel transform being bijective (documented per format below).

Blob format (all integers little-endian):
  0   4       "RIMG"
  4   4       header_len   u32 — bytes BEFORE the pixel region, verbatim
  8   ..      header bytes
  ..  4       tail_len     u32 — bytes AFTER the pixel region, verbatim
  ..  ..      tail bytes
  ..  4       meta_len     u32
  ..  ..      map_meta     ASCII "key=value\n" lines (inverse-map params)
  ..  ..      jxl          lossless JXL of the normalized PGM/PPM

map_meta keys: fmt (dcm|pnm|bmp|tif), w, h, frames, spp, bits, le, signed,
invert, planar, bottomup, padhex (BMP row padding bytes, file order).

Bijective transforms (encode applies left-to-right, decode inverts in
reverse order; each is an involution or a rotation):
  signed    x -> (x + 2^(bits-1)) mod 2^bits   (two's-complement rotation;
            self-inverse — PixelRepresentation=1 has no signed PNM form)
  invert    x -> (2^bits - 1) - x              (MONOCHROME1 / TIFF
            WhiteIsZero; self-inverse)
  le        16-bit samples are byte-swapped to the PNM big-endian wire and
            back (DICOM is always LE; II-TIFF is LE; MM-TIFF/PNM are BE)
  bottomup  BMP rows are stored bottom-up; normalized top-down (row flip)
  pad       BMP rows are padded to 4-byte alignment; the original pad bytes
            are kept in map_meta (padhex) and re-inserted on rebuild
  bgr       BMP 24-bit pixels are B,G,R on disk; swapped to R,G,B per pixel
  planar    DICOM PlanarConfiguration=1 stores R*..G*..B*; shuffled to
            interleaved RGB RGB... (and back)

Exit codes: 0 = ok, 3 = refused (unsupported variant — the FS stores the
file generically), 1 = error.

  encode <in> <out>      decode <in> <out>      estimate <in>
estimate prints the decode working set in bytes (raw pixels + 64 MiB codec
overhead) on stdout — WP10 §12.2 header-derived admission.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

REFUSE = 3
CODEC_OVERHEAD = 64 * 1024 * 1024   # fixed djxl/cjxl-side allowance


class Refused(Exception):
    pass


def refuse(msg):
    raise Refused(msg)


# ---------------------------------------------------------------- parsed ----

class Parsed:
    """A source image as (container, pixel-region) pieces."""
    def __init__(self, fmt):
        self.fmt = fmt
        self.w = self.h = self.frames = self.spp = self.bits = None
        self.le = 0          # pixel bytes little-endian on disk
        self.signed = 0
        self.invert = 0
        self.planar = 0      # DICOM PlanarConfiguration=1
        self.bottomup = 0    # BMP row order
        self.padhex = ''     # BMP row padding bytes, file order
        self.pix_off = None  # pixel region
        self.pix_len = None
        self.pix = None      # the original pixel region bytes

    @property
    def npix(self):
        return self.w * self.h * self.frames * self.spp

    def expect_len(self):
        return self.npix * self.bits // 8

    def meta_lines(self):
        keys = (('fmt', self.fmt), ('w', self.w), ('h', self.h),
                ('frames', self.frames), ('spp', self.spp),
                ('bits', self.bits), ('le', self.le), ('signed', self.signed),
                ('invert', self.invert), ('planar', self.planar),
                ('bottomup', self.bottomup), ('padhex', self.padhex))
        return ''.join('%s=%s\n' % (k, v) for k, v in keys).encode('ascii')


# ----------------------------------------------------------------- DICOM ----

# VRs with a 32-bit length field after 2 reserved bytes (PS3.5 §7.1.2)
VR_LONG = (b'OB', b'OW', b'OF', b'SQ', b'UT', b'UN', b'OD', b'OL', b'OV',
           b'UC', b'UR', b'OC', b'OG')

TS_IMPLICIT_LE = '1.2.840.10008.1.2'
TS_EXPLICIT_LE = '1.2.840.10008.1.2.1'


def dicom_elem(b, pos, explicit):
    """One element at pos. Returns (group, elem, ln, hdr_len)."""
    group, elem = struct.unpack_from('<HH', b, pos)
    if explicit:
        vr = b[pos + 4:pos + 6]
        if vr in VR_LONG:
            return group, elem, struct.unpack_from('<I', b, pos + 8)[0], 12
        return group, elem, struct.unpack_from('<H', b, pos + 6)[0], 8
    return group, elem, struct.unpack_from('<I', b, pos + 4)[0], 8


def dicom_int(b, off, ln):
    """US/UL value by element length (implicit VR gives no type)."""
    if ln == 2:
        return struct.unpack_from('<H', b, off)[0]
    if ln == 4:
        return struct.unpack_from('<I', b, off)[0]
    refuse('bad numeric element length %d' % ln)


def parse_dicom(b):
    if len(b) < 132 or b[128:132] != b'DICM':
        refuse('no DICM marker')

    # file meta group (0002) is always explicit VR little-endian
    ts = TS_EXPLICIT_LE
    pos = 132
    while pos + 8 <= len(b):
        group, elem, ln, hdr = dicom_elem(b, pos, True)
        if group != 0x0002:
            break
        if ln == 0xFFFFFFFF or pos + hdr + ln > len(b):
            refuse('bad file-meta element')
        if (group, elem) == (0x0002, 0x0010):
            ts = b[pos + hdr:pos + hdr + ln].rstrip(b'\0 ').decode(
                'ascii', 'replace')
        pos += hdr + ln
    if ts == TS_EXPLICIT_LE:
        explicit = True
    elif ts == TS_IMPLICIT_LE:
        explicit = False
    else:
        # explicit BE (1.2.840.10008.1.2.2), JPEG/RLE (1.2.840.10008.1.2.4.*)
        refuse('transfer syntax %s' % ts)

    p = Parsed('dcm')
    p.le = 1
    photo = None
    nframes = None
    pix_seen = False
    while pos + 8 <= len(b):
        group, elem, ln, hdr = dicom_elem(b, pos, explicit)
        if (group, elem) == (0x7FE0, 0x0010):
            if ln == 0xFFFFFFFF:
                refuse('encapsulated pixel data (compressed transfer syntax)')
            p.pix_off = pos + hdr
            p.pix_len = ln
            pix_seen = True
            break
        if ln == 0xFFFFFFFF:
            refuse('undefined-length non-pixel element (SQ)')
        if pos + hdr + ln > len(b):
            refuse('truncated element')
        if group == 0x0028:
            if elem == 0x0010:
                p.h = dicom_int(b, pos + hdr, ln)
            elif elem == 0x0011:
                p.w = dicom_int(b, pos + hdr, ln)
            elif elem == 0x0002:
                p.spp = dicom_int(b, pos + hdr, ln)
            elif elem == 0x0100:
                p.bits = dicom_int(b, pos + hdr, ln)
            elif elem == 0x0103:
                p.signed = dicom_int(b, pos + hdr, ln)
            elif elem == 0x0006:
                p.planar = dicom_int(b, pos + hdr, ln)
            elif elem == 0x0004:
                photo = b[pos + hdr:pos + hdr + ln].rstrip(b'\0 ').decode(
                    'ascii', 'replace')
            elif elem == 0x0008:
                try:
                    nframes = int(b[pos + hdr:pos + hdr + ln].strip())
                except ValueError:
                    refuse('bad NumberOfFrames')
        pos += hdr + ln

    if not pix_seen:
        refuse('no pixel element')
    if not p.w or not p.h or p.spp is None or p.bits is None:
        refuse('missing geometry tags')
    if p.bits not in (8, 16):
        refuse('BitsAllocated %d' % p.bits)
    if p.signed not in (0, 1):
        refuse('PixelRepresentation %d' % p.signed)
    if p.spp == 1:
        if photo == 'MONOCHROME1':
            p.invert = 1
        elif photo != 'MONOCHROME2':
            refuse('photometric %r' % photo)
    elif p.spp == 3:
        if photo != 'RGB':
            refuse('photometric %r' % photo)
        if p.planar not in (0, 1):
            refuse('PlanarConfiguration %d' % p.planar)
    else:
        refuse('SamplesPerPixel %d' % p.spp)
    p.frames = nframes if nframes else 1
    if p.frames < 1:
        refuse('bad NumberOfFrames')
    need = p.expect_len()
    if p.pix_len < need or p.pix_off + need > len(b):
        refuse('pixel element too short')
    # an even-length pad byte (or any surplus) rides in the tail, verbatim
    p.pix_len = need
    p.pix = b[p.pix_off:p.pix_off + need]
    return p


# ------------------------------------------------------------------- PNM ----

def pnm_tokens(b, pos, n):
    """Read n tokens from pos, skipping whitespace and #-comments.
    Returns (tokens, pos) with pos AT the single whitespace byte that
    separates the last token from the raster."""
    toks = []
    while len(toks) < n:
        while pos < len(b) and chr(b[pos]).isspace():
            pos += 1
        if pos < len(b) and b[pos] == 0x23:      # '#'
            while pos < len(b) and b[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(b) and not chr(b[pos]).isspace() and b[pos] != 0x23:
            pos += 1
        toks.append(b[start:pos])
    return toks, pos


def parse_pnm(b):
    if len(b) < 3 or b[:1] != b'P' or b[1:2] not in (b'5', b'6'):
        refuse('not P5/P6')
    p = Parsed('pnm')
    p.spp = 1 if b[1:2] == b'5' else 3
    toks, pos = pnm_tokens(b, 2, 3)
    try:
        p.w, p.h, maxval = int(toks[0]), int(toks[1]), int(toks[2])
    except ValueError:
        refuse('bad PNM header')
    # cjxl 0.11 accepts only byte-aligned depths ("Getting pixel data
    # failed" otherwise); the FS guard would catch it, refusing is cheaper
    if maxval == 255:
        p.bits = 8
    elif maxval == 65535:
        p.bits = 16
    else:
        refuse('maxval %d (need 255/65535)' % maxval)
    if p.w < 1 or p.h < 1:
        refuse('bad geometry')
    if pos >= len(b) or not chr(b[pos]).isspace():
        refuse('no raster separator')
    p.frames = 1
    # PNM 16-bit samples are big-endian on the wire already: pixels pass
    # through verbatim, le stays 0
    p.pix_off = pos + 1
    p.pix_len = p.expect_len()
    if p.pix_off + p.pix_len > len(b):
        refuse('short raster')
    p.pix = b[p.pix_off:p.pix_off + p.pix_len]
    return p


# ------------------------------------------------------------------- BMP ----

def parse_bmp(b):
    if len(b) < 54 or b[:2] != b'BM':
        refuse('not BMP')
    data_off = struct.unpack_from('<I', b, 10)[0]
    hdr_size = struct.unpack_from('<I', b, 14)[0]
    if hdr_size < 40:
        refuse('OS/2 BMP header')
    w = struct.unpack_from('<i', b, 18)[0]
    h = struct.unpack_from('<i', b, 22)[0]
    planes, bpp = struct.unpack_from('<HH', b, 26)
    comp = struct.unpack_from('<I', b, 30)[0]
    if planes != 1 or comp != 0:
        refuse('compressed BMP')
    if w < 1 or h == 0:
        refuse('bad geometry')
    p = Parsed('bmp')
    p.w, p.h = w, abs(h)
    p.bottomup = 0 if h < 0 else 1
    p.frames = 1
    p.bits = 8
    if bpp == 24:
        p.spp = 3
    elif bpp == 8:
        p.spp = 1
        # palette rides in the header verbatim; it must be grayscale or the
        # PGM normalization would lose color
        ncolors = struct.unpack_from('<I', b, 46)[0] or 256
        pal_off = 14 + hdr_size
        if pal_off + ncolors * 4 > len(b):
            refuse('truncated palette')
        for i in range(ncolors):
            bb, gg, rr = b[pal_off + 4 * i:pal_off + 4 * i + 3]
            if not (bb == gg == rr):
                refuse('non-gray palette')
    else:
        refuse('bpp %d' % bpp)
    row_raw = p.w * p.spp
    row_padded = (row_raw + 3) & ~3
    p.pix_off = data_off
    p.pix_len = row_padded * p.h
    if p.pix_off + p.pix_len > len(b):
        refuse('truncated pixel array')
    p.pix = b[p.pix_off:p.pix_off + p.pix_len]
    p.row_raw = row_raw
    p.row_padded = row_padded
    return p


# ------------------------------------------------------------------- TIFF ----

TTYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8,
              11: 4, 12: 8}


def parse_tiff(b):
    if b[:2] == b'II':
        en = '<'
    elif b[:2] == b'MM':
        en = '>'
    else:
        refuse('not TIFF')
    if len(b) < 8 or struct.unpack_from(en + 'H', b, 2)[0] != 42:
        refuse('not classic TIFF')
    ifd = struct.unpack_from(en + 'I', b, 4)[0]
    if ifd + 2 > len(b):
        refuse('bad IFD offset')
    n = struct.unpack_from(en + 'H', b, ifd)[0]
    if ifd + 2 + n * 12 + 4 > len(b):
        refuse('truncated IFD')
    tags = {}
    for i in range(n):
        off = ifd + 2 + i * 12
        tag, typ = struct.unpack_from(en + 'HH', b, off)
        cnt = struct.unpack_from(en + 'I', b, off + 4)[0]
        tsz = TTYPE_SIZE.get(typ)
        if tsz is None or cnt > 0x100000:
            continue
        total = tsz * cnt
        if total <= 4:
            tags[tag] = (typ, cnt, b[off + 8:off + 8 + total])
        else:
            voff = struct.unpack_from(en + 'I', b, off + 8)[0]
            if voff + total > len(b):
                refuse('truncated value')
            tags[tag] = (typ, cnt, b[voff:voff + total])
    if struct.unpack_from(en + 'I', b, ifd + 2 + n * 12)[0] != 0:
        refuse('multi-page TIFF')

    def vals(tag, default=None):
        if tag not in tags:
            return default
        typ, cnt, data = tags[tag]
        if typ == 3:
            return [struct.unpack_from(en + 'H', data, 2 * i)[0]
                    for i in range(cnt)]
        if typ == 4:
            return [struct.unpack_from(en + 'I', data, 4 * i)[0]
                    for i in range(cnt)]
        if typ == 1:
            return list(data)
        return default

    p = Parsed('tif')
    p.le = 1 if en == '<' else 0
    w, h = vals(256), vals(257)
    if not w or not h:
        refuse('missing geometry')
    p.w, p.h = w[0], h[0]
    bitsl = vals(258, [1])
    comp = vals(259, [1])[0]
    photo = vals(262, [1])[0]
    offs, cnts = vals(273), vals(279)
    p.spp = vals(277, [1])[0]
    planar = vals(284, [1])[0]
    if 320 in tags:
        refuse('palette TIFF')
    if comp != 1:
        refuse('compressed TIFF')
    if planar != 1:
        refuse('planar TIFF')
    if len(set(bitsl)) != 1 or bitsl[0] not in (8, 16):
        refuse('BitsPerSample %s' % bitsl)
    p.bits = bitsl[0]
    if photo == 2:
        if p.spp != 3:
            refuse('RGB with spp %d' % p.spp)
    elif photo in (0, 1):
        if p.spp != 1:
            refuse('gray with spp %d' % p.spp)
        p.invert = 1 if photo == 0 else 0
    else:
        refuse('photometric %d' % photo)
    if not offs or not cnts or len(offs) != 1 or len(cnts) != 1:
        refuse('multi-strip TIFF')          # v1: one contiguous strip
    p.frames = 1
    p.pix_off = offs[0]
    p.pix_len = p.expect_len()
    if cnts[0] != p.pix_len:
        refuse('strip byte count %d != %d' % (cnts[0], p.pix_len))
    if p.pix_off + p.pix_len > len(b):
        refuse('truncated strip')
    p.pix = b[p.pix_off:p.pix_off + p.pix_len]
    return p


# ----------------------------------------------------------------- driver ----

def parse_source(b):
    if len(b) >= 132 and b[128:132] == b'DICM':
        return parse_dicom(b)
    if b[:2] in (b'P5', b'P6'):
        return parse_pnm(b)
    if b[:2] == b'BM':
        return parse_bmp(b)
    if b[:2] in (b'II', b'MM'):
        return parse_tiff(b)
    refuse('unrecognized container')


def unpack_samples(buf, n, bits, le):
    """Original pixel bytes -> list of unsigned sample ints."""
    if bits == 8:
        return list(buf)
    fmt = ('<' if le else '>') + 'H'
    return [struct.unpack_from(fmt, buf, 2 * i)[0] for i in range(n)]


def pack_samples(vals, bits, be):
    """Sample ints -> bytes; be=1 for the PNM wire, be=0 for the container."""
    if bits == 8:
        return bytes(vals)
    fmt = ('>' if be else '<') + 'H'
    out = bytearray()
    pack = struct.pack
    for v in vals:
        out += pack(fmt, v)
    return bytes(out)


def normalize(p):
    """Original pixel region -> normalized PNM raster (bijective maps,
    encode order: value maps, then structural maps)."""
    if p.fmt == 'bmp':
        # row flip to top-down + drop padding + BGR->RGB (8-bit: no value
        # maps apply); the original pad bytes go into map_meta
        out = bytearray()
        pads = bytearray()
        for row in range(p.h):
            src = p.h - 1 - row if p.bottomup else row
            base = src * p.row_padded
            chunk = p.pix[base:base + p.row_raw]
            if p.spp == 3:
                chunk = bytes(b for px in range(0, p.row_raw, 3)
                              for b in (chunk[px + 2], chunk[px + 1],
                                        chunk[px + 0]))
            out += chunk
            pads += p.pix[base + p.row_raw:base + p.row_padded]
        p.padhex = pads.hex()
        return bytes(out)
    vals = unpack_samples(p.pix, p.npix, p.bits, p.le)
    half = 1 << (p.bits - 1)
    mask = (1 << p.bits) - 1
    if p.signed:
        vals = [(v + half) & mask for v in vals]      # rotation
    if p.invert:
        vals = [mask - v for v in vals]               # involution
    if p.fmt == 'dcm' and p.planar and p.spp == 3:
        # R..G..B.. per frame -> RGB RGB...
        npf = p.w * p.h
        out = [0] * (npf * 3 * p.frames)
        for fr in range(p.frames):
            base = fr * npf * 3
            for i in range(npf):
                out[(fr * npf + i) * 3 + 0] = vals[base + i]
                out[(fr * npf + i) * 3 + 1] = vals[base + npf + i]
                out[(fr * npf + i) * 3 + 2] = vals[base + 2 * npf + i]
        vals = out
    return pack_samples(vals, p.bits, 1)


def denormalize(p, raster):
    """djxl's PNM raster -> original pixel region bytes (inverse maps in
    reverse order: structural first, then value maps)."""
    if p.fmt == 'bmp':
        # RGB->BGR, re-insert the original pad bytes, flip rows back
        pads = bytes.fromhex(p.padhex)
        padlen = p.row_padded - p.row_raw
        if len(pads) != padlen * p.h:
            raise ValueError('pad length mismatch')
        region = bytearray(p.row_padded * p.h)
        for row in range(p.h):               # raster rows are top-down
            dst = p.h - 1 - row if p.bottomup else row
            chunk = raster[row * p.row_raw:(row + 1) * p.row_raw]
            if p.spp == 3:
                chunk = bytes(b for px in range(0, p.row_raw, 3)
                              for b in (chunk[px + 2], chunk[px + 1],
                                        chunk[px + 0]))
            base = dst * p.row_padded
            region[base:base + p.row_raw] = chunk
            region[base + p.row_raw:base + p.row_padded] = \
                pads[row * padlen:(row + 1) * padlen]
        return bytes(region)
    vals = unpack_samples(raster, p.npix, p.bits, 0)    # wire is BE
    if p.fmt == 'dcm' and p.planar and p.spp == 3:
        # RGB RGB... -> R..G..B.. per frame
        npf = p.w * p.h
        out = [0] * (npf * 3 * p.frames)
        for fr in range(p.frames):
            base = fr * npf * 3
            for i in range(npf):
                out[base + i] = vals[(fr * npf + i) * 3 + 0]
                out[base + npf + i] = vals[(fr * npf + i) * 3 + 1]
                out[base + 2 * npf + i] = vals[(fr * npf + i) * 3 + 2]
        vals = out
    half = 1 << (p.bits - 1)
    mask = (1 << p.bits) - 1
    if p.invert:
        vals = [mask - v for v in vals]
    if p.signed:
        vals = [(v + half) & mask for v in vals]        # rotation is self-inverse
    return pack_samples(vals, p.bits, 0 if p.le else 1)


def pnm_wire(p, raster):
    """The PGM/PPM file cjxl is fed: full-range maxval, stacked frames."""
    magic = b'P5' if p.spp == 1 else b'P6'
    maxval = 255 if p.bits == 8 else 65535
    hdr = b'%s\n%d %d\n%d\n' % (magic, p.w, p.h * p.frames, maxval)
    return hdr + raster


def pnm_unwire(pnm, p):
    """Parse the PNM djxl produced; return its raster after checking the
    geometry is exactly what we encoded."""
    toks, pos = pnm_tokens(pnm, 2, 3)
    magic_ok = (pnm[:2] == (b'P5' if p.spp == 1 else b'P6'))
    if not magic_ok:
        raise ValueError('djxl returned the wrong PNM flavor')
    w, h, maxval = int(toks[0]), int(toks[1]), int(toks[2])
    if w != p.w or h != p.h * p.frames:
        raise ValueError('djxl geometry changed')
    if maxval != (255 if p.bits == 8 else 65535):
        raise ValueError('djxl bit depth changed')
    if pos >= len(pnm) or not chr(pnm[pos]).isspace():
        raise ValueError('djxl PNM: no raster separator')
    off = pos + 1
    need = p.expect_len()
    if off + need > len(pnm):
        raise ValueError('djxl PNM: short raster')
    return pnm[off:off + need]


def run_jxl(tool, pin, pout, extra):
    r = subprocess.run([tool, pin, pout] + extra,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not os.path.exists(pout) or \
            os.path.getsize(pout) == 0:
        raise RuntimeError('%s failed' % tool)


def cmd_encode(inp, outp):
    with open(inp, 'rb') as f:
        b = f.read()
    p = parse_source(b)
    raster = normalize(p)
    if len(raster) != p.expect_len():
        raise RuntimeError('normalized length mismatch')
    work = tempfile.mkdtemp(prefix='rawimg-')
    try:
        pin = os.path.join(work, 'in.pnm')
        pj = os.path.join(work, 'out.jxl')
        with open(pin, 'wb') as f:
            f.write(pnm_wire(p, raster))
        run_jxl('cjxl', pin, pj, ['-d', '0'])
        with open(pj, 'rb') as f:
            jxl = f.read()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    header = b[:p.pix_off]
    tail = b[p.pix_off + p.pix_len:]
    meta = p.meta_lines()
    blob = b'RIMG' + struct.pack('<I', len(header)) + header + \
        struct.pack('<I', len(tail)) + tail + \
        struct.pack('<I', len(meta)) + meta + jxl
    with open(outp, 'wb') as f:
        f.write(blob)
    return 0


def cmd_decode(inp, outp):
    with open(inp, 'rb') as f:
        blob = f.read()
    if len(blob) < 16 or blob[:4] != b'RIMG':
        raise ValueError('bad blob magic')
    hlen, = struct.unpack_from('<I', blob, 4)
    off = 8
    if off + hlen + 4 > len(blob):
        raise ValueError('truncated blob (header)')
    header = blob[off:off + hlen]
    off += hlen
    tlen, = struct.unpack_from('<I', blob, off)
    off += 4
    if off + tlen + 4 > len(blob):
        raise ValueError('truncated blob (tail)')
    tail = blob[off:off + tlen]
    off += tlen
    mlen, = struct.unpack_from('<I', blob, off)
    off += 4
    if off + mlen > len(blob):
        raise ValueError('truncated blob (meta)')
    meta = {}
    for line in blob[off:off + mlen].decode('ascii').splitlines():
        k, _, v = line.partition('=')
        meta[k] = v
    off += mlen
    jxl = blob[off:]
    if not jxl:
        raise ValueError('blob has no JXL payload')

    p = Parsed(meta['fmt'])
    p.w = int(meta['w'])
    p.h = int(meta['h'])
    p.frames = int(meta['frames'])
    p.spp = int(meta['spp'])
    p.bits = int(meta['bits'])
    p.le = int(meta['le'])
    p.signed = int(meta['signed'])
    p.invert = int(meta['invert'])
    p.planar = int(meta['planar'])
    p.bottomup = int(meta['bottomup'])
    p.padhex = meta.get('padhex', '')
    if p.fmt == 'bmp':
        p.row_raw = p.w * p.spp
        p.row_padded = (p.row_raw + 3) & ~3

    work = tempfile.mkdtemp(prefix='rawimg-')
    try:
        pj = os.path.join(work, 'in.jxl')
        pnm = os.path.join(work, 'out.pgm' if p.spp == 1 else 'out.ppm')
        with open(pj, 'wb') as f:
            f.write(jxl)
        run_jxl('djxl', pj, pnm, [])
        with open(pnm, 'rb') as f:
            wire = f.read()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    raster = pnm_unwire(wire, p)
    pix = denormalize(p, raster)
    want = p.row_padded * p.h if p.fmt == 'bmp' else p.expect_len()
    if len(pix) != want:
        raise ValueError('pixel length mismatch')
    out = header + pix + tail
    with open(outp, 'wb') as f:
        f.write(out)
    return 0


def cmd_estimate(inp):
    with open(inp, 'rb') as f:
        b = f.read()
    p = parse_source(b)      # refused variants fail here, before admission
    print(p.expect_len() + CODEC_OVERHEAD)
    return 0


def main(argv):
    if len(argv) < 3:
        sys.stderr.write('usage: raw_image.py encode|decode <in> <out> | '
                         'estimate <in>\n')
        return 1
    cmd = argv[1]
    try:
        if cmd == 'encode' and len(argv) == 4:
            return cmd_encode(argv[2], argv[3])
        if cmd == 'decode' and len(argv) == 4:
            return cmd_decode(argv[2], argv[3])
        if cmd == 'estimate' and len(argv) == 3:
            return cmd_estimate(argv[2])
        sys.stderr.write('bad arguments\n')
        return 1
    except Refused as e:
        sys.stderr.write('raw_image: refused: %s\n' % e)
        return REFUSE
    except Exception as e:
        sys.stderr.write('raw_image: %s: %s\n' % (cmd, e))
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
