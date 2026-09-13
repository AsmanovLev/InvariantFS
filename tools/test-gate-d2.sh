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

mnt_up() {
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
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
        pgrep -f "invf-fuse $1" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
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
mnt_up "$IMG"

# Import some initial data to have something to sweep
$B/invf-cp "$IMG" /etc/hostname pre-sweep.txt >/dev/null 2>&1 || true
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

# Trigger sweep via extended attribute (daemon runs it synchronously)
setfattr -n user.invfs.sweep -v 1 "$MNT/" 2>/dev/null || true

# Let sweep + write run concurrently for 5 seconds
sleep 5

# Kill the background writer
kill "$WRITER_PID" 2>/dev/null || true
wait "$WRITER_PID" 2>/dev/null || true

# Give the daemon's sweep thread a moment to finish if still running
sleep 2

# Verify active.txt is readable and not corrupted
if [ ! -f "$MNT/active.txt" ]; then
    bail "D2c: active.txt missing after concurrent sweep+write"
else
    LINE_COUNT=$(grep -c '.' "$MNT/active.txt" || true)
    if [ "$LINE_COUNT" -lt 1 ]; then
        bail "D2c: active.txt is empty after concurrent sweep+write"
    else
        # Each line must match data-NNNNNNNNNNNNNNNNNNN (13-20 digit nanosecond timestamp)
        BAD=$(grep -vE '^data-[0-9]{13,20}$' "$MNT/active.txt" | wc -l || true)
        if [ "$BAD" -ne 0 ]; then
            bail "D2c: $BAD corrupted lines in active.txt"
            grep -vE '^data-[0-9]{13,20}$' "$MNT/active.txt" | head -5
        else
            echo "  D2c: active.txt has $LINE_COUNT valid lines"
        fi
    fi
fi

mnt_down "$IMG"
fsck_ok "$IMG"
pass "D2c: sweep during active writes, no corruption"

# ═══════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════
echo ""
echo "== D2 summary: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
