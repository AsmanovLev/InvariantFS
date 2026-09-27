#!/bin/bash
# test-imagelock.sh — one writer per image, across a live save-point window.
#
# HISTORY. This file was `tools/test-rocp.sh`, the WP24-lite suite for the v2
# read-only time-travel mount (`vol_open_at` / `-o at_checkpoint`). WP-M21
# retired the mechanism it tested: the CKP0 sweep checkpoint and the
# "\x01reten" retention registry went with the v2 metadata machinery, and
# `vol_ckp_begin` is now a stub that declines (`src/core/vol_rollback.c:65`),
# so no sweep has written a CKP0 since. `ckp_stage_replay` is a stub returning
# -3 (`src/core/vol_rollback.c:109`), and the only path that could reach it is
# `vol_open_at` (`src/core/volume.c:1511`). So `-o at_checkpoint` on a v3 volume
# can only ever fail: v3 rollback is the SPT0 save point (`src/core/vol_spt0.c`),
# which is a RESTORE, not a view. Per AGENTS.md 1.7 the test was the thing that
# was wrong, so it was ported rather than the feature restored. What is lost —
# a read-only view at a point in time — is filed as an OPEN finding in
# INCIDENTS.md, and leg [V] below pins the current answer (a loud refusal) so
# the retired mount option cannot drift into looking functional.
#
# What survives, and why it is worth its own suite:
#
#   [A] a corpus built through a real RW FUSE mount; fsck clean.
#   [B] the sweep arms the v3 rollback window (SPT0 save point) — the v2
#       CKP0 arm, restated against the code that exists. fsck reports it live.
#   [C] the present moves on: overwrite one file, delete another, add a third.
#   [L] THE SINGLE-WRITER LOCK. While a mount holds the image, every other
#       opener — a second RW mount, a second RO mount, an offline sweep, an
#       offline reader — is refused with "image is in use by another process"
#       (`src/core/volume.c:1176-1180`, flock LOCK_EX|LOCK_NB). This is the
#       guarantee that keeps two in-memory bitmaps from interleaving appends
#       into the same image, and it is what the v2 suite's D2 leg asserted. It
#       is version-independent, and no other suite ASSERTS it (test-flakey.sh
#       only tolerates losing the race), so dropping this file would have
#       quietly deleted the only test of it. Asserted here with the save point
#       live, which is the question the v2 leg was really asking: an armed
#       rollback window is not a lock and must not act like one.
#   [V] `-o at_checkpoint` and `-o at_checkpoint=<seq>` REFUSE loudly on a v3
#       volume and mount nothing. This is the port of the v2 legs D/D3: the
#       answer changed from "here is the view" to "there is no view", and a
#       refusal is a real contract that can be asserted. See INCIDENTS.md.
#   [E] the present is intact, the refused openers changed nothing, and a RW
#       mount still writes.
#
# NOT here, because tools/test-rollback.sh owns it (Makefile:411): that a
# rollback restores the pre-sweep state bit-exactly, that the SPN0 data pin
# holds, that a refused restore leaves the volume untouched, and that
# `invf-sweep --realize` / `spt0_drop` is the point of no return. This suite
# asserts only that a window is ARMED, which is the premise [L] needs.
#
# Run from the repo root after `make`:  bash tools/test-imagelock.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoints are absolute).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WORK=/dev/shm/wp122imglock
IMG=wp122imglock.img
MNT=$WORK/mnt        # the RW mount under test
MNT2=$WORK/mnt2      # the second-opener refusal target
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/mnt2" "$WORK/v1" "$WORK/v2" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {   # <img> [extra -o opts...] — daemonized mount, wait for /proc/mounts
    local img=$1; shift
    $B/invf-fuse "$@" "$img" "$MNT" 2>"$WORK/fuse.last.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    cat "$WORK/fuse.last.log" >&2
    fail "mount of $img never appeared"
}

mnt_down() { # unmount MNT and wait for the daemon to exit (it holds the flock)
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $IMG" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    fusermount3 -u "$MNT2" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
}
trap cleanup EXIT

# refuses_locked <label> <cmd...>: the command must exit non-zero, say the flock
# refusal, and leave nothing mounted. This is the whole of [L].
refuses_locked() {
    local label=$1; shift
    set +e
    "$@" >"$WORK/lock.txt" 2>&1
    local rc=$?
    set -e
    [ "$rc" != 0 ] || { cat "$WORK/lock.txt"; fail "[L] $label: SUCCEEDED while the image was held"; }
    grep -q "image is in use by another process" "$WORK/lock.txt" \
        || { cat "$WORK/lock.txt"; fail "[L] $label: refused, but not by the flock"; }
    if grep -q " $MNT2 " /proc/mounts; then
        fail "[L] $label: a mount appeared anyway"
    fi
    echo "  $label: refused (flock)"
}

