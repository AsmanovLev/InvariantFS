#!/bin/bash
# test-sweep-window-reclaim.sh — ONE invf-sweep must not damage a CLEAN volume.
#
#   arm 1 (the finding): build the state from tools/test-resize.sh's own leg
#     sequence -- mkfs 512M + mixed corpus + sweep + --realize (capture #1),
#     grow to 1G, write post.bin, vol_delete_file(post.bin), shrink to 640M --
#     and sweep ONCE. The volume is asserted CLEAN and bit-exact BEFORE the
#     sweep; it must be CLEAN and bit-exact AFTER it.
#
#     Why that sequence and not a synthetic one: the defect needs the volume's
#     block COUNT to move while a save point's pin is live. The pin's mark set
#     is sized in bytes by the volume it was taken on (fh.bitmap_bytes), and
#     the reclaim used to read spn_map_bytes(v) -- the count NOW -- out of the
#     run, so after any resize it read past the end of the stored bitmap and
#     took the following block of the image as mark bits. invf-resize is the
#     only thing that moves the count, and a sweep is the only thing that
#     takes a capture and then runs the reclaim. The orphans post.bin's delete
#     leaves behind are what makes the volume full enough that the fold puts
#     live base B+tree pages in the freshly grown tail -- i.e. inside the
#     window the phantom marks describe. Measured on this exact state before
#     the fix: one sweep freed 150 live base pages and recorded one of them
#     (160165) as the base_root it was about to publish; fsck then said
#     DAMAGED, "pages walked" fell 101 -> 8, and every corpus file came back
#     with the wrong bytes.
#
#   arm 2 (the control the fix cannot fake): the same corpus on a volume with
#     NO post.bin and NO resize. Sweep twice and require the second sweep's
#     capture to actually DISCHARGE the first one's pin -- "reclaim: N blocks"
#     with N > 0 in its log. Without this arm a fix that simply stopped
#     reclaiming would pass arm 1 perfectly and leak a generation forever.
#
# Run from the repo root after `make`:  bash tools/test-sweep-window-reclaim.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/spnwin
trap 'rm -rf "$WORK"' EXIT   # e2e hygiene: /dev/shm is a small tmpfs
IMG=spnwin-a.img      # arm 1: the state that reproduces
IMGCTL=spnwin-c.img   # arm 2: healthy, no orphans, no resize
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGCTL"

fail() { echo "FAIL: $*" >&2; exit 1; }

# bit-exact check of every corpus file against the host original, THROUGH
# invf-cat + cmp. A control that only checked the tool exit code would pass
# while the volume came back with the wrong bytes -- which is exactly what the
# pre-fix run did (invf-cat exited 0 on every file and every one mismatched).
check_all() { # <label>
    local ok=1 f
    for f in $(cd "$WORK/orig" && ls); do
        $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null 2>"$WORK/out/$f.err" \
            || { echo "  cat failed: $f ($1)"; ok=0; continue; }
        cmp -s "$WORK/orig/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($1)"; ok=0; }
    done
    [ "$ok" = 1 ] || fail "bit-exact check: $1"
    echo "  all files bit-exact ($1)"
}
check_all_ctl() { # <label>
    local ok=1 f
    for f in $(cd "$WORK/orig" && ls); do
        $B/invf-cat "$IMGCTL" "$f" "$WORK/out/$f" >/dev/null 2>&1 \
            || { echo "  cat failed: $f ($1)"; ok=0; continue; }
        cmp -s "$WORK/orig/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($1)"; ok=0; }
    done
    [ "$ok" = 1 ] || fail "bit-exact check: $1"
    echo "  all files bit-exact ($1)"
}

fsck_ok() { # <img> <label>
    $B/invf-fsck "$1" > "$WORK/fsck-$2.log" 2>&1 || true
    cat "$WORK/fsck-$2.log"
    grep -q "^OK$" "$WORK/fsck-$2.log" \
        || fail "$2: fsck is not OK (a maintenance pass damaged the volume)"
    # the tree must be intact, not merely readable: the pre-fix failure fell
    # from 101 walked pages / 122 base keys to 8 / 30 and still exited 0 here
    grep -q "  torn slots:   0" "$WORK/fsck-$2.log" || fail "$2: a torn root slot"
    grep -q "  bad pages:    0" "$WORK/fsck-$2.log" || fail "$2: bad base pages"
    if grep -q "a live save point pins a DAMAGED base tree" "$WORK/fsck-$2.log"; then
        fail "$2: the save point pins a damaged tree"
    fi
    return 0
}
fsck_field() { # <img> <field-label>
    $B/invf-fsck "$1" 2>/dev/null | sed -n "s/^  $2: *\\([0-9]*\\).*/\\1/p"
}
free_blocks() { $B/invf-fsck "$1" | sed -n 's/^  free blocks:  \([0-9]*\).*/\1/p'; }

echo "== corpus (same shapes as tools/test-resize.sh: texts, fake ELF/PE, an"
echo "   incompressible RAW file, and a TAR, so every transform lane fires) =="
python3 - <<'PY'
import os, random, tarfile, io
random.seed(18)
d = "/dev/shm/spnwin/orig"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

def fake_bin(name, emachine, size):
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
print("  corpus:", *sorted(os.listdir(d)))
PY

echo
echo "== [ARM 1] build the pre-state, then ONE sweep must leave it intact =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
for f in $(cd "$WORK/orig" && ls); do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
# capture #1, on a 512M volume: this is the pin whose mark set is 16384 bytes
$B/invf-sweep "$IMG" > "$WORK/sweep-a1.log" 2>&1 || { cat "$WORK/sweep-a1.log"; exit 1; }
$B/invf-sweep "$IMG" --realize >/dev/null 2>&1 || true
fsck_ok "$IMG" "pre-resize"
check_all "pre-resize baseline"
F0=$(free_blocks "$IMG")
echo "  free blocks pre-grow: $F0"

