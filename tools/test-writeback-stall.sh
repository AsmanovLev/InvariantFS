#!/bin/bash
# test-writeback-stall.sh — WP626: the 1 Hz on-demand drain must yield to a
# live foreground write path instead of holding g_io_lock across the whole
# pending set.
#
# THE SHAPE. Every FUSE_WRITE / .flush / .fsync takes g_io_lock before it can
# be served, and the tick's drain re-encodes the WHOLE pending set inside ONE
# hold of that lock (vol_sweep_pending()). So a foreground commit waits out
# the drain: a write+fsync that costs ~10 ms paid 3493 ms against a 40 x 8 MiB
# pending set (src/cli/fuse_fs.c, the g_fg_write_ns block). Scale that to a
# boot root at shutdown — the kernel pushing a whole dirty journal, thousands
# of pending inodes — and page writeback does not complete: `systemd-journal`
# parks in D in fuse_wait_on_page_writeback (INCIDENTS.md, "CI debian
# shutdown saga", layer 3).
#
# WHAT IS ASSERTED HERE (leg A, the contract):
#   1. the drain reports that it YIELDED ("yielding to a live write path") —
#      the deferral path executed, so a green leg is not "the drain had
#      nothing to do";
#   2. every timed foreground write+fsync issued during the storm completes
#      within LAT_LIMIT_MS;
#   3. LIVENESS — once the storm stops, the drain runs and `sync -f` returns
#      within SYNC_LIMIT_S. A deferral that never ends is a leak, not a fix.
#
# LEG B is the A/B control: the same storm with INVFS_SWEEP_DRAIN_DEFER=0
# (the pre-WP626 shape). It does not fail the suite — it exists so the leg A
# numbers can be read against something, and it says WEAK when this host's
# drains are too short to reproduce the stall.
#
# Run from the repo root after `make`:
#   bash tools/test-writeback-stall.sh          (or via tools/run-e2e.sh)
# Knobs: WP626_FILES=12 WP626_MB=6 WP626_STORM_S=8 WP626_LAT_LIMIT_MS=5000
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp626stall
MNT=$WORK/mnt
FILES=${WP626_FILES:-12}      # pending-set size: FILES x MB
MB=${WP626_MB:-6}
STORM_S=${WP626_STORM_S:-8}   # how long the foreground storm runs
LAT_LIMIT_MS=${WP626_LAT_LIMIT_MS:-5000}
SYNC_LIMIT_S=${WP626_SYNC_LIMIT_S:-120}

rm -rf "$WORK" && mkdir -p "$MNT"
cd /dev/shm

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

FG_PID=""
cleanup() {
    [ -n "$FG_PID" ] && kill -KILL "$FG_PID" 2>/dev/null || true
    fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
    FG_PID=""
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts 2>/dev/null || return 0
        sleep 0.1
    done
    echo "WARN: mount still present after cleanup" >&2
}
trap cleanup EXIT

# mnt_fg <img> <log> [env assignments...] — foreground mount, unbuffered log.
# stdbuf matters: the log IS the assertion surface, and a block-buffered
# daemon tail would hide the very line leg A looks for.
mnt_fg() {
    local img=$1 log=$2; shift 2
    rm -f "/dev/shm/$img"
    $B/invf-mkfs "/dev/shm/$img" ${WP626_VOL_G:-1} >/dev/null || fail "mkfs $img"
    # shellcheck disable=SC2086
    env "$@" stdbuf -o0 -e0 $B/invf-fuse -f "/dev/shm/$img" "$MNT" >"$WORK/$log" 2>&1 &
    FG_PID=$!
    disown "$FG_PID" 2>/dev/null || true
    for _ in $(seq 1 100); do
        grep -q " $MNT " /proc/mounts 2>/dev/null && return 0
        kill -0 "$FG_PID" 2>/dev/null || { tail -20 "$WORK/$log"; fail "$img: daemon died before mount appeared"; }
        sleep 0.1
    done
    tail -20 "$WORK/$log"; fail "$img: mount never appeared"
}

