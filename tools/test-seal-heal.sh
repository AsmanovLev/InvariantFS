#!/bin/bash
# test-seal-heal.sh — WP402 explicit seal heal (`invf-sweep --heal`) e2e.
#
# Placement: a focused suite, NOT an extension of tools/test-seal.sh.
# test-seal.sh is WP201's contract suite (seal/verify/unseal behaviour
# that siblings 401/403/404 depend on byte-for-byte); this suite covers
# only the NEW --heal path, so WP201's suite stays byte-stable and both
# run independently through tools/run-e2e.sh.
#
# Corpus (sorted stream order A,B,C,S; aligned at k=9, 576 KiB groups):
#   A 400000 B -> stream [0,400000)        group 0 only (slots 0-6)
#   B 159824 B -> stream [400000,559824)   group 0 only (slots 6-8)
#   C 140176 B -> stream [559824,700000)   spans group 0 tail + group 1
#   S  15000 B -> stream [700000,715000)   tail only, one symbol (slot 1)
# Total 715000 B -> group 0 (9 syms) + group 1, the tail (2 syms).
#
#   image E (basic): corrupt A -> verify STALE -> --dry-run --heal plans
#   but changes nothing -> --heal names group 0 (seal-heal-needed) and
#   rebuilds it (bit-exact, verify clean, fsck OK) -> re-run quiet;
#   explicit `--heal 1` rebuilds a drifted tail; a deleted S is recreated
#   from parity (single-symbol erasure in a slot shared with live bytes).
#
#   image F (beyond capacity + independence): A drifted twice in group 0
#   (slots 0,1; the tail's stale region [3,9) untouched) + C drifted once
#   in the tail -> --heal refuses group 0 cleanly (`cannot reconstruct:
#   needs 2, has 1`, volume otherwise untouched, exit nonzero) while group
#   1 still heals (one bad group never aborts the rest); explicit
#   `--heal 0` refuses the same way.
#
#   image G (crash): INVFS_FAULT=vol_seal_heal_crash:1 aborts a heal after
#   the first group's commit -> volume opens, state healed-or-prior per
#   file (A bit-exact, C still exactly-as-corrupted, never garbage),
#   fsck sane, clean re-heal finishes; then a REAL kill -9 mid-heal (slow
#   hook widens the window) proves the same over 12 files.
#
#   image H (refusals): --heal on unsealed / bad group id / --heal --seal.
#
#   image L (symlink): a link target inside the sealed stream -- drift in
#   a far slot heals with the target untouched; drift in the link's own
#   slot heals via the intact-target slice check (verified, never
#   written); a drifted target itself refuses cleanly (its sealed length
#   is unconfirmable, so the layout is unmappable) with nothing touched.
#
# Run from the repo root after `make`:
#   INVFS_E2E_AGENT=wp402 bash tools/run-e2e.sh tools/test-seal-heal.sh
# Uses /dev/shm (tmpfs). Like test-seal.sh, cd's into /dev/shm and uses
# RELATIVE image paths everywhere (blkio treats /dev/* as raw devices).
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

. "$REPO/tools/fsck-clean.sh"

B=$REPO/bin
WORK=/dev/shm/wp402heal
IMG=wp402heal-e.img      # image E: basic / dry-run / explicit / missing
IMGF=wp402heal-f.img     # image F: beyond capacity + independence
IMGG=wp402heal-g.img     # image G: crash probes
IMGH=wp402heal-h.img     # image H: refusals
IMGK=wp402heal-k.img     # image G kill leg: 12-file slow-heal volume
IMGL=wp402heal-l.img     # image L: symlink in the sealed stream
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG" "$IMGF" "$IMGG" "$IMGH" "$IMGK" "$IMGL"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== build the rm helper (public API only; no CLI rm exists) =="
mkdir -p "$WORK/tools"
cat > "$WORK/tools/healrm.c" <<'HEALRM_EOF'
/* healrm — WP402 test helper: healrm <img> <name> deletes one file. */
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
HEALRM_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -o "$WORK/tools/healrm" \
    "$WORK/tools/healrm.c" \
    $(for t in volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone vol_heat vol_sweep vol_read vol_write vol_records vol_ast vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro deflate_backend_system deflate_backend_stock; do printf "$REPO/build/obj/$t.o "; done) \
    $REPO/build/obj/zlib_stock_*.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
