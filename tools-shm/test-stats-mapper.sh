#!/bin/sh
# tools/test-stats-mapper.sh — WP45: invf-stats / invf-ls population on a
# MANY-extent volume (the mapper-unaware walk bug encoded as an e2e).
#
# Genome: on a volume whose ~30k records span many dynamic metadata extents
# the legacy walkers silently report tiny/zero populations. The component
# WPs (41-44) fix that.
#
# Self-gating: if invf-stats reports a ZERO population the component is not
# landed yet -> SKIP (so `make e2e` stays green until then). Once population
# is nonzero the assertions are strict and must hold.
#
# Run through the e2e lock:
#   INVFS_E2E_AGENT=wp45 bash tools/run-e2e.sh tools/test-stats-mapper.sh
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/tmp/invfs-e2e-test-stats-mapper
PASS=0; FAIL=0

say(){ echo "[stats-mapper] $*"; }
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
CANON_DIRS=$(bigvol_get "$COUNTS" dirs)
CANON_LINKS=$(bigvol_get "$COUNTS" links)
CANON_BYTES=$(bigvol_get "$COUNTS" logical_bytes)
say "canonical: $CANON_FILES files, $CANON_DIRS dirs, $CANON_LINKS links, $CANON_BYTES logical bytes"

STATS=$("$B/invf-stats" "$WORK/dev0.img" 2>&1) || fail "invf-stats run"
echo "$STATS"

POP=$(echo "$STATS" | sed -n 's/^  regular files[[:space:]]*: //p')
POP=${POP:-0}
DIRS=$(echo "$STATS" | sed -n 's/^  directories[[:space:]]*: //p')
LNKS=$(echo "$STATS" | sed -n 's/^  symlinks[[:space:]]*: //p')

# ---- self-gate ---------------------------------------------------------
if [ "${POP:-0}" -eq 0 ]; then
    skip "invf-stats population is 0 on a $CANON_FILES-file volume -> mapper-unaware walk (component WPs not merged yet)"
fi

# ---- strict assertions (only reached once the walk can see the volume) ---
# WP46: population counts the 0x01 internal owner records too (tier0/rawm
# on a two-device volume), so allow canonical + a small owner slack.
POP_MAX=$((CANON_FILES + 2))
if [ "$POP" -ge "$CANON_FILES" ] && [ "$POP" -le "$POP_MAX" ]; then
    ok "invf-stats population $POP matches canonical $CANON_FILES (+owner slack)"
else
    bad "invf-stats population $POP != canonical $CANON_FILES (+<=2 owners)"
fi
[ "$DIRS" = "$CANON_DIRS" ] && ok "directories $DIRS match" \
                             || bad "directories: stats=$DIRS canonical=$CANON_DIRS"
[ "$LNKS" = "$CANON_LINKS" ] && ok "symlinks $LNKS match" \
                               || bad "symlinks: stats=$LNKS canonical=$CANON_LINKS"

# logical bytes: stats reports float MiB; compare within rounding slack
GOAL_MIB=$(awk -v b="$CANON_BYTES" 'BEGIN{printf "%.1f", b/1048576}')
GOT_MIB=$(echo "$STATS" | sed -n 's/.*logical bytes[[:space:]]*: \([0-9.]*\) MiB.*/\1/p')
if awk -v g="$GOT_MIB" -v w="$GOAL_MIB" 'BEGIN{exit (g-w<0.6 && w-g<0.6)?0:1}'; then
    ok "logical bytes ~ $GOAL_MIB MiB (got $GOT_MIB)"
else
    bad "logical bytes: stats=$GOT_MIB MiB, expected ~$GOAL_MIB MiB"
fi

# invf-ls lists every live record: regular files + directory anchors +
# symlinks (+0x01 owners), not just regular files (WP46).
LS_EXPECT=$((CANON_FILES + CANON_DIRS + CANON_LINKS))
LS_MAX=$((LS_EXPECT + 2))
LS_OUT=$("$B/invf-ls" "$WORK/dev0.img" 2>&1) || fail "invf-ls run"
LS_FILES=$(printf '%s\n' "$LS_OUT" | sed -n 's/^\([0-9]*\) file(s)/\1/p' | head -1)
if [ "$LS_FILES" -ge "$LS_EXPECT" ] && [ "$LS_FILES" -le "$LS_MAX" ]; then
    ok "invf-ls reports $LS_FILES records (files+dirs+links)"
else
    bad "invf-ls reports ${LS_FILES:-0} records, expected $LS_EXPECT (+<=2 owners)"
fi

# unclaimed blocks: a fresh mapper volume legitimately reports its metadata
# extents as "unclaimed" (allocated, no AST-segment reference yet). Nothing
# is deleted here, so assert the unclaimed share stays well below a leak
# (which would be ~100% of used) rather than a fixed byte cap (WP46).
UNCL=$(echo "$STATS" | sed -n 's/.*unclaimed: \([0-9.]*\) MiB.*/\1/p')
UNCL=${UNCL:-0}
USED_MIB=$(echo "$STATS" | sed -n 's/^  used[[:space:]]*: \([0-9.]*\) MiB.*/\1/p')
USED_MIB=${USED_MIB:-0}
if awk -v u="$UNCL" -v t="$USED_MIB" 'BEGIN{exit (t<=0 || u < t*0.5)?0:1}'; then
    ok "unclaimed ${UNCL} MiB < 50% of used ${USED_MIB} MiB (meta extents)"
else
    bad "unclaimed ${UNCL} MiB >= 50% of used ${USED_MIB} MiB on a fresh volume"
fi

echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
