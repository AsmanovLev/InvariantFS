#!/bin/bash
# test-seal.sh — WP201 native v3 seal (par2-inspired, no compat) end-to-end.
#
#   image A: mkfs -> mixed corpus (texts + empty file + incompressible
#   binary + subdir file + symlink) -> sweep -> --seal (default pct 10):
#   [seal] line sane (k=9,m=1), fsck clean, seal files hidden from listings,
#   every file bit-exact, verify --deep clean with a 0-drift parity leg ->
#   re-seal (gen+1, full recompute) -> write-more WITHOUT sweeping:
#   verify --deep FAILS with the stale report (gen + uncovered bytes +
#   nonzero drift counters) -> sweep (auto-reseal heals) -> verify clean ->
#   delete a file: stale again (missing) -> --unseal (parity gone, verify
#   silent, reads unaffected, fsck clean) -> re-seal at --seal 25 (k=6,m=2).
#
#   image B (menu): --seal 5 / 20 legs print their (k,m); bad pct refused.
#
#   image C (crash): INVFS_FAULT=vol_seal_crash:1 between the parity commit
#   and the footer commit -> seal fails; volume opens; verify reports
#   absent-or-prior (never half-trusted); fsck sane; a clean re-seal heals.
#
#   image D (dry-run): --dry-run --seal prints the plan (groups, parity
#   bytes, overhead) and changes nothing (verify still shows the old seal).
#
#   image E (WP403 stale-mode): seal -> write-more (stale) -> bare
#   --dry-run annotates the plan per mode (auto would reseal / notify
#   would report / off no action) -> --stale-mode=notify reports STALE
#   (gen + uncovered bytes), exits 0, parity bytes identical, verify
#   still STALE -> --stale-mode=off prints one line, no reseal ->
#   explicit --seal under notify still reseals (explicit wins) ->
#   bad --stale-mode refused.
#
#   image F (WP403 unsealed silence): --stale-mode=notify/off on an
#   unsealed volume prints no mode output at all.
#
# Run from the repo root after `make`:  bash tools/test-seal.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# the format-aware "volume is clean" gate (v3 has no L2P orphans counter)
. "$REPO/tools/fsck-clean.sh"

B=$REPO/bin
WORK=/dev/shm/wp201seal
IMG=wp201seal.img        # image A: the main seal/stale/unseal line
IMGB=wp201seal-b.img     # image B: menu geometry legs
IMGC=wp201seal-c.img     # image C: crash-mid-seal probe
IMGD=wp201seal-d.img     # image D: dry-run plan
IMGS=wp201seal-s.img     # image S: WP401 scrub legs (fresh; see [S])
IMGE=wp201seal-e.img     # image E: WP403 stale-mode legs
IMGF=wp201seal-f.img     # image F: WP403 unsealed silence
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGB" "$IMGC" "$IMGD" "$IMGS" "$IMGE" "$IMGF"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== build the rm helper (public API only; no CLI rm exists) =="
mkdir -p "$WORK/tools"
cat > "$WORK/tools/sealrm.c" <<'SEALRM_EOF'
/* sealrm — WP201 test helper: sealrm <img> <name> deletes one file. */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc;
    invfs_volume *v;
    if (argc != 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    rc = vol_delete_file(v, argv[2]);
    if (rc == 0) rc = vol_flush(v);
    vol_close(v);
    return rc ? 1 : 0;
}
SEALRM_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -o "$WORK/tools/sealrm" \
    "$WORK/tools/sealrm.c" \
    $(for t in volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone vol_heat vol_sweep vol_read vol_write vol_records vol_ast vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro deflate_backend_system deflate_backend_stock; do printf "$REPO/build/obj/$t.o "; done) \
    $REPO/build/obj/zlib_stock_*.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
RM="$WORK/tools/sealrm"

# bit-exact check of every REGULAR corpus file against the host original
# (symlinks compare by target, asserted separately; dirs have no bytes).
check_all() { # <img> <origdir> <label>
    local img=$1 orig=$2 label=$3 ok=1 f
    for f in a.c notes.txt big.log empty.txt rand.bin added.txt; do
        [ -f "$WORK/$orig/$f" ] || continue
        $B/invf-cat "$img" "$f" "$WORK/out/$f" >/dev/null 2>"$WORK/out/$f.err" \
            || { echo "  cat failed: $f ($label)"; ok=0; continue; }
        cmp -s "$WORK/$orig/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($label)"; ok=0; }
    done
    # subdir member
    $B/invf-cat "$img" sub/deep.txt "$WORK/out/deep.txt" >/dev/null 2>&1 \
        || { echo "  cat failed: sub/deep.txt ($label)"; ok=0; }
    cmp -s "$WORK/$orig/sub/deep.txt" "$WORK/out/deep.txt" \
        || { echo "  MISMATCH: sub/deep.txt ($label)"; ok=0; }
    [ "$ok" = 1 ] || fail "bit-exact check: $label"
    echo "  all files bit-exact ($label)"
}

echo "== mkfs + corpus =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null

python3 - <<'PY'
import os, random
random.seed(201)
d = "/dev/shm/wp201seal/orig"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

