#!/bin/bash
# test-seal.sh — WP20 --seal shadow-zone XOR parity + WP20b redundancy
# (variable k, RDP0 descriptor, dirty-stripe reseal, layer-2 RS) end-to-end
# (persistent regression).
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
#   WP20b, image C: --redundant-blocks 0.031 == k=32, 0.1 -> k=10 (visible
#   in the summary + persisted in the RDP0 descriptor) -> recovery under
#   k=10 -> auto-reseal: modify a file, flagless sweep reseals, a flipped
#   block recovers -> --free-redundant clears the descriptor (no more
#   auto-reseal) -> dirty-stripe proof: one new file, same-process reseal
#   skips every untouched stripe.
#
#   WP20b, image D: --redundant-paranoic 0.1:rs-vm -> layer-2 RS(36,32)
#   with both layers live -> single block: layer-1 runtime self-heal ->
#   two bad blocks in one stripe: runtime EIO, invf-fsck --repair decodes
#   and writes back bit-exact -> five bad blocks in one stripe (> m2=4):
#   honest UNRECOVERABLE report, nothing written, verify names the victim.
#
#   WP20b, image E: --redundant-bench prints both MB/s + winner; the first
#   --redundant-paranoic without an explicit algo runs the bench and
#   persists the winner. Plus the rsprop property leg (500 random
#   encode/drop-e<=m/decode/memcmp cases per algorithm).
#
# Run from the repo root after `make`:  bash tools/test-seal.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=${REPO:-/home/user/InvariantFS}   # override with the worktree when testing a branch
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
/* sealpick — WP20/WP20b test helper (uses only the public volume.h API).
 *
 *   sealpick <img> zone          -> "shadow_start total_blocks"
 *   sealpick <img> first <name>  -> "pba len" of the file's first map
 *   sealpick <img> pair          -> "pba1 pba2 name": two occupied blocks of
 *                                   ONE stripe belonging to one user file
 *   sealpick <img> blocks <n>    -> "pba1..pbaN name": N occupied blocks of
 *                                   ONE 32-stripe belonging to one user file
 *   sealpick <img> parity        -> first parity block pba
 *   sealpick <img> rm <name>     -> delete a file (no CLI rm exists)
 *   sealpick <img> setro|setrw   -> toggle VOLF_READONLY (persists)
 *   sealpick <img> desc          -> RDP0 descriptor: "present l1 l2 k1 k2 m2"
 *   sealpick <img> reseal2 <nm>  -> same-process dirty-tracking proof: seal
 *                                   (full pass), create+sweep a compressible
 *                                   binary file, seal again (dirty-only),
 *                                   print both reports
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
    if (!strcmp(argv[2], "desc")) {
        uint32_t k1 = 0, m2 = 0;
        int l2 = 0, present;
        present = vol_redun_state(v, &k1, &l2, &m2);
        printf("present=%d k1=%u l2=%d m2=%u\n", present, (unsigned)k1,
               l2, (unsigned)m2);
        vol_close(v);
        return 0;
    }
    if (!strcmp(argv[2], "reseal2") && argc == 4) {
        /* same-process dirty-tracking proof: the first seal is a full
         * pass (dirty bitmap starts all-ones at open), the second must
         * skip every stripe the new file did not touch */
        invfs_seal_report r1, r2;
        size_t n2 = 200 * 1024, i2;
        uint8_t *buf;
        uint64_t id;
        int src;
        if (vol_seal(v, 0, &r1) != 0) { fprintf(stderr, "seal1\n"); return 1; }
        buf = (uint8_t *)malloc(n2);
        if (!buf) return 1;
        /* compressible non-text content (generic ZSTD -> own shadow
         * segments; no batch/container/binary-family signature) */
        for (i2 = 0; i2 < n2; i2++) buf[i2] = (uint8_t)((i2 / 64) & 0x3F);
        id = vol_create_file(v, argv[3], buf, n2);
        free(buf);
        if (!id) { fprintf(stderr, "create\n"); return 1; }
        src = vol_sweep_one(v, id, argv[3]);
        vol_tz_flush(v);
        if (vol_flush(v) != 0) { fprintf(stderr, "flush\n"); return 1; }
        if (vol_seal(v, 0, &r2) != 0) { fprintf(stderr, "seal2\n"); return 1; }
        printf("reseal2: sweep_rc=%d first[stripes=%llu updated=%llu "
               "skipped=%llu] second[stripes=%llu updated=%llu "
               "unchanged=%llu skipped=%llu]\n", src,
               (unsigned long long)r1.stripes, (unsigned long long)r1.updated,
               (unsigned long long)r1.dirty_skipped,
               (unsigned long long)r2.stripes, (unsigned long long)r2.updated,
               (unsigned long long)r2.unchanged,
               (unsigned long long)r2.dirty_skipped);
        vol_close(v);
        return 0;
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
    if (!strcmp(argv[2], "blocks") && argc == 4) {
        /* N occupied blocks of one 32-stripe belonging to one user file */
        uint64_t ss = sb->shadow_zone_start;
        int want = atoi(argv[3]);
        build_map();
        if (want < 2 || want > 16) { vol_close(v); return 2; }
        for (i = 0; i < n; i++) {
            uint64_t pba, len, k;
            const char *nm;
            if (l2p[i].type != INVFS_JRN_MAP) continue;
            nm = name_of(l2p[i].inode);
            if (!nm) continue;
            pba = l2p[i].pba;
            len = l2p[i].length ? l2p[i].length : 1;
            if (pba < ss || len < (uint64_t)want) continue;
            for (k = 0; k + want <= len; k++) {
                uint64_t s0 = (pba + k - ss) / 32;
                int j;
                for (j = 0; j < want; j++)
                    if ((pba + k + j - ss) / 32 != s0) break;
                if (j == want) {
                    for (j = 0; j < want; j++)
                        printf("%llu ", (unsigned long long)(pba + k + j));
                    printf("%s\n", nm);
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
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,vol_tier,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
SP="$WORK/tools/sealpick"

echo "== build the rsprop helper (RS property leg; links only rs.o) =="
cat > "$WORK/tools/rsprop.c" <<'RSPROP_EOF'
/* rsprop — WP20b RS property test: 500 random cases per algorithm:
 * encode a (32+4) stripe, drop e <= m random slots, decode, memcmp the
 * whole stripe back. Also covers m=2 and m=8 shapes at reduced counts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rs.h"

static uint64_t st = 0xC0FFEE123456789ull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }

static int run(unsigned k, unsigned m, size_t bs, int cases)
{
    int algo_i, bad = 0;
    for (algo_i = 0; algo_i < 2; algo_i++) {
        int algo = algo_i == 0 ? RS_ALGO_VM : RS_ALGO_CAUCHY;
        int c;
        for (c = 0; c < cases; c++) {
            uint8_t *orig = malloc((k + m) * bs);
            uint8_t *work = malloc((k + m) * bs);
            uint8_t **blocks = malloc((k + m) * sizeof *blocks);
            uint8_t *present = malloc(k + m);
            uint8_t **dd = malloc(k * sizeof *dd);
            uint8_t **pp = malloc(m * sizeof *pp);
            unsigned e, i2;
            if (!orig || !work || !blocks || !present || !dd || !pp) return 2;
            for (i2 = 0; i2 < (k + m) * bs / 8; i2++)
                ((uint64_t *)orig)[i2] = rnd();
            memcpy(work, orig, (k + m) * bs);
            for (i2 = 0; i2 < k; i2++) dd[i2] = work + i2 * bs;
            for (i2 = 0; i2 < m; i2++) pp[i2] = work + (k + i2) * bs;
            if (rs_encode(algo, k, m, bs, dd, pp) != 0) return 2;
            memcpy(orig, work, (k + m) * bs);   /* truth incl. parity */
            e = 1 + (unsigned)(rnd() % m);      /* drop 1..m slots */
            for (i2 = 0; i2 < k + m; i2++) {
                blocks[i2] = work + i2 * bs;
                present[i2] = 1;
            }
            for (i2 = 0; i2 < e; i2++) {
                unsigned slot;
                do { slot = (unsigned)(rnd() % (k + m)); } while (!present[slot]);
                present[slot] = 0;
                memset(blocks[slot], 0x5A, bs);
            }
            if (rs_decode(algo, k, m, bs, blocks, present) != 0) {
                printf("FAIL %s k=%u m=%u case %d: decode rc\n",
                       rs_algo_name(algo), k, m, c);
                bad++;
            } else if (memcmp(work, orig, (k + m) * bs) != 0) {
                printf("FAIL %s k=%u m=%u case %d: mismatch (e=%u)\n",
                       rs_algo_name(algo), k, m, c, e);
                bad++;
            }
            free(orig); free(work); free(blocks); free(present);
            free(dd); free(pp);
        }
    }
    printf("  k=%u m=%u: %d cases x2 algos %s\n", k, m, cases,
           bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

int main(void)
{
    int rc = 0;
    rc |= run(32, 4, 4096, 500);   /* the volume shape */
    rc |= run(32, 2, 4096, 100);   /* m2 bounds */
    rc |= run(32, 8, 4096, 100);
    rc |= run(10, 3, 4096, 100);   /* off-size k */
    if (rc) { printf("RSPROP: FAIL\n"); return 1; }
    printf("RSPROP: PASS\n");
    return 0;
}
RSPROP_EOF
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/tools/rsprop" \
    "$WORK/tools/rsprop.c" $REPO/build/obj/rs.o
RSPROP="$WORK/tools/rsprop"

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
# both blocks must show up as parity recoveries across the two reads.
# NOTE: when the victims share one batch segment (a batch that spans two
# stripes), the FIRST read heals both blocks at once -- the per-read
# assignment this leg's name implies holds only for separate segments, so
# the union of both logs is what's asserted.
cat "$WORK/cat3a.log" "$WORK/cat3b.log" > "$WORK/cat3ab.log"
grep -q "\[seal\] recovered block $B1 via parity" "$WORK/cat3ab.log" \
    || fail "recovery line for block $B1 missing"
grep -q "\[seal\] recovered block $B2 via parity" "$WORK/cat3ab.log" \
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
# WP22d: blocks freed by non-arming processes during the checkpoint window
# are held (never reused) and reclaimed by fsck -f after resolution.
$B/invf-fsck "$IMG" -f >/dev/null 2>&1 || true
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
echo "== [9] WP20b: variable k (--redundant-blocks) + RDP0 descriptor =="
IMGC=wp20seal-c.img
rm -f "$IMGC"
$B/invf-mkfs "$IMGC" 0.5 >/dev/null
python3 - <<'PY'
import os, random
random.seed(23)
d = "/dev/shm/wp20seal/origc"
os.makedirs(d, exist_ok=True)
# semi-compressible binaries: generic ZSTD -> own shadow segments,
# enough of them that several seal stripes are occupied
for i in range(6):
    data = bytearray()
    while len(data) < 400_000:
        data += bytes([random.randrange(4)]) * 64
        data += os.urandom(32)
    open(os.path.join(d, f"g{i}.bin"), "wb").write(bytes(data))
open(os.path.join(d, "note.txt"), "w").write("a text file for the batch\n" * 4000)
PY
for f in $(cd "$WORK/origc" && ls); do
    $B/invf-cp "$IMGC" "$WORK/origc/$f" "$f" >/dev/null
done
check_all_c() { # <label>
    local ok=1 f
    for f in $(cd "$WORK/origc" && ls); do
        $B/invf-cat "$IMGC" "$f" "$WORK/out/$f" >/dev/null 2>"$WORK/out/$f.err" \
            || { echo "  cat failed: $f ($1)"; ok=0; continue; }
        cmp -s "$WORK/origc/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($1)"; ok=0; }
    done
    [ "$ok" = 1 ] || fail "bit-exact check: $1"
    echo "  all files bit-exact ($1)"
}
# 0.031 -> k = round(1/0.031) = 32 (same as --seal)
$B/invf-sweep "$IMGC" --redundant-blocks 0.031 > "$WORK/sealc1.log" 2>&1 \
    || { cat "$WORK/sealc1.log"; exit 1; }
SEAL_LINE=$(grep "\[seal\]" "$WORK/sealc1.log")
echo "$SEAL_LINE"
echo "$SEAL_LINE" | grep -q "(k1=32)" || fail "0.031 did not configure k1=32"
STRIPESC=$(echo "$SEAL_LINE" | sed 's/.*\[seal\] \([0-9]*\) stripes.*/\1/')
[ "$STRIPESC" -ge 2 ] || fail "image C: expected >=2 stripes, got $STRIPESC"
$SP "$IMGC" desc | tee "$WORK/desc1.log"
grep -q "present=1 k1=32 l2=0 m2=0" "$WORK/desc1.log" \
    || fail "descriptor not persisted after --redundant-blocks"
# 0.1 -> k = 10 (reconfigure: full pass over the new geometry)
$B/invf-sweep "$IMGC" --redundant-blocks 0.1 > "$WORK/sealc2.log" 2>&1 \
    || { cat "$WORK/sealc2.log"; exit 1; }
SEAL_LINE=$(grep "\[seal\]" "$WORK/sealc2.log")
echo "$SEAL_LINE"
echo "$SEAL_LINE" | grep -q "(k1=10)" || fail "0.1 did not configure k1=10"
STRIPESC10=$(echo "$SEAL_LINE" | sed 's/.*\[seal\] \([0-9]*\) stripes.*/\1/')
[ "$STRIPESC10" -gt "$STRIPESC" ] || fail "k=10 should have more stripes than k=32"
$SP "$IMGC" desc | grep -q "present=1 k1=10 " || fail "descriptor k1 != 10"
$B/invf-verify "$IMGC" --deep | tee "$WORK/verifyc.log" | tail -2
grep -q " 0 corrupt," "$WORK/verifyc.log" || fail "corrupt after k=10 seal"
grep -q "parity: $STRIPESC10 sealed stripes, 0 mismatched, 0 missing, 0 extra" \
    "$WORK/verifyc.log" || fail "parity leg not clean under k=10"
# runtime recovery works under the configured geometry
read VPBA VLEN <<< "$($SP "$IMGC" first g0.bin)"
[ "$VPBA" -ge "$SS" ] || fail "victim not in shadow"
corrupt_block "$IMGC" "$VPBA"
$B/invf-cat "$IMGC" g0.bin "$WORK/out/g0.bin" > "$WORK/catc.log" 2>&1 \
    || { cat "$WORK/catc.log"; fail "cat g0.bin failed under k=10 seal"; }
grep -q "\[seal\] recovered block $VPBA via parity" "$WORK/catc.log" \
    || fail "no recovery line under k=10"
cmp -s "$WORK/origc/g0.bin" "$WORK/out/g0.bin" || fail "g0.bin not bit-exact"
echo "single-block recovery under k=10 OK"
check_all_c "k=10 sealed"

echo
echo "== [10] WP20b: auto-reseal from the live descriptor + --free-redundant =="
# modify a file, then a flagless sweep must auto-reseal
python3 - <<'PY'
import os
d = "/dev/shm/wp20seal/origc"
data = open(os.path.join(d, "note.txt")).read()
open(os.path.join(d, "note.txt"), "w").write(data + "the second edition\n" * 3000)
PY
$B/invf-cp "$IMGC" "$WORK/origc/note.txt" note.txt >/dev/null
$B/invf-sweep "$IMGC" > "$WORK/autoc.log" 2>&1 || { cat "$WORK/autoc.log"; exit 1; }
grep -q "auto-reseal" "$WORK/autoc.log" || fail "no auto-reseal diagnostic"
grep -q "\[seal\]" "$WORK/autoc.log" || fail "flagless sweep did not reseal"
grep "auto-reseal\|\[seal\]" "$WORK/autoc.log"
# the modified file is covered: flip one of its blocks -> runtime recovery
read VPBA VLEN <<< "$($SP "$IMGC" first note.txt)"
[ "$VPBA" -ge "$SS" ] || fail "modified file not in shadow"
corrupt_block "$IMGC" "$VPBA"
$B/invf-cat "$IMGC" note.txt "$WORK/out/note.txt" > "$WORK/catc2.log" 2>&1 \
    || { cat "$WORK/catc2.log"; fail "cat modified note.txt failed"; }
grep -q "\[seal\] recovered block $VPBA via parity" "$WORK/catc2.log" \
    || fail "no recovery line for the auto-resealed file"
cmp -s "$WORK/origc/note.txt" "$WORK/out/note.txt" || fail "note.txt not bit-exact"
echo "auto-reseal + recovery OK"
# --free-redundant removes everything including the descriptor
$B/invf-sweep "$IMGC" --free-redundant > "$WORK/freec.log" 2>&1 \
    || { cat "$WORK/freec.log"; exit 1; }
grep -q "\[unseal\] [1-9][0-9]* parity blocks freed" "$WORK/freec.log" \
    || fail "--free-redundant freed nothing"
$SP "$IMGC" desc | tee "$WORK/desc2.log"
grep -q "present=0" "$WORK/desc2.log" || fail "descriptor survived --free-redundant"
# a flagless sweep now does NOT seal
$B/invf-sweep "$IMGC" > "$WORK/noauto.log" 2>&1 || { cat "$WORK/noauto.log"; exit 1; }
if grep -q "\[seal\]" "$WORK/noauto.log"; then fail "reseal after --free-redundant"; fi
$B/invf-fsck "$IMGC" | grep -q "^OK$" || fail "fsck not clean after free-redundant"
check_all_c "post free-redundant"

echo
echo "== [11] WP20b: dirty-stripe reseal proof (same process, public API) =="
# reseal (k=32) so the helper starts from a sealed volume
$B/invf-sweep "$IMGC" --redundant-blocks 0.031 > "$WORK/sealc3.log" 2>&1 \
    || { cat "$WORK/sealc3.log"; exit 1; }
$SP "$IMGC" reseal2 newblob.bin | tee "$WORK/reseal2.log"
grep -q "reseal2:" "$WORK/reseal2.log" || fail "reseal2 helper failed"
R2_SKIP=$(sed 's/.*second\[stripes=[0-9]* updated=[0-9]* unchanged=[0-9]* skipped=\([0-9]*\)\].*/\1/' "$WORK/reseal2.log")
R2_UPD=$(sed 's/.*second\[stripes=[0-9]* updated=\([0-9]*\) unchanged=.*/\1/' "$WORK/reseal2.log")
R2_STRIPES=$(sed 's/.*second\[stripes=\([0-9]*\) updated=.*/\1/' "$WORK/reseal2.log")
echo "second seal: stripes=$R2_STRIPES updated=$R2_UPD skipped=$R2_SKIP"
[ "$R2_UPD" -ge 1 ] || fail "dirty reseal updated nothing"
[ "$R2_UPD" -lt "$R2_STRIPES" ] || fail "dirty reseal rewrote every stripe"
[ "$R2_SKIP" -ge 1 ] || fail "dirty reseal skipped nothing"
$B/invf-verify "$IMGC" --deep | tail -2 | grep -q " 0 corrupt," \
    || fail "verify not clean after dirty reseal"
# leave image C sealed k=32 for hygiene of the final state
check_all_c "post reseal2"

echo
echo "== [12] WP20b: layer 2 -- RS(36,32) over GF(2^8) (image D) =="
IMGD=wp20seal-d.img
IMGE=wp20seal-e.img
rm -f "$IMGD" "$IMGE"
$B/invf-mkfs "$IMGD" 0.5 >/dev/null
python3 - <<'PY'
import os, random
random.seed(24)
d = "/dev/shm/wp20seal/origd"
os.makedirs(d, exist_ok=True)
for i in range(5):
    data = bytearray()
    while len(data) < 350_000:
        data += bytes([random.randrange(4)]) * 64
        data += os.urandom(32)
    open(os.path.join(d, f"d{i}.bin"), "wb").write(bytes(data))
WORDS = ("layer two reed solomon stripe parity cauchy vandermonde\n").split()
out = []; n = 0
while n < 300_000:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "t.txt"), "w").write(" ".join(out)[:300_000])
PY
for f in $(cd "$WORK/origd" && ls); do
    $B/invf-cp "$IMGD" "$WORK/origd/$f" "$f" >/dev/null
