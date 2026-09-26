#!/bin/bash
# test-multidev.sh — WP25 two-device volumes: metadata mirroring + heat
# tiering e2e (persistent regression).
#
# Devices are two /dev/shm image files. NOTE: blkio treats /dev/* paths as
# raw devices, so the script cd's into /dev/shm and uses RELATIVE image
# paths everywhere (INVFS_DEV1 included).
#
#   leg 1  build: mkfs dev0+dev1, DEVT on both, geometry printed
#   leg 2  import mixed corpus -> sweep -> verify --deep -> bit-exact reads
#   leg 3  placement: metadata span byte-identical on both devices; RAW
#          file lives on dev0 with a dev1 mirror; swept file lives on dev1
#   leg 4  heat: 16 read sessions drive rheat over HOT -> sweep -> dev0
#          tier copies appear (byte-identical to canonical); reads exact
#   leg 5  demotion: arena pressure (<20% free) evicts the coldest copies
#   leg 6  DEGRADED (the money shot): dev0 renamed away -> open with only
#          dev1 present (mechanism: point the tool at the missing dev0
#          path with INVFS_DEV1=<dev1>; the engine opens device 1 alone and
#          serves every structure from the mirror) -> list + read
#          everything bit-exact -> write attempt refused loudly (EROFS);
#          fsck report works, fsck -f refused; reattach -> RW resumes
#   leg 7  mirror resync: rewind dev0's block 0 (stale sync_seq) -> open
#          detects divergence, reads fail over, next flush resyncs (newest
#          state wins, logged) -> spans byte-identical again
#   leg 8  resize: grow lands on the tail device (dev1); shrink refused
#   leg 9  fsck clean on both legs + a single-device smoke (compat)
#
# Legs 1-6 and 9 pass on BOTH formats. Legs 7-8 are red on Meta-v3 and NOT
# because of anything this suite owns: on v3 vol_flush returns right after
# the bitmap flush (volume.c:2435), so the whole two-device commit tail --
# vol_write_devt with its sync_seq bump, and mirror_resync -- never runs.
# Two consequences, both measured (impl_docs/AUDIT.md, E2E-BASELINE item (4)):
# the DEVT sync_seq is frozen at its mkfs value forever, so the staleness
# DETECTION at open can never fire; and the rewind this leg performs rolls
# dev0's block 0 back, which on v3 carries the RT30 root descriptor at 0x9D0,
# so it silently damages the volume with nothing to detect or repair it --
# which is what leg 8's "verify --deep reports corruption" actually is (a
# clean volume resizes and verifies 0 corrupt). The degraded -f refusal in
# leg 6 is refused on v2 only: src/cli/fsck.c's v3 branch returns at line 182
# before the WP25 `fix && vol_degraded(v)` check at line 188.
#
# Run from the repo root after `make`:  bash tools/run-e2e.sh tools/test-multidev.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WORK=/dev/shm/wp25multi
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm

D0=wp25md0.img      # fast device (metadata + RAW + tier arena)
D1=wp25md1.img      # capacity device (metadata mirror + canonical shadow)
E0=wp25me0.img      # small-arena volume for the demotion leg
E1=wp25me1.img
S0=wp25ms0.img      # single-device compat smoke
rm -f "$D0" "$D1" "$E0" "$E1" "$S0" b0save.wp25

export INVFS_DEV1=$D1   # the device-1 locator (wins over the DEVT hint)

fail() { echo "FAIL: $*"; exit 1; }