text_file("a.c", 120_000)
text_file("notes.txt", 45_000)
text_file("big.log", 500_000)
open(os.path.join(d, "empty.txt"), "w").write("")
open(os.path.join(d, "rand.bin"), "wb").write(random.randbytes(200_000))
os.makedirs(os.path.join(d, "sub"), exist_ok=True)
text_file("sub/deep.txt", 30_000)
os.symlink("a.c", os.path.join(d, "link-to-a"))
print("corpus:", *sorted(os.listdir(d)))
PY

for f in a.c notes.txt big.log empty.txt rand.bin; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-cp "$IMG" "$WORK/orig/sub/deep.txt" sub/deep.txt >/dev/null
$B/invf-cp "$IMG" "$WORK/orig/link-to-a" link-to-a >/dev/null 2>&1 \
    || echo "  (note: invf-cp of a symlink unsupported here; continuing without it)"

echo
echo "== [1] sweep + --seal (default pct 10 -> k=9,m=1) =="
$B/invf-sweep "$IMG" >/dev/null 2>&1 || fail "baseline sweep failed"
$B/invf-sweep "$IMG" --seal > "$WORK/seal1.log" 2>&1 || { cat "$WORK/seal1.log"; exit 1; }
SEAL_LINE=$(grep "\[seal\]" "$WORK/seal1.log")
echo "$SEAL_LINE"
echo "$SEAL_LINE" | grep -q "(full recompute, k=9, m=1, rs-vm)" \
    || fail "default seal is not (k=9,m=1,rs-vm)"
NSEAL_GROUPS=$(echo "$SEAL_LINE" | sed 's/.*\[seal\] \([0-9]*\) groups.*/\1/')
[ "$NSEAL_GROUPS" -ge 1 ] || fail "expected >=1 group, got $NSEAL_GROUPS"
grep -q "sealed-at-gen-1 verified clean" "$WORK/seal1.log" \
    || fail "no verify-after-write proof in the seal log"
# the seal files are ordinary hidden files: invisible to listings (skip the
# header line, whose image path legitimately contains "seal") ...
if $B/invf-ls "$IMG" 2>/dev/null | tail -n +2 | grep -q "seal"; then
    fail "seal files leaked into the directory listing"
fi
echo "  seal files hidden from listings"
$B/invf-fsck "$IMG" > "$WORK/fsck1.log" 2>&1 || { cat "$WORK/fsck1.log"; exit 1; }
grep -q "^OK$" "$WORK/fsck1.log" || fail "fsck not clean after seal"
check_all "$IMG" orig "post-seal baseline"
# symlink target is covered content: it must read back on a sealed volume
$B/invf-cat "$IMG" link-to-a "$WORK/out/link-to-a" >/dev/null 2>&1 \
    || fail "cat link-to-a failed on the sealed volume"
echo "  symlink target reads back ($(wc -c < "$WORK/out/link-to-a") bytes)"

echo
echo "== [2] verify --deep on the sealed volume =="
$B/invf-verify "$IMG" --deep > "$WORK/verify1.log" 2>&1 || { cat "$WORK/verify1.log"; exit 1; }
tail -n 2 "$WORK/verify1.log"
grep -q "parity: $NSEAL_GROUPS sealed stripes, 0 mismatched, 0 missing, 0 extra" \
    "$WORK/verify1.log" || fail "parity leg not clean after seal"
grep -q " 0 corrupt," "$WORK/verify1.log" || fail "corrupt files after seal"

echo
echo "== [3] re-seal: next generation, full recompute =="
$B/invf-sweep "$IMG" --seal > "$WORK/seal2.log" 2>&1 || { cat "$WORK/seal2.log"; exit 1; }
grep "\[seal\]" "$WORK/seal2.log"
grep -q "sealed-at-gen-2 verified clean" "$WORK/seal2.log" \
    || fail "re-seal did not advance to gen-2"
check_all "$IMG" orig "post re-seal"

echo
echo "== [4] write-more (no sweep): verify goes STALE, loudly =="
echo "brand new bytes" > "$WORK/orig/added.txt"
$B/invf-cp "$IMG" "$WORK/orig/added.txt" added.txt >/dev/null
set +e
$B/invf-verify "$IMG" --deep > "$WORK/verify-stale.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify --deep passed over a post-seal write"
grep -q "^parity: .* [1-9][0-9]* mismatched" "$WORK/verify-stale.log" \
    || { cat "$WORK/verify-stale.log"; fail "no mismatched groups reported"; }
grep -q " 1 extra" "$WORK/verify-stale.log" \
    || fail "the added file was not reported as extra"
grep -q "STALE: .* uncovered bytes" "$WORK/verify-stale.log" \
    || fail "no STALE uncovered-bytes report"
grep -q "sealed-at-gen-2" "$WORK/verify-stale.log" \
    || fail "stale report does not name the generation"
echo "  stale, loudly (rc=$VRC):"
grep -E "^parity:|STALE" "$WORK/verify-stale.log" | head -n 3
# content itself is fine — only the seal is stale (detect, not damage)
grep -q " 0 corrupt," "$WORK/verify-stale.log" \
    || fail "content wrongly flagged corrupt (stale != corrupt)"

