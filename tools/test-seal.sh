#!/bin/bash
# test-seal.sh — WP20 --seal shadow-zone XOR parity end-to-end (persistent
# regression).
#
#   image A: mkfs -> mixed corpus (texts + fabricated ELF/PE binaries + tar
#   + incompressible RAW file) -> sweep -> --seal (summary sane, fsck live,
#   owner hidden) -> bit-exact baseline -> idempotent re-seal ("0 stripes
#   updated") -> modify+sweep+re-seal (only affected stripes updated) ->
#   single-block corruption (batch header block): invf-cat still bit-exact
#   via transparent parity recovery + "[seal] recovered block" log, fsck
#   clean after self-heal -> two blocks in DIFFERENT stripes: both recover
#   -> --unseal (parity gone, fsck clean, reads unaffected) -> re-seal ->
#   PARITY block corruption (reads fine, verify --deep reports the parity
#   mismatch) -> re-seal repairs it -> read-only volume refuses to seal.
#
#   image B (destructive): two blocks in the SAME stripe -> the read fails
#   loudly (EIO), no garbage output, verify --deep reports the corrupt file
#   AND the parity mismatch, fsck stays structurally honest.
#
# Run from the repo root after `make`:  bash tools/test-seal.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp20seal
IMG=wp20seal.img       # image A: the recoverable/main line
IMGB=wp20seal-b.img    # image B: destructive same-stripe test
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGB"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== build the sealpick helper (public API only; tzrm convention) =="
mkdir -p "$WORK/tools"
cat > "$WORK/tools/sealpick.c" <<'SEALPICK_EOF'
/* sealpick — WP20 test helper (uses only the public volume.h API).
 *
 *   sealpick <img> zone          -> "shadow_start total_blocks"
 *   sealpick <img> first <name>  -> "pba len" of the file's first map
 *   sealpick <img> pair          -> "pba1 pba2 name": two occupied blocks of
 *                                   ONE stripe belonging to one user file
 *   sealpick <img> parity        -> first parity block pba
 *   sealpick <img> rm <name>     -> delete a file (no CLI rm exists)
 *   sealpick <img> setro|setrw   -> toggle VOLF_READONLY (persists)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
#include "invarifs.h"

static invfs_volume *v;

/* live user-file id -> name map (internal 0x01-prefixed owners excluded) */
typedef struct { uint64_t id; char name[256]; } idname;
static idname *g_map;
static size_t   g_n, g_cap;

static const char *name_of(uint64_t id)
{
    size_t i;
    for (i = 0; i < g_n; i++)
        if (g_map[i].id == id) return g_map[i].name;
    return NULL;
}

static void build_map(void)
{
    uint64_t pos = vol_inode_area_start(v);
    while (pos) {
        uint32_t magic, rl;
        uint64_t ino, fsz, np;
        char nm[256];
        np = vol_inode_next(v, pos, &magic, &ino, &fsz, nm, sizeof nm, &rl);
        if (!np) break;
        pos = np;
        if (magic != INODE_REC_MAGIC) continue;
        if ((uint8_t)nm[0] == 0x01) continue;      /* internal owners */
        if (vol_find(v, nm) != ino) continue;      /* superseded */
        if (name_of(ino)) continue;
        if (g_n == g_cap) {
            size_t nc = g_cap ? g_cap * 2 : 256;
            void *p = realloc(g_map, nc * sizeof *g_map);
            if (!p) return;
            g_map = p;
            g_cap = nc;
        }
        g_map[g_n].id = ino;
        snprintf(g_map[g_n].name, sizeof g_map[g_n].name, "%s", nm);
        g_n++;
    }
}