# the file's read heat (WP27: per-file, from the record's heat TLV;
# awk-only: grep would fail the pipeline on an absent TLV under pipefail)
rheat_max() {
    $B/meta_probe "$1" --heat "$2" | awk \
        '/^heat /{for(i=1;i<=NF;i++) if ($i ~ /^rheat=/) {sub("rheat=","",$i); print $i; found=1}}
         END{if(!found) print 0}'
}
# first segment's pba for a file
pba0() {
    $B/meta_probe "$1" --heat "$2" | grep "^ast i=0 " |
      tail -1 | sed "s/.*pba=//" | awk '{print $1}'
}
# which format is this volume? Printed once so a failure names the format it
# happened on. Nothing gates on it any more: WP98 made the WP25 owner indexes
# (tier + RAW mirror) durable on Meta-v3 as well as v2, so every assertion
# below is format-independent (see leg 4).
vol_is_v3() { $B/invf-fsck "$1" 2>/dev/null | grep -q "format:       v3"; }

echo "== leg 1: build the two-device volume =="
$B/invf-mkfs "$D0" 0.125 "$D1" 0.25 | tee "$WORK/mkfs.log"
grep -q "devices:         2" "$WORK/mkfs.log" || fail "mkfs did not report 2 devices"
RAW_LO=$(sed -n 's/.*raw zone: *blocks \([0-9]*\) \.\..*/\1/p' "$WORK/mkfs.log")
RAW_HI=$(sed -n 's/.*raw zone: *blocks [0-9]* \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs.log")
SH_LO=$(sed -n 's/.*shadow zone: *blocks \([0-9]*\) \.\..*/\1/p' "$WORK/mkfs.log")
META_HI=$(sed -n 's/.*metadata zone: *blocks [0-9]* \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs.log")
AR_LO=$(sed -n 's/.*tier arena: *blocks \([0-9]*\) \.\..*/\1/p' "$WORK/mkfs.log")
AR_HI=$(sed -n 's/.*tier arena: *blocks [0-9]* \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs.log")
[ -n "$RAW_LO" ] && [ -n "$RAW_HI" ] && [ -n "$SH_LO" ] && [ -n "$META_HI" ] \
    || fail "could not parse mkfs geometry"
# DEVT descriptor present on BOTH devices at 0x2A0
dd if="$D0" bs=1 skip=672 count=4 status=none | grep -q DEVT || fail "no DEVT on dev0"
dd if="$D1" bs=1 skip=672 count=4 status=none | grep -q DEVT || fail "no DEVT on dev1"
echo "geometry: raw $RAW_LO..$RAW_HI, arena $AR_LO..$AR_HI, shadow $SH_LO.."
if vol_is_v3 "$D0"; then echo "format: v3 (Meta-v3)"; else echo "format: v2"; fi

echo "== leg 2: import mixed corpus -> sweep -> verify --deep =="
python3 - <<'PY'
import os, random
random.seed(2525)
d = "/dev/shm/wp25multi/orig"
WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa "
         "lambda mu nu xi omicron pi rho sigma tau\n").split()
def tf(name, size):
    n, out = 0, []
    while n < size:
        w = random.choice(WORDS)
        out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
tf("notes.txt", 12000)
tf("readme.txt", 4000)
open(os.path.join(d, "rnd.bin"), "wb").write(random.randbytes(300*1024))
open(os.path.join(d, "hot.bin"), "wb").write(random.randbytes(1500*1024))
PY
$B/invf-import "$D0" "$WORK/orig" > "$WORK/import.log" 2>&1 || {
    cat "$WORK/import.log"; fail "import"; }
grep -q "imported: 0 dirs, 4 files" "$WORK/import.log" || {
    cat "$WORK/import.log"; fail "import count"; }
$B/invf-sweep "$D0" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; fail "sweep 1"; }
$B/invf-verify "$D0" --deep > "$WORK/verify1.log" 2>&1 || {
    cat "$WORK/verify1.log"; fail "verify --deep"; }
grep -q "0 corrupt" "$WORK/verify1.log" || fail "verify --deep corrupt"
ok=1
for f in notes.txt readme.txt rnd.bin hot.bin; do
    $B/invf-cat "$D0" "$f" "$WORK/out/$f" 2>/dev/null || ok=0
    cmp -s "$WORK/orig/$f" "$WORK/out/$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || fail "bit-exact reads after sweep"
echo "bit-exact reads: 4 files OK"

echo "== leg 3: placement =="
# (a) metadata mirrored: the whole span [0, metadata_end) byte-identical
cmp <(head -c $(( (META_HI + 1) * 4096 )) "$D0") \
    <(head -c $(( (META_HI + 1) * 4096 )) "$D1") \
    || fail "metadata span differs between dev0 and dev1"
echo "metadata span ($((META_HI + 1)) blocks) byte-identical on both devices"
# (b) swept (canonical) file on dev1: hot.bin moved to the shadow zone,
# which starts at/above dev0's block count on a two-device volume
HP=$(pba0 "$D0" hot.bin)
[ -n "$HP" ] || fail "hot.bin has no segments"
[ "$HP" -ge "$SH_LO" ] || fail "hot.bin pba $HP below the shadow zone ($SH_LO)"
echo "hot.bin canonical at pba $HP (dev1 shadow)"

echo "== leg 4: heat -> dev0 acceleration copies =="
# WP27: read heat accrues RAM-only per session; the pump persists it via
# vol_heat_persist (the sweep-cadence fold, without the sweep's decay):
# it reads the file once per session, standing in for sweep cadence.
for i in $(seq 16); do
    INVFS_JRN_FORCE_COMPACT=1 $B/invf-l2ptest pump "$D0" hot.bin >/dev/null 2>&1
done
RH=$(rheat_max "$D0" hot.bin)
[ "$RH" -ge 16 ] || fail "hot.bin rheat $RH < 16 after 16 read sessions"
echo "hot.bin rheat=$RH (>= 2x HOT, survives the sweep hysteresis)"
$B/invf-sweep "$D0" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; fail "sweep 2"; }
grep -q "^tier: [1-9][0-9]* hot segment(s) copied to dev0" "$WORK/sweep2.log" \
    || { cat "$WORK/sweep2.log"; fail "no tier copies made"; }
