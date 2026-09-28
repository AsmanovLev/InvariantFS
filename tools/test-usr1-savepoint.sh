#!/bin/bash
# test-usr1-savepoint.sh — WP134: the USR1 / user.invfs.sweep sweep arms a
# rollback save point (persistent regression).
#
# Before WP134 the FUSE daemon could be told to run a full pass two ways
# and NEITHER armed a save point: `kill -USR1 $(pidof invf-fuse)` and
# `setfattr -n user.invfs.sweep -v 1 /`. Both ran invf_sweep_worker(0),
# which rewrote the data and left nothing behind -- so the operator who
# then reached for invf-rollback got "no save point", exit 1, AFTER the
# damage. The refusal was correct; the ordering was the trap. The offline
# invf-sweep and the in-FUSE watermark pass both captured; these two did
# not.
#
# The legs below:
#   [A] kill -USR1 captures an SPT0 save point in prepare, before the walk:
#       the daemon logs it, `user.invfs` reports savepoint=live, and fsck
#       agrees. (All three are asserted: a window nobody can see is
#       useless.)
#   [W] the daemon names the rollback path in a form the operator can
#       paste -- the unmount, the exact `invf-rollback <image>`, and the
#       xattr that proves the window exists. This is the half of the fix
#       that has no test coverage on main: there, nothing was armed, so
#       there was nothing to warn about.
#   [X] the other trigger, the user.invfs.sweep xattr, arms it too. The
#       xattr sets the same in-process flag, so this is the same code --
#       it is pinned here so the two triggers cannot drift apart.
#   [R] the window is real, not a log line: after a USR1 sweep,
#       invf-rollback restores the pre-sweep state, the corpus is
#       bit-exact against the originals, fsck is OK, and the second
#       rollback is refused (the point of no return).
#   [P] the pin costs one generation and no more: the next bare sweep
#       reclaims what the window held, so the space comes back.
#
# Run via the e2e runner:  bash tools/run-e2e.sh tools/test-usr1-savepoint.sh
# Uses /dev/shm like the other soak scripts. NOTE: blkio treats /dev/*
# paths as raw devices, so the script cd's into /dev/shm and uses RELATIVE
# image paths everywhere (the mountpoint is absolute). The daemon runs
# FOREGROUND (-f) under setsid: after fuse_daemonize() the sweep thread's
# stderr goes to /dev/null, and the arming / warning lines are the test's
# observability.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
WORK=/dev/shm/wp134usr1
IMGA=wp134a.img     # leg A/W/P: USR1 arms, warns, and a later sweep reclaims
IMGB=wp134b.img     # leg X: the xattr trigger
IMGC=wp134c.img     # leg R: USR1 arms, then a rollback restores
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT" "$WORK/ref" "$WORK/src" "$WORK/out"
cd /dev/shm
rm -f "$IMGA" "$IMGB" "$IMGC"

fail() { echo "FAIL: $*" >&2; exit 1; }
say()  { echo "== $*"; }

# getfattr/setfattr are not present on every build host; python3 always is.
xget()  { python3 -c 'import os,sys; sys.stdout.write(os.getxattr(sys.argv[1], sys.argv[2]).decode())' "$1" "$2"; }
xfield() { xget "$1" "$2" | sed -n "s/^$3=//p"; }
xset()  { python3 -c 'import os,sys; os.setxattr(sys.argv[1], sys.argv[2], b"1")' "$1" "$2"; }

DPID=0
mnt_up() {   # <log-tag> <img>
    local tag=$1 img=$2
    setsid $B/invf-fuse -f "$img" "$MNT" \
        >"$WORK/fuse.$tag.log" 2>&1 < /dev/null &
    DPID=$!
    disown
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $img never appeared"
}

