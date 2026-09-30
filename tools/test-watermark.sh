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
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out"
cd /dev/shm
rm -f "$IMGA" "$IMGB" "$IMGC" "$IMGD"

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
# fat filler: random-word text (~1.6:1 under LZ4: 256KB -> ~41 blocks)
python3 - "$WORK/ref" <<'PY'
import random, sys
d = sys.argv[1]
rnd = random.Random(26)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(80):
    with open('%s/fat%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
print("  fixtures: 80 fat fillers (256KB each)")
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
wait_log a1 "\[sweep\] DONE files=" 60
grep -q "\[watermark\] RAW fill .* over 25%: kicking a sweep pass" \
    "$WORK/fuse.a1.log" || fail "no watermark kick logged"
grep -q "\[sweep\] DONE files=" "$WORK/fuse.a1.log" \
    || fail "watermark pass did not finish"
echo "  pass 1: kicked by the daemon, save point captured (no invf-sweep ran)"
# The pass armed a rollback window, and a window is a HOLD: a capture pins
# every block the PRE-sweep generation's recipes named (spt0_pin_take ->
# vol_v3_iter_inodes_at, src/core/vol_spt0.c:745) and only the NEXT
# capture's reclaim pass gives them back (spn_reclaim,
# src/core/vol_spt0.c:652, called from spt0_pin_take at :764 -- it frees
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
# already, which is why spn_free_counted (src/core/vol_spt0.c:639) counts
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
wait_log d1 "\[sweep\] DONE files=" 60
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
wait_log b1 "\[sweep\] DONE files=" 60
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
wait_log c1 "\[sweep\] DONE files=" 60
mnt_down "$IMGC"
$B/invf-rollback "$IMGC" >/dev/null 2>&1 || fail "env-armed rollback failed"
fsck_ok "$IMGC"
echo "  env fallback armed the same machinery; rollback + fsck clean"

rm -f "$IMGA" "$IMGB" "$IMGC" "$IMGD"
echo
echo "WATERMARK E2E: PASS"