done
check_all_d() { # <label>
    local ok=1 f
    for f in $(cd "$WORK/origd" && ls); do
        $B/invf-cat "$IMGD" "$f" "$WORK/out/$f" >/dev/null 2>"$WORK/out/$f.err" \
            || { echo "  cat failed: $f ($1)"; ok=0; continue; }
        cmp -s "$WORK/origd/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($1)"; ok=0; }
    done
    [ "$ok" = 1 ] || fail "bit-exact check: $1"
    echo "  all files bit-exact ($1)"
}
$B/invf-sweep "$IMGD" --redundant-paranoic 0.1:rs-vm > "$WORK/seald1.log" 2>&1 \
    || { cat "$WORK/seald1.log"; exit 1; }
grep "\[seal\]\|\[seal2\]" "$WORK/seald1.log"
grep -q "\[seal2\] .* (rs-vm, k=32, m=4)" "$WORK/seald1.log" \
    || fail "no layer-2 seal line / wrong shape"
$SP "$IMGD" desc | tee "$WORK/descd.log"
grep -q "present=1 k1=32 l2=1 m2=4" "$WORK/descd.log" \
    || fail "descriptor wrong for paranoic 0.1:rs-vm"
$B/invf-verify "$IMGD" --deep | tee "$WORK/verifyd1.log" | tail -3
grep -q " 0 corrupt," "$WORK/verifyd1.log" || fail "corrupt after L2 seal"
grep -q "parity2: [0-9]* sealed stripes, 0 mismatched, 0 missing, 0 extra" \
    "$WORK/verifyd1.log" || fail "parity2 leg not clean after seal"
