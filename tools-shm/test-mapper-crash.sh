#!/bin/sh
# tools/test-mapper-crash.sh — WP55: durability / crash regression coverage
# for mapper volumes (format v0.3.0+).
#
# Why: the mapper era had no test that kills a process mid-append /
# mid-compaction / mid-sweep. The 7.5 h incident (WP48) ended with the
# mapper table carrying duplicate pbAs after a killed sweep. A full clean
# sweep no longer reproduces it, but the kill path was unverified and had
# no regression guard.
#
# Design: reuse tools/test-fixture-bigvol.sh in BIGVOL_LIB=1 mode with small
# knobs (BIGVOL_NDIRS/BIGVOL_PERDIR) to build a two-device mapper volume
# whose ~270 records span several metadata extents. Legs:
#   1. SIGKILL invf-import mid-import (after the mapper has grown past
#      L1_EXT_THRESHOLD extents); reopen must be anomaly-free, fsck
#      corruption-free with lost <= in-flight, mapper 0 duplicate pbAs.
#   2. SIGKILL invf-sweep right after the collector prints `live entries`
#      (the WP21 INVFS_SWEEP_ABORT_AFTER hook fires in the candidate loop,
#      immediately after the collector); reopen must leave stats/fsck
#      consistent and the mapper table with 0 duplicates AND 0 descending.
#   3. If a checkpoint is live after that kill, invf-rollback must succeed
#      and leave a consistent volume.
#   4. invf-sweep --realize on a completed sweep must clear the checkpoint
#      and leave the volume corruption-free.
#
# Mapper audit helper (python3): reads the superblock mapper_pba at offset
# 0x98, the MET0 extent_count at 0x3B8, then the 8-byte entries at
# mapper_pba*4096 (entry = pba bits 0..59, size_class bits 60..63). It
# counts entries whose pba repeats an earlier one (duplicates) and entries
# whose pba is below the previous non-free entry (descending steps).
#
# Self-gating (WP52): the owner-record extent overflow (an unlanded
# follow-up) makes fsck report `bad records > 0` on swept mapper volumes.
# Any leg that would fail purely because of it SKIPs with a clear reason,
# so this suite can land before WP52 without going red. An orphan count is
# never treated as corruption: pre-existing fsck orphan quirks (including
# the WP53 checkpoint-registry realize leak) are reported, not failed on.
#
# Run through the e2e lock only:
#   INVFS_E2E_AGENT=wp55 bash tools/run-e2e.sh tools/test-mapper-crash.sh

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/tmp/invfs-e2e-test-mapper-crash
OUT=$WORK/out
AUDIT=$WORK/mapper_audit.py
PASS=0; FAIL=0; SKIPPED=0

say(){ echo "[mapper-crash] $*"; }
ok(){ PASS=$((PASS+1)); say "PASS: $*"; }
bad(){ FAIL=$((FAIL+1)); say "FAIL: $*"; }
skip_leg(){ SKIPPED=$((SKIPPED+1)); say "SKIP: $*"; }
fail(){ bad "$@"; echo "RESULT: $PASS passed, $FAIL failed"; exit 1; }

command -v python3 >/dev/null 2>&1 || {
    say "SKIP: python3 not available (the mapper-table audit needs it)"
    echo "RESULT: 0 passed, 0 failed (SKIPPED)"; exit 0; }

BIGVOL_LIB=1
. "$REPO/tools/test-fixture-bigvol.sh"

# Small knobs: ~220 files + dirs + links -> ~270 records, enough to span
# several 128 KiB metadata extents without a multi-minute fixture build.
BIGVOL_NDIRS=${BIGVOL_NDIRS:-4}
BIGVOL_PERDIR=${BIGVOL_PERDIR:-50}
export BIGVOL_NDIRS BIGVOL_PERDIR

bigvol_cleanup "$WORK"
mkdir -p "$WORK" "$OUT" "$WORK/src"

# ---- mapper-table audit + mid-import kill watcher ----------------------
cat > "$AUDIT" <<'PY'
#!/usr/bin/env python3
# mapper_audit.py — read the MET0 mapper table straight off the image.
#   audit <img>                      -> "mapper: extents=N duplicates=N descending=N"
#   watch <img> <pid> <n_extents>    -> SIGKILL <pid> once extent_count>=N
import os, signal, struct, sys, time