# A2: grow. This is what makes the stored mark set the wrong size for the
# volume that will read it back.
$B/invf-resize "$IMG" 1G > "$WORK/resize-grow.log" 2>&1 \
    || { cat "$WORK/resize-grow.log"; exit 1; }
fsck_ok "$IMG" "post-grow"
check_all "post-grow"
echo "  free blocks post-grow: $(free_blocks "$IMG")"

# A3: post.bin, larger than the pre-grow volume had free, so the fold has to
# place real pages in the grown tail.
POST_BLOCKS=$((F0 + 20000))
python3 -c "
import os, sys
open(sys.argv[1], 'wb').write(os.urandom(int(sys.argv[2]) * 4096))" \
    "$WORK/post.bin" "$POST_BLOCKS"
$B/invf-cp "$IMG" "$WORK/post.bin" post.bin >/dev/null || fail "post.bin write failed"
$B/invf-cat "$IMG" post.bin "$WORK/out/post.bin" >/dev/null
cmp -s "$WORK/post.bin" "$WORK/out/post.bin" || fail "post.bin not bit-exact"
rm -f "$WORK/post.bin" "$WORK/out/post.bin"
echo "  post.bin ($POST_BLOCKS blocks) written and bit-exact"

# A4: delete it (this is what leaves the orphans) and shrink back.
# No CLI rm exists, so this is the same public-API probe test-resize.sh uses.
mkdir -p "$WORK/tools"
cat > "$WORK/tools/spnprobe.c" <<'PROBE_EOF'
/* spnprobe — delete a file through the public volume API (no CLI rm exists). */
#include <stdio.h>
#include <string.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc;
    invfs_volume *v;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    rc = vol_delete_file(v, argv[2]);
    if (rc == 0) rc = vol_flush(v);
    vol_close(v);
    return rc ? 1 : 0;
}
PROBE_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes \
    -I$REPO/src/vendor7z -o "$WORK/tools/spnprobe" "$WORK/tools/spnprobe.c" \
    $(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt") \
    -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/tools/spnprobe" "$IMG" post.bin || fail "could not delete post.bin"
$B/invf-resize "$IMG" 640M > "$WORK/resize-shrink.log" 2>&1 || true
echo "  post.bin deleted; $(grep -c . "$WORK/resize-shrink.log") line(s) of resize output"

# The precondition, asserted loudly: the volume is CLEAN and bit-exact NOW.
echo "--- PRE-SWEEP ---"
fsck_ok "$IMG" "pre-sweep"
PAGES0=$(fsck_field "$IMG" "pages walked")
KEYS0=$(fsck_field "$IMG" "base keys")
echo "  pages walked: $PAGES0   base keys: $KEYS0"
check_all "pre-sweep"

echo
echo "--- ONE invf-sweep ---"
$B/invf-sweep "$IMG" > "$WORK/sweep-hit.log" 2>&1 || { cat "$WORK/sweep-hit.log"; exit 1; }
grep -E "\[spt0\]" "$WORK/sweep-hit.log" || true

echo
echo "--- POST-SWEEP: the whole assertion ---"
fsck_ok "$IMG" "post-sweep"
PAGES1=$(fsck_field "$IMG" "pages walked")
KEYS1=$(fsck_field "$IMG" "base keys")
[ "$PAGES1" = "$PAGES0" ] \
    || fail "the base tree lost pages: walked $PAGES0 -> $PAGES1 (a sweep freed live metadata)"
[ "$KEYS1" = "$KEYS0" ] \
    || fail "base keys $KEYS0 -> $KEYS1 (a sweep lost tree content)"
echo "  pages walked: $PAGES1   base keys: $KEYS1   (unchanged)"
check_all "post-sweep"
if grep -q "a live save point pins a DAMAGED base tree" "$WORK/fsck-post-sweep.log"; then
    fail "the save point pins a damaged tree"
fi
echo "  ARM 1: one sweep on a clean volume left it clean and bit-exact"

echo
echo "== [ARM 2] control: a healthy volume, no orphans, no resize -- the"
echo "   reclaim must still DISCHARGE the previous window's pin =="
$B/invf-mkfs "$IMGCTL" 0.5 >/dev/null
for f in $(cd "$WORK/orig" && ls); do
    $B/invf-cp "$IMGCTL" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGCTL" > "$WORK/sweep-c1.log" 2>&1 \
    || { cat "$WORK/sweep-c1.log"; exit 1; }
# sweep #1 re-encoded the corpus, so the segments its own capture pinned are
# now dead; sweep #2's capture is the only thing that can free them.
$B/invf-sweep "$IMGCTL" > "$WORK/sweep-c2.log" 2>&1 \
    || { cat "$WORK/sweep-c2.log"; exit 1; }
grep "\[spt0\]" "$WORK/sweep-c2.log" || true
RECLAIMED=$(sed -n 's/^\[spt0\] reclaim: \([0-9][0-9]*\) blocks.*/\1/p' "$WORK/sweep-c2.log")
{ [ -n "$RECLAIMED" ] && [ "$RECLAIMED" -gt 0 ]; } \
    || fail "ARM 2: the reclaim fired on no debt at all -- the fix has stopped reclaiming"
echo "  the second sweep discharged $RECLAIMED block(s) the first one held"
fsck_ok "$IMGCTL" "control"
check_all_ctl "control"
echo "  ARM 2: reclaim still runs, volume intact"

echo
echo "SWEEP-WINDOW-RECLAIM: PASS"