int main(int argc, char **argv)
{
    int err = 0;
    const invfs_superblock *sb;
    const invfs_l2p_entry *l2p;
    size_t n = 0, i;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    sb = vol_sb(v);

    if (!strcmp(argv[2], "zone")) {
        printf("%llu %llu\n", (unsigned long long)sb->shadow_zone_start,
               (unsigned long long)sb->total_blocks);
        vol_close(v);
        return 0;
    }
    if (!strcmp(argv[2], "setro") || !strcmp(argv[2], "setrw")) {
        vol_set_readonly(v, !strcmp(argv[2], "setro"));
        if (vol_flush(v) != 0) { vol_close(v); return 1; }
        vol_close(v);
        return 0;
    }
    if (!strcmp(argv[2], "rm") && argc == 4) {
        int rc = vol_delete_file(v, argv[3]);
        if (rc == 0) rc = vol_flush(v);
        vol_close(v);
        return rc ? 1 : 0;
    }

    l2p = vol_l2p(v, &n);

    if (!strcmp(argv[2], "first") && argc == 4) {
        uint64_t id = vol_find(v, argv[3]);
        if (!id) { vol_close(v); return 1; }
        for (i = n; i-- > 0;)
            if (l2p[i].type == INVFS_JRN_MAP && l2p[i].inode == id) {
                printf("%llu %u\n", (unsigned long long)l2p[i].pba,
                       l2p[i].length);
                vol_close(v);
                return 0;
            }
        vol_close(v);
        return 1;
    }
    if (!strcmp(argv[2], "parity")) {
        uint64_t owner;
        char nm[32];
        snprintf(nm, sizeof nm, "\x01parity");
        owner = vol_find(v, nm);
        if (!owner) { vol_close(v); return 1; }
        for (i = 0; i < n; i++)
            if (l2p[i].type == INVFS_JRN_MAP && l2p[i].inode == owner) {
                printf("%llu\n", (unsigned long long)l2p[i].pba);
                vol_close(v);
                return 0;
            }
        vol_close(v);
        return 1;
    }
    if (!strcmp(argv[2], "pair")) {
        uint64_t ss = sb->shadow_zone_start;
        build_map();
        for (i = 0; i < n; i++) {
            uint64_t pba, len, k;
            const char *nm;
            if (l2p[i].type != INVFS_JRN_MAP) continue;
            nm = name_of(l2p[i].inode);
            if (!nm) continue;               /* owner / dead: not a victim */
            pba = l2p[i].pba;
            len = l2p[i].length ? l2p[i].length : 1;
            if (pba < ss) continue;          /* RAW zone: not sealed */
            for (k = 0; k + 1 < len; k++) {
                if ((pba + k - ss) / 32 == (pba + k + 1 - ss) / 32) {
                    printf("%llu %llu %s\n",
                           (unsigned long long)(pba + k),
                           (unsigned long long)(pba + k + 1), nm);
                    vol_close(v);
                    return 0;
                }
            }
        }
        vol_close(v);
        return 1;
    }
    vol_close(v);
    return 2;
}
SEALPICK_EOF
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/tools/sealpick" \
    "$WORK/tools/sealpick.c" \
    $REPO/build/obj/{volume,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
SP="$WORK/tools/sealpick"

echo "== mkfs + corpus =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
python3 - <<'PY'
import os, random, tarfile, io
random.seed(20)
d = "/dev/shm/wp20seal/orig"
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

text_file("a.c", 120_000)
text_file("h.py", 45_000)
text_file("r.log", 500_000)
text_file("m.md", 90_000)
text_file("s.sh", 12_000)
text_file("d.json", 60_000)
text_file("w.py", 250_000)
fake_bin("bin_x64", 62, 400_000)     # EM_X86_64  -> BCJ batch
fake_bin("bin_a64", 183, 300_000)    # EM_AARCH64 -> non-BCJ batch
fake_bin("bin_pe", 0, 200_000)       # overwritten below: MZ/PE
# turn bin_pe into a PE: MZ stub with e_lfanew -> "PE\0\0"
p = bytearray(open(os.path.join(d, "bin_pe"), "rb").read())
p[0:2] = b"MZ"; p[0x3C:0x40] = (0x40).to_bytes(4, "little")
p[0x40:0x44] = b"PE\0\0"
open(os.path.join(d, "bin_pe"), "wb").write(bytes(p))
# incompressible file: stays in the RAW zone (explicitly NOT sealed)
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(256_000))
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

FILES=$(cd "$WORK/orig" && ls)
for f in $FILES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
read SS TB <<< "$($SP "$IMG" zone)"
echo "shadow_start=$SS total_blocks=$TB"
stripe_of() { echo $(( ($1 - SS) / 32 )); }

corrupt_block() { # pba
    python3 -c "import sys; sys.stdout.buffer.write(b'\xa5'*4096)" | \
        dd of="$1" bs=4096 seek="$2" conv=notrunc status=none
}

# bit-exact check of every corpus file against the host original
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

