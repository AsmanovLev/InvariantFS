#!/bin/bash
# test-sigterm-exit.sh — WP623: invf-fuse must exit promptly on SIGTERM.
#
#   Leg A (plain mount): mkfs, mount (foreground, so the pid is certain),
#   SIGTERM the daemon, assert it exits within EXIT_LIMIT seconds, the mount
#   is released, and the volume is CLEAN (fsck OK with no -f).
#
#   Leg B (TERM mid-sweep): stage files offline with invf-cp (so the
#   background drain cannot pre-sweep them), mount, kick a full pass with
#   SIGUSR1, SIGTERM as soon as the pass starts, assert the same prompt,
#   clean exit. The graceful-shutdown path (invf_destroy) contends with the
#   sweep thread for g_io_lock; the pass checks g_shutdown per file and
#   drops the lock between files, so the wait is bounded by one file, not
#   the pass. If the daemon ever stops doing that, this leg goes red.
#
#   Leg C (survive_term unchanged + root-storage mark): mount with
#   -o survive_term (the initramfs boot-root shape). SIGTERM must NOT exit
#   the daemon (semantics unchanged by WP623), and the daemon's argv[0]
#   must start with '@' -- systemd's root-storage-daemon protocol
#   (https://systemd.io/ROOT_STORAGE_DAEMONS): systemd-shutdown excludes a
#   root-owned '@'-prefixed process from BOTH kill broadcasts, so the guest
#   no longer waits ~90s for the daemon and then SIGKILLs its own root
#   (CI debian serial: TERM 187s, "Waiting for process: 98 (invf-fuse)"
#   200s, SIGKILL 280s, silence, no "Power down" in 300s). SIGKILL must
#   still kill the daemon promptly. Non-survive_term mounts (legs A/B)
#   must NOT carry the mark.
#
# Run from the repo root after `make`:  bash tools/test-sigterm-exit.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoint is absolute).
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp623sigterm
IMG_A=wp623-a.img
IMG_B=wp623-b.img
IMG_C=wp623-c.img
MNT=$WORK/mnt
EXIT_LIMIT=5       # WP623: prompt exit means seconds, not tens of seconds
KILL_WAIT=30       # hard tripwire: daemon alive past this is hung, SIGKILL it
rm -rf "$WORK" && mkdir -p "$WORK/mnt"
cd /dev/shm
rm -f "$IMG_A" "$IMG_B" "$IMG_C"

fail() { echo "FAIL: $*" >&2; exit 1; }

# mnt_fg <img> <log> [opts] — foreground mount; sets FG_PID. Foreground
# (not daemonized) so the pid is $!, not a pgrep guess: fuse_daemonize
# forks, and the parent's pid is gone by the time pgrep sees it.
mnt_fg() {   # <img> <log> [opts]
    local img=$1 log=$2 opts=${3:-}
    # shellcheck disable=SC2086
    $B/invf-fuse $opts -f "$img" "$MNT" >"$WORK/$log" 2>&1 &
    FG_PID=$!
    # disown so SIGKILLing the daemon later does not print the shell's
    # "Killed" job notice into the log; kill -0 polling still works.
    disown "$FG_PID" 2>/dev/null || true
    for _ in $(seq 1 100); do
        grep -q " $MNT " /proc/mounts 2>/dev/null && return 0
        kill -0 "$FG_PID" 2>/dev/null || { cat "$WORK/$log"; fail "daemon for $img died before mount appeared"; }
        sleep 0.1
    done
    cat "$WORK/$log"; fail "mount of $img never appeared"
}

# wait_dead <pid> <limit-s> — 0 if the process exits in time, 1 if not.
wait_dead() {   # <pid> <limit-s>
    local pid=$1 limit=$2 t0 t1
    t0=$(date +%s)
    while kill -0 "$pid" 2>/dev/null; do
        t1=$(date +%s)
        [ $((t1 - t0)) -ge "$limit" ] && return 1
        sleep 0.2
    done
    return 0
}

# term_exit <pid> <what> — SIGTERM, assert exit within EXIT_LIMIT, report latency.
term_exit() {   # <pid> <what>
    local pid=$1 what=$2 t0 t1 dt
    t0=$(date +%s)
    kill -TERM "$pid"
    if ! wait_dead "$pid" "$EXIT_LIMIT"; then
        echo "  $what: STILL ALIVE after ${EXIT_LIMIT}s (SIGKILLing)"
        kill -KILL "$pid" 2>/dev/null || true
        wait_dead "$pid" "$KILL_WAIT" || fail "$what: daemon survived SIGKILL"
        fail "$what: SIGTERM did not exit the daemon within ${EXIT_LIMIT}s"
    fi
    t1=$(date +%s); dt=$((t1 - t0))
    echo "  $what: SIGTERM exit latency ${dt}s"
}