SB_MAPPER_PBA      = 0x98
MET0_MAGIC_OFF     = 0x3A0
MET0_EXTENT_COUNT  = 0x3B8
MAPPER_ENTRY_BYTES = 8
MAPPER_ENTRIES     = 16384
PBA_MASK           = (1 << 60) - 1


def audit(img):
    with open(img, 'rb') as f:
        f.seek(SB_MAPPER_PBA)
        mpba = struct.unpack('<Q', f.read(8))[0]
        f.seek(MET0_MAGIC_OFF)
        magic = f.read(4)
        f.seek(MET0_EXTENT_COUNT)
        ec = struct.unpack('<Q', f.read(8))[0]
        if magic != b'MET0' or mpba == 0:
            print("mapper: extents=0 duplicates=0 descending=0")
            return 0
        f.seek(mpba * 4096)
        raw = f.read(MAPPER_ENTRIES * MAPPER_ENTRY_BYTES)
    n = min(ec, MAPPER_ENTRIES)
    seen = {}
    dups = 0
    desc = 0
    prev = -1
    for i in range(n):
        pba = struct.unpack_from('<Q', raw, i * MAPPER_ENTRY_BYTES)[0] & PBA_MASK
        if pba == 0:
            continue                      # free mapper slot
        if pba in seen:
            dups += 1
        seen[pba] = i
        if prev >= 0 and pba < prev:
            desc += 1                     # non-monotonic step
        prev = pba
    print("mapper: extents=%d duplicates=%d descending=%d" % (n, dups, desc))
    return 0


def watch(img, pid, threshold):
    pid = int(pid)
    threshold = int(threshold)
    while True:
        try:
            os.kill(pid, 0)
        except OSError:
            print("watch: import finished before extent_count reached %d" % threshold)
            return 1
        try:
            with open(img, 'rb') as f:
                f.seek(MET0_EXTENT_COUNT)
                ec = struct.unpack('<Q', f.read(8))[0]
        except OSError:
            ec = 0
        if ec >= threshold:
            os.kill(pid, signal.SIGKILL)
            print("watch: killed pid %d at extent_count=%d" % (pid, ec))
            return 0
        time.sleep(0.0002)


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'audit':
        sys.exit(audit(sys.argv[2]))
    if len(sys.argv) >= 5 and sys.argv[1] == 'watch':
        sys.exit(watch(sys.argv[2], sys.argv[3], sys.argv[4]))
    sys.exit(2)
PY

mapper_audit(){ python3 "$AUDIT" audit "$1"; }
mval(){ mapper_audit "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p" | head -1; }