echo
echo "== [1] sweep + --seal =="
$B/invf-sweep "$IMG" --seal > "$WORK/seal1.log" 2>&1 || { cat "$WORK/seal1.log"; exit 1; }
SEAL_LINE=$(grep "\[seal\]" "$WORK/seal1.log")
echo "$SEAL_LINE"
STRIPES=$(echo "$SEAL_LINE" | sed 's/.*\[seal\] \([0-9]*\) stripes.*/\1/')
PARITY=$(echo "$SEAL_LINE" | sed 's/.*stripes, \([0-9]*\) parity blocks.*/\1/')
OVERH=$(echo "$SEAL_LINE" | sed 's/.*overhead \([0-9.]*\)%.*/\1/')
UPDATED=$(echo "$SEAL_LINE" | sed 's/.*; \([0-9]*\) stripes updated.*/\1/')
[ "$STRIPES" -ge 3 ] || fail "expected >=3 sealed stripes, got $STRIPES"
[ "$PARITY" = "$STRIPES" ] || fail "parity blocks ($PARITY) != stripes ($STRIPES)"
[ "$UPDATED" = "$STRIPES" ] || fail "first seal should write every stripe"
python3 - "$OVERH" <<'PY' || fail "overhead out of range"
import sys
o = float(sys.argv[1])
# k=32 -> ~3.125% for packed content; partial tail stripes add a bit
if not (2.0 <= o <= 6.5):
    print(f"overhead {o}% insane")
    sys.exit(1)
print(f"overhead {o}% within the expected k=32 band")
PY
# the seal owner must be hidden from listings
if $B/invf-ls "$IMG" | grep -q "parity\|tzb"; then
    fail "internal owner leaked into directory listing"
fi
# parity blocks are owned -> fsck sees them as live, nothing orphaned
$B/invf-fsck "$IMG" | tee "$WORK/fsck1.log"
grep -q "^OK$" "$WORK/fsck1.log" || fail "fsck not clean after seal"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || fail "corrupt files after seal"
grep -q "parity: $STRIPES sealed stripes, 0 mismatched, 0 missing, 0 extra" \
    "$WORK/verify1.log" || fail "parity leg not clean after seal"
check_all "post-seal baseline"

echo
echo "== [5a] idempotent re-seal =="
$B/invf-sweep "$IMG" --seal > "$WORK/seal2.log" 2>&1 || { cat "$WORK/seal2.log"; exit 1; }
grep "\[seal\]" "$WORK/seal2.log"
grep -q " 0 stripes updated, $STRIPES unchanged" "$WORK/seal2.log" \
    || fail "re-seal was not a no-op"
grep -q "(0 added, 0 stale freed)" "$WORK/seal2.log" || true

echo
echo "== [5b] modify + sweep + re-seal: only affected stripes updated =="
python3 - <<'PY'
import os, random
random.seed(21)
d = "/dev/shm/wp20seal/orig"
WORDS = ("secondary modifications change stripe membership int void "
         "static char while for return\n").split()
out = []; n = 0
while n < 80_000:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "new.txt"), "w").write(" ".join(out)[:80_000])
out = []; n = 0
while n < 95_000:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "m.md"), "w").write(" ".join(out)[:95_000])  # overwrite
os.unlink(os.path.join(d, "s.sh"))                                # delete
PY
$B/invf-cp "$IMG" "$WORK/orig/new.txt" new.txt >/dev/null
$B/invf-cp "$IMG" "$WORK/orig/m.md" m.md >/dev/null
$SP "$IMG" rm s.sh
$B/invf-sweep "$IMG" --seal > "$WORK/seal3.log" 2>&1 || { cat "$WORK/seal3.log"; exit 1; }
SEAL_LINE=$(grep "\[seal\]" "$WORK/seal3.log")
echo "$SEAL_LINE"
STRIPES2=$(echo "$SEAL_LINE" | sed 's/.*\[seal\] \([0-9]*\) stripes.*/\1/')
UPDATED2=$(echo "$SEAL_LINE" | sed 's/.*; \([0-9]*\) stripes updated.*/\1/')
UNCH2=$(echo "$SEAL_LINE" | sed 's/.*updated, \([0-9]*\) unchanged.*/\1/')
[ "$UPDATED2" -ge 1 ] || fail "modification updated no stripe"
[ "$UPDATED2" -lt "$STRIPES2" ] || fail "re-seal rewrote everything"
[ "$UNCH2" -ge 1 ] || fail "no unchanged stripe survived"
check_all "post-modification"
$B/invf-verify "$IMG" --deep | tail -2

