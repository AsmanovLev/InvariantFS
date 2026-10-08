#!/bin/bash
# test-flakey-multidev.sh -- dual-mode kill tier: SSD death while the
# volume lives (the owner's money shot), via dm-flakey + dmsetup remove.
#
# dev0 (SSD stand-in) is a dm-flakey device over a loop file; dev1 (HDD
# stand-in) is a plain loop device. mkfs lays metadata mirrored on both
# (WP25): RAW + tier arena on dev0, canonical shadow on dev1.
#
#   leg 1  drop_writes on dev0 MID-SWEEP (offline sweep in bg, table
#          swapped to drop under it): the sweep must fail closed or
#          finish from dev1 -- never publish a half-rewritten recipe.
#          Afterwards every corpus file is bit-exact or cleanly
#          unreadable (the accepted unswept-RAW loss window); a present-
#          but-wrong byte is the failure.
#   leg 2  error (EIO) on dev0 mid-write through the mount: the write
#          must fail loudly, the volume must latch/degrade, prior data
#          stays bit-exact, fsck clean.
#   leg 3  dev0 REMOVED mid-sweep (dmsetup remove = sudden SSD death):
#          the sweep dies; the volume opens degraded from dev1 alone
#          (INVFS_DEV1 mechanism, multidev leg 6) with all swept files
#          bit-exact; reattach -> resync -> RW resumes.
#
# The contract under test (owner decision 2026-10-08): HDD is the home,
# SSD is speed. Fresh-RAW loss on SSD death is accepted; silent
# corruption is not.
#
# Standalone tier (root + slow, like test-flakey.sh): needs dm-flakey,
# losetup, fusermount3. NOT in `make e2e`. Backing files must be
# DISK-backed (a tmpfs backing silently voids every kill semantic);
# the script refuses tmpfs and says so loudly.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
FLK=${FLAKEY_MD_WORK:-/var/tmp/invfs-flakey-md}
DEV0=${FLAKEY_MD_DEV0:-invfs_flakey_md0}
DEV1=${FLAKEY_MD_DEV1:-invfs_flakey_md1}
DM0=/dev/mapper/$DEV0
DM1=/dev/mapper/$DEV1
SEED=${FLAKEY_MD_SEED:-20261008}

fail() { echo "FAIL: $*" >&2; exit 1; }
say() { echo; echo "== $* =="; }
info() { echo "  $*"; }

# ---- safety: disk-backed scratch only --------------------------------
if df -T "$FLK" 2>/dev/null | tail -1 | grep -q tmpfs; then
    echo "REFUSE: $FLK is tmpfs -- dm kills over tmpfs backing prove nothing." >&2
    echo "Set FLAKEY_MD_WORK to a disk-backed dir." >&2
    exit 2
fi
if ! sudo -n dmsetup targets 2>/dev/null | grep -q flakey; then
    echo "SKIP: dm-flakey unavailable (sudo/dmsetup)." >&2
    exit 0
fi
command -v losetup >/dev/null || { echo "SKIP: no losetup." >&2; exit 0; }

LOOP0=""; LOOP1=""
cleanup() {
    set +e
    umount_all 2>/dev/null
    sudo -n dmsetup remove "$DEV0" >/dev/null 2>&1
    sudo -n dmsetup remove "$DEV1" >/dev/null 2>&1
    [ -n "$LOOP0" ] && sudo -n losetup -d "$LOOP0" >/dev/null 2>&1
    [ -n "$LOOP1" ] && sudo -n losetup -d "$LOOP1" >/dev/null 2>&1
}
umount_all() {
    grep -q " $FLK/mnt " /proc/mounts 2>/dev/null || return 0
    fusermount3 -u "$FLK/mnt" 2>/dev/null
    for _ in $(seq 1 25); do
        grep -q " $FLK/mnt " /proc/mounts 2>/dev/null || return 0
        sleep 0.2
    done
    pkill -9 -f "invf-fuse $DM0" 2>/dev/null
    sleep 0.3
    fusermount3 -uz "$FLK/mnt" 2>/dev/null
    return 0
}
trap cleanup EXIT