TC=$($B/invf-stats "$D0" | sed -n 's/.*tier (dev0 copies): \([0-9]*\) live.*/\1/p')
# WP98: the cross-open accounting is asserted on BOTH formats. It used to be
# v2-only, with a byte-scan fallback for v3, because the two WP25 indexes were
# persisted through v2 owner records and wp25_index_load_one() read them back
# with meta_read_record_by_id() -- a v2 record-stream lookup that finds nothing
# on Meta-v3, so both indexes were RAM-only across a reopen. On v3 the owner is
# a hidden file whose content IS the index (a recipe blob whose AST entries
# carry each copy's pba and block count), published with
# vol_v3_publish_blob_inode and read back with vol_read_file. So invf-stats --
# a fresh open -- must now see the copies the sweep made, on either format.
[ "${TC:-0}" -ge 1 ] || fail "no live tier copies after the sweep (index did not survive the reopen)"
# the dev0 copy is byte-identical to the canonical dev1 segment
CP=$($B/invf-stats "$D0" | sed -n 's/.*first tier copy *: canonical pba \([0-9]*\) -> dev0 pba \([0-9]*\)/\1 \2/p')
set -- $CP
CAN=$1; DEV0P=$2
[ -n "$CAN" ] && [ -n "$DEV0P" ] || fail "no first-copy coordinates"
[ "$DEV0P" -ge "$AR_LO" ] && [ "$DEV0P" -le "$AR_HI" ] \
    || fail "tier copy pba $DEV0P outside the dev0 arena [$AR_LO,$AR_HI]"
cmp <(dd if="$D0" bs=4096 skip="$DEV0P" count=1 status=none) \
    <(dd if="$D1" bs=4096 skip=$((CAN - AR_HI - 1)) count=1 status=none) \
    || fail "tier copy block differs from the canonical block"
