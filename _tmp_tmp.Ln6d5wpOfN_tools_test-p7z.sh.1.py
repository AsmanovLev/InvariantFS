import os
import random
import struct
import sys
import zlib

w = sys.argv[1]
fsrc = os.path.join(w, "fsrc")
orig = os.path.join(w, "orig")
rnd = random.Random(42)

# --- member content -------------------------------------------------------
texts = []
for root, dirs, files in os.walk(os.environ["REPO"] + "/tools/busybox-src"):  # fixture input (submodule)
    dirs.sort()
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                texts.append(open(p, "rb").read())
        if len(texts) >= 2:
            break
    if len(texts) >= 2:
        break
assert len(texts) == 2, "need two busybox .c fixtures"
elf = None
for p in ("/usr/bin/passwd", "/usr/bin/gpg", "/bin/ls", "/usr/bin/ls",
          "/bin/bash", "/usr/bin/bash"):
    if os.path.isfile(p) and not os.path.islink(p):
        with open(p, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF" and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = open(p, "rb").read()
            break
assert elf, "no x86-64 ELF fixture found"

open(os.path.join(fsrc, "alpha.c"), "wb").write(texts[0])
open(os.path.join(fsrc, "beta.bin"), "wb").write(elf)
open(os.path.join(fsrc, "empty.dat"), "wb").write(b"")
os.makedirs(os.path.join(fsrc, "deep/nested"))
open(os.path.join(fsrc, "deep/nested/leaf.txt"), "wb").write(texts[1])
# 9 MiB + 12345 of seeded random: crosses the pack's 8 MiB streaming
# windows in extract/strip/rebuild, and stays unclassified in the FS.
open(os.path.join(fsrc, "rand.bin"), "wb").write(
    rnd.randbytes(9 * 1024 * 1024 + 12345))

# sname -> member source path, for the direct-read legs (sname = basename)
with open(os.path.join(orig, "members.list"), "w") as f:
    f.write("alpha.c\talpha.c\n")
    f.write("beta.bin\tbeta.bin\n")
    f.write("empty.dat\tempty.dat\n")
    f.write("leaf.txt\tdeep/nested/leaf.txt\n")
    f.write("rand.bin\trand.bin\n")

# --- hsolid.7z: hand-crafted SOLID Copy (1 folder, 2 substreams), plain
#     header. 7zz never groups Copy members solidly, so the multi-substream
#     parse path needs this; `7zz t` cross-validates below. ---------------
def vu(v):
    # 7z UINT64: k leading 1-bits in the first byte; the remaining (7-k)
    # bits are the HIGH part; the next k bytes are the LOW part (LE).
    for k in range(0, 8):
        if v < (1 << (7 + 7 * k)):
            if k == 0:
                return bytes([v])
            lo = v & ((1 << (8 * k)) - 1)
            hi = (v >> (8 * k)) & ((1 << (7 - k)) - 1)
            return bytes([((0xFF << (8 - k)) & 0xFF) | hi]) + \
                lo.to_bytes(k, "little")
    return b"\xff" + v.to_bytes(8, "little")

payload_a = b"alpha solid member\n" * 1000          # text-ish, 19000 B
payload_b = bytes(range(256)) * 40                  # 10240 B, binary-ish
data = payload_a + payload_b
names = "".join(n + "\0" for n in ("sa.txt", "sb.bin")).encode("utf-16-le")

def digests(vals):
    return b"\x01" + b"".join(struct.pack("<I", v) for v in vals)

folder = vu(1) + bytes([0x01]) + bytes([0x00])      # 1 coder, id {00} Copy
unpack = (vu(0x0B) + vu(1) + b"\x00" + folder +
          vu(0x0C) + vu(len(data)) +
          vu(0x0A) + digests([zlib.crc32(data)]) + vu(0x00))
sub = (vu(0x0D) + vu(2) +
       vu(0x09) + vu(len(payload_a)) +
       vu(0x0A) + digests([zlib.crc32(payload_a), zlib.crc32(payload_b)]) +
       vu(0x00))
pack = (vu(0x06) + vu(0) + vu(1) + vu(0x09) + vu(len(data)) + vu(0x00))
streams = pack + vu(0x07) + unpack + vu(0x08) + sub + vu(0x00)
files = (vu(0x05) + vu(2) +
         vu(0x11) + vu(len(names) + 1) + b"\x00" + names +
         vu(0x00))
header = vu(0x01) + vu(0x04) + streams + files + vu(0x00)
start = struct.pack("<QQL", len(data), len(header), zlib.crc32(header))
sig = b"7z\xBC\xAF\x27\x1C" + bytes([0, 4]) + \
    struct.pack("<I", zlib.crc32(start)) + start
assert len(sig) == 32
open(os.path.join(orig, "hsolid.7z"), "wb").write(sig + data + header)
open(os.path.join(orig, "hsolid.m0"), "wb").write(payload_a)
open(os.path.join(orig, "hsolid.m1"), "wb").write(payload_b)

# --- decline fixtures -------------------------------------------------------
open(os.path.join(orig, "garbage.7z"), "wb").write(
    b"7z\xBC\xAF\x27\x1C" + rnd.randbytes(100000))  # magic + junk
open(os.path.join(orig, "nomagic.7z"), "wb").write(rnd.randbytes(100000))
print("  member content + hsolid.7z + garbage fixtures generated")
