import os, sys
d = sys.argv[1]
# text member: a real busybox .c, >20KB (content-sniffs as text; the name
# "README" carries no extension on purpose, like the splt fixture)
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
open(os.path.join(d, "README"), "wb").write(text)
# ELF member: a real x86-64 binary, >= 64KB (binary-family magic gate)
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
open(os.path.join(d, "elf.bin"), "wb").write(elf)
print("  README %d bytes, elf.bin %d bytes" % (len(text), len(elf)))
