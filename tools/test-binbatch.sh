#!/bin/bash
# test-binbatch.sh — WP14a binary batching end-to-end (persistent regression).
#
#   mkfs -> mixed corpus (~40MB: ELF x86-64 exes from /usr/bin in varied
#   sizes incl. >4MB, /usr/lib64/*.so, synthetic i386/aarch64-ELF and Mach-O
#   binaries, a <4KB ELF [gate], an incompressible blob [generic], and text
#   files [must still go PPMd]) -> invf-sweep (expect "binary -> ZSTD batch"
#   AND "text -> PPMd batch" lines) -> invf-verify --deep -> invf-cat
#   bit-exact vs originals (sha256) -> class stamps (cls=8 binaries, cls=7
#   texts) -> re-sweep idempotent -> delete binary members -> re-sweep ->
#   GC reclaims dead batches -> invf-fsck clean -> survivors bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-binbatch.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
WORK=/dev/shm/wp14bz
IMG=wp14bz.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

echo "== mkfs =="
$B/invf-mkfs "$IMG" 0.6 >/dev/null

echo "== generate corpus =="
python3 - <<'PY'
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
PY

# expected deferral counts: every corpus member in one class
ELF_N=$(ls "$WORK/orig" | grep -cE '^elf[0-9]+$|^lib[0-9]+\.so$' || true)
BIN_N=$((ELF_N + 3))          # + a64.bin, i386.bin, macho.bin
TEXT_N=3                      # a.c, b.py, notes.txt
echo "expect: $BIN_N binary deferrals ($ELF_N host ELF + 3 synthetic), $TEXT_N text"

echo "== import =="
FILES=$(cd "$WORK/orig" && ls)
for f in $FILES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
BZ_LINES=$(grep -c "binary -> ZSTD batch" "$WORK/sweep1.log" || true)
TZ_LINES=$(grep -c "text -> PPMd batch" "$WORK/sweep1.log" || true)
echo "binary->ZSTD lines: $BZ_LINES   text->PPMd lines: $TZ_LINES"
[ "$BZ_LINES" -eq "$BIN_N" ] || { echo "FAIL: expected $BIN_N binary deferrals"; exit 1; }
[ "$TZ_LINES" -eq "$TEXT_N" ] || { echo "FAIL: expected $TEXT_N text deferrals"; exit 1; }
grep "batches flushed\|sweep done" "$WORK/sweep1.log" || true
# the shared batch owner must be hidden from listings
if $B/invf-ls "$IMG" | grep -q "tzb"; then
    echo "FAIL: \\x01tzb owner leaked into directory listing"; exit 1;
fi

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q "0 corrupt" "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

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

echo "== class stamps =="
cat > "$WORK/classof.c" <<'C'
/* usage: classof <image> <name> -> "cls=<n> algo=<n> gen=<n>" or "none" */
#include <stdio.h>
#include "volume.h"
#include "invarifs.h"
int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id;
    uint8_t cls, algo;
    uint16_t gen;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0)
        printf("none\n");
    else
        printf("cls=%u algo=%u gen=%u\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs,vol_tier}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

for f in $FILES; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    case "$f" in
    a.c|b.py|notes.txt)
        [ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: $f: want TEXT{PPMD}"; exit 1; } ;;
    a64.bin|macho.bin)
        # non-x86 binary family: batched, plain ZSTD payload (algo 1)
        [ "$C" = "cls=8 algo=1 gen=1" ] || { echo "FAIL: $f: want BATCHED_BIN{ZSTD}"; exit 1; } ;;
    tiny.elf|rand.bin)
        # below the 4KB gate / incompressible: never batched
        case "$C" in cls=8*) echo "FAIL: $f must not be batched"; exit 1;; esac ;;
    elf*|lib*.so|i386.bin)
        # x86/x86-64: batched with the BCJ prefilter (algo 14)
        [ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: $f: want BATCHED_BIN{ZSTD_BCJ}"; exit 1; } ;;
    esac
done

echo "== re-sweep is idempotent =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1
if grep -q "binary -> ZSTD batch\|text -> PPMd batch" "$WORK/sweep2.log"; then
    echo "FAIL: re-sweep re-deferred batched files"; exit 1;
fi
echo "no re-deferrals"

echo "== delete binary members =="
# delete helper (no CLI rm exists); built from the repo objects
cat > "$WORK/bzrm.c" <<'C'
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
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/bzrm" "$WORK/bzrm.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs,vol_tier}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

# phase A: two members only -- their shared batches may stay alive
"$WORK/bzrm" "$IMG" elf00 i386.bin
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1
ok=1
for f in $(echo "$FILES" | tr ' ' '\n' | grep -vE '^(elf00|i386\.bin)$'); do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.2" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.2" || { echo "MISMATCH after delete: $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "survivors bit-exact after partial delete"

# phase B: delete ALL remaining batched binaries -> every binary batch dies
REST=$(echo "$FILES" | tr ' ' '\n' | grep -E '^(elf[0-9]+|lib[0-9]+\.so|a64\.bin|macho\.bin)$' | grep -v '^elf00$')
"$WORK/bzrm" "$IMG" $REST
$B/invf-sweep "$IMG" > "$WORK/sweep4.log" 2>&1
GC_LINE=$(grep "text gc:" "$WORK/sweep4.log" || true)
echo "${GC_LINE:-no gc line}"
echo "$GC_LINE" | grep -q "text gc: [1-9]" || { echo "FAIL: GC reclaimed nothing"; exit 1; }
if grep -q "binary -> ZSTD batch\|text -> PPMd batch" "$WORK/sweep4.log"; then
    echo "FAIL: re-sweep re-deferred after deletes"; exit 1;
fi

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
$B/invf-verify "$IMG" --deep | tail -1

echo "== survivors still bit-exact =="
for f in a.c b.py notes.txt tiny.elf rand.bin; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.3" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.3" || { echo "MISMATCH final: $f"; exit 1; }
done
echo "texts + generic survivors intact"

echo "BINBATCH E2E: PASS"