echo
echo "== [5] sweep auto-reseals the live seal; verify clean again =="
$B/invf-sweep "$IMG" > "$WORK/autoseal.log" 2>&1 || { cat "$WORK/autoseal.log"; exit 1; }
grep -q "auto-reseal" "$WORK/autoseal.log" || fail "no auto-reseal diagnostic"
grep "auto-reseal\|\[seal\]" "$WORK/autoseal.log" | head -n 3
$B/invf-verify "$IMG" --deep > "$WORK/verify2.log" 2>&1 || { cat "$WORK/verify2.log"; exit 1; }
grep -q " 0 corrupt," "$WORK/verify2.log" || fail "corrupt after auto-reseal"
grep -q " 0 mismatched, 0 missing, 0 extra" "$WORK/verify2.log" \
    || fail "parity leg not clean after auto-reseal"
check_all "$IMG" orig "post auto-reseal"

echo
echo "== [6] delete a file: stale again (missing), then --unseal =="
$RM "$IMG" notes.txt || fail "sealrm notes.txt failed"
rm -f "$WORK/orig/notes.txt"
set +e
$B/invf-verify "$IMG" --deep > "$WORK/verify-missing.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify --deep passed over a post-seal delete"
grep -q " 1 missing" "$WORK/verify-missing.log" \
    || { cat "$WORK/verify-missing.log"; fail "deleted file not reported missing"; }
grep -q "STALE: .* uncovered bytes" "$WORK/verify-missing.log" \
    || fail "no STALE report for the delete"
echo "  delete stale, loudly (rc=$VRC):"
grep -E "^parity:|STALE" "$WORK/verify-missing.log" | head -n 3
$B/invf-sweep "$IMG" --unseal > "$WORK/unseal.log" 2>&1 || { cat "$WORK/unseal.log"; exit 1; }
grep "\[unseal\]" "$WORK/unseal.log"
grep -q "\[unseal\] [1-9][0-9]* parity blocks freed" "$WORK/unseal.log" \
    || fail "unseal freed nothing"
$B/invf-fsck "$IMG" > "$WORK/fsck6.log" 2>&1 || { cat "$WORK/fsck6.log"; exit 1; }
grep -q "^OK$" "$WORK/fsck6.log" || fail "fsck not clean after unseal"
# the parity leg falls silent on an unsealed volume
$B/invf-verify "$IMG" --deep > "$WORK/verify3.log" 2>&1 || { cat "$WORK/verify3.log"; exit 1; }
if grep -q "^parity:" "$WORK/verify3.log"; then
    fail "parity leg still reporting after unseal"
fi
echo "  verify silent on unsealed volume"
check_all "$IMG" orig "post-unseal"

echo
echo "== [7] re-seal at --seal 25 (k=6,m=2) =="
$B/invf-sweep "$IMG" --seal 25 > "$WORK/seal25.log" 2>&1 || { cat "$WORK/seal25.log"; exit 1; }
grep "\[seal\]" "$WORK/seal25.log"
grep -q "(full recompute, k=6, m=2, rs-vm)" "$WORK/seal25.log" \
    || fail "25 did not configure (k=6,m=2)"
$B/invf-verify "$IMG" --deep > "$WORK/verify25.log" 2>&1 || { cat "$WORK/verify25.log"; exit 1; }
grep -q " 0 mismatched, 0 missing, 0 extra" "$WORK/verify25.log" \
    || fail "parity leg not clean at pct 25"
check_all "$IMG" orig "pct 25 sealed"

echo
echo "== [B] menu geometry legs (fresh image) =="
$B/invf-mkfs "$IMGB" 0.5 >/dev/null
$B/invf-cp "$IMGB" "$WORK/orig/a.c" a.c >/dev/null
$B/invf-sweep "$IMGB" --seal 5 > "$WORK/sealb5.log" 2>&1 || { cat "$WORK/sealb5.log"; exit 1; }
grep -q "(full recompute, k=20, m=1, rs-vm)" "$WORK/sealb5.log" \
    || fail "5 did not configure (k=20,m=1)"
$B/invf-sweep "$IMGB" --seal 20 > "$WORK/sealb20.log" 2>&1 || { cat "$WORK/sealb20.log"; exit 1; }
grep -q "(full recompute, k=8, m=2, rs-vm)" "$WORK/sealb20.log" \
    || fail "20 did not configure (k=8,m=2)"
$B/invf-verify "$IMGB" --deep 2>&1 | grep -q " 0 mismatched" \
    || fail "menu image parity leg not clean"
rc=0; $B/invf-sweep "$IMGB" --seal 7 > "$WORK/sealbad.log" 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "bad pct 7 accepted"
grep -q "want 5|10|20|25" "$WORK/sealbad.log" || fail "no menu diagnostic"
echo "  menu 5/20 map to their pairs; 7 refused with a diagnostic"
# v1 maps the old spellings onto the menu (documented, not silent)
$B/invf-sweep "$IMGB" --redundant-blocks 0.031 > "$WORK/sealrb.log" 2>&1 \
    || { cat "$WORK/sealrb.log"; exit 1; }
