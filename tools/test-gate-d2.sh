#!/bin/bash
# test-gate-d2.sh — Parallel write stress: 3-leg concurrency correctness
#
#   Leg D2a: two threads write the same file simultaneously — verify no
#            truncation/corruption, all lines present, correct total.
#   Leg D2b: 10 threads write different files simultaneously — verify all
#            files exist with correct content.
#   Leg D2c: sweep runs during active writes — verify data integrity and
#            clean fsck after sweep completes.
#
# The FUSE daemon uses fuse_loop_mt (multi-threaded) with g_io_lock
# serializing all engine calls. These legs verify that write serialization
# under concurrency does not lose or corrupt data.
#
# Run from the repo root after `make`:  bash tools/test-gate-d2.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WORK=/tmp/opencode/wp26gated2
MNT=$WORK/mnt
IMG=$WORK/d2.img
rm -rf "$WORK" && mkdir -p "$WORK" "$MNT"
cd /tmp/opencode

fail() { echo "FAIL: $*" >&2; exit 1; }

DPID=0

# Run the daemon in the FOREGROUND under setsid, with its stderr kept.
# Without -f, libfuse's fuse_daemonize() forks and the parent's stderr --
# the only thing that reached $WORK/fuse.log here -- dies with it, so the
# daemon's own output (the sweep pass lines D2c needs to prove it ran) was
# never captured. With -f there is exactly one process, its pid is known,
# and the log is complete. It also removes the pgrep-by-pattern process
# hunt, which on a host where several agents run suites at once can match
# somebody else's daemon.
mnt_up() {
    setsid $B/invf-fuse -f "$1" "$MNT" >"$WORK/fuse.log" 2>&1 < /dev/null &
    DPID=$!
    for _ in $(seq 1 100); do
        grep -q " $MNT " /proc/mounts && return 0
        kill -0 "$DPID" 2>/dev/null || fail "invf-fuse died mounting $1"
        sleep 0.1
    done
    fail "mount of $1 never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        kill -0 "$DPID" 2>/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    [ "$DPID" -gt 0 ] 2>/dev/null && kill "$DPID" 2>/dev/null
    # kill any leftover background writers from leg D2c
    if [ -n "${WRITER_PID:-}" ] && [ "$WRITER_PID" -gt 0 ] 2>/dev/null; then
        kill "$WRITER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0; FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
bail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# ═══════════════════════════════════════════════════════════════════════
# Leg D2a: Two threads write the same file simultaneously
# ═══════════════════════════════════════════════════════════════════════
echo "== [D2a] two writers, same file =="
$B/invf-mkfs "$IMG" 20M >/dev/null
mnt_up "$IMG"

echo "INITIAL" > "$MNT/target.txt"

(
    for i in $(seq 1 100); do echo "AAA-$i" >> "$MNT/target.txt"; done
) &
PID_A=$!

(
    for i in $(seq 1 100); do echo "BBB-$i" >> "$MNT/target.txt"; done
) &
PID_B=$!

wait "$PID_A" "$PID_B"

# Read back and verify
# wc -l may undercount if last line lacks trailing newline; use grep -c instead
LINE_COUNT=$(grep -c '.' "$MNT/target.txt" || true)
if [ "$LINE_COUNT" -ne 201 ]; then
    bail "D2a: expected 201 lines (INITIAL + 100 AAA + 100 BBB), got $LINE_COUNT"
    mnt_down "$IMG"
else
    # Check no line is truncated or corrupted: each must match INITIAL, AAA-NNN, or BBB-NNN
    BAD=$(grep -cvE '^(INITIAL|AAA-[0-9]{1,3}|BBB-[0-9]{1,3})$' "$MNT/target.txt" || true)
    if [ "$BAD" -ne 0 ]; then
        bail "D2a: $BAD corrupted/truncated lines"
        grep -vE '^(INITIAL|AAA-[0-9]{1,3}|BBB-[0-9]{1,3})$' "$MNT/target.txt" | head -5
        mnt_down "$IMG"
    else
        AAA_COUNT=$(grep -c '^AAA-' "$MNT/target.txt" || true)
        BBB_COUNT=$(grep -c '^BBB-' "$MNT/target.txt" || true)
        if [ "$AAA_COUNT" -ne 100 ] || [ "$BBB_COUNT" -ne 100 ]; then
            bail "D2a: AAA=$AAA_COUNT BBB=$BBB_COUNT (expected 100 each)"
            mnt_down "$IMG"
        else
            mnt_down "$IMG"
            fsck_ok "$IMG"
            pass "D2a: two concurrent writers to same file, no corruption"
        fi
    fi
fi

# ═══════════════════════════════════════════════════════════════════════
# Leg D2b: 10 threads write different files simultaneously
# ═══════════════════════════════════════════════════════════════════════
echo "== [D2b] 10 writers, unique files =="
rm -f "$IMG"
$B/invf-mkfs "$IMG" 20M >/dev/null
mnt_up "$IMG"

D2B_PIDS=()
for w in $(seq 1 10); do
    (
        for i in $(seq 1 50); do echo "WRITER${w}-$i" >> "$MNT/file${w}.txt"; done
    ) &
    D2B_PIDS+=($!)
done

wait "${D2B_PIDS[@]}"

D2B_OK=1
for w in $(seq 1 10); do
    if [ ! -f "$MNT/file${w}.txt" ]; then
        bail "D2b: file${w}.txt missing"
        D2B_OK=0
        break
    fi
    LC=$(grep -c '.' "$MNT/file${w}.txt" || true)
    if [ "$LC" -ne 50 ]; then
        bail "D2b: file${w}.txt has $LC lines, expected 50"
        D2B_OK=0
        break
    fi
    BAD=$(grep -cv "^WRITER${w}-" "$MNT/file${w}.txt" || true)
    if [ "$BAD" -ne 0 ]; then
        bail "D2b: file${w}.txt has $BAD wrong-prefixed lines"
        D2B_OK=0
        break
    fi
done

if [ "$D2B_OK" -eq 1 ]; then
    mnt_down "$IMG"
    # Verify invf-ls shows all 10 files (offline tool, image must be unmounted)
    LS_COUNT=$($B/invf-ls "$IMG" | grep -c 'file[0-9]*\.txt' || true)
    if [ "$LS_COUNT" -ne 10 ]; then
        bail "D2b: invf-ls shows $LS_COUNT files, expected 10"
    else
        fsck_ok "$IMG"
        pass "D2b: 10 concurrent writers to unique files, all correct"
    fi
else
    mnt_down "$IMG"
fi

# ═══════════════════════════════════════════════════════════════════════
# Leg D2c: Sweep runs during active writes
# ═══════════════════════════════════════════════════════════════════════
echo "== [D2c] sweep during active writes =="
rm -f "$IMG"
$B/invf-mkfs "$IMG" 20M >/dev/null

# Seed data for the sweep to chew on. This has to happen BEFORE the mount:
# invf-cp is an offline tool, and running it against the image while the
# daemon holds it open writes the volume underneath a live mount (anything
# the daemon has not flushed is lost, and the daemon's own block allocator
# has no idea). It was on the mounted side of mnt_up here.
$B/invf-cp "$IMG" /etc/hostname pre-sweep.txt >/dev/null 2>&1 || true

mnt_up "$IMG"
echo "seed data for sweep" > "$MNT/seed.txt"

# Start a background writer
WRITER_PID=""
(
    while true; do
        echo "data-$(date +%s%N)" >> "$MNT/active.txt" 2>/dev/null
        sleep 0.01
    done
) &
WRITER_PID=$!
sleep 0.5  # let some data accumulate

# Trigger the sweep. This used to be
#     setfattr -n user.invfs.sweep -v 1 "$MNT/" 2>/dev/null || true
# behind an unconditional '|| true', so on any host without attr(1) -- or
# with a daemon that ignored the xattr -- the trigger silently never fired
# and the leg below asserted that concurrent sweep+write is safe without
# any sweep having run at all. 'kill -USR1 <pid>' is the other trigger the
# daemon documents for the same pass (AGENTS.md §2.5: USR1 and the xattr
# set the same in-process flag), it needs no external tool, and the pass is
# now required to leave a witness on the volume before the leg
# counts.
kill -USR1 "$DPID" || fail "D2c: could not deliver SIGUSR1 to invf-fuse (pid $DPID)"

# Let sweep + write run concurrently for 5 seconds
sleep 5

# Kill the background writer
kill "$WRITER_PID" 2>/dev/null || true
wait "$WRITER_PID" 2>/dev/null || true

# Give the daemon's sweep thread a moment to finish if still running, and
# wait (bounded) for the pass to report DONE. A pass that never reports is
# a pass that never ran, which is the whole point of the assertion below.
DONE_FILES=""
for _ in $(seq 1 150); do
    DONE_FILES=$(sed -n 's/^\[sweep\] DONE files=\([0-9][0-9]*\).*/\1/p' "$WORK/fuse.log" | tail -1)
    [ -n "$DONE_FILES" ] && break
    sleep 0.2
done
if [ -z "$DONE_FILES" ]; then
    echo "FAIL: the USR1 pass never completed -- no '[sweep] DONE' in the daemon log" >&2
    cat "$WORK/fuse.log" >&2
    fail "D2c: no sweep ran"
fi
[ "$DONE_FILES" -gt 0 ] \
    || { cat "$WORK/fuse.log" >&2; fail "D2c: the sweep pass walked 0 files"; }
grep -q '^\[manual\] save point captured' "$WORK/fuse.log" \
    || { cat "$WORK/fuse.log" >&2; fail "D2c: the pass armed no save point (not a manual full pass)"; }
echo "  D2c: the pass really ran (walked $DONE_FILES file(s), save point armed)"

# Verify active.txt is readable and not corrupted
D2C_OK=1
if [ ! -f "$MNT/active.txt" ]; then
    bail "D2c: active.txt missing after concurrent sweep+write"
    D2C_OK=0
else
    LINE_COUNT=$(grep -c '.' "$MNT/active.txt" || true)
    if [ "$LINE_COUNT" -lt 1 ]; then
        bail "D2c: active.txt is empty after concurrent sweep+write"
        D2C_OK=0
    else
        # Each line must match data-NNNNNNNNNNNNNNNNNNN (13-20 digit nanosecond timestamp)
        BAD=$(grep -vE '^data-[0-9]{13,20}$' "$MNT/active.txt" | wc -l || true)
        if [ "$BAD" -ne 0 ]; then
            bail "D2c: $BAD corrupted lines in active.txt"
            grep -vE '^data-[0-9]{13,20}$' "$MNT/active.txt" | head -5
            D2C_OK=0
        else
            echo "  D2c: active.txt has $LINE_COUNT valid lines"
        fi
    fi
fi

mnt_down "$IMG"
fsck_ok "$IMG"
# Proof that a pass really ran. The daemon's own stderr is not a witness
# here: invf-fuse daemonises, and only what the foreground parent printed
# before it forked lands in $WORK/fuse.log. The save point is. Every full
# pass arms one in `prepare`, before the walk (AGENTS.md §2.5), and it
# survives the unmount -- invf-fsck reports it as "save point:   live". A
# volume with no such line is a volume no pass ever touched, which is the
# case this leg used to pass without ever noticing.
SPT0=$(sed -n 's/^ *save point: *\(.*\)$/\1/p' "$WORK/fsck.last" | head -1)
[ "$SPT0" = "live" ] \
    || { echo "FAIL: no live save point after the USR1 pass (got: '${SPT0:-none}')" >&2
         cat "$WORK/fsck.last" >&2; fail "D2c: no sweep ran"; }
echo "  D2c: the pass ran and armed its rollback window (save point: live)"
# This used to be an unconditional 'pass' sitting after the whole if/else
# chain above, so every 'bail' in D2c still printed "PASS: D2c: sweep
# during active writes" and only the summary's FAIL counter carried the
# failure. The verdict belongs here, where the checks actually ran.
if [ "$D2C_OK" = 1 ]; then
    pass "D2c: sweep during active writes, no corruption"
else
    echo "  D2c FAILED (see above)"
fi

# ═══════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════
echo ""
echo "== D2 summary: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
