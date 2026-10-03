#!/bin/bash
# tools/test-fuse-sweep-thread.sh — the daemon must HAVE its sweep thread.
#
# WHY THIS EXISTS. fuse_sweep_thread was created twelve lines of bookkeeping
# BEFORE fuse_daemonize() (src/cli/fuse_fs.c), so the thread existed only in
# the process that daemonize forked away and exited. The daemon was left with
# main plus libfuse's workers and no consumer for g_sweep_now -- which is what
# BOTH `kill -USR1` and the `user.invfs.sweep` xattr set. AGENTS.md 2.5/2.6
# documented that path as the daemon's own recovery path; it did nothing.
#
# The defect IS an absence, so the red control asserts the ABSENCE:
#   - the thread is not in /proc/<pid>/task          (cheapest; fails pre-fix)
#   - `savepoint=none` after SIGUSR1                  (fails pre-fix)
#   - `savepoint=none` after the xattr trigger        (fails pre-fix)
#   - `pending_sweep` never drains                    (fails pre-fix)
# and the GREEN control asserts their presence:
#   - the thread IS in /proc/<pid>/task
#   - a live save point, an armed rollback, and an
#     invf-rollback that actually restores the generation.
#
# The daemonized daemon sends its own stderr to /dev/null (fuse_daemonize),
# so this gate asserts on EFFECTS (the save point, the rollback, the thread
# table) and not on log lines. A log-line assertion could not distinguish
# "broken" from "not logged".
#
# usage: test-fuse-sweep-thread.sh [green|red|both]   (default: green)
#
#   green (the gate) asserts the PRESENCE of everything 2.5/2.6 promise. It
#         FAILS on the unfixed tree and passes on the fixed one -- that is the
#         regression gate.
#   red asserts the ABSENCE, and is the control that DEMONSTRATES the defect:
#         on the unfixed tree it PASSES, because the defect is a missing
#         thread, a missing log line and a missing save point. Its output is
#         the evidence. On the fixed tree it FAILS.
#   both runs them, so a fixed tree passes nothing twice and a broken one
#         fails loudly in the same run.

set -u
MODE=${1:-green}
HERE=$(cd "$(dirname "$0")/.." && pwd)
# WP205: one scratch-root answer for the whole suite set (see
# tools/lib-scratch.sh).
. "$HERE/tools/lib-scratch.sh"
BIN=$HERE/bin
rc=0

# Must NOT run as root. The daemon is its own object-level permission
# authority (AGENTS.md 2.9 -- the mount deliberately does not negotiate
# default_permissions), so a root run takes a different path through every
# lookup than the one this gate is about. Refuse rather than report a
# result nobody can act on.
if [ "$(id -u)" = 0 ]; then
    echo "test-fuse-sweep-thread: run as a normal user, not root" >&2
    exit 2
fi

# A per-run scratch, so two runs never share an image (a shared image plus a
# leaked daemon is the "image is in use" failure). INVFS_TOOL_SCRATCH is on
# /srv here: /tmp is RAM.
SCR=$(mktemp -d "${INVFS_TOOL_SCRATCH:-$(invfs_scratch_root)}/sweepthread.XXXXXX")
mkdir -p "$SCR"

# daemon pid for THIS image only -- never `pgrep -f` on a typed pattern.
daemon_pid() {
    python3 - "$1" <<'PY'
import os, sys
want = sys.argv[1]
for p in os.listdir("/proc"):
    if not p.isdigit():
        continue
    try:
        c = open(f"/proc/{p}/cmdline", "rb").read().split(b"\0")
        if want.encode() not in c:
            continue
        tdir = f"/proc/{p}/task"
        comms = [open(f"{tdir}/{t}/comm").read().strip() for t in os.listdir(tdir)]
    except OSError:
        continue
    # the daemonize PARENT carries this image in its cmdline too and has
    # different threads. Only the MOUNTED daemon has a libfuse worker, so
    # identify by that rather than by "the last match".
    if "fuse_worker" in comms:
        print(p)
PY
}

nthreads() { ls "/proc/$1/task" 2>/dev/null | wc -l; }

# libfuse's pool is demand-grown (up to 10 workers), so the RAW thread count
# is not a stable discriminator: the unfixed daemon measured 3 and 4 on
# different runs of the same binary. What IS stable is how many threads are
# NOT libfuse workers: the unfixed daemon has exactly one (main), the fixed
# one has two (main plus the sweep thread, which inherits the process name
# because pthread_create does not set one).
nonworker_threads() {
    python3 - "$1" <<'PY'
import os, sys
t = f"/proc/{sys.argv[1]}/task"
n = 0
for x in os.listdir(t):
    try:
        if open(f"{t}/{x}/comm").read().strip() != "fuse_worker":
            n += 1
    except OSError:
        pass
print(n)
PY
}