RM="$WORK/tools/healrm"

cat > "$WORK/tools/healln.c" <<'HEALLN_EOF'
/* healln — WP402 test helper: healln <img> <name> <target> symlinks. */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v;
    uint64_t id;
    if (argc != 4) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    id = vol_create_symlink(v, argv[2], argv[3]);
    if (id == 0) { vol_close(v); return 1; }
    err = vol_flush(v);
    vol_close(v);
    return err ? 1 : 0;
}
HEALLN_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -o "$WORK/tools/healln" \
    "$WORK/tools/healln.c" \
    $(for t in volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone vol_heat vol_sweep vol_read vol_write vol_records vol_ast vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro deflate_backend_system deflate_backend_stock; do printf "$REPO/build/obj/$t.o "; done) \
    $REPO/build/obj/zlib_stock_*.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
LN="$WORK/tools/healln"

# make_corpus <dir>: the aligned A/B/S/C corpus with exact sizes
make_corpus() { # <dir>
    python3 - "$1" <<'PY'
import os, random, sys
random.seed(402)
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
text_file("A.txt", 400000)
text_file("B.txt", 159824)
text_file("C.txt", 140176)
text_file("S.txt", 15000)
PY
}

# spoil <src> <dst> <off>: same-size copy with 4 bytes flipped at <off>
spoil() { # <src> <dst> <off>
    python3 - "$1" "$2" "$3" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
o = int(sys.argv[3])
bad = d[:o] + b"ZZZZ" + d[o+4:]
assert len(bad) == len(d)
open(sys.argv[2], "wb").write(bad)
PY
}

seal_fresh() { # <img> <origdir>: mkfs + cp + sweep + seal(10) + verify clean
    local img=$1 orig=$2
    $B/invf-mkfs "$img" 0.5 >/dev/null
    for f in A.txt B.txt S.txt C.txt; do
        $B/invf-cp "$img" "$orig/$f" "$f" >/dev/null
    done
    $B/invf-sweep "$img" >/dev/null 2>&1 || fail "sweep failed ($img)"
    $B/invf-sweep "$img" --seal > "$WORK/seal-$img.log" 2>&1 \
        || { cat "$WORK/seal-$img.log"; fail "seal failed ($img)"; }
    grep -q "(full recompute, k=9, m=1, rs-vm)" "$WORK/seal-$img.log" \
        || fail "default seal is not (k=9,m=1) ($img)"
    $B/invf-verify "$img" --deep > "$WORK/verify0-$img.log" 2>&1 \
        || { cat "$WORK/verify0-$img.log"; fail "verify not clean ($img)"; }
    grep -q " 0 mismatched, 0 missing, 0 extra" "$WORK/verify0-$img.log" \
        || fail "parity leg not clean ($img)"
}

check_exact() { # <img> <origdir> <file> <label>
    $B/invf-cat "$1" "$3" "$WORK/out/$3" >/dev/null 2>"$WORK/out/$3.err" \
        || fail "cat $3 failed ($4)"
    cmp -s "$2/$3" "$WORK/out/$3" || fail "$3 not bit-exact ($4)"
}

echo
echo "== [E] basic heal: corrupt A, dry-run plans, heal rebuilds =="
make_corpus "$WORK/orig"
seal_fresh "$IMG" "$WORK/orig"
spoil "$WORK/orig/A.txt" "$WORK/orig/Abad.txt" 1000
$B/invf-cp "$IMG" "$WORK/orig/Abad.txt" A.txt >/dev/null
set +e
$B/invf-verify "$IMG" --deep > "$WORK/verifyE-stale.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify passed over corrupted A"
grep -q " 1 mismatched" "$WORK/verifyE-stale.log" \
    || fail "no mismatched group for corrupted A"
