import os, shutil, subprocess

d = "/dev/shm/wp14cb/orig"

# --- bins.tar: real x86-64 ELFs, size-varied incl. >4MB members ---
cands = []
for n in sorted(os.listdir("/usr/bin")):
    p = os.path.join("/usr/bin", n)
    if not os.path.isfile(p) or os.path.islink(p):
        continue
    sz = os.path.getsize(p)
    if sz < 4096:
        continue
    try:
        with open(p, "rb") as f:
            h = f.read(20)
    except OSError:
        continue                        # unreadable (perm-shadowed) binaries
    if h[:4] != b"\x7fELF":
        continue
    if h[18] != 62:                     # EM_X86_64 only: all parts take BCJ
        continue
    cands.append((sz, p))
cands.sort()
picked = [p for sz, p in cands[::37]][:24]          # spread of sizes
big = [p for sz, p in cands if sz > 4 * 1024 * 1024][:2]
for p in big:
    if p not in picked:
        picked.append(p)
os.makedirs(os.path.join(d, "bins"))
for i, p in enumerate(picked):
    shutil.copy(p, os.path.join(d, "bins", "elf%02d" % i))
subprocess.run(["tar", "-cf", os.path.join(d, "bins.tar"), "-C",
                os.path.join(d, "bins")] +
               ["elf%02d" % i for i in range(len(picked))], check=True)
maxsz = max(os.path.getsize(os.path.join(d, "bins", n))
            for n in os.listdir(os.path.join(d, "bins")))
assert maxsz > 4 * 1024 * 1024, "no >4MB member picked"
print("bins.tar: %d members, %.1f MB, biggest member %.1f MB"
      % (len(picked), os.path.getsize(os.path.join(d, "bins.tar")) / 1048576,
         maxsz / 1048576))

# --- texts.tar: C sources (text parts -> PPMd batches) ---
os.makedirs(os.path.join(d, "texts"))
src = "/home/user/InvariantFS/tools/busybox-src"
names = []
for root, dirs, files in os.walk(src):
dirs.sort()
    for n in sorted(files):
        p = os.path.join(root, n)
        if n.endswith(".c") and len(names) < 12:
            shutil.copy(p, os.path.join(d, "texts", n))
            names.append(n)
    if len(names) >= 12:
        break
subprocess.run(["tar", "-cf", os.path.join(d, "texts.tar"), "-C",
                os.path.join(d, "texts")] + names, check=True)
print("texts.tar: %d members" % len(names))

# --- misc.tar: incompressible + tiny members (must stay unbatched) ---
os.makedirs(os.path.join(d, "misc"))
open(os.path.join(d, "misc", "rand.bin"), "wb").write(os.urandom(200000))
open(os.path.join(d, "misc", "tiny.txt"), "w").write("hi\n")
subprocess.run(["tar", "-cf", os.path.join(d, "misc.tar"), "-C",
                os.path.join(d, "misc"), "rand.bin", "tiny.txt"], check=True)
print("misc.tar: 2 members")
