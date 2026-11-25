#!/bin/sh
# bootprobe.sh — the "is the root actually InvariantFS/FUSE" probe shared by the
# distro boot harnesses (nixos, gentoo, arch, alpine).
#
# WHY THIS IS A FILE AND NOT A GREP. The old check was:
#
#   serial_send "grep ' / ' /proc/mounts > /tmp/root_mount && cat /tmp/root_mount"
#   if grep -q -E 'invfs|fuse' "$SER"; then  "FUSE root confirmed mounted!"
#
# -- a search of the WHOLE serial log for those two words. Both appear in a guest
# whose root never came up: the kernel prints `fuse: init (API version 7.39)`
# itself, and the rescue banner of our own initramfs says InvariantFS twice.
# Measured on installation-iso-nixos 2026-11-24 (run 45337773685): virtio_blk
# could not load (`virtio_blk: Unknown symbol register_virtio_driver (err -2)`),
# so the guest printed
#     ERROR: no InvariantFS volume found on any block device
#     InvariantFS root did not come up. This initramfs carries the repair tools,
# the mount probe printed `rootfs / rootfs rw 0 0`, and the harness still
# reported "FUSE root confirmed mounted!" and
# "NixOS post-install UEFI boot verification: PASS".
# A check that the guest's own failure text satisfies is not a check.
#
# Now the guest prints INVFSROOT<fstype> ONLY for the /proc/mounts line whose
# mountpoint is "/" (printf's format is split from its argument, so the token
# cannot match the echoed command), and the guest's failure sentences fail the
# probe outright.
#
# ERREXIT: every function here is safe under `set -e`. The first version was not
# -- `BOOTPROBE_SEEN=$(grep ...)` returns grep's status, so a guest that had not
# printed the token yet killed the harness two milliseconds after the command was
# sent (installation-iso-gentoo 2026-11-24, no output, immediate FAIL). Greps that
# may legitimately miss are `|| true`, and callers capture the code as
# `bootprobe_assert "$SER" && rc=0 || rc=$?`.
#
# Usage:
#   . "$(dirname "$0")/bootprobe.sh"
#   bootprobe_send serial_send
#   serial_send "echo 'INVFS_TRIGGER_ROOT_CHECK_DONE'"
#   serial_wait_match ... || fail "..."
#   bootprobe_wait "$SER" 30          # optional: give the line time to arrive
#   bootprobe_assert "$SER" && BOOT_RC=0 || BOOT_RC=$?
#     0 = root is invfs/fuse   1 = the guest said the root did not come up
#     2 = no recognisable mount line (BOOTPROBE_SEEN holds what was printed)

bootprobe_send() {
    "$1" "awk '\$2==\"/\"{printf \"INVFSROOT%s\\n\", \$3}' /proc/mounts"
}

# Send the probe and wait for the token, RE-SENDING while the guest is busy.
#
# The one-shot form failed exactly where it mattered: on nixos the installed
# systemd was still starting when the probe was typed, the shell answered 31 s
# later, the 30 s window had closed, and the verdict was "root is not
# InvariantFS" -- while the very next serial line said INVFSROOTfuse (run
# 45667191942: FAIL at 19:03:09.565, token at .552). A guest that takes 90 s to
# boot is not a guest with the wrong root, so the ask repeats.
bootprobe_ask() {
    _ba_log=$1
    _ba_send=$2
    _ba_tmo=${3:-300}
    _ba_round=0
    while [ $((_ba_round * 15)) -lt "$_ba_tmo" ]; do
        bootprobe_send "$_ba_send"
        _ba_i=0
        while [ "$_ba_i" -lt 15 ]; do
            # A GOOD answer, not just any answer. During the hand-off the console
            # can still be answering from the initramfs, where /proc/mounts says
            # `rootfs` -- and the first such answer used to end the probe with
            # "root is not InvariantFS" while the system was, in fact, three
            # seconds from reaching its own shell (run 45707724778: hand-off at
            # 22:26:20, systemd up, probe answered INVFSROOTrootfs at 22:30:21).
            # Wait for the token that names a FUSE root; keep asking otherwise.
            if grep -qoE 'INVFSROOT(fuse|invfs)' "$_ba_log" 2>/dev/null; then
                bootprobe_assert "$_ba_log"
                return $?
            fi
            sleep 1
            _ba_i=$((_ba_i + 1))
        done
        _ba_round=$((_ba_round + 1))
    done
    return 2
}

# Poll for the token, so a slow guest is not mistaken for a wrong root.
bootprobe_wait() {
    _bp_log=$1
    _bp_tmo=${2:-30}
    _bp_i=0
    while [ "$_bp_i" -lt "$_bp_tmo" ]; do
        if grep -qE 'INVFSROOT[A-Za-z0-9]' "$_bp_log" 2>/dev/null; then
            if grep -qE 'no InvariantFS volume found|InvariantFS root did not come up' "$_bp_log" 2>/dev/null; then
                return 2
            fi
            return 0
        fi
        if grep -qE 'no InvariantFS volume found|InvariantFS root did not come up' "$_bp_log" 2>/dev/null; then
            return 2
        fi
        sleep 1
        _bp_i=$((_bp_i + 1))
    done
    return 1
}

bootprobe_assert() {
    # `grep -o` with the token pattern, not `^INVFSROOT`: on this harness the serial
    # file carries the guest's own line, and a '[guest HH:MM:SS] ' prefix rides
    # along in some paths -- the anchored form then found nothing while five
    # INVFSROOTfuse lines sat in the log (run 45682852389). The pattern also has to
    # be narrower than the bare word, because the ECHOED command contains
    # INVFSROOT%s: requiring an alphanumeric character after the token excludes it
    # without depending on where the line starts.
    BOOTPROBE_SEEN=$(grep -oE 'INVFSROOT[A-Za-z0-9]+' "$1" 2>/dev/null | tail -1) || true
    if grep -qE 'no InvariantFS volume found|InvariantFS root did not come up' "$1" 2>/dev/null; then
        return 1
    fi
    case "$BOOTPROBE_SEEN" in
        INVFSROOTinvfs*|INVFSROOTfuse*) return 0 ;;
    esac
    return 2
}