echo "  stale, loudly (rc=$VRC)"
$B/invf-sweep "$IMG" --dry-run --heal > "$WORK/healE-dry.log" 2>&1 \
    || fail "dry-run heal failed"
grep -q "seal-heal-needed 0" "$WORK/healE-dry.log" \
    || fail "dry-run names no group"
grep -q "no changes (dry-run)" "$WORK/healE-dry.log" \
    || fail "dry-run plan not marked read-only"
$B/invf-cat "$IMG" A.txt "$WORK/out/Abad.txt" >/dev/null 2>&1
cmp -s "$WORK/orig/Abad.txt" "$WORK/out/Abad.txt" \
    || fail "dry-run changed A"
echo "  dry-run planned, changed nothing"
$B/invf-sweep "$IMG" --heal > "$WORK/healE.log" 2>&1 \
    || { cat "$WORK/healE.log"; fail "heal failed"; }
grep -q "seal-heal-needed 0 content drift" "$WORK/healE.log" \
    || fail "heal did not name group 0 (scrub contract)"
grep -q "\[heal\] group 0: reconstructed" "$WORK/healE.log" \
    || fail "no reconstruction line for group 0"
grep -q "\[heal\] 1 healed, 0 parity-rewritten, 0 already clean, 0 failed" \
    "$WORK/healE.log" || fail "heal summary wrong"
check_exact "$IMG" "$WORK/orig" A.txt "post-heal"
$B/invf-verify "$IMG" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "parity leg not clean after heal"
$B/invf-fsck "$IMG" 2>&1 | grep -q "^OK$" || fail "fsck not clean after heal"
$B/invf-sweep "$IMG" --heal > "$WORK/healE2.log" 2>&1 \
    || fail "re-run heal failed on clean volume"
grep -q "nothing to do" "$WORK/healE2.log" || fail "re-run not quiet"
echo "  healed bit-exact, verify clean, re-run quiet"

echo
echo "== [E] explicit group + missing-file restore =="
spoil "$WORK/orig/C.txt" "$WORK/orig/Cbad.txt" 50000
$B/invf-cp "$IMG" "$WORK/orig/Cbad.txt" C.txt >/dev/null
$B/invf-sweep "$IMG" --heal 1 > "$WORK/healE-tail.log" 2>&1 \
    || { cat "$WORK/healE-tail.log"; fail "explicit heal 1 failed"; }
check_exact "$IMG" "$WORK/orig" C.txt "explicit tail heal"
$B/invf-verify "$IMG" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "parity leg not clean after explicit heal"
echo "  explicit group id healed the tail"
$RM "$IMG" S.txt || fail "healrm S.txt failed"
rm -f "$WORK/orig/S.txt.gone"
set +e
$B/invf-verify "$IMG" --deep > "$WORK/verifyE-missing.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify passed over deleted S"
grep -q " 1 missing" "$WORK/verifyE-missing.log" \
    || fail "deleted S not reported missing"
$B/invf-sweep "$IMG" --heal > "$WORK/healE-missing.log" 2>&1 \
    || { cat "$WORK/healE-missing.log"; fail "missing-file heal failed"; }
check_exact "$IMG" "$WORK/orig" S.txt "restored S"
$B/invf-verify "$IMG" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "parity leg not clean after restore"
echo "  deleted 1-symbol file recreated from parity, bit-exact"

