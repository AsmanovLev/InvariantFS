#!/bin/bash
# test-textzone.sh — WP10 Text Zone end-to-end (persistent regression).
#
#   mkfs -> mixed tree (texts of several families/sizes, one >4MB text,
#   binaries, one PNG) -> invf-sweep (texts defer, rc=9 lines) ->
#   invf-verify --deep -> invf-cat bit-exact vs originals (sha256) ->
#   delete text members -> invf-sweep (GC reclaims dead batches) ->
#   invf-fsck clean -> stat.
#
# Run from the repo root after `make`:  bash tools/test-textzone.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp10tz
IMG=wp10tz.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

echo "== mkfs =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null

echo "== generate tree =="
python3 - <<'PY'
import os, random, zlib, struct
random.seed(42)
d = "/dev/shm/wp10tz/orig"
# text files of several families and sizes (1KB..500KB)
specs = [("a.c", 120_000), ("b.c", 3_000), ("h.py", 45_000),
         ("w.py", 250_000), ("r.log", 500_000), ("n.txt", 1_500),
         ("m.md", 90_000), ("s.sh", 12_000), ("d.json", 60_000)]
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
for name, size in specs:
    out = []
    n = 0
    while n < size:
        w = random.choice(WORDS)
        out.append(w)
        n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
# one >4MB text file
with open(os.path.join(d, "big.txt"), "w") as f:
    n = 0
    while n < 5_300_000:
        line = " ".join(random.choices(WORDS, k=16)) + "\n"
        f.write(line)
        n += len(line)
# binaries: /bin/true + a random blob
import shutil
shutil.copy("/bin/true", os.path.join(d, "true.bin"))
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(300_000))
# one PNG (real, tiny, valid)
def chunk(t, data):
    return (struct.pack(">I", len(data)) + t + data +
            struct.pack(">I", zlib.crc32(t + data) & 0xffffffff))
ihdr = struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0)
rows = b"".join(b"\x00" + bytes(random.randrange(256) for _ in range(12))
                for _ in range(4))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
       chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
open(os.path.join(d, "t.png"), "wb").write(png)
print("tree:", *sorted(os.listdir(d)))
PY

FILES=$(cd "$WORK/orig" && ls)
echo "== import =="
for f in $FILES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
TZ_LINES=$(grep -c "text -> PPMd batch" "$WORK/sweep1.log" || true)
echo "text->PPMd lines: $TZ_LINES"
[ "$TZ_LINES" -ge 9 ] || { echo "FAIL: expected >=9 deferred texts"; exit 1; }
grep "text gc\|text batches flushed\|sweep done" "$WORK/sweep1.log" || true
# the batch owner must be hidden from listings
if $B/invf-ls "$IMG" | grep -q "tzb"; then
    echo "FAIL: \\x01tzb owner leaked into directory listing"; exit 1;
fi

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

# text-zone accounting shows up in stats while members are live
if [ -x $B/invf-stats ]; then
    $B/invf-stats "$IMG" | grep -q "TEXT  :" || { echo "FAIL: no TEXT stats line"; exit 1; }
    $B/invf-stats "$IMG" | grep "TEXT  :"
fi

echo "== cat bit-exact =="
ok=1
for f in $FILES; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1
echo "all $(echo "$FILES" | wc -w) files bit-exact"

echo "== delete text members =="
# tiny delete helper (no CLI rm exists); built from the repo objects
cat > "$WORK/tzrm.c" <<'C'
#include <stdio.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc = 0;
    invfs_volume *v;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    for (int i = 2; i < argc; i++)
        if (vol_delete_file(v, argv[i]) != 0) { rc = 1; fprintf(stderr, "rm %s failed\n", argv[i]); }
    vol_close(v);
    return rc;
}
C
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/tzrm" "$WORK/tzrm.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,vol_tier,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

# phase A: two members only -- the shared batch may stay alive
"$WORK/tzrm" "$IMG" n.txt s.sh
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1
grep "text gc\|sweep done" "$WORK/sweep2.log" || true
# surviving texts must still read bit-exactly (partial batch liveness)
ok=1
for f in a.c b.c h.py w.py r.log m.md d.json big.txt; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.2" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.2" || { echo "MISMATCH after delete: $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "survivors bit-exact after partial delete"

# phase B: delete ALL remaining texts -> every batch goes dead -> GC reclaims
"$WORK/tzrm" "$IMG" a.c b.c h.py w.py r.log m.md d.json big.txt
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1
GC_LINE=$(grep "text gc:" "$WORK/sweep3.log" || true)
echo "${GC_LINE:-no gc line}"
echo "$GC_LINE" | grep -q "text gc: [1-9]" || { echo "FAIL: GC reclaimed nothing"; exit 1; }
# nothing left to defer: no rc=9 lines on a re-sweep of the survivors
if grep -q "text -> PPMd batch" "$WORK/sweep3.log"; then
    echo "FAIL: re-sweep re-deferred texts"; exit 1;
fi

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
$B/invf-verify "$IMG" --deep | tail -1

echo "== stat =="
$B/invf-stat "$IMG" | sed 's/\x1b\[[0-9;]*m//g' | grep -E "space|files|zones" || true
if [ -x $B/invf-stats ]; then
    $B/invf-stats "$IMG" | grep -E "TEXT|SHADOW|RAW" || true
fi
# remaining files still bit-exact
for f in true.bin rand.bin t.png; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.3" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.3" || { echo "MISMATCH final: $f"; exit 1; }
done

echo "TEXTZONE E2E: PASS"