# and the bytes are there independently of the index: the canonical block must
# appear byte-identical somewhere in the dev0 arena (the v3 fallback gate)
if ! python3 - "$D0" "$D1" "$AR_LO" "$AR_HI" "$CAN" <<'PY'
import sys
d0, d1, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
can = int(sys.argv[5])
blk = open(d1, "rb").read()[can*4096:(can+1)*4096]
d0b = open(d0, "rb").read()
# the copy is a whole-segment span, so its FIRST block must sit in the arena
sys.exit(0 if d0b.find(blk, lo*4096, (hi+1)*4096) >= 0 else 1)
PY
then fail "no byte-identical copy of the canonical pba $CAN in the dev0 arena"; fi
echo "tier: $TC copies live in the dev0 arena after a reopen; first copy byte-identical"

$B/invf-cat "$D0" hot.bin "$WORK/out/hot2.bin" 2>/dev/null
cmp -s "$WORK/orig/hot.bin" "$WORK/out/hot2.bin" || fail "hot.bin read after tiering"
echo "reads prefer dev0, content bit-exact"

# RAW placement + mirror, on a file that STAYS raw through the degraded
# leg (imported after the last sweep of this volume): rawfile.bin sits in
# the dev0 RAW zone and is dual-written to dev1 (raw_mirror=1 default).
$B/invf-cp "$D0" "$WORK/orig/rnd.bin" rawfile.bin > /dev/null 2>&1 \
    || fail "cp rawfile"
RP=$(pba0 "$D0" rawfile.bin)
[ -n "$RP" ] || fail "rawfile.bin has no segments"
[ "$RP" -ge "$RAW_LO" ] && [ "$RP" -le "$RAW_HI" ] \
    || fail "rawfile.bin pba $RP outside the RAW zone [$RAW_LO,$RAW_HI] (dev0)"
MC=$($B/invf-stats "$D0" | sed -n 's/.*raw mirror *: \([0-9]*\) segments.*/\1/p')
# WP98: asserted on both formats -- the mirror index rides the same hidden
# owner as the tier index, so it survives the reopen the same way (before
# this it was a v2-only gate; see leg 4). The BYTES below are the
# independent proof and hold either way.
[ "${MC:-0}" -ge 1 ] || fail "rawfile.bin has no dev1 mirror (raw mirror=$MC)"
# the mirror block is byte-identical: find the raw block's bytes on dev1
if ! python3 - "$RP" <<'PY'
import sys
rp = int(sys.argv[1])
d0 = open("/dev/shm/wp25md0.img","rb").read()
d1 = open("/dev/shm/wp25md1.img","rb").read()
blk = d0[rp*4096:(rp+1)*4096]
sys.exit(0 if d1.find(blk) >= 0 else 1)
PY
then fail "rawfile.bin's raw block not found on dev1 (mirror missing)"; fi
echo "rawfile.bin at pba $RP (dev0 RAW) + byte-identical dev1 mirror ($MC segments)"

