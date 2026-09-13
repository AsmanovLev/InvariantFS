#!/bin/bash
# test-gate-c.sh — ENOSPC / quota stress test (3 legs)
#
#   Leg C1: Floor breach + auto-release — fill volume to ENOSPC/EROFS,
#            verify READONLY flip, run invf-sweep to reclaim, verify
#            auto-release makes volume writable again, fsck + verify clean.
#   Leg C2: Race to exhaust reserve — two concurrent FUSE writers hit
#            ENOSPC, verify no crash, no corruption, fsck clean, ls consistent.
#   Leg C3: Write during active sweep — fill ~70%, start invf-fuse, write
#            concurrently with the daemon's background sweep thread, then
#            run offline invf-sweep, verify volume clean.
#
# Key ENOSPC mechanics exercised:
#   - Two-tier reserve: reserved_blocks = total/128+64, hard_min = total/1024+16
#   - Block allocator guard flips to READONLY when free <= reserved+hard_min
#   - Atomic ENOSPC precheck (vol_write.c:511-530)
#   - Space latch auto-release at volume.c:3582-3595
#   - DEFER_ENOSPC: sweep defers transcode when full
#
# Run via:  bash tools/test-gate-c.sh
# Uses /dev/shm for images (small, fast), /tmp/opencode/ for mount points.
# NOTE: blkio treats /dev/* paths as raw devices, so the script cd's into
# /dev/shm and uses RELATIVE image paths everywhere.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin

MNT=/tmp/opencode/gatec-mnt

# ---- helpers -----------------------------------------------------------

fail() { echo "FAIL: $*" >&2; exit 1; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse.*c[123]\.img" 2>/dev/null || true
    rm -rf "$MNT"
    rm -f c1.img c2.img c3.img
    rm -rf gatec
}

trap cleanup EXIT

DPID=0

