#!/bin/bash
# test-rawimg.sh — WP13 codecpack execution path + raw_image pack e2e
# (persistent).
#
#   fixtures (inline python, stdlib only): 3 DICOMs (8-bit MONO2 explicit,
#   16-bit MONO1 signed implicit, 3-frame 16-bit), PGM 8/16-bit, PPM, BMP24
#   (padded, bottom-up), TIFF II 8-bit + MM 16-bit, plus an ENCAPSULATED
#   (JPEG transfer syntax) DICOM that must be REFUSED.
#   mkfs -> cp -> INVFS_CODECPACKS=$REPO/tools/codecpacks invf-sweep
#   (expect one "raw_image (codecpack)" line per supported file) ->
#   class stamps CODEC{13,1} via an inline vol_get_class helper ->
#   invf-verify --deep -> invf-cat bit-exact (sha256) -> the refused DICOM
#   is GENERIC_GUARD{13} and still bit-exact.
#   Negative: fresh image, INVFS_DEC_MEM_LIMIT=1M -> every supported file
#   falls to generic ZSTD with GENERIC_MEMLIMIT{13} (the pack's estimate is
#   raw pixels + 64 MiB) and stays bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-rawimg.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp13rawimg
IMG=wp13rawimg.img
IMGNEG=wp13rawimg-neg.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGNEG"

echo "== tools =="
command -v cjxl >/dev/null || { echo "FAIL: cjxl not installed"; exit 1; }
command -v djxl >/dev/null || { echo "FAIL: djxl not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }

echo "== generate fixtures =="
python3 - "$WORK/orig" <<'PY'
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
PY

# supported = everything except the encapsulated DICOM
SUPPORTED="gray.tif gray16.pgm gray16be.tif gray8.pgm mono16s.dcm mono8.dcm multi16.dcm photo.bmp rgb.ppm"
ALL="$SUPPORTED encap.dcm"

# class-stamp reader (no stock tool prints invfs.class; built from the repo
# objects like test-jxl.sh's helper)
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file.
 * usage: classof <image> <name>  ->  "cls=<n> algo=<n> gen=<n>" or "none" */
#include <stdio.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id;
    uint8_t cls, algo;
    uint16_t gen;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0)
        printf("none\n");
    else
        printf("cls=%u algo=%u gen=%u\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $REPO/build/obj/{volume,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,blake3,blake3_dispatch,blake3_portable}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in $ALL; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep (pack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
PACK_LINES=$(grep -c "raw_image (codecpack)" "$WORK/sweep1.log" || true)
echo "raw_image lines: $PACK_LINES"
[ "$PACK_LINES" -eq 9 ] || { echo "FAIL: expected 9 raw_image lines"; cat "$WORK/sweep1.log"; exit 1; }

echo "== class stamps =="
for f in $SUPPORTED; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=2 algo=13 gen=1" ] || { echo "FAIL: $f: want CODEC{RAWIMG,1}"; exit 1; }
done
C=$("$WORK/classof" "$IMG" encap.dcm)
echo "  encap.dcm: $C"
[ "$C" = "cls=6 algo=13 gen=1" ] || { echo "FAIL: encap.dcm: want GENERIC_GUARD{RAWIMG,1}"; exit 1; }

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== cat bit-exact =="
ok=1
for f in $ALL; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1
echo "all 10 files bit-exact"

echo "== negative: INVFS_DEC_MEM_LIMIT=1M =="
$B/invf-mkfs "$IMGNEG" 0.2 >/dev/null
for f in $ALL; do
    $B/invf-cp "$IMGNEG" "$WORK/orig/$f" "$f" >/dev/null
done
# estimate = raw pixels + 64 MiB > 1M -> every supported file rejected by the
# decode-memory policy; the refused DICOM stays GUARD-stamped generic
INVFS_DEC_MEM_LIMIT=1M $B/invf-sweep "$IMGNEG" > "$WORK/sweep-neg.log" 2>&1 \
    || { cat "$WORK/sweep-neg.log"; exit 1; }
if grep -q "raw_image (codecpack)" "$WORK/sweep-neg.log"; then
    echo "FAIL: a file transcoded under a 1M decode-memory limit"
    grep "raw_image" "$WORK/sweep-neg.log"; exit 1
fi
for f in $SUPPORTED; do
    C=$("$WORK/classof" "$IMGNEG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=5 algo=13 gen=1" ] || { echo "FAIL: $f: want GENERIC_MEMLIMIT{RAWIMG,1}"; exit 1; }
done
ok=1
for f in $ALL; do
    $B/invf-cat "$IMGNEG" "$f" "$WORK/out/$f.neg" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.neg" || { echo "MISMATCH neg $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
$B/invf-verify "$IMGNEG" --deep | tail -1

echo "RAWIMG E2E: PASS"