echo
echo "== [A] mkfs + v1 corpus (through a RW FUSE mount) =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
python3 - <<'PY'
import os, random
random.seed(24)
d = "/dev/shm/wp122imglock/v1"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
text_file("a.c", 120_000)
text_file("h.py", 45_000)
payload = bytearray()
pat = bytes(range(64)) * 4 + b"\x00" * 128 + os.urandom(64)
while len(payload) < 300_000:
    payload += pat
    payload += bytes([random.randrange(256)]) * 32
h = bytearray(64)
h[0:4] = b"\x7fELF"; h[18] = 62   # EM_X86_64
open(os.path.join(d, "bin_x64"), "wb").write(bytes(h) + bytes(payload[:300_000]))
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(150_000))
print("v1 corpus:", *sorted(os.listdir(d)))
PY

mnt_up "$IMG"
for f in a.c h.py bin_x64 rand.bin; do
    cp "$WORK/v1/$f" "$MNT/$f"
done
mkdir "$MNT/emptydir"
sync
mnt_down
fsck_ok "$IMG"

echo
echo "== [B] the sweep arms the v3 rollback window (SPT0 save point) =="
# The v2 leg here grepped the sweep log for "checkpoint: #1 armed" and "N
# retained blocks held for rollback". Neither string is in the tree: the sweep's
# v3 branch is tools/invf-sweep.c:1661-1700 (drop the previous window, capture
# a fresh one before the walk) and the registry write is gated on !VOLF_V3
# (tools/invf-sweep.c:2085). What it must print now is the save point.
$B/invf-sweep "$IMG" > "$WORK/sweep.log" 2>&1 || { cat "$WORK/sweep.log"; exit 1; }
grep -q "save point captured" "$WORK/sweep.log" \
    || { cat "$WORK/sweep.log"; fail "B: the sweep armed no save point"; }
grep "save point" "$WORK/sweep.log"
# and the window is durable enough that a fresh reader sees it
$B/invf-fsck "$IMG" | tee "$WORK/fsck-b.log" | grep -qE "save point: +live" \
    || { cat "$WORK/fsck-b.log"; fail "B: fsck does not report a live save point"; }
echo "  fsck: save point: live"
fsck_ok "$IMG"
# What the window BUYS — the rollback itself, the SPN0 data pin, the refusal
# path, --realize — is asserted by tools/test-rollback.sh, not here.

echo
echo "== [C] the present moves on: overwrite a.c, delete h.py, add new.txt =="
python3 -c "open('$WORK/v2/a.c','w').write('post-savepoint edition of a.c\n' * 4000)"
python3 -c "open('$WORK/v2/new.txt','w').write('born after the save point\n' * 500)"
mnt_up "$IMG"
cp "$WORK/v2/a.c" "$MNT/a.c"
rm "$MNT/h.py"
cp "$WORK/v2/new.txt" "$MNT/new.txt"
sync
sha256sum "$MNT/a.c" | awk '{print $1}' > "$WORK/v2/a.c.sha"
mnt_down
fsck_ok "$IMG"
$B/invf-ls "$IMG" | grep -q "new.txt" || fail "C: new.txt missing in the present"
if $B/invf-ls "$IMG" | grep -q "h.py"; then fail "C: h.py still present"; fi
echo "  present state: a.c=v2, h.py deleted, new.txt added"

echo
echo "== [L] one writer per image: every second opener is REFUSED =="
# The save point from [B] is still live, so this also answers the question the
# v2 D2 leg asked: a held image does not become openable just because a
# rollback window exists. The window is not a lock and must not act like one.
mnt_up "$IMG"
echo "  the mount holds the image; a save point is live (armed in [B], not yet rolled back)"
refuses_locked "second RW mount"  $B/invf-fuse "$IMG" "$MNT2"
# A shared reader is refused too. The holder took LOCK_EX, and flock's LOCK_EX
# excludes LOCK_SH as well, so even the operator's explicit opt-in
# (INVFS_RO_LOCK / INVFS_ALLOW_SHARED, src/core/volume.c:1173) cannot join a
# mounted image. That is the correct conservative answer: a shared reader is
# only safe against another SHARED holder, never against a live RW mount.
refuses_locked "second RO mount"  env INVFS_RO_LOCK=1 $B/invf-fuse "$IMG" "$MNT2"
refuses_locked "shared reader"    env INVFS_RO_LOCK=1 $B/invf-ls "$IMG"
# the offline tools are the ones this actually protects: a sweep running
# against a mounted image interleaves two in-memory bitmaps into one file
refuses_locked "offline sweep"    $B/invf-sweep "$IMG"
refuses_locked "offline reader"   $B/invf-ls "$IMG"
# the refusals wrote nothing: the mount still works and the present is intact
cmp -s "$WORK/v2/new.txt" "$MNT/new.txt" || fail "L: a refused opener damaged new.txt"
cmp -s "$WORK/v2/a.c" "$MNT/a.c" || fail "L: a refused opener damaged a.c"
[ ! -e "$MNT/h.py" ] || fail "L: a refused opener resurrected h.py"
echo "  every refused opener left the present byte-identical"
mnt_down
# Control: the same command that was refused a moment ago now succeeds, so the
# refusals above were the LOCK and not a broken tool or a stale image.
set +e
env INVFS_RO_LOCK=1 $B/invf-ls "$IMG" >"$WORK/shared-ro.log" 2>&1
RC=$?
set -e
[ "$RC" = 0 ] || { cat "$WORK/shared-ro.log"; fail "L: the reader still failed with the image free"; }
grep -q "new.txt" "$WORK/shared-ro.log" || fail "L: the unlocked reader saw no present"
echo "  control: with the image free the same reader succeeds (the refusals were the lock)"
fsck_ok "$IMG"

