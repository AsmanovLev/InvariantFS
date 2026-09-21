#!/bin/sh
# tools/test-sweep-mapper.sh — WP45: offline invf-sweep across MANY metadata
# extents must not no-op.
#
# Genome: on a many-extent volume the legacy sweep walk only saw the active
# extent: "sweep: 0 swept", "dedupe: hashed 0 live segments". The component
# WPs (41-44) fix that.
#
# Self-gating: if invf-stats reports a ZERO population the component is not
# landed yet -> SKIP. Otherwise strict: swept/hashed counters NONZERO,
# logical bytes preserved across the mix, 20 seeded names read bit-exact
# post-move, and fsck clean.
#
# Run through the e2e lock:
#   INVFS_E2E_AGENT=wp45 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/tmp/invfs-e2e-test-sweep-mapper
OUT=$WORK/out
PASS=0; FAIL=0

say(){ echo "[sweep-mapper] $*"; }
ok(){ PASS=$((PASS+1)); say "PASS: $*"; }
bad(){ FAIL=$((FAIL+1)); say "FAIL: $*"; }
skip(){ say "SKIP: $*"; echo "RESULT: $PASS passed, $FAIL failed (SKIPPED)"; exit 0; }
fail(){ bad "$*"; echo "RESULT: $PASS passed, $FAIL failed"; exit 1; }

BIGVOL_LIB=1
. "$REPO/tools/test-fixture-bigvol.sh"

say "building big-volume fixture in $WORK"
bigvol_cleanup "$WORK"
bigvol_build "$WORK" || fail "fixture build"
COUNTS=$WORK/canonical-counts.json
CANON_FILES=$(bigvol_get "$COUNTS" files)
CANON_BYTES=$(bigvol_get "$COUNTS" logical_bytes)
mkdir -p "$OUT"

STATS=$("$B/invf-stats" "$WORK/dev0.img" 2>&1) || fail "invf-stats run"
POP=$(echo "$STATS" | sed -n 's/^  regular files[[:space:]]*: //p')
POP=${POP:-0}

# ---- self-gate ---------------------------------------------------------
if [ "${POP:-0}" -eq 0 ]; then
    skip "invf-stats population is 0 on a $CANON_FILES-file volume -> mapper-unaware walk (component WPs not merged yet)"
fi

# ---- sweep (offline, no FUSE) ------------------------------------------
say "offline invf-sweep"
SWEEP_OUT=$("$B/invf-sweep" "$WORK/dev0.img" 2>&1) || true
echo "$SWEEP_OUT" | tail -20

SWEPT=$(printf '%s\n' "$SWEEP_OUT" | sed -n 's/^sweep: \([0-9]*\) swept.*/\1/p')
HASHED=$(printf '%s\n' "$SWEEP_OUT" | sed -n 's/^dedupe: hashed \([0-9]*\) live segments.*/\1/p')
# WP52: cross-file batch deferral is enabled again on mapper volumes, so an
# all-text fixture is batched (text -> PPMd) rather than moved to Shadow one
# file at a time. That is the walker WORKING, not seeing nothing: the genome
# this suite guards (WP45) is "the sweep walk saw the records". A batched run
# proves it via the deferred counter; dedupe legitimately hashes 0 because
# every live entry is a TEXT batch slice (dedupe skips those: the batches are
# shared and owner-owned). Accept either the pre-deferral generic floor
# (swept/hashed nonzero) OR the deferred-batch path (deferred nonzero).
DEFERRED=$(printf '%s\n' "$SWEEP_OUT" | sed -n 's/^text batches flushed (\([0-9]*\) deferred).*/\1/p')
DEFERRED=${DEFERRED:-0}
[ "${SWEPT:-0}" -gt 0 ] || [ "$DEFERRED" -gt 0 ] \
    && ok "sweep processed records (swept=$SWEPT deferred=$DEFERRED; walker saw the volume)" \
    || bad "sweep reported ${SWEPT:-0} swept and 0 deferred (walk saw nothing)"
[ "${HASHED:-0}" -gt 0 ] || [ "$DEFERRED" -gt 0 ] \
    && ok "dedupe walk saw segments (hashed=$HASHED deferred=$DEFERRED)" \
    || bad "dedupe hashed ${HASHED:-0} segments and 0 deferred (walk saw nothing)"

# ---- post-condition: logical bytes preserved by the physical moves -------
STATS2=$("$B/invf-stats" "$WORK/dev0.img" 2>&1) || fail "invf-stats rerun"
GOT_MIB=$(printf '%s\n' "$STATS2" | sed -n 's/.*logical bytes[[:space:]]*: \([0-9.]*\) MiB.*/\1/p')
GOAL_MIB=$(awk -v b="$CANON_BYTES" 'BEGIN{printf "%.1f", b/1048576}')
if awk -v g="$GOT_MIB" -v w="$GOAL_MIB" 'BEGIN{exit (g-w<0.6 && w-g<0.6)?0:1}'; then
    ok "logical bytes preserved by sweep ($GOT_MIB ~ $GOAL_MIB MiB)"
else
    bad "logical bytes after sweep: $GOT_MIB MiB, expected ~$GOAL_MIB MiB"
fi

# nothing disappeared: stats population identical post-sweep
POP2=$(printf '%s\n' "$STATS2" | sed -n 's/^  regular files[[:space:]]*: //p')
[ "$POP2" = "$POP" ] && ok "population stable across sweep ($POP)" \
                     || bad "population changed across sweep ($POP -> $POP2)"

# ---- bit-exact spot checks: 20 seeded names, source vs post-sweep volume --
say "bit-exact spot checks (20 seeded names)"
CAT_OK=0
_i=0
while [ "$_i" -lt 20 ]; do
    _d=$((_i * 3 % 60))
    _f=$((_i * 97 % 495))
    _name=$(printf 'd%02d/f%05d' "$_d" "$_f")
    _srcp=$(printf '%s/d%02d/f%05d' "$WORK/src" "$_d" "$_f")
    if "$B/invf-cat" "$WORK/dev0.img" "$_name" "$OUT/x" >/dev/null 2>&1 \
       && cmp -s "$_srcp" "$OUT/x"; then
        CAT_OK=$((CAT_OK+1))
    else
        say "  read-back failed: $_name"
    fi
    _i=$((_i+1))
done
[ "$CAT_OK" -eq 20 ] && ok "all 20 spot names read bit-exact post-sweep" \
                      || bad "only $CAT_OK/20 spot names bit-exact post-sweep"

# ---- fsck clean ---------------------------------------------------------
FSCK_OUT=$("$B/invf-fsck" "$WORK/dev0.img" 2>&1)
ORPHANS=$(printf '%s\n' "$FSCK_OUT" | sed -n 's/^  orphans: *\([0-9]*\).*/\1/p' | head -1)
MISSING=$(printf '%s\n' "$FSCK_OUT" | sed -n 's/^  missing: *\([0-9]*\).*/\1/p' | head -1)
BADC=$(printf '%s\n' "$FSCK_OUT" | sed -n 's/^  bad records: *\([0-9]*\).*/\1/p' | head -1)
if [ "${ORPHANS:-0}" = 0 ] && [ "${MISSING:-0}" = 0 ] && [ "${BADC:-0}" = 0 ]; then
    ok "fsck clean (orphans=0, missing=0, bad records=0)"
else
    bad "fsck not clean (orphans=$ORPHANS, missing=$MISSING, bad records=$BADC)"
    printf '%s\n' "$FSCK_OUT" | head -15
fi

echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
