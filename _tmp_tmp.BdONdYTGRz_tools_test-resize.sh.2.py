import os, shutil, subprocess
d = "/dev/shm/wp18resize/origb"
os.makedirs(d, exist_ok=True)
# text batch fodder: real C sources, duplicated with edits
src = "/home/user/InvariantFS/tools/busybox-src"
files = []
for root, dirs, names in os.walk(src):
    dirs.sort()
    for n in sorted(names):
        if n.endswith((".c", ".h")):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                files.append(p)
        if len(files) >= 6:
            break
    if len(files) >= 6:
        break
for i, p in enumerate(files):
    shutil.copy(p, os.path.join(d, "src%d.c" % i))
# a tar of 26 real x86-64 ELF binaries (the conbatch shape): TARR + ZSTD
# member batches on sweep
cands = []
for n in sorted(os.listdir("/usr/bin")):
    p = os.path.join("/usr/bin", n)
    if not os.path.isfile(p) or os.path.islink(p):
        continue
    if os.path.getsize(p) < 4096:
        continue
    try:
        with open(p, "rb") as f:
            h = f.read(20)
    except OSError:
        continue
    if h[:4] != b"\x7fELF" or h[18] != 62:
        continue
    cands.append((os.path.getsize(p), p))
cands.sort()
picked = [p for sz, p in cands[::37]][:24]
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
print("corpus B:", *sorted(os.listdir(d)))
