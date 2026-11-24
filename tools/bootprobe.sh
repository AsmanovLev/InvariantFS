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
# Usage:
#   . "$(dirname "$0")/bootprobe.sh"
#   bootprobe_send serial_send
#   serial_send "echo 'INVFS_TRIGGER_ROOT_CHECK_DONE'"
#   serial_wait_match ... || fail "..."
#   bootprobe_assert "$SER"
#     0 = root is invfs/fuse   1 = the guest said the root did not come up
#     2 = no recognisable mount line (BOOTPROBE_SEEN holds what was printed)

bootprobe_send() {
    "$1" "awk '\$2==\"/\"{printf \"INVFSROOT%s\\n\", \$3}' /proc/mounts"
}

bootprobe_assert() {
    BOOTPROBE_SEEN=$(grep -E '^INVFSROOT' "$1" 2>/dev/null | tail -1)
    if grep -qE 'no InvariantFS volume found|InvariantFS root did not come up' "$1" 2>/dev/null; then
        return 1
    fi
    case "$BOOTPROBE_SEEN" in
        INVFSROOTinvfs*|INVFSROOTfuse*) return 0 ;;
    esac
    return 2
}
