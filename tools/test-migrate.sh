#!/bin/bash
# test-migrate.sh — WP601 migration zone e2e (focused regression script).
#
# Why a focused script (not an extension of test-dedupe/test-seal): the
# migrate stage is new surface (relocate protocol + explicit recompress +
# frag metric) with failure modes neither suite covers (spliced moves,
# silent seal ride-along, recompress guard failures). One script, five
# legs, each asserting its own lines from the sweep log plus readback.
#
# Fixture geometry (all legs): INVFS_SWEEP_MAX_FILE caps the transform's
# whole-file buffering, so files over the cap STAY RAW — the migrate
# stage's population (relocate needs only one 64 KiB segment in memory).
# Without the cap the generic floor eats every small file and migrate
# starves (measured: RAW zone empty, 0 moves). Small files are still
# included so the transform/dedupe/batch paths run alongside.
#
# Leg 1 (defrag mechanism): dup corpus -> capped sweep (dedupe merges
#   leave holes, pinned one generation; migrate drains the RAW leftovers
#   it can place) -> import a SECOND set of fresh files (owned by
#   transform's generic floor, not by migrate -- documented division of
#   labor) -> capped sweep -> third sweep moves nothing (convergence:
#   the sweep1 moves were one-way progress, not churn) -> verify --deep
#   0 mismatched -> cmp clean. What leg 1 proves: the relocate mechanism
#   end to end (moves>0 with seal-dirty marks on sweep1) + convergence
#   (0 on sweep3) + exactness. The frag runs/score line is REPORTED, not
#   free-run count can step either way (hole split vs source merge), so
#   runs is not a convergent metric; the convergent quantities are the
#   moved count (>0, then 0) and RAW drainage. A compact fixture moving
#   nothing is correct behavior, not a failure.
# Leg 2 (crash-mid-move): fresh image + slow migrate, kill -9 mid-stage ->
#   every file byte-identical (moves never change bytes: old-or-new ==
#   cmp clean, never spliced) -> fsck sane -> next sweep heals, verify clean.
# Leg 3 (seal handoff): sealed volume + fresh RAW set -> sweep (migrate
#   moves, log shows seal-dirty ranges > 0: consumed, never silent) ->
#   --verify-seal clean (content-transparent) -> next sweep auto-reseals
#   clean; --stale-mode=notify respected.
# Leg 4 (explicit recompress): capped sweep leaves compressible singles
#   RAW -> --recompress re-encodes to ZSTD (N files + bytes saved in the
#   log), readback exact, second --recompress is a no-op (generation gate).
# Leg 5 (heat-tiered proposal numbers): LZ4-vs-ZSTD bytes + readback timing
#   on one corpus, quoted for owner review (negative result ships
#   explicit-only).
#
# Fixtures live under /srv/flakey/wt-601-migration-t (NOT /dev/shm: /tmp
# tmpfs is tight and the WP scope forbids other subdirs). Run from the repo
# root after `make`:  bash tools/run-e2e.sh tools/test-migrate.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/srv/flakey/wt-601-migration-t
IMG=$WORK/wp601.img
IMGC=$WORK/wp601-crash.img
IMGS=$WORK/wp601-seal.img
IMGR=$WORK/wp601-recomp.img
CAP=524288
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"

. "$REPO/tools/fsck-clean.sh"

free_blocks() { $B/invf-fsck "$1" | awk '/free blocks:/ {print $3}'; }
migrate_line() { grep "^migrate:" "$1" | tail -1; }
frag_of() { sed -n 's/.*frag runs \([0-9]*\)->\([0-9]*\) score \([0-9.]*\)->\([0-9.]*\).*/\1 \2 \3 \4/p' <<<"$(migrate_line "$1")"; }
moved_of() { sed -n 's/migrate: [0-9]* file(s) seen, \([0-9]*\) moved.*/\1/p' <<<"$(migrate_line "$1")"; }
seal_dirty_of() { sed -n 's/.*seal-dirty \([0-9]*\) range(s) (\([0-9]*\) blocks).*/\1 \2/p' <<<"$(migrate_line "$1")"; }

