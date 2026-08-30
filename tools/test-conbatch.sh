#!/bin/bash
# test-conbatch.sh — WP14b M1: batching of container members end-to-end
# (persistent regression).
#
#   Build TARs whose members are real ELF binaries (20-30 files from
#   /usr/bin, mixed sizes incl. >4MB), text files and an incompressible/
#   tiny misc member -> import -> ONE sweep (TARs explode AND the parts
#   batch in the same run; aggregate per-container sweep lines) ->
#   verify --deep -> invf-cat the CONTAINER names, sha256 vs the original
#   .tar bytes (rebuild through batched parts must be bit-exact) ->
#   class stamps (parts BATCHED_BIN / TEXT, containers CONTAINER{TARR}) ->
#   re-sweep idempotent -> delete a container (vol_unlink, the FUSE path)
#   -> all its '!' siblings die -> sweep GC reclaims dead batches ->
#   invf-fsck clean -> survivors bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-conbatch.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp14cb
IMG=wp14cb.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

echo "== mkfs =="
$B/invf-mkfs "$IMG" 1.0 >/dev/null

echo "== generate corpus =="
python3 - <<'PY'
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
for root, _dirs, files in os.walk(src):
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
PY

echo "== import =="
for f in bins.tar texts.tar misc.tar; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
# parts batch in the SAME run their container exploded, aggregated per container
grep -E "parts -> |batches flushed|sweep done" "$WORK/sweep1.log"
grep -q "bins.tar!\*: [0-9]* parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: bins.tar parts not batched"; exit 1; }
grep -q "texts.tar!\*: [0-9]* parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: texts.tar parts not batched"; exit 1; }
# no per-part line noise
if grep -q "!part[0-9]*: .*batch" "$WORK/sweep1.log"; then
    echo "FAIL: per-part deferral lines leaked"; exit 1;
fi

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== container rebuild bit-exact through batched parts =="
for f in bins.tar texts.tar misc.tar; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    [ "$a" = "$b" ] || { echo "MISMATCH $f"; exit 1; }
done
echo "all 3 containers bit-exact"

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
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread

NB=$($B/invf-ls "$IMG" | grep -c "^")  # total lines (header+files+summary)
NPARTS=$($B/invf-ls "$IMG" | grep -c '!' || true)
echo "live names with '!': $NPARTS"
ok=1
n_bz=0; n_tz=0; n_gen=0
for p in $($B/invf-ls "$IMG" | awk '/!/ {print $5}'); do
    C=$("$WORK/classof" "$IMG" "$p")
    case "$p,$C" in
    bins.tar!*,cls=8\ algo=14\ gen=1) n_bz=$((n_bz+1));;
    texts.tar!*,cls=7\ algo=2\ gen=1) n_tz=$((n_tz+1));;
    misc.tar!*,none) n_gen=$((n_gen+1));;          # the random member stays generic
    misc.tar!*,cls=7\ algo=2\ gen=1) n_tz=$((n_tz+1));;   # tiny.txt: text-batched
    *) echo "FAIL: $p has unexpected stamp: $C"; ok=0;;
    esac
done
[ "$ok" = 1 ] || exit 1
[ "$n_bz" -ge 20 ] || { echo "FAIL: only $n_bz batched binary parts"; exit 1; }
[ "$n_tz" -ge 10 ] || { echo "FAIL: only $n_tz batched text parts"; exit 1; }
[ "$n_gen" -ge 1 ] || { echo "FAIL: misc.tar members should stay generic"; exit 1; }
echo "stamps: $n_bz BATCHED_BIN{ZSTD_BCJ} parts, $n_tz TEXT{PPMD} parts, $n_gen generic parts"
for f in bins.tar texts.tar misc.tar; do
    C=$("$WORK/classof" "$IMG" "$f")
    [ "$C" = "cls=3 algo=8 gen=1" ] || { echo "FAIL: $f: want CONTAINER{TARR}, got $C"; exit 1; }
done
echo "containers stamped CONTAINER{TARR}"

echo "== re-sweep is idempotent =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1
if grep -q "batch\|parts -> " "$WORK/sweep2.log"; then
    echo "FAIL: re-sweep re-deferred"; cat "$WORK/sweep2.log"; exit 1;
fi
echo "no re-deferrals"

echo "== delete bins.tar (vol_unlink, the FUSE path) =="
cat > "$WORK/cbrm.c" <<'C'
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
        if (vol_unlink(v, argv[i]) != 0) { rc = 1; fprintf(stderr, "rm %s failed\n", argv[i]); }
    vol_flush(v);
    vol_close(v);
    return rc;
}
C
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/cbrm" "$IMG" bins.tar
# every "bins.tar!..." sibling must be gone (deletion semantics, WP14b)
if $B/invf-ls "$IMG" | grep -q "bins.tar!"; then
    echo "FAIL: bins.tar parts survived the container delete"; exit 1;
fi
echo "container + all parts deleted"

echo "== sweep: GC reclaims the dead batches =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1
GC_LINE=$(grep "text gc:" "$WORK/sweep3.log" || true)
echo "${GC_LINE:-no gc line}"
echo "$GC_LINE" | grep -q "text gc: [1-9]" || { echo "FAIL: GC reclaimed nothing"; exit 1; }

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivors bit-exact =="
for f in texts.tar misc.tar; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.2" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.2" || { echo "MISMATCH after delete: $f"; exit 1; }
done
$B/invf-verify "$IMG" --deep | tail -1

echo "== empty-member tar regression (zero-length part must not corrupt) =="
mkdir -p "$WORK/emptyfix"
printf 'some compressible content some compressible content\n' > "$WORK/emptyfix/full.txt"
: > "$WORK/emptyfix/empty.txt"
tar -cf "$WORK/emptyfix/e.tar" -C "$WORK/emptyfix" .   # includes './' dir member
$B/invf-cp "$IMG" "$WORK/emptyfix/e.tar" e.tar >/dev/null
$B/invf-sweep "$IMG" >/dev/null 2>&1
$B/invf-cat "$IMG" e.tar "$WORK/out/e.tar" >/dev/null
cmp -s "$WORK/emptyfix/e.tar" "$WORK/out/e.tar" || { echo "FAIL: empty-member tar mismatch"; exit 1; }
$B/invf-verify "$IMG" --deep | tail -1 | grep -q "0 corrupt" || { echo "FAIL: corrupt after empty-member sweep"; exit 1; }

echo "CONBATCH E2E: PASS"