grep -q "(full recompute, k=20, m=1" "$WORK/sealrb.log" \
    || fail "--redundant-blocks 0.031 did not snap to (20,1)"
$B/invf-sweep "$IMGB" --redundant-paranoic 0.2:rs-cauchy > "$WORK/sealrp.log" 2>&1 \
    || { cat "$WORK/sealrp.log"; exit 1; }
grep -q "(full recompute, k=8, m=2, rs-cauchy)" "$WORK/sealrp.log" \
    || fail "--redundant-paranoic 0.2:rs-cauchy did not map to (8,2,cauchy)"
$B/invf-verify "$IMGB" --deep 2>&1 | grep -q " 0 mismatched" \
    || fail "mapped-spelling parity leg not clean"
echo "  old spellings map onto the menu (blocks->k, paranoic->pct+algo)"

echo
echo "== [C] crash-mid-seal probe (fault between parity and footer) =="
$B/invf-mkfs "$IMGC" 0.5 >/dev/null
$B/invf-cp "$IMGC" "$WORK/orig/big.log" big.log >/dev/null
$B/invf-cp "$IMGC" "$WORK/orig/rand.bin" rand.bin >/dev/null
$B/invf-sweep "$IMGC" >/dev/null 2>&1
set +e
INVFS_FAULT="vol_seal_crash:1" $B/invf-sweep "$IMGC" --seal > "$WORK/crash.log" 2>&1
CRC=$?
set -e
[ "$CRC" != 0 ] || fail "faulted seal reported success"
grep -q "vol_seal_crash.*fired" "$WORK/crash.log" || fail "fault did not fire"
grep -q "seal failed" "$WORK/crash.log" || fail "no seal-failed diagnostic"
echo "  faulted seal failed loudly (rc=$CRC)"
# the volume opens (listing works) and the seal reads absent (first seal)
$B/invf-ls "$IMGC" >/dev/null 2>&1 || fail "volume does not open after the fault"
$B/invf-verify "$IMGC" --deep > "$WORK/crashv.log" 2>&1 || { cat "$WORK/crashv.log"; exit 1; }
if grep -q "^parity:" "$WORK/crashv.log"; then
    fail "partial seal trusted after crash (parity leg present)"
fi
grep -q " 0 corrupt," "$WORK/crashv.log" || fail "content damaged by the fault"
$B/invf-fsck "$IMGC" 2>&1 | grep -q "^OK$" || fail "fsck not clean after the fault"
echo "  absent, never half-trusted (verify silent, fsck OK)"
# and a clean re-seal heals everything
$B/invf-sweep "$IMGC" --seal > "$WORK/crash-heal.log" 2>&1 \
    || { cat "$WORK/crash-heal.log"; exit 1; }
$B/invf-verify "$IMGC" --deep 2>&1 | grep -q " 0 mismatched" \
    || fail "re-seal after crash not clean"
$B/invf-cat "$IMGC" big.log "$WORK/out/big.log" >/dev/null
cmp -s "$WORK/orig/big.log" "$WORK/out/big.log" || fail "big.log not bit-exact"
echo "  clean re-seal heals (verify clean, bit-exact)"

echo
echo "== [D] dry-run plan changes nothing =="
$B/invf-mkfs "$IMGD" 0.5 >/dev/null
$B/invf-cp "$IMGD" "$WORK/orig/a.c" a.c >/dev/null
$B/invf-sweep "$IMGD" --seal >/dev/null 2>&1
BEFORE=$($B/invf-verify "$IMGD" --deep 2>&1 | grep -c "^parity:" || true)
$B/invf-sweep "$IMGD" --dry-run --seal 20 > "$WORK/dry.log" 2>&1 \
    || { cat "$WORK/dry.log"; exit 1; }
grep -q "\[seal\] plan:" "$WORK/dry.log" || fail "no seal plan in dry-run output"
grep -q "no changes (dry-run)" "$WORK/dry.log" || fail "plan not marked read-only"
grep -q "(k=8,m=2" "$WORK/dry.log" || fail "plan does not show the requested menu"
echo "  $(grep '\[seal\] plan:' "$WORK/dry.log")"
AFTER=$($B/invf-verify "$IMGD" --deep 2>&1 | grep -c "^parity:" || true)
[ "$BEFORE" = "$AFTER" ] && [ "$BEFORE" = 1 ] \
    || fail "dry-run mutated the seal (parity legs: $BEFORE -> $AFTER)"
echo "  dry-run left the live seal untouched"