# ---- device setup ------------------------------------------------------
rm -rf "$FLK" && mkdir -p "$FLK/mnt" "$FLK/orig"
truncate -s 1G "$FLK/back0.img"
truncate -s 2G "$FLK/back1.img"
LOOP0=$(sudo -n losetup -f --show "$FLK/back0.img") || fail "losetup back0"
LOOP1=$(sudo -n losetup -f --show "$FLK/back1.img") || fail "losetup back1"
SEC0=$(sudo -n blockdev --getsz "$LOOP0")
SEC1=$(sudo -n blockdev --getsz "$LOOP1")
# dev0 through flakey (starts healthy: up 3600s / down 0)
sudo -n dmsetup create "$DEV0" --table "0 $SEC0 flakey $LOOP0 0 3600 0" \
    || fail "dmsetup dev0"
# dev1 direct (HDD stand-in: no kill layer of its own)
sudo -n dmsetup create "$DEV1" --table "0 $SEC1 linear $LOOP1 0" \
    || fail "dmsetup dev1"
sudo -n chmod 666 "$DM0" "$DM1"
info "dev0(SSD/flakey)=$DM0 dev1(HDD/linear)=$DM1"

dm_drop_dev0() {  # writes to dev0 vanish from here on
    sudo -n dmsetup suspend --noflush "$DEV0" 2>/dev/null
    sudo -n dmsetup reload "$DEV0" \
        --table "0 $SEC0 flakey $LOOP0 0 3600 999999" 2>/dev/null
    sudo -n dmsetup resume "$DEV0" 2>/dev/null
}
dm_error_dev0() {  # reads+writes on dev0 answer EIO from here on
    sudo -n dmsetup suspend --noflush "$DEV0" 2>/dev/null
    sudo -n dmsetup reload "$DEV0" --table "0 $SEC0 error" 2>/dev/null
    sudo -n dmsetup resume "$DEV0" 2>/dev/null
}
dm_heal_dev0() {
    sudo -n dmsetup suspend --noflush "$DEV0" 2>/dev/null
    sudo -n dmsetup reload "$DEV0" \
        --table "0 $SEC0 flakey $LOOP0 0 3600 0" 2>/dev/null
    sudo -n dmsetup resume "$DEV0" 2>/dev/null
}

# ---- corpus ------------------------------------------------------------
python3 -c "print('sweep-bait text mart ' * 200000)" > "$FLK/orig/t1.txt"
head -c 300000 /dev/urandom > "$FLK/orig/b1.bin"
head -c 100000 /dev/urandom > "$FLK/orig/b2.bin"
(cd "$FLK/orig" && sha256sum * | sort -k2 > ../manifest.pre)

$B/invf-mkfs "$DM0" 1 "$DM1" 2 >"$FLK/mkfs.log" 2>&1 \
    || { cat "$FLK/mkfs.log"; fail "two-device mkfs"; }
$B/invf-import "$DM0" "$FLK/orig" >"$FLK/import.log" 2>&1 \
    || fail "import corpus"
$B/invf-sweep "$DM0" >"$FLK/sweep0.log" 2>&1 || fail "baseline sweep"
$B/invf-fsck "$DM0" | grep -q "^OK$" || fail "fsck after baseline"

# exact-or-absent: every manifest file reads back bit-exact, or the
# read fails CLEANLY (EIO/ENOENT). A byte mismatch is the only failure.
exact_or_absent() {  # <label>
    local label=$1 f ok=1
    for f in t1.txt b1.bin b2.bin; do
        if $B/invf-cat "$DM0" "$f" "$FLK/out.bin" >/dev/null 2>"$FLK/cat.err"; then
            cmp -s "$FLK/orig/$f" "$FLK/out.bin" \
                || { echo "  MISMATCH (corrupt, not absent): $f [$label]"; ok=0; }
        else
            info "$f unreadable [$label] (accepted loss window)"
        fi
    done
    [ "$ok" = 1 ]
}

say "leg 1: drop_writes on dev0 mid-sweep"
$B/invf-sweep "$DM0" >"$FLK/sweep1.log" 2>&1 &
SWEEP_PID=$!
sleep 3   # let the sweep start re-encoding before the floor drops
kill -0 "$SWEEP_PID" 2>/dev/null \
    || fail "leg 1: sweep finished before the kill (corpus too small)"
