#!/bin/bash
# run-e2e.sh — serialized e2e runner with owner attribution + async mode.
#
#   bash tools/run-e2e.sh <suite.sh>      foreground under the global lock
#   bash tools/run-e2e.sh --bg <suite.sh> background; returns immediately,
#                                          status lands in /tmp/invfs-e2e-bg/
#   bash tools/run-e2e.sh --wait          block until the lock is free AND all
#                                          background suites finished; prints
#                                          who holds/held what and results
#
# The lock serializes suites because they share /dev/shm image names.
# While waiting, the holder's identity (suite, pid, branch, since) is shown.
LOCK=/tmp/invfs-e2e.lock
INFO=/tmp/invfs-e2e.lock.info
BGDIR=/tmp/invfs-e2e-bg
BGDONE="$BGDIR/done"          # one file per finished bg suite: "<suite> rc=N"

who_running() {
    if [ -f "$INFO" ]; then
        echo "e2e lock held by: $(cat "$INFO" 2>/dev/null)"
    else
        echo "e2e lock held (holder info unavailable)"
    fi
}

lock_free() { flock -n "$LOCK" -c true 2>/dev/null; }

case "${1:-}" in
--bg)
    shift
    suite="$1"
    [ -n "$suite" ] || { echo "usage: run-e2e.sh --bg <suite.sh>"; exit 2; }
    mkdir -p "$BGDIR"
    name=$(basename "$suite" .sh)
    log="$BGDIR/$name.log"
    (
        echo $$ > "$BGDIR/$name.pid"
        exec 9>"$LOCK"
        if ! flock -w 7200 9; then
            echo "timeout waiting for e2e lock" > "$log"
            echo "$name rc=124 (lock timeout)" >> "$BGDONE"
            rm -f "$BGDIR/$name.pid"
            exit 124
        fi
        branch=$(git -C "$(dirname "$0")/.." branch --show-current 2>/dev/null)
        echo "suite=$name pid=$$ branch=${branch:-?} started=$(date -Is)" > "$INFO"
        bash "$suite" > "$log" 2>&1
        rc=$?
        echo "$name rc=$rc" >> "$BGDONE"
        rm -f "$INFO" "$BGDIR/$name.pid"
        exit $rc
    ) &
    echo "bg started: $name (log: $log; check: bash tools/run-e2e.sh --wait)"
    ;;
--wait)
    mkdir -p "$BGDIR"
    touch "$BGDONE"
    while :; do
        lock_free || { who_running; sleep 5; continue; }
        # any live background suite? (pidfiles, not pgrep: pgrep -f matches
        # the caller's own cmdline and self-deadlocks)
        alive=0
        for pf in "$BGDIR"/*.pid; do
            [ -e "$pf" ] || continue
            kill -0 "$(cat "$pf")" 2>/dev/null && { alive=1; sleep 5; break; }
        done
        [ "$alive" = 0 ] && break
    done
    echo "== e2e lock free; background results: =="
    cat "$BGDONE" 2>/dev/null || echo "(none)"
    ;;
"")
    echo "usage: run-e2e.sh [--bg|--wait] <suite.sh>"; exit 2
    ;;
*)
    exec 9>"$LOCK"
    if ! flock -w 10 9; then
        who_running
        echo "waiting for the e2e lock..."
        flock -w 7200 9 || { echo "e2e lock timeout"; exit 124; }
    fi
    branch=$(git -C "$(dirname "$0")/.." branch --show-current 2>/dev/null)
    echo "suite=$(basename "$1") pid=$$ branch=${branch:-?} started=$(date -Is)" > "$INFO"
    bash "$@"
    rc=$?
    rm -f "$INFO"
    exit $rc
    ;;
esac