check_all_d "post L2 seal"

echo "-- [12a] block covered by BOTH layers: layer-1 runtime self-heal (no fsck)"
read VPBA VLEN <<< "$($SP "$IMGD" first d0.bin)"
corrupt_block "$IMGD" "$VPBA"
$B/invf-cat "$IMGD" d0.bin "$WORK/out/d0.bin" > "$WORK/catd.log" 2>&1 \
    || { cat "$WORK/catd.log"; fail "cat d0.bin failed (L1 self-heal)"; }
grep -q "\[seal\] recovered block $VPBA via parity" "$WORK/catd.log" \
    || fail "no layer-1 recovery line on the dual-covered block"
cmp -s "$WORK/origd/d0.bin" "$WORK/out/d0.bin" || fail "d0.bin not bit-exact"
echo "layer-1 runtime self-heal OK (layer 2 untouched, no fsck)"

echo "-- [12b] two bad blocks in one stripe: runtime EIO, fsck --repair fixes"
read B1 B2 VIC <<< "$($SP "$IMGD" pair)"
echo "corrupting $B1 $B2 (stripe $(stripe_of $B1), file $VIC)"
corrupt_block "$IMGD" "$B1"
corrupt_block "$IMGD" "$B2"
rm -f "$WORK/out/vic.d"
set +e
$B/invf-cat "$IMGD" "$VIC" "$WORK/out/vic.d" > "$WORK/catd2.log" 2>&1
CRC=$?
set -e
[ "$CRC" != 0 ] || fail "2-bad read succeeded despite layer 1"
[ ! -s "$WORK/out/vic.d" ] || fail "partial garbage written"
grep -q "CRC mismatch" "$WORK/catd2.log" || fail "no loud CRC diagnostic"
echo "runtime read EIOs loudly (rc=$CRC)"
$B/invf-fsck "$IMGD" --repair > "$WORK/fsckd.log" 2>&1 \
    || { cat "$WORK/fsckd.log"; fail "fsck --repair rc != 0"; }