ctl() { python3 - "$1" <<'PY'
import os, sys
try:
    sys.stdout.write(os.getxattr(sys.argv[1], "user.invfs").decode())
except OSError as e:
    print("getxattr:", e)
PY
}
ctlval() { ctl "$1" | grep "^$2=" | head -1 | cut -d= -f2; }

ok()   { echo "  ok   -- $*"; }
bad()  { echo "  FAIL -- $*"; rc=1; }

# A leaked invf-fuse holds the image lock, and the next run then dies with
# "image is in use by another process" -- an error that reads like a
# filesystem problem and is not one. Reap anything still holding THIS
# script's scratch images. Scoped to $SCR and to an exact `invf-fuse` argv[0],
# so no other agent's daemon is ever a candidate.
reap() {
    python3 - "$SCR" <<'PY'
import os, signal, sys
scr = sys.argv[1].encode()
for p in os.listdir("/proc"):
    if not p.isdigit():
        continue
    try:
        c = open(f"/proc/{p}/cmdline", "rb").read().split(b"\0")
    except OSError:
        continue
    if c and c[0].endswith(b"invf-fuse") and any(x.startswith(scr + b"/") for x in c):
        try:
            os.kill(int(p), signal.SIGKILL)
            print(f"  (reaped a leaked daemon: {p})")
        except OSError:
            pass
PY
}
trap 'reap >/dev/null 2>&1; rm -rf "$SCR"' EXIT
reap

# stage: mkfs a volume, mount it daemonized, write some reclaimable data.
# echoes "PID MNT IMG"; caller unmounts.
setup() {
    local tag=$1; local IMG=$SCR/$tag.img MNT=$SCR/$tag-mnt LOG=$SCR/$tag.log
    rm -f "$IMG"; rm -rf "$MNT"; mkdir -p "$MNT"; : > "$LOG"
    "$BIN/invf-mkfs" "$IMG" 64 >/dev/null 2>&1 || { echo "mkfs failed"; return 1; }
    setsid nohup "$BIN/invf-fuse" -o raw_watermark=20 "$IMG" "$MNT" >"$LOG" 2>&1 &
    local i
    for i in $(seq 1 60); do mountpoint -q "$MNT" && break; sleep 0.25; done
    mountpoint -q "$MNT" || { echo "mount failed:"; cat "$LOG"; return 1; }
    sleep 2   # let the daemonize parent finish exiting
    echo "$IMG $MNT"
}

stage() {   # stage <mnt>: data to reclaim
    local i
    for i in 1 2 3 4 5 6; do head -c 65536 /dev/urandom > "$1/d$i.bin"; done
    sync
}

assert_absent() {
    local PID=$1 MNT=$2
    local n; n=$(nonworker_threads "$PID")
    echo "  threads in the daemon: $(nthreads "$PID") total, $n of them not libfuse workers"
    [ "$n" -ge 2 ] && bad "a daemon with $n non-worker threads HAS a sweep thread" \
                    || ok "no sweep thread ($n non-worker thread: main only)"
    [ "$(ctlval "$MNT" savepoint)" = none ] \
        && ok "savepoint=none (no worker to consume g_sweep_now)" \
        || bad "savepoint=$(ctlval "$MNT" savepoint)"
    local pend; pend=$(ctlval "$MNT" pending_sweep)
    [ "${pend:-0}" -gt 0 ] \
        && ok "pending_sweep=$pend never drained" \
        || bad "pending_sweep=$pend -- the drain ran"
}

assert_present() {
    local PID=$1 MNT=$2
    local n; n=$(nonworker_threads "$PID")
    echo "  threads in the daemon: $(nthreads "$PID") total, $n of them not libfuse workers"
    [ "$n" -ge 2 ] && ok "the sweep thread is in the daemon ($n non-worker threads)" \
                    || bad "only $n non-worker thread -- the sweep thread is gone again"
    [ "$(ctlval "$MNT" savepoint)" = live ] \
        && ok "savepoint=live (a window the operator can take back)" \
        || bad "savepoint=$(ctlval "$MNT" savepoint) after USR1"
}

