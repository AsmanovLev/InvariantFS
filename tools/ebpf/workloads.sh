#!/bin/bash
# workloads.sh — WP622 deterministic VFS workloads, one function per leg.
# Sourced by run-leg.sh; every workload takes the target mount dir as $1
# and prints its own wall-clock + byte counts to stdout (the runner logs it).
#
# Cache-state control (documented per leg in docs/benchmarks/VFS-PROBE.md):
#   wl_drop <path>  — posix_fadvise(DONTNEED) on every file under path
#                     (pagecache drop scoped to the corpus, no global
#                     drop_caches needed); remount discipline only where
#                     the leg says so (leg4 keeps the cache warm).

wl_seed() {  # <scratch> <mnt> — build once, copy identical bytes to each mount
    local scratch=$1 mnt=$2
    local src=$scratch/corpus
    if [ ! -d "$src" ]; then
        mkdir -p "$src/photos" "$src/text"
python3 - "$src" <<'PY'
import os, sys
import numpy as np
from PIL import Image
d = sys.argv[1]
rng = np.random.default_rng(622)
for i in range(20):
    w, h = 320 + i * 16, 240 + i * 12
    n = rng.normal(0, 12, (h, w))
    x = np.linspace(0, 255, w, dtype=np.float32)
    a = np.clip(x + n, 0, 255).astype(np.uint8)
    Image.fromarray(np.dstack([a, a[::-1, :], np.fliplr(a)])).save(
        os.path.join(d, "photos", "photo%02d.jpg" % i), "JPEG", quality=85)
for i in range(40):
    with open(os.path.join(d, "text", "doc%02d.txt" % i), 'w') as f:
        for j in range(200):
            f.write("line %d of doc %d: the quick brown fox %d\n" % (j, i, j * i))
print("corpus built")
PY
    fi
    # NOTE: plain cp -r, not cp -a: recursive -a fails (silent rc=1)
    # setting directory timestamps/ownership on the invfs FUSE mount
    # (engine quirk, out of scope for this observability WP). File
    # bytes are identical either way, which is all the legs need.
    cp -r "$src/." "$mnt/"
    echo "seeded $mnt ($(du -sh "$src" | cut -f1))"
}

wl_drop() {  # <dir> — per-file DONTNEED, no global side effects
python3 - "$1" <<'PY'
import os, sys
d = sys.argv[1]
for root, _, files in os.walk(d):
    for n in files:
        p = os.path.join(root, n)
        try:
            fd = os.open(p, os.O_RDONLY)
            try:
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            finally:
                os.close(fd)
        except OSError:
            pass
PY
}

wl_leg1_rw() {  # <mnt> — sequential + random read/write mix, cold then warm
    local mnt=$1 t0 t1
    echo "--- leg1 workload on $mnt ---"
    wl_drop "$mnt"
    t0=$(date +%s.%N)
    # cold sequential read of every corpus file
    python3 - "$mnt" <<'PY'
import os, sys, hashlib
mnt = sys.argv[1]
h = hashlib.sha256()
n = 0
for root, dirs, files in os.walk(mnt):
    dirs.sort()  # os.walk dir order is fs-dependent; sort for a stable hash
    for x in sorted(files):
        with open(os.path.join(root, x), 'rb') as f:
            while True:
                b = f.read(131072)
                if not b:
                    break
                h.update(b)
                n += len(b)
print("cold-read bytes=%d sha=%s" % (n, h.hexdigest()[:16]))
PY
    # warm re-read (pagecache hit path) + 4K random reads
    python3 - "$mnt" <<'PY'
import os, sys, random
mnt = sys.argv[1]
paths = []
for root, _, files in os.walk(mnt):
    for x in sorted(files):
        paths.append(os.path.join(root, x))
random.Random(622).shuffle(paths)
tot = 0
for p in paths[:400]:
    with open(p, 'rb') as f:
        sz = os.fstat(f.fileno()).st_size
        if sz > 4096:
            f.seek(random.Random(hash(p) & 0xffff).randrange(0, sz - 4096))
        tot += len(f.read(4096))
print("random4k files=%d bytes=%d" % (min(400, len(paths)), tot))
PY
    # sustained write: 16 MiB in 128K chunks + fsync per file
    python3 - "$mnt/leg1-write.bin" <<'PY'
import sys
import os
with open(sys.argv[1], 'wb') as f:
    chunk = bytes((i & 255 for i in range(131072)))
    for _ in range(128):
        f.write(chunk)
    f.flush()
    os.fsync(f.fileno())
print("write bytes=16777216")
PY
    t1=$(date +%s.%N)
    echo "leg1-wall seconds=$(echo "$t1 - $t0" | bc)"
}