# storm <tag> — build the pending set, then run the foreground storm and
# print "MAXLAT <ms>". Latency = small write + fdatasync, once a second,
# which is the shape of a page writeback commit against this mount.
storm() {
    python3 - "$MNT" "$FILES" "$MB" "$STORM_S" <<'PY'
import os, sys, threading, time
mnt, nfiles, mb, storm_s = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
# Compressible text: the drain has real work to do (a verbatim-stored
# incompressible file would make the drain cheap and the leg vacuous).
block = (b"lorem ipsum dolor sit amet consectetur adipiscing elit " * 64)
data = (block * ((mb * 1024 * 1024) // len(block) + 1))[: mb * 1024 * 1024]
for i in range(nfiles):
    with open(os.path.join(mnt, "f%03d.txt" % i), "wb") as f:
        f.write(data)
print("pending set: %d x %d MiB" % (nfiles, mb), flush=True)

stop = threading.Event()
def stormer():                      # continuous foreground arrivals
    # Rate-limited on purpose (~3 MiB/s): the leg needs the write path
    # ARRIVING every tick, not a volume-filling write. An unbounded storm
    # hit ENOSPC on the first version of this gate and turned the leg into
    # an I/O-error test.
    with open(os.path.join(mnt, "storm.bin"), "ab", buffering=0) as f:
        chunk = b"s" * 65536
        while not stop.is_set():
            f.write(chunk)
            f.flush()
            time.sleep(0.02)

t1 = threading.Thread(target=stormer, daemon=True)
t1.start()
lat = []
deadline = time.time() + storm_s
while time.time() < deadline:
    time.sleep(1.0)                 # one probe per tick of the 1 Hz loop
    t0 = time.time()
    fd = os.open(os.path.join(mnt, "probe.bin"), os.O_WRONLY | os.O_CREAT, 0o644)
    os.write(fd, b"p" * 4096)
    os.fsync(fd)
    os.close(fd)
    lat.append((time.time() - t0) * 1000.0)
    print("  probe %d: %.0f ms" % (len(lat), lat[-1]), flush=True)
stop.set(); t1.join(5)
print("MAXLAT %d" % (int(max(lat)) if lat else -1), flush=True)
PY
}

# drains <log> — drains the daemon actually RAN (the yield line is counted
# separately: a leg whose only "on-demand" line is a yield ran nothing).
drains() { grep -c "on-demand: processed" "$WORK/$1" 2>/dev/null || true; }

leg() {   # <name> <defer-env-value|default>
    local name=$1 defer=$2 maxlat before after t0 dt
    echo "== leg $name (INVFS_SWEEP_DRAIN_DEFER=$defer) =="
    if [ "$defer" = default ]; then
        mnt_fg "wp626-$name.img" "$name.log"          # shipped default: unset
    else
        mnt_fg "wp626-$name.img" "$name.log" "INVFS_SWEEP_DRAIN_DEFER=$defer"
    fi
    before=$(drains "$name.log")
    maxlat=$(storm "$name" | tee "$WORK/$name.storm" | awk '/^MAXLAT/ {print $2}')
    after=$(drains "$name.log")
    echo "  $name: max foreground write+fsync ${maxlat} ms; drains during storm $((after-before)); yields $(grep -c 'yielding to a live write path' "$WORK/$name.log" || true)"
    grep "held g_io_lock" "$WORK/$name.log" | tail -3 | sed 's/^/    /' || true
    # LIVENESS: storm over, so the drain must run and sync must return.
    # LIVENESS: the storm is over, so the drain must come back BY ITSELF —
    # no USR1, no offline sweep. A fresh file guarantees the pending list is
    # non-empty (a flush during the storm can empty it), and the poll gives
    # the 1 Hz tick room: the marker write itself is an arrival, so the next
    # tick defers once more and the one after it drains.
    local ok=0
    echo quiet > "$MNT/wp626-quiet-marker"
    sync -f "$MNT" 2>/dev/null || true
    t0=$(date +%s)
    for _ in $(seq 1 20); do
        grep -q "on-demand: processed" "$WORK/$name.log" && { ok=1; break; }
        sleep 1
    done
    dt=$(($(date +%s) - t0))
    grep -q "commit .* failed" "$WORK/$name.log" && { grep "commit .* failed" "$WORK/$name.log" | tail -2; fail "$name: the volume filling up ended the leg (ENOSPC); raise WP626_VOL_G"; }
    [ "$ok" = 1 ] || fail "$name: the drain never ran after the write path went quiet (${dt}s) — a deferral that never ends is a leak, not a yield"
    echo "  $name: drain resumed ${dt}s after the write path went quiet; drains $(drains "$name.log")"
    t0=$(date +%s)
    timeout "$SYNC_LIMIT_S" sync -f "$MNT" || fail "$name: sync -f did not return within ${SYNC_LIMIT_S}s with the daemon alive"
    echo "  $name: post-drain sync -f $(($(date +%s) - t0))s"
    cleanup
    MAXLAT=$maxlat
}

MAXLAT=0
leg gate-on default            # the shipped behaviour: the gate is ON unset
A_MAX=$MAXLAT
grep -q "yielding to a live write path" "$WORK/gate-on.log" \
    || fail "legA: the drain never yielded — the gate did not engage, so this leg proves nothing"
[ "$A_MAX" -ge 0 ] || fail "legA: no latency probe completed"
[ "$A_MAX" -le "$LAT_LIMIT_MS" ] \
    || fail "legA: foreground write+fsync peaked at ${A_MAX} ms (limit ${LAT_LIMIT_MS} ms) while the drain held g_io_lock"

leg control-off 0
B_MAX=$MAXLAT
if [ "$B_MAX" -lt $((A_MAX + 500)) ]; then
    echo "WEAK: control leg peaked at ${B_MAX} ms vs ${A_MAX} ms — this host's drain is too short to reproduce the stall; leg A's limit still holds"
else
    echo "CONTROL: pre-gate shape peaked at ${B_MAX} ms vs ${A_MAX} ms with the gate"
fi

pass "drain yields to the foreground (legA peak ${A_MAX} ms, limit ${LAT_LIMIT_MS} ms; control ${B_MAX} ms); drain still runs once the write path is quiet"
echo "logs: $WORK"