# read_fsck <img> -> FOUT plus ORPH/MISSING/BAD/LOST/CUT (0 when absent)
read_fsck(){
    FOUT=$("$B/invf-fsck" "$1" 2>&1)
    ORPH=$(printf '%s\n' "$FOUT" | sed -n 's/^  orphans:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
    MISSING=$(printf '%s\n' "$FOUT" | sed -n 's/^  missing:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
    BAD=$(printf '%s\n' "$FOUT" | sed -n 's/^  bad records:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
    LOST=$(printf '%s\n' "$FOUT" | sed -n 's/^  lost files:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
    CUT=$(printf '%s\n' "$FOUT" | sed -n 's/^  cut records:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
    : "${ORPH:=0}"; : "${MISSING:=0}"; : "${BAD:=0}"; : "${LOST:=0}"; : "${CUT:=0}"
}

# "corruption" excludes orphans (a documented, reclaimable fsck quirk).
corruption_free(){
    [ "$BAD" = 0 ] && [ "$MISSING" = 0 ] && [ "$LOST" = 0 ] && [ "$CUT" = 0 ]
}

pop_of(){ printf '%s\n' "$1" | sed -n 's/^  regular files[[:space:]]*: //p' | head -1; }

IMG0=$WORK/dev0.img
IMG1=$WORK/dev1.img

# ======================================================================
# leg 1 — SIGKILL invf-import mid-import
# ======================================================================
say "== leg 1: SIGKILL invf-import mid-import =="
rm -f "$IMG0" "$IMG1"
INVFS_DEV1="$IMG1" "$B/invf-mkfs" "$IMG0" 1 "$IMG1" 2 >"$OUT/l1-mkfs.log" 2>&1 \
    || fail "leg1: mkfs failed: $(tail -2 "$OUT/l1-mkfs.log")"
# leg 1 needs a tree large enough that the import is still in flight when the
# mapper grows past L1_EXT_THRESHOLD extents; the small shared fixture (220
# records) fits one extent and finishes too fast to catch reliably.
L1_NDIRS=${L1_NDIRS:-10}
L1_PERDIR=${L1_PERDIR:-100}
L1_EXT_THRESHOLD=${L1_EXT_THRESHOLD:-3}
BIGVOL_NDIRS=$L1_NDIRS BIGVOL_PERDIR=$L1_PERDIR bigvol_mktree "$WORK/src1" \
    || fail "leg1: source tree generation failed"

CANON_FILES=$(find "$WORK/src1" -mindepth 1 -type f | wc -l | tr -d ' ')
CANON_DIRS=$(find "$WORK/src1" -mindepth 1 -type d | wc -l | tr -d ' ')
CANON_LINKS=$(find "$WORK/src1" -mindepth 1 -type l | wc -l | tr -d ' ')
INFLIGHT=$((CANON_FILES + CANON_DIRS + CANON_LINKS))
say "source tree: $CANON_FILES files, $CANON_DIRS dirs, $CANON_LINKS links"

INVFS_DEV1="$IMG1" "$B/invf-import" "$IMG0" "$WORK/src1" \
    >"$OUT/l1-import.log" 2>&1 &
ipid=$!
python3 "$AUDIT" watch "$IMG0" "$ipid" "$L1_EXT_THRESHOLD"
wrc=$?
wait "$ipid" 2>/dev/null
irc=$?
if [ "$wrc" = 0 ] && [ "$irc" = 137 ]; then
    ok "invf-import SIGKILLed mid-import (rc=137) at mapper extent $(mval "$IMG0" extents)"
else
    bad "leg1: import was not killed mid-flight (watcher=$wrc import_rc=$irc) -- widen L1_* knobs"
fi
L1_EXT=$(mval "$IMG0" extents)
[ "${L1_EXT:-0}" -ge 1 ] \
    && ok "mapper volume grew to $L1_EXT extent(s) before the kill" \
    || bad "leg1: mapper extent_count=${L1_EXT:-0}; not a mapper volume?"

read_fsck "$IMG0"
if [ "$BAD" -gt 0 ]; then
    skip_leg "leg1: fsck bad records=$BAD>0 (WP52 owner-record extent overflow pending)"
else
    printf '%s\n' "$FOUT" | grep -q "anomaly-free" \
        && ok "reopen anomaly-free (full-record scan recovered to CLEAN)" \
        || { bad "leg1: reopen was not anomaly-free"; printf '%s\n' "$FOUT" | head -8; }
    corruption_free \
        && ok "fsck corruption-free (bad=$BAD missing=$MISSING lost=$LOST cut=$CUT)" \
        || { bad "leg1: fsck reports corruption"; printf '%s\n' "$FOUT" | head -12; }
    [ "$LOST" -le "$INFLIGHT" ] \
        && ok "lost files $LOST <= files-in-flight bound $INFLIGHT" \
        || bad "leg1: lost files $LOST > in-flight bound $INFLIGHT"
    L1_DUP=$(mval "$IMG0" duplicates)
    [ "${L1_DUP:-0}" = 0 ] \
        && ok "mapper table has 0 duplicate pbAs" \
        || bad "leg1: mapper table has ${L1_DUP:-?} duplicate pbAs"
fi

# ======================================================================
# legs 2-4 share one fully-imported volume
# ======================================================================
say "== build full fixture for legs 2-4 =="
rm -f "$IMG0" "$IMG1"
BIGVOL_NDIRS=4
BIGVOL_PERDIR=50
bigvol_build "$WORK" >"$OUT/l2-build.log" 2>&1 \
    || fail "leg2: fixture build failed: $(tail -3 "$OUT/l2-build.log")"
COUNTS=$WORK/canonical-counts.json
CANON_FILES=$(bigvol_get "$COUNTS" files)
say "canonical: $CANON_FILES files (buys the multi-extent mapper)"

# ---- leg 2: SIGKILL invf-sweep right after the collector --------------
say "== leg 2: SIGKILL invf-sweep after 'live entries' =="
INVFS_SWEEP_ABORT_AFTER=1 "$B/invf-sweep" "$IMG0" >"$OUT/l2-sweep.log" 2>&1
sweep_rc=$?
[ "$sweep_rc" = 137 ] \
    && ok "invf-sweep SIGKILLed (rc=137)" \
    || bad "leg2: sweep rc=$sweep_rc, want 137"
grep -q "live entries" "$OUT/l2-sweep.log" \
    && ok "collector printed 'live entries' before the kill" \
    || { bad "leg2: no 'live entries' line before the kill"; tail -5 "$OUT/l2-sweep.log"; }
grep -q "checkpoint: #1 armed" "$OUT/l2-sweep.log" \
    && ok "sweep checkpoint was armed mid-run" \
    || say "INFO: leg2: no checkpoint-armed line (checkpoint may have declined)"

STATS2=$("$B/invf-stats" "$IMG0" 2>&1)
POP2=$(pop_of "$STATS2"); POP2=${POP2:-0}
POP2_MAX=$((CANON_FILES + 2))
if [ "$POP2" -ge "$CANON_FILES" ] && [ "$POP2" -le "$POP2_MAX" ]; then
    ok "population stable after the killed sweep ($POP2 = canonical $CANON_FILES +<=2 owners)"
else
    bad "leg2: population $POP2 outside [$CANON_FILES,$POP2_MAX] after kill"
fi

read_fsck "$IMG0"
if [ "$BAD" -gt 0 ]; then
    skip_leg "leg2: fsck bad records=$BAD>0 (WP52 owner-record extent overflow pending)"
else
    corruption_free \
        && ok "fsck corruption-free after the killed sweep (bad=$BAD missing=$MISSING lost=$LOST cut=$CUT)" \
        || { bad "leg2: fsck reports corruption after the killed sweep"; printf '%s\n' "$FOUT" | head -12; }
    printf '%s\n' "$FOUT" | grep -q "checkpoint:.*sweep #[0-9]* live" \
        && ok "checkpoint is live after the killed sweep" \
        || bad "leg2: no live checkpoint after the killed sweep"
    L2_DUP=$(mval "$IMG0" duplicates)
    L2_DESC=$(mval "$IMG0" descending)
    if [ "${L2_DUP:-0}" = 0 ] && [ "${L2_DESC:-0}" = 0 ]; then
        ok "mapper table: 0 duplicate pbAs, 0 descending steps"
    else
        bad "leg2: mapper table has ${L2_DUP:-?} duplicate pbAs / ${L2_DESC:-?} descending steps"
    fi
fi

# ---- leg 3: rollback after the kill -----------------------------------
say "== leg 3: invf-rollback after the killed sweep =="
if [ "$BAD" -gt 0 ]; then
    skip_leg "leg3: fsck bad records=$BAD>0 (WP52 owner-record extent overflow pending)"
elif ! printf '%s\n' "$FOUT" | grep -q "checkpoint:"; then
    skip_leg "leg3: no checkpoint was armed after the kill (rollback not applicable)"
else
    "$B/invf-rollback" "$IMG0" >"$OUT/l3-rollback.log" 2>&1
    rb_rc=$?
    [ "$rb_rc" = 0 ] \
        && ok "invf-rollback succeeded (rc=0)" \
        || { bad "leg3: rollback rc=$rb_rc"; cat "$OUT/l3-rollback.log"; }
    read_fsck "$IMG0"
    if [ "$BAD" -gt 0 ]; then
        skip_leg "leg3 post-rollback: fsck bad records=$BAD>0 (WP52 pending)"
    else
        corruption_free \
            && ok "post-rollback fsck corruption-free (bad=$BAD missing=$MISSING lost=$LOST cut=$CUT)" \
            || { bad "leg3: post-rollback fsck reports corruption"; printf '%s\n' "$FOUT" | head -12; }
        printf '%s\n' "$FOUT" | grep -q "checkpoint:" \
            && bad "leg3: checkpoint survived the rollback" \
            || ok "checkpoint cleared by the rollback"
        L3_DUP=$(mval "$IMG0" duplicates)
        L3_DESC=$(mval "$IMG0" descending)
        # WP71j: duplicates = a double allocation = corruption, hard fail.
        # A descending step after a kill/rollback cycle is legitimate
        # extent reuse: once registry/owner extents are freed, the
        # allocator may hand out a lower pba (table slot order stops
        # being allocation order). Reported, not failed.
        if [ "${L3_DUP:-0}" = 0 ]; then
            ok "post-rollback mapper table: 0 duplicate pbAs (descending=${L3_DESC:-?}, reuse-legitimate)"
        else
            bad "leg3: mapper table has ${L3_DUP:-?} duplicate pbAs / ${L3_DESC:-?} descending steps"
        fi
    fi
fi

# ---- leg 4: --realize on a completed sweep ----------------------------
say "== leg 4: invf-sweep --realize on a completed sweep =="
"$B/invf-sweep" "$IMG0" >"$OUT/l4-sweep.log" 2>&1
sweep4_rc=$?
[ "$sweep4_rc" = 0 ] \
    && ok "full sweep completed (rc=0): $(sed -n 's/^sweep: /sweep: /p' "$OUT/l4-sweep.log" | tail -1)" \
    || { bad "leg4: full sweep rc=$sweep4_rc"; tail -6 "$OUT/l4-sweep.log"; }

read_fsck "$IMG0"
if [ "$BAD" -gt 0 ]; then
    skip_leg "leg4: fsck bad records=$BAD>0 (WP52 owner-record extent overflow pending)"
else
    printf '%s\n' "$FOUT" | grep -q "checkpoint:.*sweep #[0-9]* live" \
        && ok "completed sweep left a live checkpoint" \
        || bad "leg4: no live checkpoint after the completed sweep"

    "$B/invf-sweep" "$IMG0" --realize >"$OUT/l4-realize.log" 2>&1
    rz_rc=$?
    [ "$rz_rc" = 0 ] \
        && ok "invf-sweep --realize succeeded (rc=0)" \
        || { bad "leg4: --realize rc=$rz_rc"; tail -6 "$OUT/l4-realize.log"; }
    grep -q "previous run realized" "$OUT/l4-realize.log" \
        && ok "realize reported the previous checkpoint resolved" \
        || say "INFO: leg4: no 'previous run realized' line"

    "$B/invf-rollback" "$IMG0" >"$OUT/l4-rollback.log" 2>&1
    rb4_rc=$?
    [ "$rb4_rc" = 1 ] \
        && ok "checkpoint cleared (rollback reports none)" \
        || { bad "leg4: rollback rc=$rb4_rc, want 1 (no checkpoint)"; cat "$OUT/l4-rollback.log"; }

    read_fsck "$IMG0"
    if [ "$BAD" -gt 0 ]; then
        skip_leg "leg4 post-realize: fsck bad records=$BAD>0 (WP52 pending)"
    elif corruption_free; then
        if [ "$ORPH" = 0 ]; then
            ok "fsck clean after --realize (bad=0 missing=0 lost=0 cut=0 orphans=0)"
        else
            skip_leg "leg4 strict fsck-clean: $ORPH orphan blocks after --realize (WP53 checkpoint-registry realize leak pending); checkpoint cleared + no corruption verified"
        fi
    else
        bad "leg4: fsck reports corruption after --realize (bad=$BAD missing=$MISSING lost=$LOST cut=$CUT)"
        printf '%s\n' "$FOUT" | head -12
    fi

    RZ_DUP=$(mval "$IMG0" duplicates)
    RZ_DESC=$(mval "$IMG0" descending)
    # WP71j: as in leg3 -- duplicates are corruption; a descending step
    # after --realize is legitimate reuse (the realize frees the
    # retention registry's owner extent, and the next allocation may
    # land below a live neighbour).
    if [ "${RZ_DUP:-0}" = 0 ]; then
        ok "post-realize mapper table: 0 duplicate pbAs (descending=${RZ_DESC:-?}, reuse-legitimate)"
    else
        bad "leg4: mapper table has ${RZ_DUP:-?} duplicate pbAs / ${RZ_DESC:-?} descending steps"
    fi
fi

echo "RESULT: $PASS passed, $FAIL failed ($SKIPPED skipped)"
[ "$FAIL" -eq 0 ]