echo
echo "== [F] beyond capacity refuses, one bad group never aborts the rest =="
mkdir -p "$WORK/forig"
make_corpus "$WORK/forig"
seal_fresh "$IMGF" "$WORK/forig"
# two drifts in group 0 (slots 0,1; the tail's stale slots [3,9) untouched)
python3 - <<'PY'
d = open("/dev/shm/wp402heal/forig/A.txt", "rb").read()
bad = d[:1000] + b"ZZZZ" + d[1004:]
bad = bad[:70000] + b"QQQQ" + bad[70004:]
assert len(bad) == len(d)
open("/dev/shm/wp402heal/forig/Abad2.txt", "wb").write(bad)
PY
spoil "$WORK/forig/C.txt" "$WORK/forig/Cbad.txt" 100000
$B/invf-cp "$IMGF" "$WORK/forig/Abad2.txt" A.txt >/dev/null
$B/invf-cp "$IMGF" "$WORK/forig/Cbad.txt" C.txt >/dev/null
set +e
$B/invf-sweep "$IMGF" --heal > "$WORK/healF.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "heal succeeded over beyond-capacity drift"
grep -q "heal: group 0: cannot reconstruct: needs 2, has 1" "$WORK/healF.log" \
    || { cat "$WORK/healF.log"; fail "no clean beyond-capacity refusal"; }
grep -q "\[heal\] group 1: reconstructed" "$WORK/healF.log" \
    || fail "healable tail did not heal alongside the refusal"
check_exact "$IMGF" "$WORK/forig" C.txt "independent tail heal"
$B/invf-cat "$IMGF" A.txt "$WORK/out/Astillbad.txt" >/dev/null 2>&1
cmp -s "$WORK/forig/Abad2.txt" "$WORK/out/Astillbad.txt" \
    || fail "refused group was touched"
set +e
$B/invf-verify "$IMGF" --deep > "$WORK/verifyF-still.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify passed over refused group"
grep -q " 1 mismatched" "$WORK/verifyF-still.log" \
    || fail "group 0 no longer flagged after refusal"
echo "  refused cleanly (needs 2, has 1), exit nonzero, rest healed"
set +e
$B/invf-sweep "$IMGF" --heal 0 > "$WORK/healF0.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "explicit heal 0 succeeded over beyond-capacity drift"
grep -q "cannot reconstruct: needs 2, has 1" "$WORK/healF0.log" \
    || fail "explicit refusal message wrong"
echo "  explicit group id refuses the same way"

echo
echo "== [G] crash-mid-heal: fault seam, healed-or-prior =="
mkdir -p "$WORK/gorig"
make_corpus "$WORK/gorig"
seal_fresh "$IMGG" "$WORK/gorig"
spoil "$WORK/gorig/A.txt" "$WORK/gorig/Abad.txt" 1000
spoil "$WORK/gorig/C.txt" "$WORK/gorig/Cbad.txt" 100000
$B/invf-cp "$IMGG" "$WORK/gorig/Abad.txt" A.txt >/dev/null
$B/invf-cp "$IMGG" "$WORK/gorig/Cbad.txt" C.txt >/dev/null
set +e
INVFS_FAULT="vol_seal_heal_crash:1" $B/invf-sweep "$IMGG" --heal \
    > "$WORK/healG-fault.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "faulted heal reported success"
grep -q "vol_seal_heal_crash.*fired" "$WORK/healG-fault.log" \
    || fail "fault did not fire"
$B/invf-ls "$IMGG" >/dev/null 2>&1 || fail "volume does not open after fault"
$B/invf-cat "$IMGG" A.txt "$WORK/out/Afault.txt" >/dev/null 2>&1
cmp -s "$WORK/gorig/A.txt" "$WORK/out/Afault.txt" \
    || fail "group 0 not healed before the fault"
$B/invf-cat "$IMGG" C.txt "$WORK/out/Cfault.txt" >/dev/null 2>&1
cmp -s "$WORK/gorig/Cbad.txt" "$WORK/out/Cfault.txt" \
    || fail "unhealed group not exactly prior (garbage written?)"
$B/invf-fsck "$IMGG" 2>&1 | grep -q "^OK$" || fail "fsck not sane after fault"
echo "  killed heal left healed-or-prior (A healed, C exactly prior)"
$B/invf-sweep "$IMGG" --heal > "$WORK/healG-clean.log" 2>&1 \
    || { cat "$WORK/healG-clean.log"; fail "clean re-heal failed"; }
check_exact "$IMGG" "$WORK/gorig" A.txt "post-fault A"
check_exact "$IMGG" "$WORK/gorig" C.txt "post-fault C"
$B/invf-verify "$IMGG" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "parity leg not clean after re-heal"
echo "  clean re-heal finishes bit-exact"

