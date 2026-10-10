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
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGB" "$IMGC" "$IMGD"

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
echo "SEAL E2E: PASS"
