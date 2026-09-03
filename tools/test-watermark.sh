#!/bin/bash
# test-watermark.sh — WP26 watermark-triggered early sweep e2e
# (persistent regression).
#
# Mount option raw_watermark=<pct> (env INVFS_RAW_WATERMARK fallback)
# makes the FUSE background sweep thread kick a checkpoint-armed full
# sweep pass whenever the RAW-zone fill exceeds <pct>% — the first rung
# of the pressure ladder (WP23 added the adaptive write-effort rungs;
# this one reclaims). The kick re-arms only on RISING fill: while a
# pass's checkpoint is live its retention holds the retired blocks, so
# the fill cannot drop until a later pass's realize-after-arm; chaining
# passes would auto-realize (and finally disarm) the very rollback
# window the pass just created.
#
#   Leg 0: lazy default — no option, fill past 25% staged offline,
#          mounted 4s: no watermark kick, no checkpoint, fill unchanged.
#   Leg 1: trigger — raw_watermark=25, fill ~45% staged OFFLINE (so the
#          trigger is attributable to the watermark: the daemon's
#          every-second pending drain would relieve mounted writes on
#          its own; offline-staged files are invisible to it). The
#          daemon sweeps with NO explicit invf-sweep call: pass 1 arms
#          CKP0 (#1), 6 further mounted writes (drain defers while the
#          checkpoint is live) raise the fill past the first pass's exit
#          fill -> pass 2 auto-realizes #1 and arms #2. No third kick.
#          After unmount: RAW fill < 25% (dropped via the daemon's own
#          passes), fsck reports sweep #2 live.
#   Leg 2 (task leg c): with the watermark checkpoint live, compaction
#          refuses (the rule is already in the tree,
#          vol_records.c:vol_inode_compact — asserted here, not
#          reimplemented).
#   Leg 3 (task leg b): rollback reaches checkpoint #2 only — pass-1
#          sweep results SURVIVE (fillers stay out of RAW), pass-2
#          results are undone (the post-#1 files are RAW again and
#          bit-exact). fsck clean.
#   Leg 4 (task leg a): fresh image, one watermark pass, rollback ->
#          fillers back in RAW, bit-exact, pre-sweep fill, fsck clean.
#   Leg 5: INVFS_RAW_WATERMARK env fallback arms the same machinery.
#
# Run via the global e2e lock:  bash tools/run-e2e.sh tools/test-watermark.sh
# Uses /dev/shm like the other soak scripts. NOTE: blkio treats /dev/*
# paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoint is absolute).
# The daemon runs FOREGROUND (-f) under setsid: after fuse_daemonize()
# the sweep thread's stderr goes to /dev/null, and the watermark pass's
# log lines ("checkpoint: #N armed") are the test's observability.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp26watermark
IMGA=wp26wm-a.img    # legs 1-3: trigger, two sweeps, window = last
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

# first AST segment's zone (0 = RAW)
zone_of() { $B/meta_probe "$1" --heat "$2" 2>/dev/null \
    | awk '/^ast /{for(i=1;i<=NF;i++) if ($i ~ /^zone=/) {sub("zone=","",$i); print $i; exit}}'; }

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
echo "== [0] lazy default: no option -> no kick, no checkpoint =="
fill_to "$IMGA" "$T45"
LAZYFILL=$(raw_used "$IMGA")
mnt_up lazy "$IMGA"
sleep 4
mnt_down "$IMGA"
grep -q "watermark" "$WORK/fuse.lazy.log" \
    && fail "lazy: watermark fired without the option"
grep -q "checkpoint:" "$WORK/fuse.lazy.log" \
    && fail "lazy: a checkpoint was armed without the option"
[ "$(raw_used "$IMGA")" = "$LAZYFILL" ] \
    || fail "lazy: fill changed without the option"
echo "  no kick, no checkpoint, fill unchanged at $LAZYFILL/$RAWBLOCKS"

echo
echo "== [1] raw_watermark=25: daemon sweeps past the mark on its own =="
mnt_up a1 "$IMGA" -- -o raw_watermark=25
wait_log a1 "checkpoint: #1 armed" 30
wait_log a1 "retained blocks held for rollback" 10
grep -q "\[watermark\] RAW fill .* over 25%: kicking a sweep pass" \
    "$WORK/fuse.a1.log" || fail "no watermark kick logged"
echo "  pass 1: kicked by the daemon, checkpoint #1 armed (no invf-sweep ran)"
# retention pins the fill while #1 is live; new pressure must rise above
# the pass-1 exit fill to earn pass 2. Six mounted writes (~16% of RAW)
# do that; the pending drain defers the whole time (checkpoint live).
for k in 0 1 2 3 4 5; do
    f=$(printf 'fat%02d.txt' $((FATN+k)))
    cp "$WORK/ref/$f" "$MNT/$f"
done
FATN=$((FATN+6))
sync
wait_log a1 "checkpoint: #2 armed" 30
grep -q "checkpoint: previous run realized" "$WORK/fuse.a1.log" \
    || fail "pass 2 did not auto-realize checkpoint #1"
