#!/bin/bash
# bench-fs.sh — WP11 corpus benchmark: InvariantFS (import + sweep) against
# the usual offline references over the same bytes: plain tar, tar|zstd -19,
# tar|xz -6.
#
# Deterministic corpus in a temp dir:
#   * 30 JPEG photos (PIL, gradient+noise, ~0.1..3MB, q82..89)
#   * a text tree: every .c/.h from tools/busybox-src (~9.5MB)
#   * 5 binaries from /usr/bin (1..5MB each)
#   * a few small incompressible random files
# then: mkfs (sized to the corpus) -> invf-import -> invf-sweep -> report
# image used bytes (invf-verify block count) + overall ratio.
#
# Run from the repo root after `make`:  bash tools/bench-fs.sh
# Uses /dev/shm (tmpfs); image paths stay RELATIVE (blkio treats /dev/* as
# a raw device).
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp11bench
IMG=${IMG:-wp11bench.img}
CORPUS_DIR=${CORPUS_DIR:-}   # set to bench an existing tree instead of the generated one
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

if [ -n "$CORPUS_DIR" ]; then
    CORPUS="$CORPUS_DIR"
    echo "== corpus (external) =="
    du -sh "$CORPUS"
else
    CORPUS="$WORK/corpus"
    mkdir -p "$CORPUS"
    echo "== corpus =="
python3 - <<'PY'
import os, hashlib
import numpy as np
from PIL import Image

d = "/dev/shm/wp11bench/corpus"
os.makedirs(os.path.join(d, "photos"))
rng = np.random.default_rng(20260826)

# 30 photos, growing size and noise -> ~0.1..3MB at q82..89
sizes = []
for i in range(30):
    w = 640 + i * 70
    h = w * 3 // 4
    sigma = 8 + (i % 6) * 3
    q = 82 + i % 8
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    p = os.path.join(d, "photos", "photo%02d.jpg" % i)
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(p, "JPEG", quality=q)
    sizes.append(os.path.getsize(p))
print("photos: 30 jpg, min %.0f KB, max %.0f KB, total %.1f MB"
      % (min(sizes)/1024, max(sizes)/1024, sum(sizes)/1048576))

# deterministic incompressible small files (sha256 counter stream)
os.makedirs(os.path.join(d, "rand"))
for i in range(4):
    out = b""
    ctr = 0
    while len(out) < 16 * 1024 * (i + 1):
        out += hashlib.sha256(b"wp11-%d-%d" % (i, ctr)).digest()
        ctr += 1
    open(os.path.join(d, "rand", "r%d.bin" % i), "wb").write(out)
print("rand: 4 incompressible files")
PY

# text tree: the .c/.h of busybox, layout preserved
mkdir -p "$CORPUS/src"
( cd "$REPO/tools/busybox-src" && \
  find . \( -name '*.c' -o -name '*.h' \) -print0 | tar --null -cf - --files-from=- ) \
  | ( cd "$CORPUS/src" && tar -xf - )
echo "text: $(find "$CORPUS/src" -type f | wc -l) files, $(du -sm "$CORPUS/src" | cut -f1) MB"

# 5 binaries, 1..5MB, deterministic pick (smallest five in that band)
mkdir -p "$CORPUS/bin"
find /usr/bin -maxdepth 1 -type f -size +1M -size -5M -printf "%s %p\n" \
    | sort -n | head -5 | while read -r sz p; do
    cp "$p" "$CORPUS/bin/$(basename "$p")"
    echo "bin: $(basename "$p") $sz"
done
fi

RAW_BYTES=$(du -sb "$CORPUS" | cut -f1)
echo "corpus total: $RAW_BYTES bytes"

echo "== references =="
tar -cf "$WORK/corpus.tar" -C "$(dirname "$CORPUS")" "$(basename "$CORPUS")"
TAR_BYTES=$(stat -c%s "$WORK/corpus.tar")
zstd -q -19 -f "$WORK/corpus.tar" -o "$WORK/corpus.tar.zst"
ZSTD_BYTES=$(stat -c%s "$WORK/corpus.tar.zst")
xz -6 -k -f "$WORK/corpus.tar"
XZ_BYTES=$(stat -c%s "$WORK/corpus.tar.xz")
echo "tar: $TAR_BYTES  zstd-19: $ZSTD_BYTES  xz-6: $XZ_BYTES"

echo "== invfs =="
# RAW zone = 1/5 of (image - metadata): size so the unswept corpus fits,
# plus journal + slack for the swept shadow copy. ~2500 names x (import +
# meta + sweep + tombstone) records want a bigger inode area than the /64
# default -- mkfs's own comment points rootfs-like trees at META_FRAC 16-24.
GB=$(python3 -c "
import math
c = $RAW_BYTES
gb = (5.5 * c + 80 * 1024 * 1024) / 1024**3
print('%.2f' % (math.ceil(gb * 20) / 20))")
echo "image size: ${GB} GB"
INVFS_META_FRAC=24 $B/invf-mkfs "$IMG" "$GB" >/dev/null
$B/invf-import "$IMG" "$CORPUS" | tail -1
$B/invf-sweep "$IMG" > "$WORK/sweep.log" 2>&1 || { tail -20 "$WORK/sweep.log"; exit 1; }
grep -E "sweep done|text batches flushed" "$WORK/sweep.log"
echo "JPEG->JXL: $(grep -c 'JPEG -> JXL' "$WORK/sweep.log" || true) files"
# data-zone used bytes (invf-stat "space": raw+shadow, metadata excluded);
# whole-image allocated blocks (invf-verify) for the footprint incl. the
# fixed 32MB journal + inode area
INVFS_SPACE=$($B/invf-stat "$IMG" | sed 's/\x1b\[[0-9;]*m//g' \
    | sed -n 's/^  space : \([0-9.]*\) \([KMG]i\)B used .*/\1 \2/p')
INVFS_USED=$(python3 -c "
v, u = '''$INVFS_SPACE'''.split()
m = {'Ki': 1024, 'Mi': 1024**2, 'Gi': 1024**3}[u]
print(int(float(v) * m))")
ALLOC_BLK=$($B/invf-verify "$IMG" | sed -n 's/.*blocks: [0-9]* total, [0-9]* free, \([0-9]*\) allocated.*/\1/p')
INVFS_IMG=$((ALLOC_BLK * 4096))

echo
echo "============ bench: $(du -sh "$CORPUS" | cut -f1) corpus ============"
printf "%-26s %14s %10s\n" "target" "bytes" "vs raw"
printf "%-26s %14s %10s\n" "--------------------------" "--------------" "----------"
python3 - <<PY
raw = $RAW_BYTES
rows = [
    ("raw corpus (du)",        $RAW_BYTES),
    ("tar",                    $TAR_BYTES),
    ("tar | zstd -19",         $ZSTD_BYTES),
    ("tar | xz -6",            $XZ_BYTES),
    ("invfs data (raw+shadow)", $INVFS_USED),
    ("invfs image (allocated)", $INVFS_IMG),
]
for name, b in rows:
    print("%-26s %14d %9.2fx" % (name, b, raw / b))
print()
print("invfs image = data + fixed metadata (32MB journal + inode area);")
print("it amortizes on corpora larger than this %d MB one." % (raw // 1048576))
PY
echo "=========================================================================="
echo
echo "== invf-stats =="
$B/invf-stats "$IMG" | grep -E "RAW|SHADOW|TEXT|logical|est\. ratio"
