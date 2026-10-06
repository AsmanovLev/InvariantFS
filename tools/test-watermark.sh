#!/bin/bash
# test-watermark.sh — WP26 watermark-triggered early sweep e2e, v3 SPT0
# (persistent regression).
#
# Mount option raw_watermark=<pct> (env INVFS_RAW_WATERMARK fallback)
# makes the FUSE background sweep thread kick a full sweep pass whenever
# the RAW-zone fill exceeds <pct>% — the first rung of the pressure
# ladder (WP23 added the adaptive write-effort rungs; this one reclaims).
#
# On Meta-v3 (the default format) the pass's rollback window is the SPT0
# save point (WP77): the worker drops any previous pass's save point
# (K=1) and captures a fresh one {base_root, delta_end} before the walk,
# printing "[watermark] save point captured (...)". invf-rollback
# restores that save point and reports "rolled back to save point".
# The v2 CKP0/retention machinery is retired and is not exercised here.
#
#   Leg 0: lazy default — no option, fill past 25% staged offline,
#          mounted 4s: no watermark kick, no save point, fill unchanged.
#   Leg 1: trigger — raw_watermark=25, fill ~45% staged OFFLINE (so the
#          trigger is attributable to the watermark: the daemon's
#          every-second pending drain would relieve mounted writes on
#          its own; offline-staged files are invisible to it). The
#          daemon sweeps with NO explicit invf-sweep call and captures a
#          v3 save point. The fill does NOT drop at this generation —
#          the window it just armed is a hold on the pre-sweep
#          generation's blocks — so leg 1 asserts the pin.
#   Leg 1b: on its OWN image, the next bare sweep discharges that hold: a
#          non-zero reclaim, RAW fill back under the mark, files still
#          bit-exact. (A separate image on purpose: it leaves IMGA's
#          rollback chain — legs 2 and 3 — exactly as it was.)
#   Leg 1c: the same discharge with the DAEMON ALONE — no invf-sweep, no
#          write, no signal, no unmount between the passes. The re-arm rule
#          used to be satisfied only by a hand-run sweep, so the volume
#          never recovered under daemon-only care. Asserts the reclaim, the
#          pass count, the settle (no re-pinning), and bit-exactness.
#   Leg 2: an offline sweep with --no-realize keeps the live save point
#          (the rollback window survives an unrelated maintenance run).
#   Leg 3: rollback undoes the watermark sweep — the swept file is
#          bit-exact and fsck is clean.
#   Leg 4: fresh image, one watermark pass, rollback -> bit-exact,
#          fsck clean.
#   Leg 5: INVFS_RAW_WATERMARK env fallback arms the same machinery.
#
# Run via the global e2e lock:  bash tools/run-e2e.sh tools/test-watermark.sh
# Uses /dev/shm like the other soak scripts. NOTE: blkio treats /dev/*
# paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoint is absolute).
# The daemon runs FOREGROUND (-f) under setsid: after fuse_daemonize()
# the sweep thread's stderr goes to /dev/null, and the watermark pass's
# log lines ("[watermark] save point captured") are the test's
# observability.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp26watermark
IMGA=wp26wm-a.img    # legs 1-3: trigger, offline maintenance, rollback
IMGB=wp26wm-b.img    # leg 4: rollback undoes the watermark sweep
IMGC=wp26wm-c.img    # leg 5: env fallback
IMGD=wp26wm-d.img    # leg 1b: the pin's blocks come back on the next sweep
IMGE=wp26wm-e.img    # leg 1c: the ladder discharges its own debt, daemon-only
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out"
cd /dev/shm
rm -f "$IMGA" "$IMGB" "$IMGC" "$IMGD" "$IMGE"

fail() { echo "FAIL: $*" >&2; exit 1; }

# engine-measured RAW-zone fill in blocks (the truth the daemon reads)
raw_used() { $B/meta_probe "$1" --zonefree 2>/dev/null \
    | awk '/^raw /{split($2,a,"/"); print a[1]}'; }

DPID=0
mnt_up() {   # <log-tag> <img> [ENV=VAL ...] [-- extra mount args...]
    local tag=$1 img=$2; shift 2
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    setsid env "${envs[@]}" $B/invf-fuse -f "$@" "$img" "$MNT" \
        >"$WORK/fuse.$tag.log" 2>&1 < /dev/null &
    DPID=$!
    disown
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $img never appeared"
}