echo
echo "== [S] WP401 seal scrub (read-only per-group drift report) =="
# Placement: this file, not a new script -- the sealed corpus states the
# scrub must distinguish (clean seal, stale-after-write, deleted-member,
# unsealed) are built above; a focused script would re-scaffold all of
# them. Image S is fresh so its group count and parity bytes are known.
cat > "$WORK/tools/sealflip.c" <<'SEALFLIP_EOF'
/* sealflip -- WP401 test helper: sealflip <img> <group> flips the first
 * byte of <group>'s first parity symbol in the hidden \x01seal-parity
 * file, through the public read/replace API (recipe fork, bitmap
 * consistent): surgical parity damage, content untouched. Refuses
 * anything with unexpected magic/geometry/size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "volume.h"
static uint16_t g16(const uint8_t *p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t g32(const uint8_t *p){return (uint32_t)(p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24));}
int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v;
    uint8_t *d = NULL; size_t dl = 0;
    uint64_t grp, m, ng, off;
    uint32_t sym;
    if (argc != 3) { fprintf(stderr, "usage: sealflip <img> <group>\n"); return 2; }
    grp = strtoull(argv[2], 0, 10);
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    if (vol_read_named(v, "\x01seal-parity", &d, &dl) != 0 || !d || dl < 34) {
        fprintf(stderr, "sealflip: no parity file\n"); vol_close(v); return 1;
    }
    if (memcmp(d, "INVFSEAP", 8) != 0) { fprintf(stderr, "sealflip: bad parity magic\n"); free(d); vol_close(v); return 1; }
    m = g16(d + 12); sym = g32(d + 14); ng = g32(d + 18);
    if (m < 1 || m > 2 || sym != 65536 || grp >= ng) {
        fprintf(stderr, "sealflip: implausible geometry\n");
        free(d); vol_close(v); return 1;
    }
    if (dl != 34 + ng * m * (uint64_t)sym) { fprintf(stderr, "sealflip: size mismatch (%lu)\n", (unsigned long)dl); free(d); vol_close(v); return 1; }
    off = 34 + grp * m * (uint64_t)sym;
    d[off] ^= 0x01;
    if (!vol_replace_file(v, "\x01seal-parity", d, dl)) { fprintf(stderr, "sealflip: replace failed\n"); free(d); vol_close(v); return 1; }
    if (vol_flush(v) != 0) { fprintf(stderr, "sealflip: flush failed\n"); free(d); vol_close(v); return 1; }
    printf("sealflip: flipped parity byte at file offset %llu (group %llu)\n",
           (unsigned long long)off, (unsigned long long)grp);
    free(d); vol_close(v);
    return 0;
}
SEALFLIP_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -o "$WORK/tools/sealflip" \
    "$WORK/tools/sealflip.c" \
    $(for t in volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone vol_heat vol_sweep vol_read vol_write vol_records vol_ast vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro deflate_backend_system deflate_backend_stock; do printf "$REPO/build/obj/$t.o "; done) \
    $REPO/build/obj/zlib_stock_*.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
FLIP="$WORK/tools/sealflip"

echo "-- [S1] sealed-clean volume scrubs 0 on both CLIs, byte-identical --"
$B/invf-mkfs "$IMGS" 0.5 >/dev/null
$B/invf-cp "$IMGS" "$WORK/orig/a.c" a.c >/dev/null
$B/invf-cp "$IMGS" "$WORK/orig/big.log" big.log >/dev/null
$B/invf-sweep "$IMGS" >/dev/null 2>&1 || fail "S1 baseline sweep failed"
$B/invf-sweep "$IMGS" --seal > "$WORK/scrub-seal.log" 2>&1 || { cat "$WORK/scrub-seal.log"; exit 1; }
SNGROUPS=$(grep "\[seal\]" "$WORK/scrub-seal.log" | sed 's/.*\[seal\] \([0-9]*\) groups.*/\1/')
[ "$SNGROUPS" -ge 2 ] || fail "S1 wants >=2 groups, got $SNGROUPS"
echo "  sealed $SNGROUPS groups"
sha256sum "$IMGS" | cut -d' ' -f1 > "$WORK/scrub.pre"
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/scrub-sweep.log" 2>"$WORK/scrub-sweep.err" || fail "S1 sweep scrub rc=$? (want 0)"
$B/invf-verify "$IMGS" --verify-seal > "$WORK/scrub-verify.log" 2>"$WORK/scrub-verify.err" || fail "S1 verify scrub rc=$? (want 0)"
[ "$(grep -c "^seal-heal-needed" "$WORK/scrub-sweep.log")" = 0 ] || fail "S1 ledger lines on a clean seal (sweep)"
[ "$(grep -c "^seal-heal-needed" "$WORK/scrub-verify.log")" = 0 ] || fail "S1 ledger lines on a clean seal (verify)"
grep -q "parity: $SNGROUPS sealed stripes, 0 mismatched, 0 missing, 0 extra" "$WORK/scrub-sweep.log" \
    || fail "S1 sweep scrub summary not clean"
grep -q "parity: $SNGROUPS sealed stripes, 0 mismatched, 0 missing, 0 extra" "$WORK/scrub-verify.log" \
    || fail "S1 verify scrub summary not clean"
# scrub summary == verify --deep parity leg on the same volume
$B/invf-verify "$IMGS" --deep > "$WORK/scrub-deep.log" 2>&1 || { cat "$WORK/scrub-deep.log"; exit 1; }
[ "$(grep "^parity:" "$WORK/scrub-sweep.log")" = "$(grep "^parity:" "$WORK/scrub-deep.log")" ] \
    || fail "S1 scrub summary != verify --deep parity leg"