echo "  pass 2: rising fill re-kicked; #1 auto-realized, #2 armed"
sleep 4   # a third pass must NOT chain: the window must stay #2
[ "$(grep -c 'kicking a sweep pass' "$WORK/fuse.a1.log")" = "2" ] \
    || fail "daemon chained passes: $(grep -c 'kicking a sweep pass' "$WORK/fuse.a1.log") kicks"
mnt_down "$IMGA"
FILLNOW=$(raw_used "$IMGA")
echo "  RAW fill after the daemon's passes: $FILLNOW/$RAWBLOCKS"
[ "$FILLNOW" -lt "$T25" ] \
    || fail "fill $FILLNOW did not drop below the 25% mark ($T25)"
fsck_ok "$IMGA"
grep -q "checkpoint:   sweep #2 live" "$WORK/fsck.last" \
    || fail "checkpoint #2 not live after the two passes"

echo
echo "== [2] while the checkpoint is live, compaction refuses =="
$B/invf-sweep "$IMGA" --compact > "$WORK/compact.log" 2>&1 \
    || { cat "$WORK/compact.log"; fail "--compact run failed"; }
grep -q "inode compact: skipped (sweep checkpoint #2 live" "$WORK/compact.log" \
    || { cat "$WORK/compact.log"; fail "compaction did not refuse under the live checkpoint"; }
echo "  compaction refused under the live checkpoint (rc=0)"

echo
echo "== [3] rollback window = the LAST watermark sweep only =="
$B/invf-rollback "$IMGA" > "$WORK/rb-a.log" 2>&1 || { cat "$WORK/rb-a.log"; fail "rollback failed"; }
grep -q "rolled back to checkpoint #2" "$WORK/rb-a.log" \
    || fail "rollback did not name checkpoint #2"
# pass 1 survives: the offline-staged fillers stay OUT of RAW
z=$(zone_of "$IMGA" fat00.txt)
[ "$z" != "0" ] && [ -n "$z" ] || fail "fat00.txt back in RAW: pass 1 was undone"
# pass 2 is undone: the mounted-write files are RAW again, bit-exact
z=$(zone_of "$IMGA" "$(printf 'fat%02d.txt' $((FATN-6)))")
[ "$z" = "0" ] || fail "pass-2 file not back in RAW (zone=$z)"
$B/invf-cat "$IMGA" "$(printf 'fat%02d.txt' $((FATN-6)))" "$WORK/out/w2.bin" >/dev/null
cmp "$WORK/ref/$(printf 'fat%02d.txt' $((FATN-6)))" "$WORK/out/w2.bin" \
    || fail "pass-2 file not bit-exact after the rollback"
fsck_ok "$IMGA"
if grep -q "checkpoint:" "$WORK/fsck.last"; then
    fail "checkpoint still reported post-rollback"
fi
echo "  pass 1 intact (fillers swept), pass 2 undone (files RAW, bit-exact)"

echo
echo "== [4] rollback undoes the watermark sweep wholesale =="
$B/invf-mkfs "$IMGB" 0.0625 >/dev/null
fill_to "$IMGB" "$T45"
PREFILL=$(raw_used "$IMGB")
mnt_up b1 "$IMGB" -- -o raw_watermark=25
wait_log b1 "checkpoint: #1 armed" 30
wait_log b1 "retained blocks held for rollback" 10
mnt_down "$IMGB"
$B/invf-rollback "$IMGB" > "$WORK/rb-b.log" 2>&1 || { cat "$WORK/rb-b.log"; fail "rollback failed"; }
grep -q "rolled back to checkpoint #1" "$WORK/rb-b.log" \
    || fail "rollback did not name checkpoint #1"
z=$(zone_of "$IMGB" "$(printf 'fat%02d.txt' $((FATN-7)))")
[ "$z" = "0" ] || fail "swept file not back in RAW post-rollback (zone=$z)"
$B/invf-cat "$IMGB" "$(printf 'fat%02d.txt' $((FATN-7)))" "$WORK/out/b.bin" >/dev/null
cmp "$WORK/ref/$(printf 'fat%02d.txt' $((FATN-7)))" "$WORK/out/b.bin" \
    || fail "file not bit-exact post-rollback"
[ "$(raw_used "$IMGB")" = "$PREFILL" ] \
    || fail "fill not restored: $(raw_used "$IMGB") vs pre-sweep $PREFILL"
fsck_ok "$IMGB"
echo "  watermark sweep undone: RAW again, bit-exact, fill back to $PREFILL"

echo
echo "== [5] INVFS_RAW_WATERMARK env fallback =="
$B/invf-mkfs "$IMGC" 0.0625 >/dev/null
fill_to "$IMGC" "$T45"
mnt_up c1 "$IMGC" INVFS_RAW_WATERMARK=25
wait_log c1 "checkpoint: #1 armed" 30
mnt_down "$IMGC"
$B/invf-rollback "$IMGC" >/dev/null 2>&1 || fail "env-armed rollback failed"
fsck_ok "$IMGC"
echo "  env fallback armed the same machinery; rollback + fsck clean"

rm -f "$IMGA" "$IMGB" "$IMGC"
echo
echo "WATERMARK E2E: PASS"