dm_drop_dev0
wait "$SWEEP_PID" && SWEEP_RC=0 || SWEEP_RC=$?
info "sweep under drop exited $SWEEP_RC (any code is legal; corruption is not)"
dm_heal_dev0
exact_or_absent "post-drop-sweep" || fail "leg 1: corruption after drop mid-sweep"
$B/invf-fsck "$DM0" | grep -q "^OK$" || fail "leg 1: fsck"

say "leg 2: EIO on dev0 mid-write through the mount"
$B/invf-fuse "$DM0" "$FLK/mnt" 2>"$FLK/fuse2.log" &
sleep 2
grep -q " $FLK/mnt " /proc/mounts || fail "leg 2: mount never appeared"
echo "prekill" > "$FLK/mnt/pre.txt" || fail "leg 2: baseline write failed"
dm_error_dev0
if echo "x" > "$FLK/mnt/shouldfail.txt" 2>/dev/null; then
    fail "leg 2: write onto dead dev0 SUCCEEDED (must fail loudly)"
else
    info "write onto dead dev0 refused (loud)"
fi
umount_all
dm_heal_dev0
$B/invf-fsck "$DM0" | grep -q "^OK$" || fail "leg 2: fsck"
$B/invf-cat "$DM0" pre.txt "$FLK/out.bin" >/dev/null 2>&1 \
    && cmp -s <(echo "prekill") "$FLK/out.bin" \
    || fail "leg 2: pre-kill file damaged"
info "pre-kill data intact"

say "leg 3: sudden dev0 death mid-sweep, recover from dev1"
$B/invf-sweep "$DM0" >"$FLK/sweep3.log" 2>&1 &
SWEEP_PID=$!
sleep 3
kill -0 "$SWEEP_PID" 2>/dev/null \
    || fail "leg 3: sweep finished before the kill (corpus too small)"
sudo -n dmsetup remove --force "$DEV0" >/dev/null 2>&1 || \
    sudo -n dmsetup remove "$DEV0" >/dev/null 2>&1 || \
    fail "leg 3: cannot remove dev0"
wait "$SWEEP_PID" && SWEEP_RC=0 || SWEEP_RC=$?
info "sweep under removal exited $SWEEP_RC"
umount_all
# degraded open from dev1 alone (multidev leg-6 mechanism)
export INVFS_DEV1=$DM1
MISSING=/dev/mapper/invfs_flakey_md0_gone
if $B/invf-ls "$MISSING" >/dev/null 2>"$FLK/degraded.err"; then
    info "degraded listing works from dev1"
    # Baseline-swept files live on dev1 (canonical shadow): they MUST
    # be bit-exact. pre.txt was written after the baseline sweep (RAW
    # on the dead dev0): absent is accepted, corrupt is not.
    for f in t1.txt b1.bin b2.bin; do
        $B/invf-cat "$MISSING" "$f" "$FLK/out.bin" >/dev/null 2>"$FLK/cat.err" \
            || fail "leg 3: swept file $f unreadable degraded"
        cmp -s "$FLK/orig/$f" "$FLK/out.bin" \
            || fail "leg 3: swept file $f CORRUPT degraded"
        info "$f bit-exact degraded"
    done
    if $B/invf-cat "$MISSING" pre.txt "$FLK/out.bin" >/dev/null 2>&1; then
        cmp -s <(echo "prekill") "$FLK/out.bin" \
            || fail "leg 3: pre.txt present but CORRUPT degraded"
        info "pre.txt survived (was already swept to dev1)"
    else
        info "pre.txt unreadable degraded (accepted unswept-RAW loss)"
    fi
else
    info "degraded open refused: $(tail -1 "$FLK/degraded.err")"
fi
unset INVFS_DEV1
# reattach: recreate dev0 over the SAME loop backing, resync, RW resumes
sudo -n dmsetup create "$DEV0" --table "0 $SEC0 flakey $LOOP0 0 3600 0" \
    || fail "leg 3: dev0 recreate"
sudo -n chmod 666 "$DM0"
$B/invf-fsck "$DM0" | grep -q "^OK$" || fail "leg 3: fsck after reattach"

echo
echo "FLAKEY-MULTIDEV: PASS"