mnt_gone() {   # <what>
    local what=$1
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts 2>/dev/null || return 0
        sleep 0.1
    done
    fail "$what: mount still present after daemon exit"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

cleanup() {
    [ -n "${FG_PID:-}" ] && kill -KILL "$FG_PID" 2>/dev/null || true
    fusermount3 -u "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "== [A] plain mount: SIGTERM exits promptly, volume CLEAN =="
$B/invf-mkfs "$IMG_A" 0.5 >/dev/null
mnt_fg "$IMG_A" fuse-a.log
echo "hello" > "$MNT/probe.txt"
[ "$(cat "$MNT/probe.txt")" = hello ] || fail "leg A: write/read through mount"
# non-survive_term mounts must NOT carry the root-storage mark
first=$(head -c 1 "/proc/$FG_PID/cmdline")
[ "$first" != "@" ] || fail "leg A: plain mount carries the '@' mark (survive_term-only)"
term_exit "$FG_PID" "leg A idle mount"
FG_PID=""
mnt_gone "leg A"
fusermount3 -u "$MNT" 2>/dev/null || true
fsck_ok "$IMG_A"
echo "  leg A: PASS (prompt exit, mount released, fsck CLEAN)"

echo "== [B] SIGTERM mid-sweep: same prompt exit =="
$B/invf-mkfs "$IMG_B" 1 >/dev/null
python3 - "$WORK" <<'EOF'
import random, sys
work = sys.argv[1]
rnd = random.Random(623)
words = ['kernel','filesystem','invariant','sweep','segment','recipe','delta']
for n in range(4):
    with open('%s/hostbig%d.txt' % (work, n), 'w') as f:
        for i in range(120000):
            f.write('line %d %s %s %08x\n' % (i, words[rnd.randrange(len(words))],
                                              words[rnd.randrange(len(words))],
                                              rnd.randrange(1 << 32)))
print('staged 4 host files')
EOF
for n in 0 1 2 3; do
    $B/invf-cp "$IMG_B" "$WORK/hostbig$n.txt" "big$n.txt" >/dev/null
done
rm -f "$WORK"/hostbig*.txt
mnt_fg "$IMG_B" fuse-b.log
kill -USR1 "$FG_PID"
midpass=0
for _ in $(seq 1 50); do
    grep -q "pass started" "$WORK/fuse-b.log" 2>/dev/null && break
    sleep 0.2
done
grep -q "pass started" "$WORK/fuse-b.log" || fail "leg B: sweep pass never started"
grep -q "DONE" "$WORK/fuse-b.log" 2>/dev/null || midpass=1
term_exit "$FG_PID" "leg B mid-sweep mount"
FG_PID=""
mnt_gone "leg B"
fusermount3 -u "$MNT" 2>/dev/null || true
fsck_ok "$IMG_B"
echo "  leg B: PASS (TERM landed mid-pass: $([ "$midpass" = 1 ] && echo yes || echo "pass already done"); prompt exit, fsck CLEAN)"

echo "== [C] survive_term: TERM ignored (unchanged) + '@' mark, KILL kills =="
$B/invf-mkfs "$IMG_C" 0.5 >/dev/null
mnt_fg "$IMG_C" fuse-c.log "-o survive_term"
# WP623 root-storage mark: argv[0] starts with '@' (systemd skips the
# daemon in both shutdown kill broadcasts). Absent pre-fix: this is the
# red-before/green-after assertion.
first=$(head -c 1 "/proc/$FG_PID/cmdline")
[ "$first" = "@" ] || fail "leg C: survive_term mount lacks the '@' root-storage mark"
echo "  leg C: '@' mark present: $(tr '\0' ' ' < "/proc/$FG_PID/cmdline" | cut -c1-60)"
# comm (what pidof/systemd log) is unchanged by the argv mark
[ "$(cat "/proc/$FG_PID/comm")" = invf-fuse ] || fail "leg C: comm changed (pidof patterns would break)"
kill -TERM "$FG_PID"
sleep 3
kill -0 "$FG_PID" 2>/dev/null || fail "leg C: survive_term mount exited on SIGTERM (semantics changed!)"
grep -q " $MNT " /proc/mounts || fail "leg C: mount gone after SIGTERM on survive_term"
echo "  leg C: SIGTERM ignored, mount still serving (semantics unchanged)"
t0=$(date +%s)
kill -KILL "$FG_PID"
wait_dead "$FG_PID" "$EXIT_LIMIT" || fail "leg C: daemon survived SIGKILL"
echo "  leg C: SIGKILL exit latency $(( $(date +%s) - t0 ))s"
FG_PID=""
fusermount3 -u "$MNT" 2>/dev/null || true
# a KILLed daemon never runs vol_close: tolerate the DIRTY flag exactly
# like the boot harnesses do (-f recovery must still reach OK).
if ! $B/invf-fsck "$IMG_C" 2>&1 | grep -q "^OK$"; then
    $B/invf-fsck -f "$IMG_C" >/dev/null 2>&1 || true
    fsck_ok "$IMG_C"
fi
echo "  leg C: PASS"

rm -f "$IMG_A" "$IMG_B" "$IMG_C"
rm -rf "$WORK"
trap - EXIT
echo "PASS: test-sigterm-exit (WP623)"