echo
echo "== [2] single-block corruption (batch header block): transparent recovery =="
read VPBA VLEN <<< "$($SP "$IMG" first a.c)"
[ "$VPBA" -ge "$SS" ] || fail "victim not in shadow zone"
echo "corrupting block $VPBA (stripe $(stripe_of $VPBA), first map of a.c)"
corrupt_block "$IMG" "$VPBA"
$B/invf-cat "$IMG" a.c "$WORK/out/a.c" > "$WORK/cat2.log" 2>&1 \
    || { cat "$WORK/cat2.log"; fail "cat a.c failed despite the seal"; }
grep -q "\[seal\] recovered block $VPBA via parity" "$WORK/cat2.log" \
    || { cat "$WORK/cat2.log"; fail "recovery log line missing"; }
grep "\[seal\] recovered" "$WORK/cat2.log"
cmp -s "$WORK/orig/a.c" "$WORK/out/a.c" || fail "a.c not bit-exact after recovery"
echo "a.c bit-exact after transparent recovery (block rewritten)"
# a second read takes the self-healed block: no recovery line anymore
$B/invf-cat "$IMG" a.c "$WORK/out/a.c.2" > "$WORK/cat2b.log" 2>&1
cmp -s "$WORK/orig/a.c" "$WORK/out/a.c.2" || fail "a.c not bit-exact on re-read"
if grep -q "\[seal\] recovered" "$WORK/cat2b.log"; then
    fail "self-heal did not persist (second read recovered again)"
fi
echo "self-heal persisted (second read is clean)"
check_all "post-recovery"
$B/invf-fsck "$IMG" | tee "$WORK/fsck2.log" | grep -E "orphans|missing|OK"
grep -q "^OK$" "$WORK/fsck2.log" || fail "fsck not clean after self-heal"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify2.log" | tail -2
grep -q " 0 corrupt," "$WORK/verify2.log" || fail "verify not clean after heal"
grep -q "0 mismatched" "$WORK/verify2.log" \
    || fail "healed block left parity drift"

echo
echo "== [3a] two blocks in DIFFERENT stripes (different segments) =="
# victim 1: a block deep in the shared text batch (last block of a.c's map);
# victim 2: the first block of another file's segment, in a different stripe.
read VPBA VLEN <<< "$($SP "$IMG" first a.c)"
B1=$(( VPBA + VLEN - 1 ))
S1=$(stripe_of $B1)
B2=""
V1=a.c
for f in bin_x64 bin_a64 bin_pe new.txt m.md d.json w.py; do
    read p l <<< "$($SP "$IMG" first "$f")"
    [ "$p" -ge "$SS" ] || continue
    [ "$(stripe_of $p)" != "$S1" ] || continue
    V2=$f; B2=$p; break
done
[ -n "$B2" ] || fail "no second segment in a different stripe"
echo "corrupting $B1 (stripe $S1, batch of $V1) and $B2 (stripe $(stripe_of $B2), $V2)"
corrupt_block "$IMG" "$B1"
corrupt_block "$IMG" "$B2"
$B/invf-cat "$IMG" a.c "$WORK/out/v1.3" > "$WORK/cat3a.log" 2>&1 \
    || { cat "$WORK/cat3a.log"; fail "cat a.c failed"; }
$B/invf-cat "$IMG" "$V2" "$WORK/out/v2.3" > "$WORK/cat3b.log" 2>&1 \
    || { cat "$WORK/cat3b.log"; fail "cat $V2 failed"; }
cmp -s "$WORK/orig/a.c" "$WORK/out/v1.3" || fail "a.c mismatch"
cmp -s "$WORK/orig/$V2" "$WORK/out/v2.3" || fail "$V2 mismatch"
grep -q "\[seal\] recovered block $B1 via parity" "$WORK/cat3a.log" \
    || fail "recovery line for block $B1 missing"
grep -q "\[seal\] recovered block $B2 via parity" "$WORK/cat3b.log" \
    || fail "recovery line for block $B2 missing"
echo "both recovered independently (one stripe syndrome each)"