echo
echo "== [G] crash-mid-heal: REAL kill -9 over a 12-file volume =="
mkdir -p "$WORK/korig"
python3 - <<'PY'
import os, random
random.seed(407)
d = "/dev/shm/wp402heal/korig"
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
for i in range(12):
    out = []; n = 0
    while n < 500000:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, "f%02d.txt" % i), "w").write(" ".join(out)[:500000])
PY
$B/invf-mkfs "$IMGK" 0.5 >/dev/null
for f in "$WORK"/korig/f*.txt; do
    bn=$(basename "$f")
    $B/invf-cp "$IMGK" "$f" "$bn" >/dev/null
done
$B/invf-sweep "$IMGK" >/dev/null 2>&1
$B/invf-sweep "$IMGK" --seal >/dev/null 2>&1
$B/invf-verify "$IMGK" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "kill-leg seal not clean"
# corrupt every other file (6 drifted groups), keep the .bad copies
for i in 01 03 05 07 09 11; do
    spoil "$WORK/korig/f$i.txt" "$WORK/korig/f${i}bad.txt" 1000
    $B/invf-cp "$IMGK" "$WORK/korig/f${i}bad.txt" "f$i.txt" >/dev/null
done
set +e
INVFS_HEAL_SLOW_MS=700 $B/invf-sweep "$IMGK" --heal > "$WORK/healK.log" 2>&1 &
HEALPID=$!
sleep 3
kill -0 $HEALPID 2>/dev/null || fail "heal finished before the kill window"
kill -9 $HEALPID
KRC=$?
wait $HEALPID 2>/dev/null
set -e
[ "$KRC" = 0 ] || fail "kill -9 did not land (rc=$KRC)"
if grep -qE "^\[heal\] [0-9]+ healed" "$WORK/healK.log"; then
    fail "heal finished before the kill (no partial state to prove)"
fi
echo "  kill -9 landed mid-heal (pid $HEALPID)"
$B/invf-ls "$IMGK" >/dev/null 2>&1 || fail "volume does not open after kill -9"
# every file: healed (== orig) or prior (== .bad) -- never anything else
for i in 01 03 05 07 09 11; do
    $B/invf-cat "$IMGK" "f$i.txt" "$WORK/out/kf$i.txt" >/dev/null 2>&1 \
        || fail "cat f$i failed after kill"
    if cmp -s "$WORK/korig/f$i.txt" "$WORK/out/kf$i.txt"; then
        echo "  f$i.txt: healed"
    elif cmp -s "$WORK/korig/f${i}bad.txt" "$WORK/out/kf$i.txt"; then
        echo "  f$i.txt: prior"
    else
        fail "f$i.txt is neither healed nor prior (half-written)"
    fi
done
for i in 00 02 04 06 08 10; do
    check_exact "$IMGK" "$WORK/korig" "f$i.txt" "untouched f$i after kill"
done
$B/invf-fsck "$IMGK" 2>&1 | grep -q "^OK$" || fail "fsck not sane after kill -9"
echo "  healed-or-prior everywhere, fsck sane"
$B/invf-sweep "$IMGK" --heal > "$WORK/healK-clean.log" 2>&1 \
    || { cat "$WORK/healK-clean.log"; fail "post-kill re-heal failed"; }
for i in 01 03 05 07 09 11; do
    check_exact "$IMGK" "$WORK/korig" "f$i.txt" "re-healed f$i"
done
$B/invf-verify "$IMGK" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "parity leg not clean after post-kill re-heal"
echo "  post-kill re-heal finishes bit-exact"

echo
echo "== [H] refusals: unsealed, bad gid, conflicting flags =="
$B/invf-mkfs "$IMGH" 0.5 >/dev/null
$B/invf-cp "$IMGH" "$WORK/orig/A.txt" A.txt >/dev/null
set +e
$B/invf-sweep "$IMGH" --heal > "$WORK/healH-unsealed.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "--heal on unsealed volume succeeded"
grep -q "not sealed" "$WORK/healH-unsealed.log" \
    || fail "no not-sealed diagnostic"
