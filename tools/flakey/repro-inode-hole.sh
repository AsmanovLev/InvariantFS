#!/bin/bash
# repro-inode-hole.sh — minimal reproducer for the WP22b leg-2 finding:
#
#   "fsync-acknowledged files vanish after a dm-flakey error window:
#    a failed append to the append-only inode area leaves a zero hole on
#    the device, the in-RAM append cursor has already advanced past it,
#    so every record committed AFTER the hole is valid CRC-wise but
#    unreachable at next open (vol_open's scan stops at the hole).
#    fsck reports the lost files' blocks as orphans; verify --deep is
#    blind to them (they are not live). Silent loss of acknowledged
#    writes."
#
# Stack: sparse backing file -> loop -> dm-flakey(up) -> mkfs -> FUSE
# mount -> background writer (small files, fsync each) -> 3s full-error
# window -> 3s more writing -> clean unmount -> fsck. Invariant: every
# file whose fsync returned 0 on the healthy device must be present and
# bit-exact. A missing one = the bug.
#
# Exit: 0 = invariant held (bug NOT reproduced); 1 = reproduced / error.
# Env: FLK_REPRO_WORK (default /tmp/invfs-flk-repro), KEEP=1 keeps it.
set -uo pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
W=${FLK_REPRO_WORK:-/tmp/invfs-flk-repro}
DEV=invfs_flk_repro
DM=/dev/mapper/$DEV
BACK=$W/backing.img
MNT=$W/mnt
LOOP=""

cleanup() {
    set +e
    fusermount3 -u "$MNT" 2>/dev/null
    for i in $(seq 1 50); do pgrep -f "invf-fuse $DM" >/dev/null || break; sleep 0.2; done
    pkill -9 -f "invf-fuse $DM" 2>/dev/null
    dmset "0 $SEC flakey $LOOP 0 3600 0" 2>/dev/null
    sudo -n dmsetup remove "$DEV" 2>/dev/null
    [ -n "$LOOP" ] && sudo -n losetup -d "$LOOP" 2>/dev/null
    [ "${KEEP:-0}" = 1 ] || rm -rf "$W"
}
trap cleanup EXIT

dmset() {  # dmset "<table>"
    local i
    for i in $(seq 1 20); do
        sudo -n dmsetup suspend --noflush "$DEV" 2>/dev/null && break
        sleep 0.2
    done
    sudo -n dmsetup reload "$DEV" --table "$1" 2>/dev/null
    sudo -n dmsetup resume "$DEV" 2>/dev/null
}

sudo -n modprobe dm-flakey 2>/dev/null
sudo -n dmsetup targets | grep -q flakey || { echo "no dm-flakey"; exit 2; }
rm -rf "$W"; mkdir -p "$W" "$MNT"
truncate -s 2G "$BACK"
LOOP=$(sudo -n losetup -f --show "$BACK") || exit 2
SEC=$(sudo -n blockdev --getsz "$LOOP")
sudo -n dmsetup create "$DEV" --table "0 $SEC flakey $LOOP 0 3600 0" || exit 2
sudo -n chmod 666 "$DM"

$B/invf-mkfs "$DM" >/dev/null || { echo "mkfs failed"; exit 2; }
# preload: ~120MB swept corpus so the daemon's drain has real work and the
# volume carries the usual metadata history (this matches the leg-2 shape
# that first reproduced the hole)
python3 - "$W" <<'PY'
import os, random, sys
random.seed(7)
d = sys.argv[1]
WORDS = ("the quick brown fox jumps over lazy dogs int static return\n").split()
out = []; n = 0
while n < 40 * 1024 * 1024:
    w = random.choice(WORDS); out.append(w); n += len(w) + 1
open(os.path.join(d, "corpus.txt"), "w").write(" ".join(out))
open(os.path.join(d, "corpus.bin"), "wb").write(random.randbytes(60 * 1024 * 1024))
PY
$B/invf-cp "$DM" "$W/corpus.txt" corpus.txt >/dev/null
$B/invf-cp "$DM" "$W/corpus.bin" corpus.bin >/dev/null
$B/invf-sweep "$DM" >/dev/null 2>&1
$B/invf-sweep "$DM" --realize >/dev/null 2>&1
rm -f "$W/corpus.txt" "$W/corpus.bin"
$B/invf-fuse "$DM" "$MNT" 2>"$W/fuse.log"
for i in $(seq 1 50); do grep -q " $MNT " /proc/mounts && break; sleep 0.1; done
grep -q " $MNT " /proc/mounts || { echo "mount failed"; exit 2; }

# writer: 2MB files, fsync each, log OK/ERR per file
python3 - "$MNT" "$W/stop" "$W/writer.log" <<'PY' &
import os, sys, hashlib, random
MNT, STOP, LOG = sys.argv[1:4]
rng = random.Random(1234)
i = 0
with open(LOG, "w") as lg:
    while not os.path.exists(STOP) and i < 400:
        name = "w%03d.bin" % i
        data = rng.randbytes(2 * 1024 * 1024)
        try:
            fd = os.open(os.path.join(MNT, name), os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
            try:
                mv = memoryview(data)
                while mv:
                    n = os.write(fd, mv); mv = mv[n:]
                os.fsync(fd)
            finally:
                os.close(fd)
            lg.write("OK %s %s\n" % (name, hashlib.sha256(data).hexdigest()))
        except OSError as e:
            lg.write("ERR %s %s\n" % (name, e.strerror or e))
        lg.flush(); i += 1
PY
WPID=$!

sleep 2
dmset "0 $SEC error"
echo "storm on (5s)"
sleep 5
dmset "0 $SEC flakey $LOOP 0 3600 0"
echo "storm off"
sleep 2
touch "$W/stop"; wait $WPID

fusermount3 -u "$MNT"
for i in $(seq 1 300); do pgrep -f "invf-fuse $DM" >/dev/null || break; sleep 0.2; done
pgrep -f "invf-fuse $DM" >/dev/null && { echo "daemon wedged"; exit 2; }

# ground truth: close everything, then read the device fresh
$B/invf-fsck "$DM" | tail -3
lsout=$($B/invf-ls "$DM")
python3 - "$W/writer.log" "$B" "$DM" "$W" <<'PY'
import os, subprocess, sys, hashlib
log, B, DM, W = sys.argv[1:5]
present = set()
for ln in subprocess.run([os.path.join(B, "invf-ls"), DM],
                         capture_output=True, text=True).stdout.splitlines():
    p = ln.split()
    if len(p) >= 4 and p[1] == "bytes" and p[2] == "inode":
        present.add(p[-1])
missing = bad = ok = 0
for ln in open(log):
    p = ln.split()
    if len(p) < 3 or p[0] != "OK":
        continue
    name, sha = p[1], p[2]
    if name not in present:
        missing += 1
        if missing <= 5: print("  MISSING after clean unmount: %s (fsync had succeeded)" % name)
        continue
    outf = os.path.join(W, "x.out")
    r = subprocess.run([os.path.join(B, "invf-cat"), DM, name, outf],
                       capture_output=True)
    if r.returncode != 0 or hashlib.sha256(open(outf, "rb").read()).hexdigest() != sha:
        bad += 1
    else:
        ok += 1
print("fsync-acked files: %d ok, %d WRONG BYTES, %d MISSING" % (ok, bad, missing))
if bad or missing:
    print("BUG REPRODUCED: acknowledged writes lost across an error window")
    sys.exit(1)
print("invariant held (bug not reproduced this run)")
sys.exit(0)
PY