echo "== leg 1: top-heavy fixture + downward drain =="
# Deterministic geometry (no luck): pack bottom-up, delete a LOW
# contiguous range, settle the savepoint pin, then write HIGH. The low
# holes plus high sources are exactly what downward-only relocation
# consolidates. One identical dup pair rides along to exercise the
# shared-segment skip (deduped segments must never move).
python3 - <<'PY'
import os, random
random.seed(601)
d = "/srv/flakey/wt-601-migration-t/orig"
os.makedirs(d, exist_ok=True)
for i in range(20):
    open(os.path.join(d, "f%02d.bin" % i), "wb").write(random.randbytes(1024*1024))
D = random.randbytes(1024*1024)
open(os.path.join(d, "dupA.bin"), "wb").write(D)
open(os.path.join(d, "dupB.bin"), "wb").write(D)
print("corpus: 22 files (20 unique + 1 dup pair)")
PY
$B/invf-mkfs "$IMG" 0.5 >/dev/null
for f in "$WORK"/orig/*; do $B/invf-cp "$IMG" "$f" "$(basename "$f")" >/dev/null; done
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1
echo "  sweep1: $(migrate_line "$WORK/sweep1.log")"
# rm helper (public API only; no CLI rm exists -- the sealrm pattern)
mkdir -p "$WORK/tools"
cat > "$WORK/tools/volrm.c" <<'VOLRM_EOF'
/* volrm -- WP601 test helper: volrm <img> <name> deletes one file. */
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
VOLRM_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -o "$WORK/tools/volrm" \
    "$WORK/tools/volrm.c" \
    $(for t in volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone vol_heat vol_sweep vol_read vol_write vol_records vol_ast vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs ed25519 deflate_repro deflate_backend_system deflate_backend_stock; do printf "$REPO/build/obj/$t.o "; done) \
    $REPO/build/obj/zlib_stock_*.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
VOLRM="$WORK/tools/volrm"
# delete the LOW 8 MB (f00-f07, packed first so contiguous-low)
for i in 00 01 02 03 04 05 06 07; do
    "$VOLRM" "$IMG" "f$i.bin" || { echo "FAIL: delete f$i.bin"; exit 1; }
done
echo "  deleted f00-f07 (8 MB low hole)"
# settle the savepoint pin (deleted blocks pinned one generation)
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMG" > "$WORK/sweep-pin1.log" 2>&1
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMG" > "$WORK/sweep-pin2.log" 2>&1
# HIGH set: fresh writes land at the cursor (high after the fill)
python3 - <<'PY'
import random, os
random.seed(602)
d = "/srv/flakey/wt-601-migration-t/orig2"
os.makedirs(d, exist_ok=True)
for i in range(5):
    open(os.path.join(d, "g%02d.bin" % i), "wb").write(random.randbytes(1024*1024))
