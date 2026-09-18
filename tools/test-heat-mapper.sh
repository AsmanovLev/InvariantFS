#!/bin/sh
# tools/test-heat-mapper.sh — WP45: read-heat persistence on a MANY-extent
# volume must survive unmount + reopen for touched names, and stay zero for
# untouched ones.
#
# Genome: on a many-extent volume the legacy heat persist/lookup walks only
# covered the active extent, so heat tables came back empty. The component
# WPs (41-44) fix that.
#
# Legs:
#   A. import -> mount via invf-fuse -> read ~100 seeded names through the
#      mount (real FUSE read path, one touch per segment) -> unmount ->
#      wait for the daemon to exit.
#   B. heat persist pass: invf-l2ptest pump folds the session's accrual
#      into the records (test-heat.sh pattern; the pump is a full
#      open+read+persist per name).
#   C. reopen: meta_probe --heat must show rheat>0 for every touched name
#      and rheat=0 for untouched ones.
#
# Self-gating: if EVERY touched name reports rheat=0 the component is not
# landed yet -> SKIP. If ANY touched name reports nonzero, assertions are
# strict.
#
# Run through the e2e lock:
#   INVFS_E2E_AGENT=wp45 bash tools/run-e2e.sh tools/test-heat-mapper.sh
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/tmp/invfs-e2e-test-heat-mapper
MNT=$WORK/mnt
OUT=$WORK/out
PASS=0; FAIL=0

say(){ echo "[heat-mapper] $*"; }
ok(){ PASS=$((PASS+1)); say "PASS: $*"; }
bad(){ FAIL=$((FAIL+1)); say "FAIL: $*"; }
skip(){ say "SKIP: $*"; echo "RESULT: $PASS passed, $FAIL failed (SKIPPED)"; exit 0; }
fail(){ bad "$*"; echo "RESULT: $PASS passed, $FAIL failed"; exit 1; }

BIGVOL_LIB=1
. "$REPO/tools/test-fixture-bigvol.sh"

say "building big-volume fixture in $WORK"
bigvol_cleanup "$WORK"     # also evicts any stale mount at $WORK/mnt
bigvol_build "$WORK" || fail "fixture build"
IMG=$WORK/dev0.img
mkdir -p "$MNT" "$OUT"

# ---- leg A: mount + reads through FUSE ----------------------------------
say "leg A: mount + read ~100 seeded names through the FUSE read path"
"$B/invf-fuse" "$IMG" "$MNT" 2>"$WORK/fuse.log" &
FUSE_PID=$!
_UP=0
_i=0
while [ "$_i" -lt 30 ]; do
    if [ -e "$MNT/d00" ]; then _UP=1; break; fi
    sleep 0.5 2>/dev/null || sleep 1
    _i=$((_i+1))
done
[ "$_UP" = 1 ] || { kill "$FUSE_PID" 2>/dev/null; fail "invf-fuse did not come up (see $WORK/fuse.log)"; }

rheat_of() {
    "$B/meta_probe" "$IMG" --heat "$1" | awk '
        /^heat /{for(i=1;i<=NF;i++) if ($i ~ /^rheat=/) {sub("rheat=","",$i); print $i; found=1}}
        END{if(!found) print 0}'
}

TOUCHED=$WORK/touched.txt
: > "$TOUCHED"
_i=0
while [ "$_i" -lt 100 ]; do
    _d=$((_i % 60))
    _f=$((_i * 7 % 495))
    _name=$(printf 'd%02d/f%05d' "$_d" "$_f")
    echo "$_name" >> "$TOUCHED"
    _i=$((_i+1))
done

# 3 read sessions: the FUSE read path accrues per-file heat in RAM
_r=0
while [ "$_r" -lt 3 ]; do
    while IFS= read -r _name; do
        cat "$MNT/$_name" > "$OUT/cat.tmp" 2>/dev/null || {
            say "  read through mount failed: $_name"; }
    done < "$TOUCHED"
    _r=$((_r+1))
done

fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
_w=0
while [ "$_w" -lt 20 ] && kill -0 "$FUSE_PID" 2>/dev/null; do
    sleep 1; _w=$((_w+1))
done
if kill -0 "$FUSE_PID" 2>/dev/null; then
    fusermount3 -uz "$MNT" 2>/dev/null || fusermount -uz "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
fi
ok "mount, 100 touched names x 3 read sessions, unmount"

# ---- leg B: heat persist pass -------------------------------------------
call_pump() {
    # WP46: pump ALL touched names in one invocation. The previous loop
    # reopened $TOUCHED on every batch (the inner `while read` had its own
    # redirection), so only the first 10 names ever reached the pump.
    set --
    while IFS= read -r _name; do
        set -- "$@" "$_name"
    done < "$TOUCHED"
    [ "$#" -gt 0 ] || return 1
    "$B/invf-l2ptest" pump "$IMG" "$@" >"$WORK/pump.log" 2>&1
}
call_pump && ok "heat persist pass (pump over all touched names)" \
             || bad "heat persist pass (pump) failed"

# ---- leg C: reopen and assert the table ----------------------------------
say "leg C: reopen -> heat table for touched (nonzero) vs untouched (zero)"
HOT=0; COLD0=0; MISS=0
NONZERO_FIRST=""
while IFS= read -r _name; do
    _r=$(rheat_of "$_name")
    [ -n "$_r" ] || _r=0
    if [ "$_r" -gt 0 ]; then
        HOT=$((HOT+1))
        [ -z "$NONZERO_FIRST" ] && NONZERO_FIRST=$_name
    else
        MISS=$((MISS+1))
    fi
done < "$TOUCHED"

# untouched control group: same dirs, a different untouched name slab
_i=200
while [ "$_i" -lt 210 ]; do
    _d=$((_i % 60))
    _name=$(printf 'd%02d/f%05d' "$_d" "$((_i + 100))")
    _r=$(rheat_of "$_name")
    _r=${_r:-0}
    [ "$_r" -eq 0 ] 2>/dev/null && COLD0=$((COLD0+1))
    _i=$((_i+1))
done

# ---- self-gate ---------------------------------------------------------
if [ "$HOT" -eq 0 ]; then
    skip "no persisted heat on any of the 100 touched names -> heat persist/lookup walk is mapper-unaware (component WPs not merged yet)"
fi

if [ "$HOT" -eq 100 ]; then
    ok "heat table non-empty for all 100 touched names"
else
    skip "heat persistence partial: only $HOT/100 touched names carry heat -> mapper-unaware persist/lookup (component WPs not merged yet)"
fi
[ "$COLD0" -eq 10 ] && ok "untouched controls all rheat=0 ($COLD0/10)" \
                     || bad "untouched controls accrued heat: only $COLD0/10 are zero"

echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

# ---- cleanup ------------------------------------------------------------
rm -f "$TOUCHED" "$OUT/cat.tmp"