echo
echo "== [3b] two blocks of ONE segment in different stripes (pass B) =="
# two blocks of the same text batch in different stripes: one read must
# repair both at once
C1=""; C2=""
for ((i = 0; i + 1 < VLEN; i++)); do
    for ((j = i + 16; j < VLEN; j += 16)); do
        if [ "$(stripe_of $((VPBA + i)))" != "$(stripe_of $((VPBA + j)))" ]; then
            C1=$((VPBA + i)); C2=$((VPBA + j)); break 2
        fi
    done
done
[ -n "$C2" ] || fail "batch does not span two stripes"
echo "corrupting $C1 (stripe $(stripe_of $C1)) and $C2 (stripe $(stripe_of $C2)), one segment"
corrupt_block "$IMG" "$C1"
corrupt_block "$IMG" "$C2"
$B/invf-cat "$IMG" a.c "$WORK/out/v1.3b" > "$WORK/cat3c.log" 2>&1 \
    || { cat "$WORK/cat3c.log"; fail "cat a.c failed after 2-stripe hit"; }
cmp -s "$WORK/orig/a.c" "$WORK/out/v1.3b" || fail "a.c mismatch (pass B)"
grep -q "\[seal\] recovered block $C1 via parity" "$WORK/cat3c.log" \
    || fail "recovery line for block $C1 missing"
grep -q "\[seal\] recovered block $C2 via parity" "$WORK/cat3c.log" \
    || fail "recovery line for block $C2 missing"
echo "both blocks of one segment recovered (pass B)"
check_all "post double-recovery"
$B/invf-verify "$IMG" --deep | tail -2 | grep -q " 0 corrupt," \
    || fail "verify not clean after double recovery"
check_all "post double-recovery"
$B/invf-verify "$IMG" --deep | tail -2 | grep -q " 0 corrupt," \
    || fail "verify not clean after double recovery"

echo
echo "== [6] --unseal =="
$B/invf-sweep "$IMG" --unseal > "$WORK/unseal.log" 2>&1 || { cat "$WORK/unseal.log"; exit 1; }
grep "\[unseal\]" "$WORK/unseal.log"
grep -q "\[unseal\] [1-9][0-9]* parity blocks freed" "$WORK/unseal.log" \
    || fail "unseal freed nothing"
$B/invf-fsck "$IMG" | tee "$WORK/fsck6.log" | grep -E "orphans|missing|OK"
grep -q "^OK$" "$WORK/fsck6.log" || fail "fsck not clean after unseal"
# the parity leg must fall silent on an unsealed volume
if $B/invf-verify "$IMG" --deep | grep -q "^parity:"; then
    fail "parity leg still reporting after unseal"
fi
check_all "post-unseal"

echo
echo "== [7] verify --deep parity leg: corrupt a PARITY block =="
$B/invf-sweep "$IMG" --seal > "$WORK/seal4.log" 2>&1 || { cat "$WORK/seal4.log"; exit 1; }
grep "\[seal\]" "$WORK/seal4.log"
PPBA=$($SP "$IMG" parity)
[ -n "$PPBA" ] || fail "no parity block found"
echo "corrupting parity block $PPBA"
corrupt_block "$IMG" "$PPBA"
# data reads never touch parity: everything still bit-exact
check_all "parity-corrupt reads"
set +e
$B/invf-verify "$IMG" --deep > "$WORK/verify7.log" 2>&1
VRC=$?
set -e
grep "parity:" "$WORK/verify7.log"
[ "$VRC" != 0 ] || fail "verify --deep did not flag the parity corruption"
grep -q "parity: [0-9]* sealed stripes, [1-9][0-9]* mismatched" \
    "$WORK/verify7.log" || fail "no parity mismatch reported"
grep -q " 0 corrupt," "$WORK/verify7.log" || fail "files wrongly flagged corrupt"
# re-seal repairs the parity block (check-and-update rewrites it)
$B/invf-sweep "$IMG" --seal > "$WORK/seal5.log" 2>&1 || { cat "$WORK/seal5.log"; exit 1; }
grep "\[seal\]" "$WORK/seal5.log"
$B/invf-verify "$IMG" --deep | tee "$WORK/verify7b.log" | tail -2
grep -q " 0 corrupt," "$WORK/verify7b.log" || fail "verify not clean after re-seal"
grep -q "0 mismatched, 0 missing, 0 extra" "$WORK/verify7b.log" \
    || fail "re-seal did not repair the parity block"