mnt_down() {
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

sweep_done() { wait_log "$1" "\[sweep\] DONE" 120; }
fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

# the daemon must still be alive and the mount live when we poke it
assert_mounted() {
    grep -q " $MNT " /proc/mounts || fail "$1: the mount went away"
    kill -0 "$DPID" 2>/dev/null || fail "$1: the daemon died"
}

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    [ "$DPID" != 0 ] && kill "$DPID" 2>/dev/null || true
    pkill -f "invf-fuse.*wp134" 2>/dev/null || true
}
trap cleanup EXIT

# ---- fixture: 3.1 MiB of text + a 3.0 MiB tar, so the sweep has real
# ---- lanes to run (the tar exercises the container lane's decompose).
python3 - "$WORK/ref" <<'PY'
import os, random, sys
d = sys.argv[1]
rnd = random.Random(134)
vocab = [('word%05d' % i).encode() for i in range(20000)]
for k in range(12):
    words = [vocab[rnd.randrange(len(vocab))] for _ in range(26000)]
    with open(os.path.join(d, 'doc%02d.txt' % k), 'wb') as f:
        f.write(b' '.join(words))
PY
tar -cf "$WORK/src/corpus.tar" -C "$WORK/ref" .
cp "$WORK/ref/doc00.txt" "$WORK/src/doc00.txt"
say "corpus: $(ls "$WORK/ref" | wc -l) files, $(du -sh "$WORK/ref" | cut -f1) + $(du -h "$WORK/src/corpus.tar" | cut -f1) tar"

mk() { # <img>
    $B/invf-mkfs "$1" 4 >/dev/null
    $B/invf-import "$1" "$WORK/src" >/dev/null
    fsck_ok "$1"
}
mk "$IMGA"; mk "$IMGB"; mk "$IMGC"

# ---- [A] kill -USR1 arms an SPT0 save point ---------------------------
say "[A] kill -USR1 arms the save point (savepoint=none before, live after)"
mnt_up a "$IMGA"
assert_mounted A
[ "$(xfield "$MNT" user.invfs savepoint)" = "none" ] \
    || fail "A: a save point was already live before any sweep"
kill -USR1 "$DPID"
wait_log a "ROLLBACK WINDOW ARMED" 60
sweep_done a
grep -q "\[spt0\] save point: pinned " "$WORK/fuse.a.log" \
    || fail "A: the daemon did not report a pinned block set"
grep -q "save point captured" "$WORK/fuse.a.log" \
    || fail "A: the daemon did not report the capture"
# the ordering is the fix: the capture precedes the walk. If the save
# point were taken after the pass, every assertion below would still
# pass and the trap would still be there.
cap_line=$(grep -n "ROLLBACK WINDOW ARMED" "$WORK/fuse.a.log" | head -1 | cut -d: -f1)
walk_line=$(grep -n "manual pass started" "$WORK/fuse.a.log" | head -1 | cut -d: -f1)
[ -n "$cap_line" ] && [ -n "$walk_line" ] && [ "$cap_line" -lt "$walk_line" ] \
    || fail "A: the save point was NOT armed before the walk (cap=$cap_line walk=$walk_line)"
say "  capture at line $cap_line, walk starts at line $walk_line: capture first"
[ "$(xfield "$MNT" user.invfs savepoint)" = "live" ] \
    || fail "A: user.invfs does not report the live save point"
say "  user.invfs reports savepoint=$(xfield "$MNT" user.invfs savepoint)"
# and the same fact survives a clean unmount + fsck
mnt_down
fsck_ok "$IMGA"
grep -q "save point:   live" "$WORK/fsck.last" \
    || { cat "$WORK/fsck.last"; fail "A: fsck does not see the save point"; }
say "  fsck: save point live"

# ---- [W] the warning names the rollback path --------------------------
say "[W] the daemon warning is actionable: it names the actual command"
log=$WORK/fuse.a.log
grep -q "invf-rollback $IMGA" "$log" \
    || fail "W: the warning does not name 'invf-rollback $IMGA'"
grep -q "fusermount3 -u $MNT" "$log" \
    || fail "W: the warning does not name the unmount step"
grep -q "getfattr -n user.invfs -m- $MNT" "$log" \
    || fail "W: the warning does not point at the command that proves the window"