echo "== leg 5: demotion under arena pressure (small-arena volume) =="
# dev0=0.11 GB -> arena ~2225 blocks; watermark = 20%. A = 2 MB, B = 5 MB.
$B/invf-mkfs "$E0" 0.11 "$E1" 0.25 > "$WORK/mkfs-e.log" 2>&1 || fail "mkfs E"
EAR_LO=$(sed -n 's/.*tier arena: *blocks \([0-9]*\) \.\..*/\1/p' "$WORK/mkfs-e.log")
EAR_HI=$(sed -n 's/.*tier arena: *blocks [0-9]* \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs-e.log")
[ -n "$EAR_LO" ] && [ -n "$EAR_HI" ] || fail "no arena geometry for E"
python3 - <<'PY'
import os, random
random.seed(2626)
d = "/dev/shm/wp25multi/orig"
open(os.path.join(d, "coldA.bin"), "wb").write(random.randbytes(2*1024*1024))
open(os.path.join(d, "coldB.bin"), "wb").write(random.randbytes(5*1024*1024))
PY
(
    export INVFS_DEV1=$E1
    $B/invf-cp "$E0" "$WORK/orig/coldA.bin" A.bin >/dev/null 2>&1 || fail "cp A"
    $B/invf-sweep "$E0" >/dev/null 2>&1 || fail "sweep A->shadow"
    for i in $(seq 16); do
        INVFS_JRN_FORCE_COMPACT=1 $B/invf-l2ptest pump "$E0" A.bin >/dev/null 2>&1
    done
    $B/invf-sweep "$E0" > "$WORK/e-sweepA.log" 2>&1 || fail "sweep promote A"
    grep -q "^tier: [1-9]" "$WORK/e-sweepA.log" || fail "A not promoted"
    # the two "copies live" counts come from invf-stats, i.e. a REOPEN, and
    # WP98 made the WP25 index survive one on Meta-v3 as well as v2 (leg 4
    # explains the shape), so both are asserted unconditionally now. The
    # promotion and the demotion themselves are also read out of the sweep's
    # own tier: line, asserted below.
    TA=$($B/invf-stats "$E0" | sed -n 's/.*tier (dev0 copies): \([0-9]*\) live.*/\1/p')
    [ "${TA:-0}" -ge 1 ] || fail "A has no live copies after a reopen"
    echo "A promoted: ${TA:-0} copies live after a reopen"
    # let A go cold: 3 idle sweeps halve rheat 8 -> 4 -> 2 -> 1
    for i in 1 2 3; do $B/invf-sweep "$E0" >/dev/null 2>&1 || fail "idle sweep $i"; done
    $B/invf-cp "$E0" "$WORK/orig/coldB.bin" B.bin >/dev/null 2>&1 || fail "cp B"
    $B/invf-sweep "$E0" >/dev/null 2>&1 || fail "sweep B->shadow"
    for i in $(seq 16); do
        INVFS_JRN_FORCE_COMPACT=1 $B/invf-l2ptest pump "$E0" B.bin >/dev/null 2>&1
    done
    $B/invf-sweep "$E0" > "$WORK/e-sweepB.log" 2>&1 || fail "sweep promote B"
    grep "^tier: " "$WORK/e-sweepB.log" || fail "no tier line for B"
    DEM=$(sed -n 's/.*copied to dev0 (\([0-9]*\) blocks), \([0-9]*\) demoted.*/\2/p' \
          "$WORK/e-sweepB.log" | tail -1)
    [ "${DEM:-0}" -ge 1 ] || { cat "$WORK/e-sweepB.log"; fail "no demotion under pressure"; }
    # the coldest (A's) copies went; B's hot copies stay live
    LIVE=$($B/invf-stats "$E0" | sed -n 's/.*tier (dev0 copies): \([0-9]*\) live.*/\1/p')
    [ "${LIVE:-0}" -ge 1 ] || {
        cat "$WORK/e-sweepB.log"; fail "demotion evicted the hot copies too"; }
    echo "pressure demoted $DEM cold copies (A's); ${LIVE:-0} hot copies (B's) live after a reopen"
    $B/invf-cat "$E0" A.bin "$WORK/out/A.bin" 2>/dev/null
    cmp -s "$WORK/orig/coldA.bin" "$WORK/out/A.bin" || fail "A read after demotion"
    $B/invf-cat "$E0" B.bin "$WORK/out/B.bin" 2>/dev/null
    cmp -s "$WORK/orig/coldB.bin" "$WORK/out/B.bin" || fail "B read after demotion"
    echo "A (demoted, canonical on dev1) and B (dev0-cached) both bit-exact"
)

echo "== leg 6: DEGRADED mount (dev0 absent) =="
mv "$D0" "$D0.hidden"
# The mechanism: the tool is pointed at the (missing) dev0 path with
# INVFS_DEV1=<dev1>; vol_open finds dev0 absent and opens device 1 alone,
# READ-ONLY, every structure served from the dev1 mirror.
$B/invf-ls "$D0" > "$WORK/dls.log" 2>&1 || fail "degraded ls"
grep -q "DEGRADED" "$WORK/dls.log" || fail "no DEGRADED diagnostic"
grep -q "rawfile.bin" "$WORK/dls.log" || fail "degraded ls lost rawfile.bin"
ok=1
for f in notes.txt readme.txt rnd.bin hot.bin; do
    $B/invf-cat "$D0" "$f" "$WORK/out/d-$f" 2>/dev/null || ok=0
    cmp -s "$WORK/orig/$f" "$WORK/out/d-$f" || { echo "DEGRADED MISMATCH $f"; ok=0; }
done
# rawfile.bin (content == rnd.bin) lives in the RAW zone on the ABSENT
# dev0: a bit-exact read is only possible through its dev1 mirror -- the
# mirror's functional proof
$B/invf-cat "$D0" rawfile.bin "$WORK/out/d-rawfile.bin" 2>/dev/null || ok=0
cmp -s "$WORK/orig/rnd.bin" "$WORK/out/d-rawfile.bin" \
    || { echo "DEGRADED MISMATCH rawfile.bin"; ok=0; }
[ "$ok" = 1 ] || fail "degraded reads"
echo "degraded: all 5 files list + read bit-exact (RAW file via the dev1 mirror)"
# write attempt -> loud refusal, non-zero exit
if $B/invf-cp "$D0" "$WORK/orig/notes.txt" wr.txt > "$WORK/dwr.log" 2>&1; then
    fail "degraded write succeeded?!"
fi
grep -qE "DEGRADED|EROFS|read-only" "$WORK/dwr.log" \
    || { cat "$WORK/dwr.log"; fail "degraded write not loudly refused"; }
echo "degraded write refused loudly: $(tail -1 "$WORK/dwr.log")"
# fsck: report mode works, -f refused
$B/invf-fsck "$D0" > "$WORK/dfsck.log" 2>&1 || fail "degraded fsck report"
grep -q "OK" "$WORK/dfsck.log" || { cat "$WORK/dfsck.log"; fail "degraded fsck not clean"; }
if $B/invf-fsck "$D0" -f > "$WORK/dfsckf.log" 2>&1; then
    fail "degraded fsck -f succeeded?!"
fi
grep -q "DEGRADED" "$WORK/dfsckf.log" || fail "fsck -f refusal silent"
echo "degraded fsck: report OK, -f refused loudly"
# reattach -> RW resumes, bit-exact
mv "$D0.hidden" "$D0"
$B/invf-cp "$D0" "$WORK/orig/notes.txt" back.txt > /dev/null 2>&1 \
    || fail "reattach write"
$B/invf-cat "$D0" back.txt "$WORK/out/back.txt" 2>/dev/null
cmp -s "$WORK/orig/notes.txt" "$WORK/out/back.txt" || fail "reattach read"
$B/invf-cat "$D0" hot.bin "$WORK/out/hot3.bin" 2>/dev/null
cmp -s "$WORK/orig/hot.bin" "$WORK/out/hot3.bin" || fail "reattach hot read"
echo "reattach: RW resumed, writes + reads bit-exact"

echo "== leg 7: mirror resync (stale dev0) =="
dd if="$D0" of=b0save.wp25 bs=4096 count=1 status=none
echo leg7 > leg7.txt
$B/invf-cp "$D0" leg7.txt leg7.txt > /dev/null 2>&1 || fail "cp leg7"
dd if=b0save.wp25 of="$D0" bs=4096 count=1 conv=notrunc status=none
rm -f b0save.wp25
$B/invf-ls "$D0" > "$WORK/rs.log" 2>&1 || fail "stale open"
grep -q "stale" "$WORK/rs.log" || { cat "$WORK/rs.log"; fail "staleness not detected"; }
echo y > y.txt
$B/invf-cp "$D0" y.txt y.txt > "$WORK/rs2.log" 2>&1 || fail "resync flush"
grep -q "mirror resync" "$WORK/rs2.log" || { cat "$WORK/rs2.log"; fail "resync did not run"; }
cmp <(head -c $(( (META_HI + 1) * 4096 )) "$D0") \
    <(head -c $(( (META_HI + 1) * 4096 )) "$D1") \
    || fail "mirror spans differ after resync"
$B/invf-stats "$D0" | grep -q "mirror *: in sync" \
    || fail "mirror not back in sync"
rm -f leg7.txt y.txt
echo "stale dev0 detected, reads failed over, resync newest-wins + byte-identical"

echo "== leg 8: resize (grow the tail device only) =="
SZ1_BEFORE=$(stat -c %s "$D1")
$B/invf-resize "$D0" 512M > "$WORK/resize.log" 2>&1 || {
    cat "$WORK/resize.log"; fail "resize grow"; }
grep -q "invf-resize: OK" "$WORK/resize.log" || fail "resize not OK"
[ "$(stat -c %s "$D1")" -gt "$SZ1_BEFORE" ] || fail "dev1 did not grow"
# dev0's size must not move
[ "$(stat -c %s "$D0")" = "134217728" ] || fail "dev0 changed size"
$B/invf-verify "$D0" --deep > "$WORK/verify2.log" 2>&1 || fail "verify after resize"
grep -q "0 corrupt" "$WORK/verify2.log" || fail "corrupt after resize"
if $B/invf-resize "$D0" 256M > "$WORK/shrink.log" 2>&1; then
    fail "two-device shrink succeeded?!"
fi
grep -q "grows only the TAIL device" "$WORK/shrink.log" \
    || fail "shrink refusal not loud"
echo "resize: growth landed on dev1, reads intact, shrink refused loudly"

echo "== leg 9: fsck clean + single-device compat smoke =="
$B/invf-fsck "$D0" > "$WORK/fsck1.log" 2>&1 || fail "fsck"
grep -q "OK" "$WORK/fsck1.log" || { cat "$WORK/fsck1.log"; fail "fsck not clean"; }
$B/invf-fsck "$D0" -f > "$WORK/fsckf.log" 2>&1 || fail "fsck -f"
grep -q "OK" "$WORK/fsckf.log" || { cat "$WORK/fsckf.log"; fail "fsck -f not clean"; }
$B/invf-fsck "$E0" > "$WORK/fsck-e.log" 2>&1 || fail "fsck E"
grep -q "OK" "$WORK/fsck-e.log" || { cat "$WORK/fsck-e.log"; fail "fsck E not clean"; }
# single-device: no DEVT, the pre-WP25 layout byte-for-byte
$B/invf-mkfs "$S0" 0.0625 > /dev/null 2>&1 || fail "single-device mkfs"
if dd if="$S0" bs=1 skip=672 count=4 status=none | grep -q DEVT; then
    fail "single-device image carries a DEVT"
fi
$B/invf-cp "$S0" "$WORK/orig/notes.txt" n.txt > /dev/null 2>&1 || fail "1dev cp"
$B/invf-cat "$S0" n.txt "$WORK/out/s-n.txt" 2>/dev/null
cmp -s "$WORK/orig/notes.txt" "$WORK/out/s-n.txt" || fail "1dev read"
$B/invf-verify "$S0" --deep > "$WORK/verify-s.log" 2>&1 || fail "1dev verify"
grep -q "0 corrupt" "$WORK/verify-s.log" || fail "1dev corrupt"
$B/invf-fsck "$S0" > "$WORK/fsck-s.log" 2>&1 || fail "1dev fsck"
grep -q "OK" "$WORK/fsck-s.log" || fail "1dev fsck not clean"
echo "fsck clean on both 2-device volumes; single-device path byte-identical"

echo
echo "PASS: two-device volumes (mirror + tiering + degraded + resync + resize)"