echo
echo "== [V] -o at_checkpoint is a RETIRED option: it refuses, it does not view =="
# This is the port of the v2 legs D / D3 / D4. On v3 there is no read-only view
# at a point in time: vol_open_at asks for a live CKP0, no v3 sweep has written
# one since WP-M21, and ckp_stage_replay is a stub, so the open always fails
# with -11 (src/core/volume.c:1444-1452). Asserting the REFUSAL is the honest
# v3 contract: the option is still parsed, and it cannot mount a view. If a
# future WP gives SPT0 a read-only view, this leg fails — which is the point.
set +e
$B/invf-fuse -o at_checkpoint "$IMG" "$MNT" >"$WORK/fuse.tt.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "V: -o at_checkpoint mounted a view on a v3 volume"
grep -q "cannot mount at_checkpoint: no live sweep checkpoint" "$WORK/fuse.tt.log" \
    || { cat "$WORK/fuse.tt.log"; fail "V: the refusal was not the checkpoint one"; }
if grep -q " $MNT " /proc/mounts; then fail "V: a mount appeared"; fi
echo "  -o at_checkpoint: refused, nothing mounted"
# ...and the refusal is specifically about the missing checkpoint, never a
# lock we are accidentally passing off for one. The two must stay
# distinguishable, or the message lies about the real cause.
set +e
$B/invf-fuse -o at_checkpoint=1 "$IMG" "$MNT" >"$WORK/fuse.seq.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "V: -o at_checkpoint=1 mounted a view"
grep -q "no live sweep checkpoint" "$WORK/fuse.seq.log" \
    || { cat "$WORK/fuse.seq.log"; fail "V: at_checkpoint=1 gave a different failure"; }
if grep -q "image is in use by another process" "$WORK/fuse.seq.log"; then
    fail "V: at_checkpoint was refused by the flock, not by the missing checkpoint"
fi
if grep -q " $MNT " /proc/mounts; then fail "V: a mount appeared"; fi
echo "  -o at_checkpoint=1: same refusal (checkpoint, not lock)"
fsck_ok "$IMG"

echo
echo "== [E] the present is intact and still writable =="
mnt_up "$IMG"
if grep -q "not closed cleanly" "$WORK/fuse.last.log"; then
    fail "E: a refused opener left the volume dirty"
fi
sha256sum "$MNT/a.c" | awk '{print $1}' > "$WORK/out/a.c.v2.sha"
cmp -s "$WORK/v2/a.c.sha" "$WORK/out/a.c.v2.sha" \
    || fail "E: a.c not at v2 bytes in the present"
[ ! -e "$MNT/h.py" ] || fail "E: h.py resurrected in the present"
cmp -s "$WORK/v2/new.txt" "$MNT/new.txt" || fail "E: new.txt lost from the present"
for f in bin_x64 rand.bin; do
    cmp -s "$WORK/v1/$f" "$MNT/$f" || fail "E: $f lost bit-exactness"
done
# a RW mount still writes fine after all the refusals
cp "$WORK/v1/h.py" "$MNT/after.txt"
sync
cmp -s "$WORK/v1/h.py" "$MNT/after.txt" || fail "E: post-refusal write broken"
rm "$MNT/after.txt"
sync
mnt_down
fsck_ok "$IMG"
$B/invf-verify "$IMG" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "E: verify --deep not clean"
echo "  present intact: a.c=v2, h.py deleted, new.txt present, corpus bit-exact, RW writes work"

echo
echo "IMAGELOCK E2E: PASS"