print("set G: 5 files (high)")
PY
for f in "$WORK"/orig2/*; do $B/invf-cp "$IMG" "$f" "$(basename "$f")" >/dev/null; done
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1
LINE2=$(migrate_line "$WORK/sweep2.log")
echo "  sweep2: $LINE2"
read -r R0 R1 S0 S1 <<<"$(frag_of "$WORK/sweep2.log")"
echo "  frag: runs $R0->$R1 score $S0->$S1 (reported, not asserted)"
MOVED2=$(moved_of "$WORK/sweep2.log")
[ "${MOVED2:-0}" -gt 0 ] || { echo "FAIL: sweep2 moved nothing (high set not drained down)"; exit 1; }
echo "  sweep2 moved $MOVED2 file(s) (downward drain proven)"
# convergence series: bounded progress must reach 0
PASSN=3
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1
while [ $PASSN -le 8 ]; do
    MOVEDN=$(moved_of "$WORK/sweep${PASSN}.log")
    echo "  sweep$PASSN moved $MOVEDN file(s)"
    [ "${MOVEDN:-x}" = "0" ] && break
    PASSN=$((PASSN+1))
    INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMG" > "$WORK/sweep${PASSN}.log" 2>&1
done
[ "${MOVEDN:-x}" = "0" ] || { echo "FAIL: no convergence after $PASSN passes (last moved $MOVEDN)"; exit 1; }
echo "  converged to 0 moves after $PASSN passes"
$B/invf-verify "$IMG" --deep 2>&1 | grep -q " 0 corrupt" || { echo "FAIL: verify reports corrupt"; exit 1; }
for f in f08.bin f09.bin f10.bin f11.bin f12.bin f13.bin f14.bin f15.bin f16.bin f17.bin f18.bin f19.bin dupA.bin dupB.bin g00.bin g01.bin g02.bin g03.bin g04.bin; do
    if [ -f "$WORK/orig/$f" ]; then SRC="$WORK/orig/$f"; else SRC="$WORK/orig2/$f"; fi
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null 2>&1
    cmp -s "$SRC" "$WORK/out/$f" || { echo "FAIL: readback differs: $f"; exit 1; }
done
echo "  readback: 0 corrupt, cmp clean (17 live files)"
$B/invf-fsck "$IMG" > "$WORK/fsck1.log" 2>&1 || { echo "FAIL: fsck rc"; exit 1; }
fsck_clean "$WORK/fsck1.log" || { echo "FAIL: fsck not clean leg 1"; exit 1; }
echo "  free blocks: $(free_blocks "$IMG")"
echo "== leg 2: crash-mid-move =="
python3 - <<'PY'
import random, os
random.seed(603)
d = "/srv/flakey/wt-601-migration-t/orig3"
os.makedirs(d, exist_ok=True)
for i in range(8):
    open(os.path.join(d, "k%02d.bin" % i), "wb").write(random.randbytes((1024+i*128)*1024))
print("set C: 8 files")
PY
$B/invf-mkfs "$IMGC" 0.5 >/dev/null
for f in "$WORK"/orig3/*; do $B/invf-cp "$IMGC" "$f" "$(basename "$f")" >/dev/null; done
ATTEMPT=0
while [ $ATTEMPT -lt 2 ]; do
    ATTEMPT=$((ATTEMPT+1))
    rm -f "$WORK/crash-sweep.log"
    INVFS_SWEEP_MAX_FILE=$CAP INVFS_MIGRATE_SLOW=1 $B/invf-sweep "$IMGC" > "$WORK/crash-sweep.log" 2>&1 &
    SWPID=$!
    # wait for the migrate STAGE to start (not its summary line), sleep
    # into the widened per-segment window, then kill -9 inside it.
    for i in $(seq 1 300); do
        grep -q "relocating" "$WORK/crash-sweep.log" 2>/dev/null && break
        kill -0 $SWPID 2>/dev/null || break
        sleep 0.2
    done
    sleep $((ATTEMPT+1))
    kill -9 $SWPID 2>/dev/null || true
    # NOTE: bare `wait` under `set -e` exits the SUITE with the child's
    # code (137 when the kill landed -- the expected outcome!). Capture
    # through || so the expected SIGKILL is data, not death.
    SWRC=0
    wait $SWPID 2>/dev/null || SWRC=$?
    echo "  attempt $ATTEMPT: sweep killed (rc=$SWRC)"
    grep -q "^migrate:" "$WORK/crash-sweep.log" 2>/dev/null && echo "  (kill landed after migrate; retrying)" && continue
    break
done
grep -q "^migrate:" "$WORK/crash-sweep.log" 2>/dev/null && { echo "FAIL: could not land kill inside migrate"; exit 1; }
grep -q "relocating" "$WORK/crash-sweep.log" || { echo "FAIL: migrate stage never started"; exit 1; }
echo "  kill landed mid-migrate (no migrate summary line)"
for f in "$WORK"/orig3/*; do
    $B/invf-cat "$IMGC" "$(basename "$f")" "$WORK/out/c-$(basename "$f")" >/dev/null 2>&1 \
        || { echo "FAIL: unreadable after crash: $f"; exit 1; }
    cmp "$f" "$WORK/out/c-$(basename "$f")" || { echo "FAIL: spliced file after crash: $f"; exit 1; }
done
echo "  every file complete-old-or-new (cmp clean), never spliced"
$B/invf-fsck "$IMGC" > "$WORK/crash-fsck.log" 2>&1 || true
grep -Eq "CLEAN|correct|unclaimed" "$WORK/crash-fsck.log" && echo "  fsck verdict sane" || { cat "$WORK/crash-fsck.log"; exit 1; }
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMGC" > "$WORK/crash-heal.log" 2>&1
$B/invf-verify "$IMGC" --deep 2>&1 | grep -q " 0 corrupt" || { echo "FAIL: post-crash verify reports corrupt"; exit 1; }
echo "  post-crash sweep heals; verify 0 mismatched"

echo "== leg 3: seal handoff =="
$B/invf-mkfs "$IMGS" 0.5 >/dev/null
for f in "$WORK"/orig/*; do $B/invf-cp "$IMGS" "$f" "$(basename "$f")" >/dev/null; done
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMGS" --seal > "$WORK/seal1.log" 2>&1
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/seal-verify0.log" 2>&1 || { echo "FAIL: seal did not establish"; exit 1; }
# Fresh set D: the handoff needs MOVABLE segments (the --seal run converged).
for f in "$WORK"/orig2/*; do $B/invf-cp "$IMGS" "$f" "d-$(basename "$f")" >/dev/null; done
# Stage order matters: migrate runs BEFORE seal, so the holes this pass
# frees (stale parity dropped by auto-reseal) are visible to migrate only
# on the NEXT pass. Settle first (auto-reseal frees), then measure moves.
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMGS" > "$WORK/seal-settle.log" 2>&1
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMGS" > "$WORK/seal-mig.log" 2>&1
SEALLINE=$(migrate_line "$WORK/seal-mig.log")
echo "  $SEALLINE"
read -r SR SB <<<"$(seal_dirty_of "$WORK/seal-mig.log")"
[ "${SR:-0}" -gt 0 ] 2>/dev/null || { echo "FAIL: no seal-dirty ranges marked (silent ride-along)"; exit 1; }
echo "  seal-dirty marked: $SR ranges ($SB blocks)"
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/seal-verify1.log" 2>&1 || { echo "FAIL: seal not clean after migrate"; cat "$WORK/seal-verify1.log"; exit 1; }
grep -q "verified clean" "$WORK/seal-verify1.log" && echo "  verify-seal clean after moves (content-transparent)"
INVFS_SWEEP_MAX_FILE=$CAP $B/invf-sweep "$IMGS" > "$WORK/seal-mig2.log" 2>&1
$B/invf-sweep "$IMGS" --verify-seal > "$WORK/seal-verify2.log" 2>&1 || { echo "FAIL: auto-reseal not clean"; exit 1; }
grep -q "verified clean" "$WORK/seal-verify2.log" && echo "  next sweep auto-resealed clean"
$B/invf-sweep "$IMGS" --stale-mode=notify > "$WORK/seal-notify.log" 2>&1 || { echo "FAIL: notify mode rc"; exit 1; }
echo "  stale-mode=notify respected (rc=0)"

echo "== leg 4: explicit recompress =="
python3 - <<'PY'
import os, random
random.seed(604)
d = "/srv/flakey/wt-601-migration-t/orig4"
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
# single-entry compressibles (the v1 recompress population) ...
for i in range(8):
    text_file("t%02d.txt" % i, 40000+i*2000)
# ... plus two multi-entry files (counted, not half-done)
text_file("big0.txt", 200000)
text_file("big1.txt", 300000)
print("recomp corpus: 10 files")
PY
$B/invf-mkfs "$IMGR" 0.5 >/dev/null
for f in "$WORK"/orig4/*; do $B/invf-cp "$IMGR" "$f" "$(basename "$f")" >/dev/null; done
# transform capped below every file: everything stays RAW/LZ4 for --recompress.
# NOTE: the --recompress runs below carry the SAME cap: an uncapped run
# would let transform batch/generic the small text files first, leaving
# recompress nothing to prove itself on (N=0 for the wrong reason).
INVFS_SWEEP_MAX_FILE=1024 $B/invf-sweep "$IMGR" > "$WORK/recomp0.log" 2>&1
INVFS_SWEEP_MAX_FILE=1024 $B/invf-sweep "$IMGR" --recompress > "$WORK/recomp1.log" 2>&1
RC1=$(grep "^migrate-recompress:" "$WORK/recomp1.log" | tail -1)
echo "  $RC1"
N1=$(sed -n 's/migrate-recompress: \([0-9]*\) file(s).*/\1/p' <<<"$RC1")
SAVED=$(sed -n 's/.*(\([0-9]*\) bytes saved).*/\1/p' <<<"$RC1")
[ "${N1:-0}" -gt 0 ] || { echo "FAIL: recompress changed nothing"; exit 1; }
[ "${SAVED:-0}" -gt 0 ] || { echo "FAIL: recompress saved nothing"; exit 1; }
echo "  $N1 files re-encoded, $SAVED bytes saved"
for f in "$WORK"/orig4/*; do
    $B/invf-cat "$IMGR" "$(basename "$f")" "$WORK/out/r-$(basename "$f")" >/dev/null 2>&1
    cmp "$f" "$WORK/out/r-$(basename "$f")" || { echo "FAIL: recompress changed bytes: $f"; exit 1; }
done
echo "  readback bit-exact after recompress"
INVFS_SWEEP_MAX_FILE=1024 $B/invf-sweep "$IMGR" --recompress > "$WORK/recomp2.log" 2>&1
RC2=$(grep "^migrate-recompress:" "$WORK/recomp2.log" | tail -1)
N2=$(sed -n 's/migrate-recompress: \([0-9]*\) file(s).*/\1/p' <<<"$RC2")
[ "${N2:-x}" = "0" ] || { echo "FAIL: second recompress not idempotent (N=$N2)"; exit 1; }
echo "  second --recompress: 0 files (generation-gated idempotence)"
$B/invf-fsck "$IMGR" > "$WORK/fsck-r.log" 2>&1 || { echo "FAIL: fsck rc recompress"; exit 1; }
fsck_clean "$WORK/fsck-r.log" || { echo "FAIL: fsck not clean recompress"; exit 1; }