sha256sum "$IMGS" | cut -d' ' -f1 > "$WORK/scrub.post"
cmp -s "$WORK/scrub.pre" "$WORK/scrub.post" || fail "S1 scrub mutated the image"
echo "  both CLIs rc=0, summaries agree, image bytes identical"

echo "-- [S2] flipped parity byte: LOUD, exact ledger line, rc=3 --"
SLAST=$((SNGROUPS - 1))
"$FLIP" "$IMGS" "$SLAST" >/dev/null 2>&1 || fail "S2 sealflip failed"
sha256sum "$IMGS" | cut -d' ' -f1 > "$WORK/scrub-flip.pre"
set +e
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/scrub-flip.log" 2>"$WORK/scrub-flip.err"
SRC=$?
$B/invf-verify "$IMGS" --verify-seal > "$WORK/scrub-flipv.log" 2>"$WORK/scrub-flipv.err"
VRC=$?
set -e
[ "$SRC" = 3 ] || fail "S2 sweep scrub rc=$SRC (want 3)"
[ "$VRC" = 3 ] || fail "S2 verify scrub rc=$VRC (want 3)"
grep -q "^seal-heal-needed $SLAST mismatched$" "$WORK/scrub-flip.log" \
    || { cat "$WORK/scrub-flip.log"; fail "S2 no exact ledger line for group $SLAST"; }
grep -q "^seal-heal-needed $SLAST mismatched$" "$WORK/scrub-flipv.log" \
    || fail "S2 verify CLI ledger line differs"
grep -q "^parity: $SNGROUPS sealed stripes, 1 mismatched, 0 missing, 0 extra$" "$WORK/scrub-flip.log" \
    || fail "S2 scrub summary wrong"
[ "$(grep "^parity:" "$WORK/scrub-flip.log")" = "$(grep "^parity:" "$WORK/scrub-flipv.log")" ] \
    || fail "S2 CLI summaries disagree"
grep -q "SEAL-SCRUB DRIFT: $IMGS: 1 group(s) need healing" "$WORK/scrub-flip.err" \
    || fail "S2 no stderr banner naming the volume+count"
grep -q "1 mismatched, 0 missing, 0 extra" "$WORK/scrub-flip.err" \
    || fail "S2 banner missing the drift counts"
grep -q "invf-sweep --heal $IMGS" "$WORK/scrub-flip.err" \
    || fail "S2 banner missing the exact heal command"
# verify --deep sees the same drift (same engine counters)
$B/invf-verify "$IMGS" --deep > "$WORK/scrub-flip-deep.log" 2>&1 || true
[ "$(grep "^parity:" "$WORK/scrub-flip.log")" = "$(grep "^parity:" "$WORK/scrub-flip-deep.log")" ] \
    || fail "S2 scrub summary != verify --deep parity leg"
# parity damage heals nothing and breaks nothing: content bit-exact
$B/invf-cat "$IMGS" a.c "$WORK/out/scrub-a.c" >/dev/null 2>&1
cmp -s "$WORK/orig/a.c" "$WORK/out/scrub-a.c" || fail "S2 a.c not bit-exact after parity damage"
$B/invf-cat "$IMGS" big.log "$WORK/out/scrub-big.log" >/dev/null 2>&1
cmp -s "$WORK/orig/big.log" "$WORK/out/scrub-big.log" || fail "S2 big.log not bit-exact after parity damage"
sha256sum "$IMGS" | cut -d' ' -f1 > "$WORK/scrub-flip.post"
cmp -s "$WORK/scrub-flip.pre" "$WORK/scrub-flip.post" || fail "S2 scrub on drift mutated the image"
echo "  rc=3 both CLIs, exact ledger line, banner, bit-exact, read-only"

echo "-- [S3] write-more (no sweep): stale, LOUD --"
$B/invf-sweep "$IMGS" --seal >/dev/null 2>&1 || fail "S3 re-seal failed"
echo "stale bytes" > "$WORK/orig/stale.txt"
$B/invf-cp "$IMGS" "$WORK/orig/stale.txt" stale.txt >/dev/null
set +e
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/scrub-stale.log" 2>"$WORK/scrub-stale.err"
SRC=$?
set -e
[ "$SRC" = 3 ] || fail "S3 scrub rc=$SRC (want 3)"
grep -qE "^seal-heal-needed [0-9]+ (mismatched|missing|extra)$" "$WORK/scrub-stale.log" \
    || fail "S3 no well-formed ledger line"
grep -q " 1 extra$" "$WORK/scrub-stale.log" || fail "S3 added file not reported as extra"
grep -q "SEAL-SCRUB DRIFT: $IMGS:" "$WORK/scrub-stale.err" || fail "S3 no banner"
$RM "$IMGS" stale.txt || fail "S3 cleanup rm failed"
rm -f "$WORK/orig/stale.txt"

