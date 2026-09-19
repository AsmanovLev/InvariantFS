#!/bin/bash
# run-e2e.sh — parallel-safe e2e runner.
#
#   bash tools/run-e2e.sh <suite.sh>       run one suite (foreground)
#   bash tools/run-e2e.sh --bg <suite.sh>  background; returns immediately
#   bash tools/run-e2e.sh --wait           block until all bg suites finish
#
# Two execution modes, chosen per suite:
#
#   * ISOLATED (default for suites that do not need root/sudo/loop and do
#     not touch shared /tmp paths): the suite runs inside a private
#     user+mount namespace with its own tmpfs on /dev/shm. Two invocations
#     of the SAME suite in different worktrees (e.g. parallel subagents)
#     therefore cannot collide on image names, and run concurrently.
#
#   * LOCKED (suites matching sudo|losetup|id -u|EUID|mount -o loop|/tmp/,
#     or when namespaces are unavailable/disabled): serialized on the legacy
#     global lock, run as the invoking user. /tmp is NOT isolated because
#     AGENTS worktrees live under /tmp.
#
# Concurrency is bounded by INVFS_E2E_SLOTS (default 4) so parallel suites
# do not exhaust RAM/CPU. Every holder publishes attribution:
#   /tmp/invfs-e2e-slots/slot<N>.info   (isolated)
#   /tmp/invfs-e2e.lock.info            (locked)
#
# Knobs: INVFS_E2E_SLOTS=N  INVFS_E2E_NO_NS=1  INVFS_E2E_FORCE_NS=1
#        INVFS_E2E_FORCE_LOCK=1
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
LOCK="${INVFS_E2E_LOCK:-/tmp/invfs-e2e.lock}"
INFO="${INVFS_E2E_INFO:-/tmp/invfs-e2e.lock.info}"
SLOTDIR="${INVFS_E2E_SLOTDIR:-/tmp/invfs-e2e-slots}"
BGDIR="${INVFS_E2E_BGDIR:-/tmp/invfs-e2e-bg}"
BGDONE="$BGDIR/done"
NSLOTS="${INVFS_E2E_SLOTS:-4}"

ns_usable() {
    [ "${INVFS_E2E_NO_NS:-0}" = 1 ] && return 1
    command -v unshare >/dev/null 2>&1 || return 1
    unshare -rm sh -c 'mount -t tmpfs tmpfs /dev/shm 2>/dev/null' >/dev/null 2>&1
}
NS_OK=0
ns_usable && NS_OK=1

# Suites that must run LOCKED: they need the real uid/root, or they touch
# shared /tmp paths (not isolated, since worktrees live under /tmp).
suite_needs_root() {
    grep -qE '(^|[^[:alnum:]_])(sudo|losetup)([^[:alnum:]_]|$)|id -u|EUID|mount -o loop|/tmp/' \
        "$1" 2>/dev/null
}

mode_for() {
    if [ "${INVFS_E2E_FORCE_LOCK:-0}" = 1 ]; then echo locked; return; fi
    if [ "${INVFS_E2E_FORCE_NS:-0}" = 1 ]; then echo ns; return; fi
    if [ "$NS_OK" = 1 ] && ! suite_needs_root "$1"; then echo ns; else echo locked; fi
}

who_running() {
    local f
    for f in "$SLOTDIR"/slot*.info; do
        [ -e "$f" ] || continue
        echo "  $(basename "$f" .info): $(cat "$f" 2>/dev/null)"
    done
    [ -f "$INFO" ] && echo "  global-lock: $(cat "$INFO" 2>/dev/null)"
    true
}

slot_acquire() {           # sets SLOT; holds fd 8 for the run
    local i
    mkdir -p "$SLOTDIR"
    while :; do
        for i in $(seq 0 $((NSLOTS - 1))); do
            exec 8>"$SLOTDIR/slot$i.lock"
            if flock -n 8; then SLOT=$i; return 0; fi
            exec 8>&-
        done
        sleep 2
    done
}
slot_release() { rm -f "$SLOTDIR/slot$SLOT.info"; exec 8>&- 2>/dev/null; }
slot_mark() {              # $1 suite
    local branch
    branch=$(git -C "$REPO" branch --show-current 2>/dev/null)
    echo "suite=$(basename "$1") pid=$$ branch=${branch:-?} agent=${INVFS_E2E_AGENT:-?} started=$(date -Is) mode=isolated" \
        > "$SLOTDIR/slot$SLOT.info"
}

run_suite() {              # $1 suite, $2 mode
    if [ "$2" = ns ]; then
        # private mount ns: fresh tmpfs on /dev/shm; mounts vanish when the
        # namespace exits (even a crashed FUSE mount). /tmp is NOT touched.
        unshare -rm --propagation private bash -c '
            mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true
            exec bash "$1"
        ' bash "$1"
    else
        bash "$1"
    fi
}

run_locked() {             # $1 suite; holds fd 9
    exec 9>"$LOCK"
    if ! flock -w 10 9; then
        who_running
        echo "waiting for the e2e global lock..."
        flock -w 7200 9 || { echo "e2e lock timeout"; return 124; }
    fi
    echo "suite=$(basename "$1") pid=$$ branch=$(git -C "$REPO" branch --show-current 2>/dev/null) agent=${INVFS_E2E_AGENT:-?} started=$(date -Is) mode=locked" > "$INFO"
    run_suite "$1" locked
    local rc=$?
    rm -f "$INFO"
    return $rc
}

run_one() {                # $1 suite
    local suite="$1" mode rc
    mode=$(mode_for "$suite")
    if [ "$mode" = ns ]; then
        slot_acquire
        slot_mark "$suite"
        run_suite "$suite" ns
        rc=$?
        slot_release
        return $rc
    fi
    run_locked "$suite"
}

case "${1:-}" in
--bg)
    shift
    suite="${1:-}"
    [ -n "$suite" ] || { echo "usage: run-e2e.sh --bg <suite.sh>"; exit 2; }
    mkdir -p "$BGDIR"
    name=$(basename "$suite" .sh)
    log="$BGDIR/$name.$$.log"
    (
        # BASHPID, not $$: in a subshell $$ is the (already-exited) parent,
        # which would make --wait treat the job as dead immediately.
        pid=$BASHPID
        echo "$pid" > "$BGDIR/$name.$pid.pid"
        run_one "$suite" > "$log" 2>&1
        rc=$?
        echo "$name rc=$rc log=$log" >> "$BGDONE"
        rm -f "$BGDIR/$name.$pid.pid"
        exit $rc
    ) &
    echo "bg started: $name (log: $log; wait: bash tools/run-e2e.sh --wait)"
    ;;
--wait)
    mkdir -p "$BGDIR"
    touch "$BGDONE"
    sleep 1   # let just-started --bg jobs register their pidfiles
    while :; do
        alive=0
        for pf in "$BGDIR"/*.pid; do
            [ -e "$pf" ] || continue
            kill -0 "$(cat "$pf")" 2>/dev/null && { alive=1; break; }
        done
        [ "$alive" = 0 ] && break
        sleep 3
    done
    echo "== background results =="
    cat "$BGDONE"
    ;;
"")
    echo "usage: run-e2e.sh [--bg|--wait] <suite.sh>"; exit 2
    ;;
*)
    run_one "$1"
    ;;
esac
