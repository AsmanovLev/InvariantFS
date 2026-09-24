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
#          daemon sweeps with NO explicit invf-sweep call, captures a v3
#          save point, and the RAW fill drops below the mark.
#   Leg 2: an offline sweep with --no-realize keeps the live save point
#          (the rollback window survives an unrelated maintenance run).
#   Leg 3: rollback restores the save point — the swept file is
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
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out"
cd /dev/shm
rm -f "$IMGA" "$IMGB" "$IMGC"

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
for k in range(64):
    with open('%s/fat%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
print("  fixtures: 64 fat fillers (256KB each)")
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
mnt_down "$IMGA"
FILLNOW=$(raw_used "$IMGA")
echo "  RAW fill after the daemon's pass: $FILLNOW/$RAWBLOCKS"
[ "$FILLNOW" -lt "$T25" ] \
    || fail "fill $FILLNOW did not drop below the 25% mark ($T25)"
fsck_ok "$IMGA"

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

rm -f "$IMGA" "$IMGB" "$IMGC"
echo
echo "WATERMARK E2E: PASS"