echo "-- [S4] delete a sealed file: missing, LOUD --"
set +e
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/scrub-reclean.log" 2>&1
SRC=$?
set -e
[ "$SRC" = 0 ] || fail "S4 stale.txt delete did not restore the seal (rc=$SRC)"
$RM "$IMGS" a.c || fail "S4 rm a.c failed"
set +e
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/scrub-del.log" 2>"$WORK/scrub-del.err"
SRC=$?
set -e
[ "$SRC" = 3 ] || fail "S4 scrub rc=$SRC (want 3)"
grep -qE "^seal-heal-needed [0-9]+ (mismatched|missing|extra)$" "$WORK/scrub-del.log" \
    || fail "S4 no well-formed ledger line"
grep -q " 1 missing" "$WORK/scrub-del.log" || fail "S4 deleted file not reported missing"
grep -q "SEAL-SCRUB DRIFT: $IMGS:" "$WORK/scrub-del.err" || fail "S4 no banner"
echo "  delete drift loud (rc=3, missing counted, ledger well-formed)"

echo "-- [S5] unsealed volume: quiet 0, 'not sealed' --"
$B/invf-mkfs "$IMGS" 0.5 >/dev/null
$B/invf-cp "$IMGS" "$WORK/orig/big.log" big.log >/dev/null
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/scrub-unsealed.log" 2>"$WORK/scrub-unsealed.err" || fail "S5 scrub rc=$? (want 0)"
$B/invf-verify "$IMGS" --verify-seal > "$WORK/scrub-unsealedv.log" 2>&1 || fail "S5 verify scrub rc=$? (want 0)"
grep -q "^not sealed$" "$WORK/scrub-unsealed.log" || fail "S5 no 'not sealed' line (sweep)"
grep -q "^not sealed$" "$WORK/scrub-unsealedv.log" || fail "S5 no 'not sealed' line (verify)"
[ "$(grep -c "^seal-heal-needed" "$WORK/scrub-unsealed.log")" = 0 ] || fail "S5 ledger lines on unsealed volume"
[ "$(grep -c "SEAL-SCRUB" "$WORK/scrub-unsealed.err")" = 0 ] || fail "S5 banner on unsealed volume"
echo "  quiet 0, 'not sealed', never an error"

echo "-- [S6] flag conflicts refused with rc=2 --"
rc=0; $B/invf-sweep "$IMGS" --verify-seal --seal > "$WORK/scrub-conf.log" 2>&1 || rc=$?
[ "$rc" = 2 ] || fail "S6 --verify-seal --seal rc=$rc (want 2)"
rc=0; $B/invf-verify "$IMGS" --deep --verify-seal > "$WORK/scrub-confv.log" 2>&1 || rc=$?
[ "$rc" = 2 ] || fail "S6 --deep --verify-seal rc=$rc (want 2)"
echo "  conflicting modes refused"
echo "== [E] stale-mode legs (WP403; fresh image) =="
$B/invf-mkfs "$IMGE" 0.5 >/dev/null
$B/invf-cp "$IMGE" "$WORK/orig/a.c" a.c >/dev/null
$B/invf-cp "$IMGE" "$WORK/orig/big.log" big.log >/dev/null
$B/invf-sweep "$IMGE" >/dev/null 2>&1 || fail "E baseline sweep failed"
$B/invf-sweep "$IMGE" --seal >/dev/null 2>&1 || fail "E seal failed"
echo "stale-mode probe" > "$WORK/orig/probe.txt"
$B/invf-cp "$IMGE" "$WORK/orig/probe.txt" probe.txt >/dev/null
set +e
$B/invf-verify "$IMGE" --deep > "$WORK/emode-pre.log" 2>&1
set -e
grep -q "STALE: .* uncovered bytes" "$WORK/emode-pre.log" \
    || fail "E fixture is not stale"
GEN_PRE=$(grep -o "sealed-at-gen-[0-9]*" "$WORK/emode-pre.log" | head -n 1)
[ -n "$GEN_PRE" ] || fail "E no sealed-at-gen in the stale report"
$B/invf-cat "$IMGE" $'\x01seal-parity' "$WORK/epar-before" >/dev/null 2>&1 \
    || fail "E cannot dump parity bytes"
echo "  fixture stale ($GEN_PRE)"

echo "  -- [E1] dry-run plans annotate the mode --"
$B/invf-sweep "$IMGE" --dry-run > "$WORK/edry-auto.log" 2>&1 \
    || fail "E dry-run auto failed"
grep -q "\[seal\] plan:" "$WORK/edry-auto.log" \
    || fail "E auto dry plan missing"
grep -q "stale-mode=auto: would reseal" "$WORK/edry-auto.log" \
    || fail "E auto dry plan not annotated"
$B/invf-sweep "$IMGE" --dry-run --stale-mode=notify > "$WORK/edry-notify.log" 2>&1 \
    || fail "E dry-run notify failed"
grep -q "stale-mode=notify: would report, no reseal" "$WORK/edry-notify.log" \
    || fail "E notify dry plan not annotated"
$B/invf-sweep "$IMGE" --dry-run --stale-mode off > "$WORK/edry-off.log" 2>&1 \
    || fail "E dry-run off failed"
