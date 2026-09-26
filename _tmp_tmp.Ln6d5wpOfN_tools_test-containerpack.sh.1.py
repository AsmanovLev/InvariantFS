import os
import random
import struct
import sys

def splt(members):
    b = b"SPLT" + struct.pack("<I", len(members))
    for m in members:
        b += struct.pack("<Q", len(m))
    return b + b"".join(members)

d = sys.argv[1]
rnd = random.Random(16)

# text member: a real C source (content-sniffs as text; the member name
# "chunk0" carries no extension on purpose)
text = None
src = "/home/user/InvariantFS/tools/busybox-src"
for root, dirs, files in os.walk(src):
    dirs.sort()
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                text = open(p, "rb").read()
                break
    if text:
        break
assert text, "no busybox .c fixture found"

# ELF member: a real x86-64 binary, >= 4 KB (binary-family magic gate)
elf = None
for p in ("/usr/bin/passwd", "/usr/bin/gpg", "/bin/ls", "/usr/bin/ls",
          "/bin/bash", "/usr/bin/bash"):
    if os.path.isfile(p) and not os.path.islink(p):
        with open(p, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF" and h[18] == 62 and \
           os.path.getsize(p) > 65536:
            elf = open(p, "rb").read()
            break
assert elf, "no x86-64 ELF fixture found"

multi = splt([text, b"", elf, rnd.randbytes(32768)])
open(os.path.join(d, "multi.splt"), "wb").write(multi)
open(os.path.join(d, "member0.bin"), "wb").write(text)   # for direct reads
open(os.path.join(d, "member2.bin"), "wb").write(elf)

# the member layout, for the ranged-read legs (offsets in the ORIGINAL)
data_off = 8 + 4 * 8
m0_len = len(text)
m2_off = data_off + m0_len          # member 1 is empty: m0 end == m2 start
with open(os.path.join(d, "multi.layout"), "w") as f:
    f.write("size=%d data_off=%d m0_len=%d m2_off=%d m2_len=%d\n"
            % (len(multi), data_off, m0_len, m2_off, len(elf)))

inner = splt([b"inner member alpha\n" * 500, b"inner member beta\n" * 700])
nest = splt([inner, b"outer tail\n" * 300])
open(os.path.join(d, "nest.splt"), "wb").write(nest)

# negative: the table announces 5 payload bytes but 10 are present --
# enumerate/extract/strip succeed; for the seekable pack the FS-side MAP
# VALIDATION refuses (recipe+members cannot partition the container), for
# a map-less pack the rebuild guard refuses (the nomap leg below).
open(os.path.join(d, "neg.splt"), "wb").write(
    b"SPLT" + struct.pack("<I", 1) + struct.pack("<Q", 5) + b"0123456789")

print("  multi.splt", len(multi), "(4 members: text %d, empty, elf %d, rand 32768)"
      % (len(text), len(elf)))
print("  nest.splt ", len(nest), "(member 0 = inner SPLT of %d bytes)" % len(inner))
print("  neg.splt  ", 26, "(corrupt table)")