echo
echo "== [8] read-only volume refuses to seal =="
$SP "$IMG" setro
set +e
$B/invf-sweep "$IMG" --seal > "$WORK/ro.log" 2>&1
RORC=$?
set -e
[ "$RORC" != 0 ] || fail "seal succeeded on a read-only volume"
grep -q "read-only" "$WORK/ro.log" || { cat "$WORK/ro.log"; fail "no read-only diagnostic"; }
echo "read-only seal refused (rc=$RORC)"
$SP "$IMG" setrw
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean after RO dance"
# unseal again so the image ends neutral; reads unaffected
$B/invf-sweep "$IMG" --unseal >/dev/null 2>&1
check_all "final"

echo
echo "== [4] image B: two blocks in the SAME stripe fail loudly =="
$B/invf-mkfs "$IMGB" 0.5 >/dev/null
python3 - <<'PY'
import os, random
random.seed(22)
d = "/dev/shm/wp20seal/origb"
os.makedirs(d, exist_ok=True)
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
text_file("t1.txt", 900_000)
text_file("t2.txt", 800_000)
text_file("t3.txt", 700_000)
fake_bin("b_x64", 62, 400_000)
fake_bin("b_a64", 183, 300_000)
print("corpus B:", *sorted(os.listdir(d)))
PY
for f in $(cd "$WORK/origb" && ls); do
    $B/invf-cp "$IMGB" "$WORK/origb/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGB" --seal > "$WORK/sealb.log" 2>&1 || { cat "$WORK/sealb.log"; exit 1; }
grep "\[seal\]" "$WORK/sealb.log"
read B1 B2 VICTIM <<< "$($SP "$IMGB" pair)"
S1=$(stripe_of $B1); S2=$(stripe_of $B2)
[ "$S1" = "$S2" ] || fail "pair not in one stripe"
echo "corrupting $B1 and $B2 (both in stripe $S1, file $VICTIM)"
corrupt_block "$IMGB" "$B1"
corrupt_block "$IMGB" "$B2"
rm -f "$WORK/out/should-not-exist"
set +e
$B/invf-cat "$IMGB" "$VICTIM" "$WORK/out/should-not-exist" > "$WORK/catb.log" 2>&1
CRC=$?
set -e
[ "$CRC" != 0 ] || fail "double-corrupted read succeeded!"
[ ! -s "$WORK/out/should-not-exist" ] || fail "partial garbage was written"
grep -q "CRC mismatch" "$WORK/catb.log" \
    || { cat "$WORK/catb.log"; fail "no loud CRC diagnostic"; }
echo "read failed loudly (rc=$CRC), no garbage written:"
grep "CRC mismatch" "$WORK/catb.log" | head -2
# an unaffected file (different batch, different stripe) still reads fine
read C1 CL <<< "$($SP "$IMGB" first b_a64)"
if [ "$(stripe_of $C1)" != "$S1" ]; then
    $B/invf-cat "$IMGB" b_a64 "$WORK/out/b_a64" >/dev/null
    cmp -s "$WORK/origb/b_a64" "$WORK/out/b_a64" \
        || fail "unaffected file corrupted"
    echo "unaffected file b_a64 (stripe $(stripe_of $C1)) still bit-exact"
else
    echo "note: control file shares the stripe; skipping the control read"
fi
# verify --deep reports the corrupt file AND the parity drift
set +e
$B/invf-verify "$IMGB" --deep > "$WORK/verifyb.log" 2>&1
VBRC=$?
set -e
tail -3 "$WORK/verifyb.log"
[ "$VBRC" != 0 ] || fail "verify --deep passed over the damage"
grep -q "CORRUPT: $VICTIM" "$WORK/verifyb.log" \
    || fail "verify did not name the corrupt file"
grep -q "parity: [0-9]* sealed stripes, [1-9][0-9]* mismatched" \
    "$WORK/verifyb.log" || fail "no parity mismatch reported"
# fsck stays structurally honest: blocks are still owned (not lost), the
# damage is content-level and verify's domain
$B/invf-fsck "$IMGB" | tee "$WORK/fsckb.log" | grep -E "orphans|missing|bad rec|OK|ISSUES"
grep -q "^OK$" "$WORK/fsckb.log" || fail "fsck misreported the structure"

echo
echo "SEAL E2E: PASS"