grep -q "stale-mode=off: no action" "$WORK/edry-off.log" \
    || fail "E off dry plan not annotated"
set +e
$B/invf-verify "$IMGE" --deep > "$WORK/emode-dry.log" 2>&1
set -e
grep -q "$GEN_PRE" "$WORK/emode-dry.log" \
    || fail "E dry-run mutated the seal"
echo "  dry plans annotated; seal untouched"

echo "  -- [E2] notify: report, no reseal, exit 0 --"
$B/invf-sweep "$IMGE" --stale-mode=notify > "$WORK/enotify.log" 2>&1 \
    || fail "E notify run failed (exit nonzero)"
grep -q "stale-mode=notify" "$WORK/enotify.log" \
    || fail "E no notify diagnostic"
grep -q "STALE: .* uncovered bytes" "$WORK/enotify.log" \
    || fail "E no stale report in notify output"
grep -q "$GEN_PRE" "$WORK/enotify.log" \
    || fail "E notify lost the generation"
grep -q "sealed-but-stale" "$WORK/enotify.log" \
    || fail "E no sealed-but-stale line"
$B/invf-cat "$IMGE" $'\x01seal-parity' "$WORK/epar-notify" >/dev/null 2>&1 \
    || fail "E cannot re-dump parity bytes"
cmp -s "$WORK/epar-before" "$WORK/epar-notify" \
    || fail "E notify resealed (parity bytes differ)"
set +e
$B/invf-verify "$IMGE" --deep > "$WORK/emode-notify.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "E verify passed over notify-stale volume"
grep -q "STALE: .* uncovered bytes" "$WORK/emode-notify.log" \
    || fail "E verify not STALE after notify"
grep -q "$GEN_PRE" "$WORK/emode-notify.log" \
    || fail "E generation advanced under notify"
echo "  notify reported ($GEN_PRE), parity identical, verify still STALE"

echo "  -- [E3] off: one line, no reseal --"
$B/invf-sweep "$IMGE" --stale-mode off > "$WORK/eoff.log" 2>&1 \
    || fail "E off run failed (exit nonzero)"
[ "$(grep -c "stale-mode" "$WORK/eoff.log")" = 1 ] \
    || fail "E off printed more/less than one mode line"
grep -q "no reseal, no report" "$WORK/eoff.log" || fail "E off line wrong"
if grep -q "STALE:" "$WORK/eoff.log"; then
    fail "E off leaked a stale report"
fi
$B/invf-cat "$IMGE" $'\x01seal-parity' "$WORK/epar-off" >/dev/null 2>&1 \
    || fail "E cannot re-dump parity bytes (off)"
cmp -s "$WORK/epar-before" "$WORK/epar-off" \
    || fail "E off resealed (parity bytes differ)"
set +e
$B/invf-verify "$IMGE" --deep > "$WORK/emode-off.log" 2>&1
set -e
grep -q "$GEN_PRE" "$WORK/emode-off.log" \
    || fail "E generation advanced under off"
echo "  off: one line, parity identical, still $GEN_PRE"

echo "  -- [E4] explicit --seal wins over notify --"
$B/invf-sweep "$IMGE" --stale-mode=notify --seal > "$WORK/eexplicit.log" 2>&1 \
    || fail "E explicit seal failed"
grep -q "full recompute" "$WORK/eexplicit.log" \
    || fail "E explicit did not reseal"
$B/invf-verify "$IMGE" --deep > "$WORK/emode-exp.log" 2>&1 \
    || fail "E verify not clean after explicit reseal"
grep -q " 0 mismatched, 0 missing, 0 extra" "$WORK/emode-exp.log" \
    || fail "E parity leg not clean after explicit reseal"
echo "  explicit --seal resealed; verify clean"

echo "  -- [E5] bad mode refused --"
rc=0; $B/invf-sweep "$IMGE" --stale-mode=bogus > "$WORK/ebad.log" 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "E bad stale-mode accepted"
grep -q "want auto|notify|off" "$WORK/ebad.log" || fail "E no mode diagnostic"
echo "  bogus mode refused (rc=$rc)"

echo
echo "== [F] unsealed volumes: no mode output (WP403) =="
$B/invf-mkfs "$IMGF" 0.5 >/dev/null
$B/invf-cp "$IMGF" "$WORK/orig/a.c" a.c >/dev/null
$B/invf-sweep "$IMGF" --stale-mode=notify > "$WORK/eunsealed.log" 2>&1 \
    || fail "F unsealed notify run failed"
if grep -q "stale-mode\|auto-reseal\|\[seal\]" "$WORK/eunsealed.log"; then
    fail "F mode output on an unsealed volume"
fi
$B/invf-sweep "$IMGF" --stale-mode=off > "$WORK/eunsealed-off.log" 2>&1 \
    || fail "F unsealed off run failed"
if grep -q "stale-mode\|auto-reseal\|\[seal\]" "$WORK/eunsealed-off.log"; then
    fail "F off output on an unsealed volume"
fi
echo "  unsealed runs silent"

echo
echo "SEAL E2E: PASS"
