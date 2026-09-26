import zlib, struct, subprocess, os
import numpy as np
from PIL import Image

rng = np.random.default_rng(20260903)
D = "/dev/shm/w12cpngflac/orig"

def mkpx(w, h, sigma, ct=2, seed=None):
    r_ = np.random.default_rng(seed) if seed is not None else rng
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = r_.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255); g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    if ct == 2: return np.dstack([r, g, b]).astype(np.uint8)
    if ct == 6: return np.dstack([r, g, b, np.clip(128 + n, 0, 255)]).astype(np.uint8)
    if ct == 0: return np.clip((x + y) / 2 + n, 0, 255).astype(np.uint8)
    raise ValueError(ct)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)

def filter_rows(rows, bpp, filters):
    out = bytearray(); prev = bytes(len(rows[0]))
    for idx, row in enumerate(rows):
        f = filters[idx % len(filters)]
        dst = bytearray(len(row))
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 0:   v = row[i]
            elif f == 1: v = (row[i] - a) & 255
            elif f == 2: v = (row[i] - b) & 255
            elif f == 3: v = (row[i] - ((a + b) >> 1)) & 255
            else:        v = (row[i] - paeth(a, b, c)) & 255
            dst[i] = v
        out.append(f); out += dst; prev = row
    return bytes(out)

def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + \
           struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)

def write_png(fn, arr, ct, filters, level, mem, split=1,
              strategy=zlib.Z_DEFAULT_STRATEGY, pre=b"", post=b""):
    """Hand-rolled PNG writer: explicit per-row filters and zlib params, so
       the IDAT stream sits at a known (level, memLevel, strategy) point of
       the FS replica grid."""
    h, w = arr.shape[:2]
    bpp = {0: 1, 2: 3, 3: 1, 6: 4}[ct]
    rows = [arr[r].tobytes() for r in range(h)]
    filt = filter_rows(rows, bpp, filters)
    co = zlib.compressobj(level, zlib.DEFLATED, 15, mem, strategy)
    idat = co.compress(filt) + co.flush()
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    parts = []
    if split <= 1:
        parts = [idat]
    else:   # uneven split: exercises the recipe's split table
        bounds = [len(idat) // 3, len(idat) // 2]
        o = 0
        for bnd in bounds:
            parts.append(idat[o:o + bnd]); o += bnd
        parts.append(idat[o:])
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + pre
    for p in parts: png += chunk(b"IDAT", p)
    png += post + chunk(b"IEND", b"")
    open(os.path.join(D, fn), "wb").write(png)
    print(" ", fn, len(png), "bytes")

# --- transcodable set (leg 1) ---
a = mkpx(320, 240, 6)
write_png("grad_rgb.png", a, 2, [0, 1, 2, 3, 4], 6, 8)          # grid[0]
write_png("paeth_split.png", a, 2, [4], 9, 9, split=3)           # grid[5], 3 IDATs
write_png("rgba_up.png", mkpx(200, 150, 10, 6), 6, [2], 7, 8)    # RGBA, all-Up
write_png("gray_sub.png", mkpx(333, 77, 8, 0), 0, [1], 1, 8)     # odd dims, gray
pre = chunk(b"tEXt", b"Title\x00WP12c fixture")
post = chunk(b"vpAg", b"\x01\x02\x03\x04")                       # ancillary, private
write_png("ancil.png", mkpx(257, 129, 7), 2, [3], 6, 9, split=2,
          pre=pre, post=post)                                    # grid[2] + chunks

# --- refused set (leg 2) ---
# 16-bit grayscale (PIL): the v1 guard (bitdepth != 8) refuses.
g16 = rng.integers(0, 65536, (128, 128)).astype(np.uint16)
Image.fromarray(g16).save(os.path.join(D, "gray16.png"))
# incompressible noise: the size guard (jxl+recipe >= png) refuses.
write_png("noise64.png", rng.integers(0, 256, (64, 64, 3)).astype(np.uint8),
          2, [0], 6, 8)
# Z_FIXED strategy: outside the replica grid (grid = Z_DEFAULT_STRATEGY +
# miniz tdefl) -> "unknown encoder" refusal.
write_png("fixed.png", mkpx(200, 150, 6), 2, [1], 6, 8,
          strategy=zlib.Z_FIXED)
# palette (crafted: PLTE + indexed rows, grid zlib): djxl expands to RGB,
# the pixel-shape memcmp refuses.
pal = bytes(range(256)) * 3
idx = (np.arange(150 * 200, dtype=np.uint32).reshape(150, 200) % 256
       ).astype(np.uint8)
write_png("palette.png", idx, 3, [0], 6, 8,
          pre=chunk(b"PLTE", pal))
# PIL default save: on this box Pillow's stream is zlib-ng (6,9,Z_FILTERED),
# outside the grid -> refused. (Elsewhere it may sit in the grid and
# transcode — either disposition must read back bit-exactly.)
Image.fromarray(mkpx(160, 120, 6)).save(os.path.join(D, "pil_default.png"))

# --- MEMLIMIT set (leg 4) ---
write_png("big.png", mkpx(1200, 900, 6), 2, [0, 1, 2, 3, 4], 6, 8)
write_png("small.png", mkpx(64, 64, 6), 2, [0, 1, 2, 3, 4], 6, 8)

# --- fixture sanity: the interlaced + 16-bit fixtures must be what they
# --- claim (asserted against IHDR) ---
subprocess.run(["magick", os.path.join(D, "grad_rgb.png"),
                "-interlace", "PNG", os.path.join(D, "interlaced.png")],
               check=True)
def ihdr_of(fn):
    d = open(os.path.join(D, fn), "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    return d[16:16 + 13]
ih = ihdr_of("interlaced.png"); assert ih[12] == 1, "interlace flag not set"
ih = ihdr_of("gray16.png");     assert ih[8] == 16, "not a 16-bit PNG"
print("  interlaced.png + gray16.png sanity OK")