grep -q "savepoint=live" "$log" \
    || fail "W: the warning does not say what to look for"
say "  warning names: fusermount3 -u <mnt>; invf-rollback <img>; the proof xattr"
grep "ROLLBACK WINDOW ARMED" -A 11 "$log" | sed 's/^/  | /'

# ---- [P] the pin costs one generation ---------------------------------
say "[P] the next bare sweep reclaims what the window held"
$B/invf-sweep "$IMGA" >"$WORK/sweep2.log" 2>&1 || true
grep -qE "save point: realizing|no longer referenced by any live recipe" \
    "$WORK/sweep2.log" \
    || { cat "$WORK/sweep2.log"; fail "P: the second sweep did not reclaim the window"; }
reclaimed=$(grep -o "reclaim: [0-9]* blocks" "$WORK/sweep2.log" | grep -o "[0-9]*" | head -1)
say "  second sweep reclaimed ${reclaimed:-0} blocks the window had held"
fsck_ok "$IMGA"

# ---- [X] the xattr trigger arms it too --------------------------------
say "[X] the user.invfs.sweep xattr arms the same save point"
mnt_up b "$IMGB"
assert_mounted X
xset "$MNT" user.invfs.sweep
wait_log b "triggered via xattr" 30
sweep_done b
grep -q "ROLLBACK WINDOW ARMED" "$WORK/fuse.b.log" \
    || fail "X: the xattr trigger armed no window"
[ "$(xfield "$MNT" user.invfs savepoint)" = "live" ] \
    || fail "X: the xattr trigger left savepoint!=live"
say "  xattr trigger: savepoint=$(xfield "$MNT" user.invfs savepoint)"
mnt_down
fsck_ok "$IMGB"

# ---- [R] the window is real: rollback undoes the USR1 sweep -----------
say "[R] invf-rollback undoes a USR1 sweep; the corpus stays bit-exact"
mnt_up c "$IMGC"
assert_mounted R
kill -USR1 "$DPID"
sweep_done c
mnt_down
fsck_ok "$IMGC"
# the swept state reads back fine
$B/invf-cat "$IMGC" doc00.txt "$WORK/out/swept.bin" >/dev/null 2>&1 \
    || fail "R: invf-cat failed on the swept volume"
cmp -s "$WORK/src/doc00.txt" "$WORK/out/swept.bin" \
    || fail "R: the swept volume is not bit-exact"
say "  swept volume reads back bit-exact"
# now undo the sweep
out=$($B/invf-rollback "$IMGC" 2>&1) || { echo "$out"; fail "R: invf-rollback refused"; }
echo "$out" | grep -qi "save point" || { echo "$out"; fail "R: rollback did not name a save point"; }
say "  $(echo "$out" | grep -i 'save point' | head -1)"
fsck_ok "$IMGC"
# the restore consumes the window: the second rollback must refuse
if $B/invf-rollback "$IMGC" >"$WORK/rb2.log" 2>&1; then
    cat "$WORK/rb2.log"; fail "R: a second rollback was allowed (the window must be consumed)"
fi
grep -qi "no save point" "$WORK/rb2.log" \
    || { cat "$WORK/rb2.log"; fail "R: the second rollback failed for the wrong reason"; }
say "  second rollback refused: no save point (the point of no return)"
# and the corpus is still bit-exact after the round trip
for f in doc00.txt corpus.tar; do
    $B/invf-cat "$IMGC" "$f" "$WORK/out/after.$f" >/dev/null 2>&1 \
        || fail "R: invf-cat failed on $f after the rollback"
    cmp -s "$WORK/src/$f" "$WORK/out/after.$f" \
        || fail "R: $f is not bit-exact after the rollback"
    say "  bit-exact after rollback: $f"
done
fsck_ok "$IMGC"

echo "PASS: USR1 and the sweep xattr arm a rollback save point, warn with the"
echo "      rollback command, and the window costs exactly one generation."