mnt_down() { # <img>
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    # the unmounted daemon still holds the image open; offline probes
    # must wait for the process to die (two writers = corrupt volume)
    for _ in $(seq 1 300); do
        kill -0 "$DPID" 2>/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

wait_log() { # <log-tag> <pattern> <seconds> — poll the daemon log
    local n=$(( $3 * 2 ))
    for _ in $(seq 1 "$n"); do
        grep -q "$2" "$WORK/fuse.$1.log" && return 0
        sleep 0.5
    done
    echo "---- fuse.$1.log ----" >&2; cat "$WORK/fuse.$1.log" >&2
    fail "log $1 never showed: $2"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    [ "$DPID" != 0 ] && kill "$DPID" 2>/dev/null || true
    pkill -f "invf-fuse.*wp26wm" 2>/dev/null || true
}
trap cleanup EXIT

# ---- fixtures ---------------------------------------------------------
# fat filler: random-word text (~1.6:1 under LZ4: 256KB -> ~41 blocks).
# 120, not 80: five legs each stage a 45% fill of their own image, and
# leg 1c added a sixth consumer of the same pool. (It is really five images;
# a 45% fill of the 1535-block RAW zone costs ~17 fillers, so 80 ran out
# mid-suite.)
python3 - "$WORK/ref" <<'PY'
import random, sys
d = sys.argv[1]
rnd = random.Random(26)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(120):
    with open('%s/fat%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
print("  fixtures: 120 fat fillers (256KB each)")
PY

# ---- geometry ----------------------------------------------------------
$B/invf-mkfs "$IMGA" 0.0625 > "$WORK/mkfs.log"
RAWBLOCKS=$(sed -n 's/.*raw zone:.*(\([0-9]*\) blocks,.*/\1/p' "$WORK/mkfs.log")
[ -n "$RAWBLOCKS" ] || fail "could not parse raw zone size"
T25=$(( RAWBLOCKS * 25 / 100 ))
T45=$(( RAWBLOCKS * 45 / 100 ))
echo "  RAW zone: $RAWBLOCKS blocks (25% = $T25, 45% = $T45)"

FATN=0
fill_to() { # <img> <target-blocks> — offline fillers until fill >= target
    local f u
    u=$(raw_used "$1")
    while [ "$u" -lt "$2" ]; do
        f=$(printf 'fat%02d.txt' "$FATN"); FATN=$((FATN+1))
        [ -f "$WORK/ref/$f" ] || fail "filler pool exhausted at $u blocks"
        $B/invf-cp "$1" "$WORK/ref/$f" "$f" >/dev/null
        u=$(raw_used "$1")
    done
    echo "  fill: $u/$RAWBLOCKS blocks (engine-measured)"
}

echo
echo "== [0] lazy default: no option -> no kick, no save point =="
fill_to "$IMGA" "$T45"
LAZYFILL=$(raw_used "$IMGA")
mnt_up lazy "$IMGA"
sleep 4
mnt_down "$IMGA"
grep -q "kicking a sweep pass" "$WORK/fuse.lazy.log" \
    && fail "lazy: watermark fired without the option"
grep -q "save point captured" "$WORK/fuse.lazy.log" \
    && fail "lazy: a save point was captured without the option"
[ "$(raw_used "$IMGA")" = "$LAZYFILL" ] \
    || fail "lazy: fill changed without the option"
echo "  no kick, no save point, fill unchanged at $LAZYFILL/$RAWBLOCKS"

echo
echo "== [1] raw_watermark=25: daemon sweeps past the mark on its own =="
mnt_up a1 "$IMGA" -- -o raw_watermark=25
wait_log a1 "save point captured" 30
wait_log a1 "DONE found=[0-9]* files=" 60
grep -q "\[watermark\] RAW fill .* over 25%: kicking a sweep pass" \
    "$WORK/fuse.a1.log" || fail "no watermark kick logged"
grep -q "DONE found=[0-9]* files=" "$WORK/fuse.a1.log" \
    || fail "watermark pass did not finish"
# The DONE line reports found= and files= side by side precisely so a
# truncated collection is visible: found is what the walk SAW, files is what
# it handed back, and found > files means the sweep only covered a prefix.
# The old line printed files= alone, so a pass that silently swept a subset
# was indistinguishable from one that swept everything.
_a1_line=$(grep -ao "DONE found=[0-9]* files=[0-9]*" "$WORK/fuse.a1.log" | head -1)
_a1_found=$(printf '%s' "$_a1_line" | sed -n 's/.*found=\([0-9]*\).*/\1/p')
_a1_files=$(printf '%s' "$_a1_line" | sed -n 's/.*files=\([0-9]*\).*/\1/p')
[ -n "$_a1_found" ] && [ -n "$_a1_files" ] \
    || fail "could not read found=/files= from the DONE line: $_a1_line"
[ "$_a1_found" -eq "$_a1_files" ] \
    || fail "the sweep collected a PREFIX: found $_a1_found > swept $_a1_files"
echo "  pass 1: kicked by the daemon, save point captured (no invf-sweep ran)"
# The pass armed a rollback window, and a window is a HOLD: a capture pins
# every block the PRE-sweep generation's recipes named (spt0_pin_take ->
# vol_iter_inodes_at, src/core/vol_spt0.c:795) and only the NEXT
# capture's reclaim pass gives them back (spn_reclaim,
# src/core/vol_spt0.c:672, called from spt0_pin_take at :814 -- it frees
# where the old mark set has a block, the new one does not, and the bitmap
# still has it, :676). This leg used to assert the fill dropped below the
# 25% mark after ONE pass, which can only hold on a volume that has lost
# the window the pass just armed. Assert the pin instead: it is what makes
# the flat fill explicable, and its absence is the real defect.
PINNED=$(sed -n 's/.*save point: pinned \([0-9][0-9]*\) blocks.*/\1/p' \
    "$WORK/fuse.a1.log" | head -1)
[ -n "$PINNED" ] && [ "$PINNED" -gt 0 ] \
    || fail "the watermark pass armed no data pin; a flat fill would then be a broken reclaim, not a live window"
mnt_down "$IMGA"
FILLNOW=$(raw_used "$IMGA")
echo "  the window pinned $PINNED blocks; RAW fill $FILLNOW/$RAWBLOCKS (held by the pin)"
[ "$FILLNOW" = "$LAZYFILL" ] \
    || fail "RAW fill moved during the pass: $LAZYFILL -> $FILLNOW; the pin should be holding the pre-sweep generation"
fsck_ok "$IMGA"

echo
echo "== [1b] the space comes back on the NEXT bare sweep (one-generation pin) =="
# The reclamation leg 1 used to imply and never measured. A bare sweep
# drops the previous window and captures a fresh one (invf-sweep.c:1687
# then :1689), and that capture's spn_reclaim discharges the daemon pass's
# debt. Asserted as a RECLAIM (a non-zero "[spt0] reclaim:" line) AND as
# a FILL below the mark: "the log said it reclaimed" proved nothing once
# already, which is why spn_free_counted (src/core/vol_spt0.c:659) counts
# the blocks the bitmap changed rather than a run length.
#
# Its own image on purpose. Sweeping IMGA here would work, but it would
# also leave IMGA's live window one generation further along, and legs 2
# and 3 are about the window the DAEMON armed -- so keep that chain as it
# was and measure the reclaim where it does not perturb anything.
$B/invf-mkfs "$IMGD" 0.0625 >/dev/null
FIRSTD=$(printf 'fat%02d.txt' "$FATN")
fill_to "$IMGD" "$T45"
mnt_up d1 "$IMGD" -- -o raw_watermark=25
wait_log d1 "save point captured" 30
wait_log d1 "DONE found=[0-9]* files=" 60
PINNED_D=$(sed -n 's/.*save point: pinned \([0-9][0-9]*\) blocks.*/\1/p' \
    "$WORK/fuse.d1.log" | head -1)
[ -n "$PINNED_D" ] && [ "$PINNED_D" -gt 0 ] \
    || fail "the watermark pass on $IMGD armed no data pin"
mnt_down "$IMGD"
HELD=$(raw_used "$IMGD")
echo "  the daemon's window pinned $PINNED_D blocks; RAW fill held at $HELD/$RAWBLOCKS"
$B/invf-sweep "$IMGD" > "$WORK/sweep-d2.log" 2>&1 \
    || { cat "$WORK/sweep-d2.log"; fail "the bare sweep after the watermark pass failed"; }
RECLAIMED=$(sed -n 's/.*reclaim: \([0-9][0-9]*\) blocks.*/\1/p' \
    "$WORK/sweep-d2.log" | head -1)
[ -n "$RECLAIMED" ] && [ "$RECLAIMED" -gt 0 ] \
    || fail "the bare sweep reclaimed nothing, so the watermark pass's window is a leak, not a one-generation pin"
FILL2=$(raw_used "$IMGD")
[ "$FILL2" -lt "$T25" ] \
    || fail "sweep 2 reclaimed $RECLAIMED blocks but RAW fill is still $FILL2/$RAWBLOCKS, over the 25% mark ($T25)"
echo "  sweep 2 reclaimed the $RECLAIMED blocks the window held; RAW fill $FILL2/$RAWBLOCKS"
$B/invf-cat "$IMGD" "$FIRSTD" "$WORK/out/after2.bin" >/dev/null
cmp "$WORK/ref/$FIRSTD" "$WORK/out/after2.bin" \
    || fail "the reclaiming sweep cost bit-exactness on $FIRSTD"
fsck_ok "$IMGD"
echo "  reclaimed, and $FIRSTD is still bit-exact"

echo
echo "== [1c] the ladder repays its own debt: recovery with the DAEMON ALONE =="
# The regression leg for the self-deadlock. Leg 1b measures the same reclaim
# but pays for it with a MANUAL invf-sweep, which is the operator step the
# daemon exists to remove: a pass arms a rollback window, that window HOLDS
# the pre-sweep generation's blocks, and the fill the ladder records at the
# pass's exit is therefore the fill WITH the hold. The re-arm rule was
# "fill > the last pass's exit fill", so the hold made the re-arm unsatisfiable
# and the pass that created the debt was the last pass that ever ran. Nothing
# but the next capture reclaims (spn_reclaim, src/core/vol_spt0.c:672, called
# from spt0_pin_take at :814), so the volume stayed short until an operator
# swept it by hand -- the opposite of what a watermark-enabled mount promises.
#
# Timeline, asserted rather than narrated: from the first pass to the
# fixed point there is NO invf-sweep, NO write, NO signal and NO unmount in
# this leg. The daemon has to take the further passes by itself.
$B/invf-mkfs "$IMGE" 0.0625 >/dev/null
FIRSTE=$(printf 'fat%02d.txt' "$FATN")
fill_to "$IMGE" "$T45"
FILLE0=$(raw_used "$IMGE")
mnt_up e1 "$IMGE" -- -o raw_watermark=25
wait_log e1 "DONE found=[0-9]* files=" 120
PINNED_E=$(sed -n 's/.*save point: pinned \([0-9][0-9]*\) blocks.*/\1/p' \
    "$WORK/fuse.e1.log" | head -1)
[ -n "$PINNED_E" ] && [ "$PINNED_E" -gt 0 ] \
    || fail "the first watermark pass armed no data pin, so there is no debt to discharge"
T_START=$(date +%s)
# The discharge is a RECLAIM, and it can only come from a capture the daemon
# chose to make: nothing in this leg runs invf-sweep.
wait_log e1 "reclaim: [1-9][0-9]* blocks" 120
T_RECLAIM=$(( $(date +%s) - T_START ))
RECLAIMED_E=$(sed -n 's/.*reclaim: \([0-9][0-9]*\) blocks.*/\1/p' \
    "$WORK/fuse.e1.log" | head -1)
# ... and the ladder must then STOP on its own. The termination test is an
# EMPTY reclaim, not a pass count: a pass that superseded nothing creates no
# debt, so an empty capture proves the live window is holding only live
# blocks. Without that, "re-arm whenever the fill is high" would pass this
# leg's first half by re-pinning forever. So poll until the pass count has
# been quiet for a while -- how long the ladder needs is not what this leg is
# pinning down, that it goes QUIET is.
NP1=0; STABLE=0; LAST=-1; T_IDLE=0
while [ "$T_IDLE" -lt 180 ]; do
    sleep 2; T_IDLE=$((T_IDLE+2))
    N=$(grep -c "DONE found=[0-9]* files=" "$WORK/fuse.e1.log")
    if [ "$N" = "$LAST" ]; then STABLE=$((STABLE+2)); else STABLE=0; fi
    LAST=$N
    [ "$STABLE" -ge 16 ] && break
done
NP1=$LAST
NR1=$(grep -c "reclaim: " "$WORK/fuse.e1.log")
T_TOTAL=$(( $(date +%s) - T_START ))
[ "$STABLE" -ge 16 ] \
    || fail "the ladder never went quiet: still taking passes after 180s idle ($NP1 done, $NR1 reclaims); it is re-pinning instead of reaching a fixed point"
[ "$NP1" -ge 2 ] \
    || fail "the debt was discharged inside the pass that armed it, which would trade the rollback window for a smaller number; only $NP1 pass ran"
[ "$NR1" -ge 1 ] || fail "no reclaim line at all"
echo "  the daemon alone: $NP1 passes, reclaim $RECLAIMED_E blocks ${T_RECLAIM}s after the first, quiet ${T_TOTAL}s after it"
mnt_down "$IMGE"
FILLE1=$(raw_used "$IMGE")
[ "$FILLE1" -lt "$T25" ] \
    || fail "daemon-only recovery left RAW fill at $FILLE1/$RAWBLOCKS, still over the 25% mark ($T25); the space did not come back"
echo "  RAW fill $FILLE0/$RAWBLOCKS -> $FILLE1/$RAWBLOCKS, back under the mark, no invf-sweep anywhere in this leg"
LASTE=$(printf 'fat%02d.txt' $((FATN-1)))
$B/invf-cat "$IMGE" "$LASTE" "$WORK/out/e.bin" >/dev/null
cmp "$WORK/ref/$LASTE" "$WORK/out/e.bin" \
    || fail "the self-healing passes cost bit-exactness on $LASTE"
$B/invf-cat "$IMGE" "$FIRSTE" "$WORK/out/e0.bin" >/dev/null
cmp "$WORK/ref/$FIRSTE" "$WORK/out/e0.bin" \
    || fail "the self-healing passes cost bit-exactness on $FIRSTE"
fsck_ok "$IMGE"
echo "  bit-exact after $NP1 self-triggered passes; fsck clean"

echo
echo "== [2] offline --no-realize keeps the live save point =="
$B/invf-sweep "$IMGA" --compact --no-realize > "$WORK/compact.log" 2>&1 \
    || { cat "$WORK/compact.log"; fail "--compact --no-realize run failed"; }
grep -q "save point: kept previous" "$WORK/compact.log" \
    || { cat "$WORK/compact.log"; fail "offline sweep did not keep the save point"; }
echo "  offline sweep kept the save point (rollback window intact)"

echo
echo "== [3] rollback undoes the watermark sweep =="
$B/invf-rollback "$IMGA" > "$WORK/rb-a.log" 2>&1 || { cat "$WORK/rb-a.log"; fail "rollback failed"; }
grep -q "rolled back to save point" "$WORK/rb-a.log" \
    || fail "rollback did not report the v3 save point"
$B/invf-cat "$IMGA" fat00.txt "$WORK/out/a.bin" >/dev/null
cmp "$WORK/ref/fat00.txt" "$WORK/out/a.bin" \
    || fail "swept file not bit-exact after the rollback"
fsck_ok "$IMGA"
echo "  swept file bit-exact post-rollback; fsck clean"

echo
echo "== [4] rollback undoes the watermark sweep wholesale =="
$B/invf-mkfs "$IMGB" 0.0625 >/dev/null
fill_to "$IMGB" "$T45"
mnt_up b1 "$IMGB" -- -o raw_watermark=25
wait_log b1 "save point captured" 30
wait_log b1 "DONE found=[0-9]* files=" 60
mnt_down "$IMGB"
$B/invf-rollback "$IMGB" > "$WORK/rb-b.log" 2>&1 || { cat "$WORK/rb-b.log"; fail "rollback failed"; }
grep -q "rolled back to save point" "$WORK/rb-b.log" \
    || fail "rollback did not report the v3 save point"
LAST=$(printf 'fat%02d.txt' $((FATN-1)))
$B/invf-cat "$IMGB" "$LAST" "$WORK/out/b.bin" >/dev/null
cmp "$WORK/ref/$LAST" "$WORK/out/b.bin" \
    || fail "file not bit-exact post-rollback"
fsck_ok "$IMGB"
echo "  watermark sweep undone: bit-exact, fsck clean"

echo
echo "== [5] INVFS_RAW_WATERMARK env fallback =="
$B/invf-mkfs "$IMGC" 0.0625 >/dev/null
fill_to "$IMGC" "$T45"
mnt_up c1 "$IMGC" INVFS_RAW_WATERMARK=25
wait_log c1 "save point captured" 30
wait_log c1 "DONE found=[0-9]* files=" 60
mnt_down "$IMGC"
$B/invf-rollback "$IMGC" >/dev/null 2>&1 || fail "env-armed rollback failed"
fsck_ok "$IMGC"
echo "  env fallback armed the same machinery; rollback + fsck clean"

rm -f "$IMGA" "$IMGB" "$IMGC" "$IMGD" "$IMGE"
echo
echo "WATERMARK E2E: PASS"
