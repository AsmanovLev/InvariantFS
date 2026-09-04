#!/bin/bash
# test-resize.sh — WP18 offline volume resize (invf-resize) end-to-end
# (persistent regression).
#
#   image A (grow leg): mkfs 512MB -> mixed corpus (~50MB: texts, fabricated
#   ELF/PE binaries, a tar, an incompressible RAW file) -> sweep -> fsck OK
#   -> verify --deep -> bit-exact baseline -> GROW to 1G: fsck OK, verify
#   --deep, bit-exact, free blocks grew by exactly the delta -> write MORE
#   data post-grow (a file larger than the OLD volume's free space -- the
#   proof the new room is real) -> fsck OK, bit-exact.
#
#   image A cont. (shrink leg): drop the big post-grow file, shrink
#   1G -> 640M (tail free): works, fsck OK, verify --deep, bit-exact.
#   Add a 300MB incompressible file whose shadow occupancy crosses the
#   300MB boundary: shrink 640M -> 300M refuses honestly (count + first
#   block named), volume untouched (still mounts, reads, fsck OK).
#   Shrink 640M -> 448M with the file in place (tail above 448M is free):
#   works, bit-exact.
#
#   image B (batches + container members): text corpus (real C sources ->
#   PPMd batches) + a tar of 26 real ELF binaries (TARR + ZSTD member
#   batches) -> sweep -> verify --deep -> grow 512M -> 768M -> fsck OK,
#   verify --deep, container sha256 + texts bit-exact.
#
#   image C (sealed refusal): mkfs -> corpus -> sweep --seal -> resize
#   refuses with the unseal message -> --free-redundant -> resize succeeds,
#   fsck OK, bit-exact.
#
#   image D (crash legs, INVFS_RESIZE_ABORT_AT hook):
#   "staged" (kill -9 after the staging copy, before the arm) -> volume
#   opens CLEAN at the OLD size, bit-exact, and a plain re-run completes;
#   "armed" (kill after the descriptor) and "moved" (kill mid-apply, inside
#   the roll-forward) -> the NEXT open finishes the resize (roll-forward),
#   fsck OK, bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-resize.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp18resize
trap 'rm -rf "$WORK" /dev/shm/wp18r-*.img' EXIT  # e2e hygiene: /dev/shm is a small tmpfs
IMG=wp18r-a.img      # grow + shrink legs
IMGB=wp18r-b.img     # batches + container members
IMGC=wp18r-c.img     # sealed refusal
IMGD=wp18r-d.img     # crash legs
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGB" "$IMGC" "$IMGD"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== build the probe helper (public API only; sealpick convention) =="
mkdir -p "$WORK/tools"
cat > "$WORK/tools/rszprobe.c" <<'PROBE_EOF'
/* rszprobe — WP18 test helper (uses only the public volume.h API).
 *
 *   rszprobe <img> blocks       -> superblock total_blocks
 *   rszprobe <img> rm <name>    -> delete a file (no CLI rm exists)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }

    if (!strcmp(argv[2], "blocks")) {
        printf("%llu\n", (unsigned long long)vol_sb(v)->total_blocks);
        vol_close(v);
        return 0;
    }
    if (!strcmp(argv[2], "rm") && argc == 4) {
        int rc = vol_delete_file(v, argv[3]);
        if (rc == 0) rc = vol_flush(v);
        vol_close(v);
        return rc ? 1 : 0;
    }
    vol_close(v);
    return 2;
}
PROBE_EOF
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -I$REPO/src-extracted/VFS/src/core -I$REPO/src-extracted/VFS/src/codecs -I$REPO/src-extracted/VFS/src/recipes -I$REPO/src-extracted/VFS/src/vendor7z -o "$WORK/tools/rszprobe" \
    "$WORK/tools/rszprobe.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,vol_tier,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
PROBE="$WORK/tools/rszprobe"

# bit-exact check of every corpus file against the host original
check_all() { # <label> <orig-dir>
    local ok=1 f
    for f in $(cd "$2" && ls); do
        $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null 2>"$WORK/out/$f.err" \
            || { echo "  cat failed: $f ($1)"; ok=0; continue; }
        cmp -s "$2/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($1)"; ok=0; }
    done
    [ "$ok" = 1 ] || fail "bit-exact check: $1"
    echo "  all files bit-exact ($1)"
}

free_blocks() { $B/invf-fsck "$1" | sed -n 's/^  free blocks:  \([0-9]*\).*/\1/p'; }

