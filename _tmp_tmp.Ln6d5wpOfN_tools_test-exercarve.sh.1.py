import os
import numpy as np
from PIL import Image

rng = np.random.default_rng(7)
d = "/dev/shm/wp14exr/orig"

def elf_head():
    # minimal but valid ELF64 little-endian header, e_machine=62 (x86-64)
    h = bytearray(64)
    h[0:4] = b"\x7fELF"
    h[4] = 2; h[5] = 1                       # 64-bit, LE
    h[6] = 1                                 # EV_CURRENT
    h[16:18] = (2).to_bytes(2, "little")     # ET_EXEC
    h[18:20] = (62).to_bytes(2, "little")    # EM_X86_64
    h[20:24] = (1).to_bytes(4, "little")     # version
    return bytes(h)

def photo_jpg(path, w, h, sigma, quality=85):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(
        path, "JPEG", quality=quality)

def photo_png(path, w, h, sigma):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(path, "PNG")

# deterministic code filler: no FF D8 FF / PNG signature in it
def code(n, seed):
    r = np.random.default_rng(seed)
    b = bytearray(r.integers(0, 256, n, dtype=np.uint8))
    for i in range(len(b) - 8):
        if b[i] == 0xFF and b[i+1] == 0xD8 and b[i+2] == 0xFF:
            b[i+1] = 0x11
        if b[i:i+8] == b"\x89PNG\r\n\x1a\n":
            b[i] = 0x88
    return bytes(b)

# --- (a) game.exe: ELF + 8KB code + real JPEG + real PNG + tail ---
photo_jpg(os.path.join(d, "m.jpg"), 640, 480, 14)
photo_png(os.path.join(d, "m.png"), 256, 192, 8)
jpg = open(os.path.join(d, "m.jpg"), "rb").read()
png = open(os.path.join(d, "m.png"), "rb").read()
assert len(jpg) >= 64 * 1024 and len(png) >= 16 * 1024
exe = elf_head() + code(8192, 1) + jpg + png + code(4096, 2)
open(os.path.join(d, "game.exe"), "wb").write(exe)
print("game.exe: %d bytes (jpeg %d @%d, png %d @%d)"
      % (len(exe), len(jpg), 64 + 8192, len(png), 64 + 8192 + len(jpg)))

# --- (b) fake.exe: corrupt pseudo-JPEG (marker walk ok, cjxl refuses) ---
b = bytearray()
b += b"\xff\xd8"                                    # SOI
b += b"\xff\xe0" + (16).to_bytes(2, "big") + \
     b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"  # APP0
b += b"\xff\xdb" + (67).to_bytes(2, "big") + b"\x00" + bytes([8] * 64)
b += b"\xff\xc0" + (17).to_bytes(2, "big") + b"\x08" + \
     (16).to_bytes(2, "big") + (16).to_bytes(2, "big") + \
     b"\x03\x01\x22\x00\x02\x11\x00\x03\x11\x00"      # SOF0 (saw_sof)
b += b"\xff\xc4" + (20).to_bytes(2, "big") + b"\x00" + bytes(17)
b += b"\xff\xda" + (12).to_bytes(2, "big") + \
     b"\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00"      # SOS
b += b"\x11" * 68000                                 # garbage entropy
b += b"\xff\xd9"                                     # EOI
exe = elf_head() + code(8192, 3) + bytes(b) + code(4096, 4)
open(os.path.join(d, "fake.exe"), "wb").write(exe)
print("fake.exe: %d bytes (fake-jpeg %d)" % (len(exe), len(b)))

# --- (c) tiny.exe: 2KB JPEG, below the 16 KiB member minimum ---
photo_jpg(os.path.join(d, "t.jpg"), 128, 128, 20)
t = open(os.path.join(d, "t.jpg"), "rb").read()
assert 1024 < len(t) < 16 * 1024, len(t)
exe = elf_head() + code(8192, 5) + t + code(4096, 6)
open(os.path.join(d, "tiny.exe"), "wb").write(exe)
print("tiny.exe: %d bytes (jpeg %d)" % (len(exe), len(t)))

os.unlink(os.path.join(d, "m.jpg"))
os.unlink(os.path.join(d, "m.png"))
os.unlink(os.path.join(d, "t.jpg"))