wl_leg2_meta() {  # <mnt> — metadata-heavy tree: create/stat/unlink churn
    local mnt=$1 t0 t1
    echo "--- leg2 workload on $mnt ---"
    t0=$(date +%s.%N)
    python3 - "$mnt" <<'PY'
import os, sys, time
mnt = sys.argv[1]
tree = os.path.join(mnt, "meta-tree")
os.makedirs(tree, exist_ok=True)
N = 2000
t = time.time()
for i in range(N):
    d = os.path.join(tree, "d%03d" % (i % 50))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "f%05d" % i), 'w') as f:
        f.write("x" * 64)
t_create = time.time() - t
t = time.time()
for i in range(N):
    os.stat(os.path.join(tree, "d%03d" % (i % 50), "f%05d" % i))
t_stat = time.time() - t
t = time.time()
for i in range(N):
    p = os.path.join(tree, "d%03d" % (i % 50), "f%05d" % i)
    os.chmod(p, 0o644)
t_chmod = time.time() - t
t = time.time()
for i in range(N):
    os.unlink(os.path.join(tree, "d%03d" % (i % 50), "f%05d" % i))
t_unlink = time.time() - t
print("create2000=%.3fs stat2000=%.3fs chmod2000=%.3fs unlink2000=%.3fs"
      % (t_create, t_stat, t_chmod, t_unlink))
PY
    t1=$(date +%s.%N)
    echo "leg2-wall seconds=$(echo "$t1 - $t0" | bc)"
}

wl_leg3_sync() {  # <mnt> — write + fsync + fdatasync + global sync
    local mnt=$1 t0 t1
    echo "--- leg3 workload on $mnt ---"
    t0=$(date +%s.%N)
    python3 - "$mnt" <<'PY'
import os, sys, time
mnt = sys.argv[1]
CLOCK = time.CLOCK_MONOTONIC
print("mark start %d" % time.clock_gettime_ns(CLOCK))
with open(os.path.join(mnt, "leg3-dd.bin"), 'wb') as f:
    for _ in range(32):
        f.write(os.urandom(131072))
    f.flush()
    t = time.clock_gettime_ns(CLOCK)
    os.fsync(f.fileno())
    print("mark dd-fsync-ret %d (issued %d)" % (time.clock_gettime_ns(CLOCK), t))
print("dd-fsync bytes=4194304")
with open(os.path.join(mnt, "leg3-fdata.bin"), 'wb') as f:
    f.write(os.urandom(2097152))
    f.flush()
    t = time.clock_gettime_ns(CLOCK)
    os.fdatasync(f.fileno())
    print("mark fdata-fsync-ret %d (issued %d)" % (time.clock_gettime_ns(CLOCK), t))
print("fdatasync bytes=2097152")
PY
    python3 -c "import time; print('mark sync-issued %d' % time.clock_gettime_ns(time.CLOCK_MONOTONIC))"
    sync
    python3 -c "import time; print('mark sync-ret %d' % time.clock_gettime_ns(time.CLOCK_MONOTONIC))"
    echo "sync returned"
    t1=$(date +%s.%N)
    echo "leg3-wall seconds=$(echo "$t1 - $t0" | bc)"
}

wl_leg4_pressure() {  # <mnt> — warm dentry tree, then churn to provoke reclaim
    local mnt=$1 t0 t1
    echo "--- leg4 workload on $mnt (cache stays warm; no drop) ---"
    t0=$(date +%s.%N)
    python3 - "$mnt" <<'PY'
import os, sys
mnt = sys.argv[1]
tree = os.path.join(mnt, "press-tree")
os.makedirs(tree, exist_ok=True)
N = 3000
for i in range(N):
    d = os.path.join(tree, "p%03d" % (i % 100))
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "g%05d" % i)
    if not os.path.exists(p):
        with open(p, 'w') as f:
            f.write("y" * 32)
# two warm walks: everything resident in dcache
tot = 0
for _ in range(2):
    for root, _, files in os.walk(tree):
        for x in files:
            os.stat(os.path.join(root, x))
            tot += 1
print("warm-stats=%d" % tot)
# churn: rename half the files (dentry alloc/free pressure)
for i in range(0, N, 2):
    a = os.path.join(tree, "p%03d" % (i % 100), "g%05d" % i)
    b = os.path.join(tree, "p%03d" % (i % 100), "h%05d" % i)
    if os.path.exists(a):
        os.rename(a, b)
print("renames=%d" % (N // 2))
PY
    t1=$(date +%s.%N)
    echo "leg4-wall seconds=$(echo "$t1 - $t0" | bc)"
}