echo
echo "== [A1] mkfs + mixed corpus + sweep (grow leg) =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
python3 - <<'PY'
import os, random, tarfile, io
random.seed(18)
d = "/dev/shm/wp18resize/orig"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

def fake_bin(name, emachine, size):
    # compressible fabricated binary: ELF magic + e_machine + payload of
    # repeating patterns (sniffs as a binary family, batches well)
    payload = bytearray()
    pat = bytes(range(64)) * 4 + b"\x00" * 128 + os.urandom(64)
    while len(payload) < size:
        payload += pat
        payload += bytes([random.randrange(256)]) * 32
    h = bytearray(64)
    h[0:4] = b"\x7fELF"; h[18] = emachine & 0xFF; h[19] = emachine >> 8
    open(os.path.join(d, name), "wb").write(bytes(h) + bytes(payload[:size]))

text_file("a.c", 1_200_000)
text_file("h.py", 450_000)
text_file("r.log", 5_000_000)
text_file("m.md", 900_000)
text_file("d.json", 600_000)
text_file("w.py", 2_500_000)
fake_bin("bin_x64", 62, 4_000_000)     # EM_X86_64  -> BCJ batch
fake_bin("bin_a64", 183, 3_000_000)    # EM_AARCH64 -> non-BCJ batch
fake_bin("bin_pe", 0, 2_000_000)       # overwritten below: MZ/PE
p = bytearray(open(os.path.join(d, "bin_pe"), "rb").read())
p[0:2] = b"MZ"; p[0x3C:0x40] = (0x40).to_bytes(4, "little")
p[0x40:0x44] = b"PE\0\0"
open(os.path.join(d, "bin_pe"), "wb").write(bytes(p))
# incompressible file: stays in the RAW zone
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(2_560_000))
# a tar of some texts (TARR decomposition + part batching)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as tf:
    for name in ["a.c", "h.py", "r.log"]:
        data = open(os.path.join(d, name), "rb").read()
        ti = tarfile.TarInfo("t/" + name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
open(os.path.join(d, "t.tar"), "wb").write(buf.getvalue())
print("corpus:", *sorted(os.listdir(d)))
PY
for f in $(cd "$WORK/orig" && ls); do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMG" > "$WORK/sweep-a.log" 2>&1 || { cat "$WORK/sweep-a.log"; exit 1; }
# WP22d: a productive sweep leaves a live checkpoint; resize refuses it
$B/invf-sweep "$IMG" --realize >/dev/null 2>&1 || true
$B/invf-fsck "$IMG" | tee "$WORK/fsck-a0.log" | grep -q "^OK$" || fail "fsck not clean pre-resize"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify-a0.log" | grep -q " 0 corrupt," \
    || fail "verify not clean pre-resize"
check_all "pre-resize baseline" "$WORK/orig"
F0=$(free_blocks "$IMG")
echo "  free blocks pre-grow: $F0"

echo
echo "== [A2] grow 512M -> 1G =="
$B/invf-resize "$IMG" 1G | tee "$WORK/resize-a1.log"
grep -q "invf-resize: OK" "$WORK/resize-a1.log" || fail "grow did not report OK"
[ "$($PROBE "$IMG" blocks)" = "262144" ] || fail "total_blocks != 262144 after grow"
$B/invf-fsck "$IMG" | tee "$WORK/fsck-a1.log" | grep -q "^OK$" || fail "fsck not clean post-grow"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify-a1.log" | grep -q " 0 corrupt," \
    || fail "verify not clean post-grow"
check_all "post-grow" "$WORK/orig"
F1=$(free_blocks "$IMG")
echo "  free blocks post-grow: $F1 (delta $((F1 - F0)))"
# 512M -> 1G is +131072 blocks; the staging rides the grown tail for free
[ "$((F1 - F0))" = "131072" ] || fail "free space did not grow by the exact delta"

echo
echo "== [A3] write MORE data post-grow (bigger than the old free space) =="
# A file with more blocks than the pre-grow volume had free could not have
# fitted before: proving the new room is real, not accounting.
POST_BLOCKS=$((F0 + 20000))
python3 -c "
import os
open('/dev/shm/wp18resize/post.bin','wb').write(os.urandom($POST_BLOCKS * 4096))"
$B/invf-cp "$IMG" "$WORK/post.bin" post.bin >/dev/null || fail "post-grow write failed"
echo "  wrote post.bin ($POST_BLOCKS blocks > old free $F0)"
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean after post-grow write"
$B/invf-cat "$IMG" post.bin "$WORK/out/post.bin" >/dev/null
cmp -s "$WORK/post.bin" "$WORK/out/post.bin" || fail "post.bin not bit-exact"
rm -f "$WORK/post.bin" "$WORK/out/post.bin"   # 500+MB of tmpfs back
echo "  post.bin bit-exact"
check_all "post-grow-write" "$WORK/orig"

echo
echo "== [A4] shrink 1G -> 640M (tail free) =="
$PROBE "$IMG" rm post.bin || fail "could not delete post.bin"
F2=$(free_blocks "$IMG")
$B/invf-resize "$IMG" 640M | tee "$WORK/resize-a2.log"
grep -q "invf-resize: OK" "$WORK/resize-a2.log" || fail "shrink to 640M failed"
[ "$($PROBE "$IMG" blocks)" = "163840" ] || fail "total_blocks != 163840 after shrink"
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean post-shrink"
$B/invf-verify "$IMG" --deep | grep -q " 0 corrupt," || fail "verify not clean post-shrink"
check_all "post-shrink-640M" "$WORK/orig"
F3=$(free_blocks "$IMG")
echo "  free blocks: $F2 (1G) -> $F3 (640M)"

echo
echo "== [A5] shrink 640M -> 300M with data in the way: honest refusal =="
# 300MB of incompressible data: RAW zone takes ~94MB, the rest lands in the
# shadow zone past the 300M boundary (block 76800) -> the tail-free check
# must fire.
python3 -c "
import os
open('/dev/shm/wp18resize/big.bin','wb').write(os.urandom(300 * 1024 * 1024))"
$B/invf-cp "$IMG" "$WORK/big.bin" big.bin >/dev/null || fail "big.bin write failed"
set +e
$B/invf-resize "$IMG" 300M > "$WORK/resize-a3.log" 2>&1
RC=$?
set -e
cat "$WORK/resize-a3.log"
[ "$RC" != 0 ] || fail "shrink to 300M succeeded with live blocks in the way"
grep -q "live block(s) at/above the new boundary" "$WORK/resize-a3.log" \
    || fail "no honest refusal message"
grep -q "no compaction in v1" "$WORK/resize-a3.log" || fail "no guidance in refusal"
echo "  refused honestly (rc=$RC)"
# volume untouched: still 640M, mounts, reads, fsck OK
[ "$($PROBE "$IMG" blocks)" = "163840" ] || fail "size changed despite the refusal"
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean after refused shrink"
$B/invf-cat "$IMG" big.bin "$WORK/out/big.bin" >/dev/null
cmp -s "$WORK/big.bin" "$WORK/out/big.bin" || fail "big.bin not bit-exact after refusal"
check_all "post-refusal" "$WORK/orig"

echo
echo "== [A6] shrink 640M -> 448M (tail above 448M is free) =="
# big.bin's shadow tail sits below block 114688 (448M), so this one works.
$B/invf-resize "$IMG" 448M | tee "$WORK/resize-a4.log"
grep -q "invf-resize: OK" "$WORK/resize-a4.log" || fail "shrink to 448M failed"
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean post-shrink-448M"
$B/invf-cat "$IMG" big.bin "$WORK/out/big.bin.2" >/dev/null
cmp -s "$WORK/big.bin" "$WORK/out/big.bin.2" || fail "big.bin not bit-exact post-448M"
check_all "post-shrink-448M" "$WORK/orig"
rm -f "$WORK/big.bin"   # host copy: 300MB of tmpfs back

echo
echo "== [B] text batches + container members across a grow =="
$B/invf-mkfs "$IMGB" 0.5 >/dev/null
python3 - <<'PY'
import os, shutil, subprocess
d = "/dev/shm/wp18resize/origb"
os.makedirs(d, exist_ok=True)
# text batch fodder: real C sources, duplicated with edits
src = "/home/user/InvariantFS/tools/busybox-src"
files = []
for root, _, names in os.walk(src):
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
PY
for f in $(cd "$WORK/origb" && ls | grep -v '^bins$'); do
    $B/invf-cp "$IMGB" "$WORK/origb/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGB" > "$WORK/sweep-b.log" 2>&1 || { cat "$WORK/sweep-b.log"; exit 1; }
$B/invf-sweep "$IMGB" --realize >/dev/null 2>&1 || true
grep -q "PPMd batch" "$WORK/sweep-b.log" || fail "no text batching happened"
grep -q "bins.tar!\*: .* -> ZSTD batch" "$WORK/sweep-b.log" \
    || fail "container members were not batched"
$B/invf-verify "$IMGB" --deep | grep -q " 0 corrupt," || fail "B: verify not clean pre-resize"
SHA0=$(sha256sum "$WORK/origb/bins.tar" | awk '{print $1}')
$B/invf-resize "$IMGB" 768M | tee "$WORK/resize-b.log" | grep -q "invf-resize: OK" \
    || fail "B: grow failed"
[ "$($PROBE "$IMGB" blocks)" = "196608" ] || fail "B: total_blocks != 196608"
$B/invf-fsck "$IMGB" | grep -q "^OK$" || fail "B: fsck not clean post-grow"
$B/invf-verify "$IMGB" --deep | grep -q " 0 corrupt," || fail "B: verify not clean post-grow"
$B/invf-cat "$IMGB" bins.tar "$WORK/out/bins.tar" >/dev/null
SHA1=$(sha256sum "$WORK/out/bins.tar" | awk '{print $1}')
[ "$SHA0" = "$SHA1" ] || fail "B: container not bit-exact after grow"
echo "  container sha256 identical across the grow"
for f in $(cd "$WORK/origb" && ls | grep '\.c$'); do
    $B/invf-cat "$IMGB" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/origb/$f" "$WORK/out/$f" || fail "B: MISMATCH $f"
done
echo "  all text-batch members bit-exact"

echo
echo "== [C] sealed volume refuses, unsealed resizes =="
$B/invf-mkfs "$IMGC" 0.5 >/dev/null
$B/invf-cp "$IMGC" "$WORK/orig/a.c" a.c >/dev/null
$B/invf-cp "$IMGC" "$WORK/orig/w.py" w.py >/dev/null
$B/invf-sweep "$IMGC" --seal > "$WORK/seal-c.log" 2>&1 || { cat "$WORK/seal-c.log"; exit 1; }
grep -q "\[seal\]" "$WORK/seal-c.log" || fail "C: seal did not happen"
set +e
$B/invf-resize "$IMGC" 1G > "$WORK/resize-c.log" 2>&1
RC=$?
set -e
cat "$WORK/resize-c.log"
[ "$RC" != 0 ] || fail "C: resize succeeded on a sealed volume"
grep -q "unseal first" "$WORK/resize-c.log" || fail "C: no unseal-first message"
grep -q "free-redundant" "$WORK/resize-c.log" || fail "C: no --free-redundant hint"
[ "$($PROBE "$IMGC" blocks)" = "131072" ] || fail "C: size changed despite refusal"
$B/invf-fsck "$IMGC" | grep -q "^OK$" || fail "C: fsck not clean after refusal"
echo "  sealed volume refused (rc=$RC), volume untouched"
$B/invf-sweep "$IMGC" --free-redundant > "$WORK/free-c.log" 2>&1 \
    || { cat "$WORK/free-c.log"; exit 1; }
# the sweep --seal above left a live checkpoint (CKP0) which resize also
# refuses (it would move the positions the checkpoint pins); realize it,
# then fsck -f reclaims the degraded-retention leftovers (WP22d: frees
# under a live checkpoint stay allocated-unregistered until resolution)
$B/invf-sweep "$IMGC" --realize >/dev/null 2>&1 || true
$B/invf-fsck "$IMGC" -f >/dev/null 2>&1 || true
$B/invf-resize "$IMGC" 1G | grep -q "invf-resize: OK" || fail "C: grow after unseal failed"
$B/invf-fsck "$IMGC" | grep -q "^OK$" || fail "C: fsck not clean post-grow"
$B/invf-cat "$IMGC" a.c "$WORK/out/a.c" >/dev/null
cmp -s "$WORK/orig/a.c" "$WORK/out/a.c" || fail "C: a.c not bit-exact"
echo "  unsealed volume grew fine"

echo
echo "== [D1] crash after staging, before the arm: old size survives =="
$B/invf-mkfs "$IMGD" 0.5 >/dev/null
for f in a.c h.py bin_x64; do
    $B/invf-cp "$IMGD" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGD" >/dev/null 2>&1
$B/invf-sweep "$IMGD" --realize >/dev/null 2>&1 || true
set +e
INVFS_RESIZE_ABORT_AT=staged $B/invf-resize "$IMGD" 1G > "$WORK/resize-d1.log" 2>&1
RC=$?
set -e
[ "$RC" = "137" ] || { cat "$WORK/resize-d1.log"; fail "D1: expected SIGKILL (137), got $RC"; }
# killed mid-staging: superblock untouched (CLEAN, old size); the image file
# is already ftruncated to 1G, which the format tolerates (blocks past
# total_blocks are never allocated) -- the next open must not even notice.
[ "$($PROBE "$IMGD" blocks)" = "131072" ] || fail "D1: size moved despite no arm"
$B/invf-fsck "$IMGD" | tee "$WORK/fsck-d1.log" | grep -q "^OK$" || fail "D1: fsck not clean"
grep -q "state:        CLEAN" "$WORK/fsck-d1.log" || fail "D1: not CLEAN after pre-arm kill"
for f in a.c h.py bin_x64; do
    $B/invf-cat "$IMGD" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || fail "D1: MISMATCH $f"
done
echo "  killed pre-arm: volume opens CLEAN at 512M, bit-exact"
# a plain re-run completes
$B/invf-resize "$IMGD" 1G | grep -q "invf-resize: OK" || fail "D1: re-run failed"
[ "$($PROBE "$IMGD" blocks)" = "262144" ] || fail "D1: re-run did not reach 1G"
$B/invf-fsck "$IMGD" | grep -q "^OK$" || fail "D1: fsck not clean after re-run"
echo "  re-run completed the grow"

echo
echo "== [D2] crash after the arm: the next open rolls forward =="
set +e
INVFS_RESIZE_ABORT_AT=armed $B/invf-resize "$IMGD" 768M > "$WORK/resize-d2.log" 2>&1
RC=$?
set -e
[ "$RC" = "137" ] || { cat "$WORK/resize-d2.log"; fail "D2: expected SIGKILL (137), got $RC"; }
grep -q "armed: state=RECOVERY" "$WORK/resize-d2.log" || fail "D2: never armed"
# the fsck open IS the roll-forward trigger
$B/invf-fsck "$IMGD" 2>&1 | tee "$WORK/fsck-d2.log" | grep -q "^OK$" \
    || fail "D2: fsck not clean after roll-forward"
grep -q "completed an interrupted resize" "$WORK/fsck-d2.log" \
    || fail "D2: no roll-forward happened"
[ "$($PROBE "$IMGD" blocks)" = "196608" ] || fail "D2: size != 768M after roll-forward"
for f in a.c h.py bin_x64; do
    $B/invf-cat "$IMGD" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || fail "D2: MISMATCH $f"
done
echo "  killed post-arm: fsck open rolled forward to 768M, bit-exact"

echo
echo "== [D3] crash mid-apply (payload moved, no commit): re-entrant roll-forward =="
set +e
INVFS_RESIZE_ABORT_AT=moved $B/invf-resize "$IMGD" 1G > "$WORK/resize-d3.log" 2>&1
RC=$?
set -e
[ "$RC" = "137" ] || { cat "$WORK/resize-d3.log"; fail "D3: expected SIGKILL (137), got $RC"; }
# the apply died between the payload rewrite and the commit; the next open
# re-runs it from the staging (the apply reads only the staging area)
$B/invf-fsck "$IMGD" 2>&1 | tee "$WORK/fsck-d3.log" | grep -q "^OK$" \
    || fail "D3: fsck not clean after roll-forward"
grep -q "completed an interrupted resize" "$WORK/fsck-d3.log" \
    || fail "D3: no roll-forward happened"
[ "$($PROBE "$IMGD" blocks)" = "262144" ] || fail "D3: size != 1G after roll-forward"
$B/invf-verify "$IMGD" --deep | grep -q " 0 corrupt," || fail "D3: verify not clean"
for f in a.c h.py bin_x64; do
    $B/invf-cat "$IMGD" "$f" "$WORK/out/$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || fail "D3: MISMATCH $f"
done
echo "  killed mid-apply: fsck open rolled forward to 1G, bit-exact"

echo
echo "RESIZE E2E: PASS"