set +e
$B/invf-sweep "$IMG" --heal 99999 > "$WORK/healH-gid.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "--heal of missing group succeeded"
grep -q "no such group" "$WORK/healH-gid.log" || fail "no no-such-group note"
set +e
$B/invf-sweep "$IMG" --heal --seal > "$WORK/healH-conf.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "--heal --seal accepted"
grep -q "standalone mode" "$WORK/healH-conf.log" || fail "no conflict note"
echo "  unsealed / bad gid / flag conflict all refuse loudly"

echo
echo "== [L] symlink in the stream: mapping holds, drift refuses =="
$B/invf-mkfs "$IMGL" 0.5 >/dev/null
$B/invf-cp "$IMGL" "$WORK/orig/A.txt" A.txt >/dev/null
$LN "$IMGL" M.lnk A.txt || fail "healln failed"
$B/invf-sweep "$IMGL" >/dev/null 2>&1
$B/invf-sweep "$IMGL" --seal >/dev/null 2>&1
$B/invf-verify "$IMGL" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "link seal not clean"
$B/invf-cat "$IMGL" M.lnk "$WORK/out/Mlnk.txt" >/dev/null 2>&1 \
    || fail "cat symlink failed"
[ "$(cat "$WORK/out/Mlnk.txt")" = "A.txt" ] || fail "link target wrong"
# drift sharing no slot with the link: plain rebuild, link untouched
$B/invf-cp "$IMGL" "$WORK/orig/Abad.txt" A.txt >/dev/null
$B/invf-sweep "$IMGL" --heal > "$WORK/healL1.log" 2>&1 \
    || { cat "$WORK/healL1.log"; fail "link-image heal failed"; }
check_exact "$IMGL" "$WORK/orig" A.txt "link-image A"
$B/invf-cat "$IMGL" M.lnk "$WORK/out/Mlnk2.txt" >/dev/null 2>&1
[ "$(cat "$WORK/out/Mlnk2.txt")" = "A.txt" ] || fail "link target moved"
echo "  drift away from the link heals, target intact"
# drift inside the link's own slot: the intact-target slice check fires
spoil "$WORK/orig/A.txt" "$WORK/orig/Abad6.txt" 395000
$B/invf-cp "$IMGL" "$WORK/orig/Abad6.txt" A.txt >/dev/null
$B/invf-sweep "$IMGL" --heal > "$WORK/healL2.log" 2>&1 \
    || { cat "$WORK/healL2.log"; fail "link-slot heal failed"; }
check_exact "$IMGL" "$WORK/orig" A.txt "link-slot A"
$B/invf-verify "$IMGL" --deep 2>&1 | grep -q " 0 mismatched, 0 missing, 0 extra" \
    || fail "parity leg not clean after link-slot heal"
echo "  drift in the link's slot heals (target slice verified, never written)"
# drift the link itself: sealed length unconfirmable -> clean refusal
$RM "$IMGL" M.lnk || fail "healrm M.lnk failed"
$LN "$IMGL" M.lnk B.txt || fail "healln drift failed"
set +e
$B/invf-sweep "$IMGL" --heal > "$WORK/healL3.log" 2>&1
HRC=$?
set -e
[ "$HRC" != 0 ] || fail "heal over drifted symlink succeeded"
grep -q "unmappable layout" "$WORK/healL3.log" \
    || { cat "$WORK/healL3.log"; fail "no unmappable refusal"; }
check_exact "$IMGL" "$WORK/orig" A.txt "A untouched by refusal"
$B/invf-cat "$IMGL" M.lnk "$WORK/out/Mlnk3.txt" >/dev/null 2>&1
[ "$(cat "$WORK/out/Mlnk3.txt")" = "B.txt" ] || fail "drifted target moved"
echo "  drifted symlink refuses cleanly (unmappable), nothing touched"

echo
echo "SEAL-HEAL E2E: PASS"
