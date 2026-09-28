#!/bin/sh
# Shared hardened loop mounting for e2e suites. Source it, do not run it.
#
# Why this exists: `mount -o loop` picks a loop device by itself and accepts a
# wedged one without complaint. On this host /dev/loop4 sits at ro=1 and
# `losetup -d` cannot clear it, so a suite can end up with a read-only mount --
# which is indistinguishable from a dead device. The symptom is a flood of
# "cp: cannot create regular file ...: Read-only file system" during fixture
# population, which blames the filesystem under test for a broken loop device.
#
# That is not hypothetical: it is what took out test-fatfs.sh during the full
# e2e run, and it fired in test-xfs.sh too. Both guards below (device health,
# mount writability) caught a real condition.
#
# Usage:
#   . tools/lib-loopmount.sh
#   invfs_loop_mount <img> <mountpoint> [extra mount opts]  # aborts on failure
#   ... populate ...
#   invfs_loop_umount <mountpoint>
#
# Design notes:
# - Devices are WALKED, not taken from `losetup --find`. On this host --find
#   kept handing back the wedged loop4, so one broken device would have stopped
#   every suite on the machine from measuring anything.
# - The writability probe runs at the caller's privilege (su), because these
#   suites mount with sudo and then chown. Probing as the invoking user fails
#   on a perfectly good mount that has not been chowned yet, and a test that
#   cries wolf is worse than no test.
# - No /srv/bench dependency: this has to work in a fresh worktree.

LOOPDEV=""

# Pick a free, healthy loop device and attach <img> to it.
_invfs_attach() {
    _img="$1"
    _c=""
    for _dev in /dev/loop[0-9]*; do
        [ -e "$_dev" ] || continue
        # ro=1 means wedged. Skip it rather than discover it through a mount.
        [ "$(cat "/sys/block/$(basename "$_dev")/ro" 2>/dev/null)" = "0" ] || continue
        # Already attached? losetup prints info and exits 0 for a bound device.
        if losetup "$_dev" >/dev/null 2>&1; then continue; fi
        if timeout 30 sudo -n losetup "$_dev" "$_img" 2>/dev/null; then _c="$_dev"; break; fi
    done
    [ -n "$_c" ] || return 1
    LOOPDEV="$_c"
    return 0
}

# Attach and mount <img> at <mnt>; aborts the calling suite on any problem.
invfs_loop_mount() {
    # Options are passed as separate words and forwarded with "$@" after a
    # shift. An earlier version passed them as one string and expanded it
    # unquoted, which split `uid=1000` into its own argv entry and made the
    # kernel report "exfat: Unknown parameter ' uid'" -- an error that points
    # at exfat when the fault is in the caller's word splitting.
    _img="$1"; _mnt="$2"
    [ "$#" -ge 2 ] || { echo "FAIL: invfs_loop_mount needs <img> <mnt>"; exit 1; }
    shift 2
    if ! _invfs_attach "$_img"; then
        echo "FAIL: no free, healthy loop device for $_img (some may be wedged ro=1)"
        exit 1
    fi
    # Extra opts are passed through verbatim, so a suite that needs
    # `uid=$(id -u)` or `-t ntfs-3g` keeps it: the point of this helper is the
    # device health and writability checks, not to take options away.
    if ! sudo -n mount "$@" "$LOOPDEV" "$_mnt"; then
        echo "FAIL: mount $LOOPDEV $_mnt failed"
        sudo -n losetup -d "$LOOPDEV" 2>/dev/null || true
        exit 1
    fi
    if ! sudo -n touch "$_mnt/.invfs-writable" 2>/dev/null; then
        echo "FAIL: $_mnt is not writable -- a read-only mount looks like a dead device,"
        echo "       and populating it would blame the filesystem under test"
        sudo -n umount "$_mnt" 2>/dev/null || true
        sudo -n losetup -d "$LOOPDEV" 2>/dev/null || true
        LOOPDEV=""
        exit 1
    fi
    sudo -n rm -f "$_mnt/.invfs-writable"
}

# Unmount and detach. Safe to call when nothing is mounted.
invfs_loop_umount() {
    _mnt="$1"
    sudo -n umount "$_mnt" 2>/dev/null || true
    [ -n "$LOOPDEV" ] && { sudo -n losetup -d "$LOOPDEV" 2>/dev/null || true; LOOPDEV=""; }
    return 0
}