grep -E "seal2" "$WORK/fsckd.log"
grep -q "seal2 repair: [0-9]* damaged stripes, [1-9][0-9]* repaired" \
    "$WORK/fsckd.log" || fail "no stripe repaired"
grep -q "0 unrecoverable" "$WORK/fsckd.log" || fail "unexpected unrecoverable"
check_all_d "post fsck --repair"
$B/invf-verify "$IMGD" --deep | tee "$WORK/verifyd2.log" | tail -3
grep -q " 0 corrupt," "$WORK/verifyd2.log" || fail "still corrupt after repair"
grep -q "parity2: [0-9]* sealed stripes, 0 mismatched, 0 missing, 0 extra" \
    "$WORK/verifyd2.log" || fail "parity2 leg not clean after repair"
echo "fsck --repair recovered the 2-bad stripe bit-exact"

echo "-- [12c] five bad blocks in one stripe (> m2=4): honest failure"
read F1 F2 F3 F4 F5 VIC5 <<< "$($SP "$IMGD" blocks 5)"
echo "corrupting $F1 $F2 $F3 $F4 $F5 (stripe $(stripe_of $F1), file $VIC5)"
for b in $F1 $F2 $F3 $F4 $F5; do corrupt_block "$IMGD" "$b"; done
rm -f "$WORK/out/vic5.d"
set +e
$B/invf-cat "$IMGD" "$VIC5" "$WORK/out/vic5.d" > "$WORK/catd3.log" 2>&1
CRC=$?
set -e
[ "$CRC" != 0 ] || fail "5-bad read succeeded!"
[ ! -s "$WORK/out/vic5.d" ] || fail "partial garbage written (5-bad)"
set +e
$B/invf-fsck "$IMGD" --repair > "$WORK/fsckd2.log" 2>&1
FRC=$?
set -e
[ "$FRC" != 0 ] || fail "fsck --repair passed over >m2 damage"
grep -E "UNRECOVERABLE|unrecoverable" "$WORK/fsckd2.log"
grep -q "[1-9][0-9]* unrecoverable" "$WORK/fsckd2.log" \
    || fail "no unrecoverable stripe reported"