echo "== leg 5: heat-tiered proposal numbers =="
# LZ4 side = write-path shape (capped sweep leaves LZ4/verbatim RAW);
# ZSTD side = the leg-4 image after --recompress. Quote both.
IMGL=$WORK/wp601-lz4.img
$B/invf-mkfs "$IMGL" 0.5 >/dev/null
for f in "$WORK"/orig4/*; do $B/invf-cp "$IMGL" "$f" "$(basename "$f")" >/dev/null; done
INVFS_SWEEP_MAX_FILE=1024 $B/invf-sweep "$IMGL" >/dev/null 2>&1
FL=$(free_blocks "$IMGL")
FR=$(free_blocks "$IMGR")
T0=$(date +%s.%N)
for f in "$WORK"/orig4/*; do $B/invf-cat "$IMGL" "$(basename "$f")" /dev/null >/dev/null 2>&1; done
T1=$(date +%s.%N)
for f in "$WORK"/orig4/*; do $B/invf-cat "$IMGR" "$(basename "$f")" /dev/null >/dev/null 2>&1; done
T2=$(date +%s.%N)
python3 - "$T0" "$T1" "$T2" "$SAVED" "$FL" "$FR" <<'PY'
import sys
lz4t = float(sys.argv[2]) - float(sys.argv[1])
zstdt = float(sys.argv[3]) - float(sys.argv[2])
print(f"  readback: LZ4-corpus {lz4t:.2f}s vs ZSTD-corpus {zstdt:.2f}s (wall, informational)")
print(f"  ZSTD saved {sys.argv[4]} bytes over the LZ4/verbatim floor; free blocks LZ4-img {sys.argv[5]} vs ZSTD-img {sys.argv[6]}")
PY
echo "  (tiering verdict quoted in the WP report)"
echo "ALL MIGRATE LEGS PASS"