run_red() {
    echo "== RED: the daemon has NO sweep thread =="
    local s PID MNT IMG
    s=$(setup red) || { rc=1; return; }
    IMG=${s%% *}; MNT=${s#* }
    PID=$(daemon_pid "$IMG"); [ -n "$PID" ] || { bad "no daemon pid"; return; }
    stage "$MNT"
    kill -USR1 "$PID" 2>/dev/null; sleep 4
    kill -0 "$PID" 2>/dev/null && ok "SIGUSR1 did not kill the daemon" \
                                || bad "SIGUSR1 killed the daemon (no handler)"
    python3 -c 'import os,sys; os.setxattr(sys.argv[1],"user.invfs.sweep",b"1")' "$MNT"
    sleep 4
    assert_absent "$PID" "$MNT"
    fusermount3 -u "$MNT" 2>/dev/null; sleep 1
    kill -9 "$PID" 2>/dev/null
}

run_green() {
    echo "== GREEN: the daemon HAS its sweep thread, and the window is real =="
    local s PID MNT IMG before after
    s=$(setup green) || { rc=1; return; }
    IMG=${s%% *}; MNT=${s#* }
    PID=$(daemon_pid "$IMG"); [ -n "$PID" ] || { bad "no daemon pid"; return; }
    stage "$MNT"
    before=$(cd "$SCR" && find green-mnt -type f | sort | xargs md5sum 2>/dev/null | cut -d" " -f1)
    kill -USR1 "$PID" 2>/dev/null; sleep 6
    kill -0 "$PID" 2>/dev/null && ok "SIGUSR1 did not kill the daemon" \
                                || bad "SIGUSR1 killed the daemon (no handler)"
    assert_present "$PID" "$MNT"
    local pend; pend=$(ctlval "$MNT" pending_sweep)
    [ "${pend:-0}" -eq 0 ] && ok "pending_sweep drained to 0" \
                           || bad "pending_sweep=$pend -- the drain did not run"
    after=$(cd "$SCR" && find green-mnt -type f | sort | xargs md5sum 2>/dev/null | cut -d" " -f1)
    [ -n "$before" ] && [ "$before" = "$after" ] \
        && ok "the pass preserved every byte (bit-exactness holds)" \
        || bad "the pass changed the data"

    fusermount3 -u "$MNT" 2>/dev/null; sleep 1

    # the other half of the promise: a window invf-rollback can spend
    echo "  -- rollback --"
    "$BIN/invf-rollback" "$IMG" >"$SCR/green-rollback.log" 2>&1
    local rr=$?
    sed 's/^/     /' "$SCR/green-rollback.log"
    # "exited 0" alone is not "the rollback DID something": a no-op that
    # happens to return 0 would satisfy it. Assert the restore happened --
    # the published root is named and the window is consumed.
    grep -q "rolled back to save point" "$SCR/green-rollback.log" \
        && ok "invf-rollback restored the captured generation" \
        || bad "invf-rollback exited $rr without restoring anything"
    [ $rr -eq 0 ] && ok "invf-rollback accepted the armed window" \
                   || bad "invf-rollback exited $rr"
    sleep 1
    mkdir -p "$SCR/green-mnt2"
    setsid nohup "$BIN/invf-fuse" "$IMG" "$SCR/green-mnt2" >/dev/null 2>&1 &
    local j
    for j in $(seq 1 60); do mountpoint -q "$SCR/green-mnt2" && break; sleep 0.25; done
    sleep 1
    if mountpoint -q "$SCR/green-mnt2"; then
        local rolled; rolled=$(cd "$SCR" && find green-mnt2 -type f | sort | xargs md5sum 2>/dev/null | cut -d" " -f1)
        [ -n "$rolled" ] && [ "$rolled" = "$before" ] \
            && ok "the post-rollback mount reads back the pre-sweep bytes" \
            || bad "the post-rollback mount does not match the pre-sweep bytes"
        # the window is spent: this is the point of no return (AGENTS.md 2.6)
        [ "$(ctlval "$SCR/green-mnt2" savepoint)" = none ] \
            && ok "the window is consumed -- savepoint=none after the rollback" \
            || bad "savepoint=$(ctlval "$SCR/green-mnt2" savepoint) after the rollback"
        fusermount3 -u "$SCR/green-mnt2" 2>/dev/null
    else
        bad "could not remount after rollback"
    fi
}

case "$MODE" in
    red)   run_red ;;
    green) run_green ;;
    *)     run_red; echo; run_green ;;
esac

echo
[ $rc -eq 0 ] && echo "test-fuse-sweep-thread: PASS" \
              || echo "test-fuse-sweep-thread: FAIL"
exit $rc