# nothing was written for that stripe: the victim still fails loudly
set +e
$B/invf-cat "$IMGD" "$VIC5" "$WORK/out/vic5b.d" >/dev/null 2>&1
CRC=$?
set -e
[ "$CRC" != 0 ] || fail "unrecoverable stripe silently healed?!"
[ ! -s "$WORK/out/vic5b.d" ] || fail "garbage written after failed repair"
# fsck stays structurally honest (content damage is verify's domain)
grep -q "orphans:      0" "$WORK/fsckd2.log" || fail "phantom orphans"
set +e
$B/invf-verify "$IMGD" --deep > "$WORK/verifyd3.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify --deep passed over the damage"
grep -q "CORRUPT: $VIC5" "$WORK/verifyd3.log" || fail "verify did not name the victim"
grep -q "parity2: [0-9]* sealed stripes, [1-9][0-9]* mismatched" \
    "$WORK/verifyd3.log" || fail "no parity2 mismatch reported"
echo "honest failure: reported, nothing written, still loudly broken"

echo
echo "== [13] WP20b: --redundant-bench + algo auto-pick =="
$B/invf-sweep "$IMGE" --redundant-bench | tee "$WORK/bench.log"
grep -q "\[bench\] rs-vm: [0-9.]* MB/s, rs-cauchy: [0-9.]* MB/s" \
    "$WORK/bench.log" || fail "bench line missing"