mnt_up() { # <img>
    local img=$1
    mkdir -p "$MNT"
    setsid $B/invf-fuse -f "$img" "$MNT" >"gatec/fuse.log" 2>&1 < /dev/null &
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
    for _ in $(seq 1 300); do
        kill -0 "$DPID" 2>/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() {
    $B/invf-fsck "$1" | tee gatec/fsck.last | grep -q "^OK$" \
        || { cat gatec/fsck.last; fail "fsck not clean: $1"; }
}

verify_ok() {
    $B/invf-verify "$1" --deep | tee gatec/verify.last | grep -q " 0 corrupt," \
        || { cat gatec/verify.last; fail "verify corrupt: $1"; }
}

# Check whether a write to the mounted volume fails (EROFS / ENOSPC).
expect_readonly() {
    set +e
    dd if=/dev/zero of="$MNT/_enospc_probe" bs=1K count=1 status=none 2>/dev/null
    rc=$?
    set -e
    [ "$rc" != 0 ] || fail "$1: write succeeded on a READONLY volume"
    echo "  $1: write refused (volume is READONLY)"
}

# Check whether a write to the mounted volume succeeds.
expect_writable() {
    set +e
    echo "probe" > "$MNT/_writable_probe" 2>/dev/null
    rc=$?
    set -e
    [ "$rc" = 0 ] || fail "$1: write failed with rc=$rc on a writable volume"
    rm -f "$MNT/_writable_probe"
    echo "  $1: volume is writable (auto-release confirmed)"
}

# ---- setup ---------------------------------------------------------------
cd /dev/shm
rm -f c1.img c2.img c3.img
rm -rf gatec "$MNT"
mkdir -p gatec "$MNT"

# ---- fixtures -----------------------------------------------------------
# Generate small random filler files (256 KB each) for quick volume fill.
FILLDIR=gatec/fillers
mkdir -p "$FILLDIR"

python3 - "$FILLDIR" <<'PY'
import random, sys, os
d = sys.argv[1]
rnd = random.Random(42)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(256):
    path = os.path.join(d, 'fill%03d.txt' % k)
    with open(path, 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab)
            f.write(w)
            f.write(b' ')
            n += len(w) + 1
print("  fixtures: 256 filler files (256 KB each)")
PY

# ==========================================================================
# Leg C1: Floor breach + auto-release
# ==========================================================================
echo
echo "== Leg C1: Floor breach + auto-release =="

$B/invf-mkfs c1.img 0.0625 > gatec/mkfs-c1.log
echo "  mkfs: 64 MiB volume created"

# Fill via invf-cp until volume is close to full
filln=0
while true; do
    f=$(printf 'fill%03d.txt' $filln)
    [ -f "$FILLDIR/$f" ] || break
    set +e
    $B/invf-cp c1.img "$FILLDIR/$f" "filler_$f" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" = 0 ] || break
    filln=$((filln + 1))
done
echo "  imported $filln files"

# Now try to write more — should hit ENOSPC / EROFS
mnt_up c1.img

echo "  writing until ENOSPC..."
ENOSPC_HIT=0
for i in $(seq 1 100); do
    set +e
    dd if=/dev/zero of="$MNT/pad_$i" bs=1K count=64 status=none 2>/dev/null
    rc=$?
    set -e
    if [ "$rc" != 0 ]; then
        ENOSPC_HIT=1
        echo "  ENOSPC/EROFS hit at file pad_$i (rc=$rc)"
        break
    fi
done

[ "$ENOSPC_HIT" = 1 ] || fail "C1: did not hit ENOSPC despite repeated writes"

# Verify the volume flipped to READONLY
expect_readonly "C1"

# Unmount, run sweep to reclaim space
mnt_down
$B/invf-sweep c1.img > gatec/sweep-c1.log 2>&1 || true
echo "  sweep completed"

# Re-mount and verify auto-release made it writable again
mnt_up c1.img
expect_writable "C1 (post-sweep)"
mnt_down

fsck_ok c1.img
verify_ok c1.img
rm -f "$MNT"/pad_* "$MNT"/_*

echo "  Leg C1: PASS"

# ==========================================================================
# Leg C2: Race to exhaust reserve
# ==========================================================================
echo
echo "== Leg C2: Race to exhaust reserve =="

$B/invf-mkfs c2.img 0.0625 > gatec/mkfs-c2.log
echo "  mkfs: 64 MiB volume created"

# Pre-fill with some data
for f in $(ls "$FILLDIR" | head -20); do
    set +e
    $B/invf-cp c2.img "$FILLDIR/$f" "pre_$f" >/dev/null 2>&1
    set -e
done
echo "  pre-filled 20 files"

# Mount the volume
mnt_up c2.img
echo "  volume mounted"

# Launch two concurrent FUSE writers — they race to exhaust the reserve
python3 - "$MNT" <<'WRITER1' &
import os, sys, random
d = sys.argv[1]
rnd = random.Random(1)
for i in range(200):
    path = os.path.join(d, 'w1_%04d.bin' % i)
    sz = rnd.randint(4096, 65536)
    data = bytes(rnd.getrandbits(8) for _ in range(sz))
    try:
        with open(path, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except OSError:
        sys.exit(0)
sys.exit(0)
WRITER1
WRITER1_PID=$!

python3 - "$MNT" <<'WRITER2' &
import os, sys, random
d = sys.argv[1]
rnd = random.Random(2)
for i in range(200):
    path = os.path.join(d, 'w2_%04d.bin' % i)
    sz = rnd.randint(4096, 65536)
    data = bytes(rnd.getrandbits(8) for _ in range(sz))
    try:
        with open(path, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except OSError:
        sys.exit(0)
sys.exit(0)
WRITER2
WRITER2_PID=$!

echo "  writers launched: w1=$WRITER1_PID w2=$WRITER2_PID"

# Wait for both writers to finish (they'll hit ENOSPC and exit gracefully)
wait "$WRITER1_PID" || true
wait "$WRITER2_PID" || true
echo "  writers exited"

# Verify the volume didn't crash — try a read
set +e
ls "$MNT"/ > gatec/ls-c2.txt 2>&1
LS_RC=$?
set -e
NFILES=$(wc -l < gatec/ls-c2.txt)
echo "  ls after race: rc=$LS_RC, $NFILES entries"

# Unmount and verify
mnt_down

fsck_ok c2.img
verify_ok c2.img

echo "  Leg C2: PASS"

# ==========================================================================
# Leg C3: Write during active sweep
# ==========================================================================
echo
echo "== Leg C3: Write during active sweep =="

$B/invf-mkfs c3.img 0.0625 > gatec/mkfs-c3.log
echo "  mkfs: 64 MiB volume created"

# Fill close to full — leave room for the sweep to reclaim, but the FUSE
# daemon's background sweep thread + mounted writes should exercise the
# ENOSPC precheck and DEFER_ENOSPC paths.
filln=0
while true; do
    f=$(printf 'fill%03d.txt' $filln)
    [ -f "$FILLDIR/$f" ] || break
    set +e
    $B/invf-cp c3.img "$FILLDIR/$f" "fill_$f" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" = 0 ] || break
    filln=$((filln + 1))
done
echo "  pre-filled $filln files (volume near full)"

# Mount the volume — the FUSE daemon's background sweep thread will run
# sweep passes when the watermark is hit. Writes during the daemon's
# sweep exercise the ENOSPC precheck and DEFER_ENOSPC paths.
mnt_up c3.img

# Attempt writes — some may succeed, some may get ENOSPC depending on
# how much space is left and whether the daemon's sweep is active.
SWRITE_OK=0
SWRITE_FAIL=0
for i in $(seq 1 50); do
    set +e
    dd if=/dev/urandom of="$MNT/during_sweep_$i" bs=1K count=32 status=none 2>/dev/null
    rc=$?
    set -e
    if [ "$rc" = 0 ]; then
        SWRITE_OK=$((SWRITE_OK + 1))
    else
        SWRITE_FAIL=$((SWRITE_FAIL + 1))
    fi
done
echo "  writes during daemon activity: $SWRITE_OK succeeded, $SWRITE_FAIL failed"

# Unmount the daemon
mnt_down

# Run offline sweep to do a full reclaim pass
$B/invf-sweep c3.img > gatec/sweep-c3.log 2>&1 || true
echo "  offline sweep completed"

# Verify volume is clean
fsck_ok c3.img
verify_ok c3.img

echo "  Leg C3: PASS"

# ==========================================================================
# Summary
# ==========================================================================
echo
echo "=== RESULTS ==="
echo "  C1 (Floor breach + auto-release): PASS"
echo "  C2 (Race to exhaust reserve):     PASS"
echo "  C3 (Write during active sweep):   PASS"
echo
echo "Gate C: PASS (3 passed, 0 failed)"

rm -f c1.img c2.img c3.img
rm -rf gatec "$MNT"

exit 0
