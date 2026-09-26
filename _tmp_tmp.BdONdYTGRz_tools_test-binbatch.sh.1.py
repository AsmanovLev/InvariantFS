import os, shutil, random
random.seed(42)
d = "/dev/shm/wp14bz/orig"

# --- ELF executables from /usr/bin, size-varied, deterministic ---
cands = []
for n in sorted(os.listdir("/usr/bin")):
    p = os.path.join("/usr/bin", n)
    if not os.path.isfile(p) or os.path.islink(p):
        continue
    try:
        sz = os.path.getsize(p)
        if sz < 4096:
            continue
        with open(p, "rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
    except OSError:
        continue                    # unreadable (perm-shadowed) binaries
    cands.append((sz, p))
cands.sort()
picked = []
total = 0
for i in range(0, len(cands), 7):
    sz, p = cands[i]
    if sz > 4 * 1024 * 1024:
        continue                    # the >4MB ones are added below
    picked.append(p)
    total += sz
    if total > 28 * 1024 * 1024:
        break
big = [p for sz, p in cands if sz > 4 * 1024 * 1024][:2]   # cross-batch slices
picked += big
for i, p in enumerate(picked):
    shutil.copy(p, os.path.join(d, "elf%02d" % i))
n_elf = len(picked)

# --- shared objects (ELF x86-64 as well: same BCJ family) ---
n_so = 0
for libdir in ("/usr/lib64", "/lib64"):
    if not os.path.isdir(libdir):
        continue
    for n in sorted(os.listdir(libdir)):
        p = os.path.join(libdir, n)
        if not (os.path.isfile(p) and not os.path.islink(p)
                and n.endswith(".so")):
            continue
        with open(p, "rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
        sz = os.path.getsize(p)
        if 4096 <= sz <= 8 * 1024 * 1024:
            shutil.copy(p, os.path.join(d, "lib%d.so" % n_so))
            n_so += 1
            if n_so >= 3:
                break
    if n_so:
        break

# --- synthetic family variants over a real ELF body ---
base = open("/usr/bin/true", "rb").read()
a64 = bytearray(base); a64[18] = 183; a64[19] = 0     # EM_AARCH64: no BCJ
open(os.path.join(d, "a64.bin"), "wb").write(bytes(a64) * 40)      # ~1.3MB
i386 = bytearray(base); i386[18] = 3; i386[19] = 0    # EM_386: BCJ
open(os.path.join(d, "i386.bin"), "wb").write(bytes(i386) * 100)   # ~3.2MB
# Mach-O fat magic: binary batch WITHOUT the x86 prefilter
open(os.path.join(d, "macho.bin"), "wb").write(
    b"\xca\xfe\xba\xbe" + b"\xde\xad" * 4096 + os.urandom(2048))
# <4KB ELF: below the classifier's size gate -> generic path
tiny = bytearray(base[:2048]); tiny[18] = 62
open(os.path.join(d, "tiny.elf"), "wb").write(bytes(tiny))

# --- texts (must still go PPMd) ---
WORDS = ("the quick brown fox jumps over lazy dogs int static return "
         "while for struct char void NULL size_t\n").split()
for name, size in [("a.c", 90000), ("b.py", 45000), ("notes.txt", 120000)]:
    out = []
    n = 0
    while n < size:
        w = random.choice(WORDS)
        out.append(w)
        n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

# --- incompressible blob: no magic -> generic path ---
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(300000))

mb = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d)) / 1048576
print("elf:", n_elf, "so:", n_so, "big:", [os.path.basename(p) for p in big])
print("corpus: %.1f MB" % mb)