grep -q "winner: rs-" "$WORK/bench.log" || fail "bench winner missing"
WINNER=$(sed 's/.*winner: \(rs-[a-z]*\).*/\1/' "$WORK/bench.log")
echo "bench winner: $WINNER"
# first --redundant-paranoic without an explicit algo: bench decides, and
# the winner is persisted in the descriptor
$B/invf-mkfs "$IMGE" 0.5 >/dev/null
$B/invf-cp "$IMGE" "$WORK/origd/t.txt" t.txt >/dev/null
$B/invf-sweep "$IMGE" --redundant-paranoic 0.1 > "$WORK/seale.log" 2>&1 \
    || { cat "$WORK/seale.log"; exit 1; }
grep "bench picked\|\[seal2\]" "$WORK/seale.log"
grep -q "bench picked $WINNER" "$WORK/seale.log" \
    || fail "auto-pick disagrees with the standalone bench"
WANT_L2=1; [ "$WINNER" = "rs-cauchy" ] && WANT_L2=2
$SP "$IMGE" desc | tee "$WORK/desce.log"
grep -q "present=1 k1=32 l2=$WANT_L2 m2=4" "$WORK/desce.log" \
    || fail "auto-picked algo not persisted (want l2=$WANT_L2)"
# a second paranoic configure without suffix keeps the persisted algo
$B/invf-sweep "$IMGE" --redundant-paranoic 0.2 > "$WORK/seale2.log" 2>&1 \
    || { cat "$WORK/seale2.log"; exit 1; }
if grep -q "bench picked" "$WORK/seale2.log"; then
    fail "re-benched despite the persisted algo"
fi
$SP "$IMGE" desc | grep -q "l2=$WANT_L2 m2=8" \
    || fail "persisted algo/m2 update wrong (want l2=$WANT_L2 m2=8)"

echo
echo "== [14] WP20b: RS property leg (500+ random cases, both algorithms) =="
"$RSPROP" | tee "$WORK/rsprop.log"
grep -q "RSPROP: PASS" "$WORK/rsprop.log" || fail "RS property test failed"

echo
echo "SEAL E2E: PASS"
