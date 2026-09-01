#!/bin/busybox sh
# sweepboot-init.sh — InvariantFS maintenance boot (WP23).
#
# SOURCED by the initramfs /init (vm/initramfs/init) when the kernel
# cmdline carries `invfs.sweepboot`:
#
#     linux /boot/vmlinuz console=ttyS0 root=/dev/vda2 rw invfs.sweepboot
#
# The branch runs the sweep EXCLUSIVE/ENGINE-SIDE — the rootfs volume is
# NEVER FUSE-mounted here, so no daemon, no lookup storm, no second
# writer — and then reboots back into the normal boot. This is the
# grub-reboot oneshot pattern: a oneshot boot entry (or
# `grub-reboot sweepboot-entry` + reboot) schedules the maintenance boot,
# the pass runs with the volume quiescent, and `reboot -f` returns to the
# default entry. See doc/13-linux-rootfs.md ("Sweepboot").
#
#   $1 = the root device (e.g. /dev/vda2), already resolved by /init.
#
# Ordering inside the pass:
#   1. mount a tmpfs scratch (the extracted packs live only in RAM)
#   2. extract the codecpacks the volume hosts FOR ITSELF (engine-side:
#      invf-sweep --extract-packs reads them through the core codecs —
#      ELF tools come out of ZSTD batches, scripts out of PPMd batches,
#      always in-process — and materializes them into the scratch)
#   3. invf-sweep --seal  (the maintenance pass: sweep walk + parity)
#   4. reboot -f
#
# Failure contract: ANY failure falls through to the normal boot (the
# /init caller continues past this script) — a maintenance boot must
# never become a boot loop. The console message is loud on purpose.

SWEEPBIN=/usr/local/bin/invf-sweep
SCRATCH=/mnt/sweepboot

echo "[sweepboot] maintenance boot on $1 (no FUSE, no switch_root)"

mkdir -p "$SCRATCH"
if ! mount -t tmpfs -o size=64m tmpfs "$SCRATCH"; then
    echo "[sweepboot] !! tmpfs scratch failed — falling through to NORMAL boot"
    return 1
fi

# 1-2. the self-hosting step: packs live ON the volume at
# /.invfs/codecpacks (or /usr/lib/invfs/codecpacks) and come out through
# the engine alone. Absent packs are not an error: the builtin codecs
# (NONE/LZ4/ZSTD/PPMd) still run the pass.
if PACKS=$($SWEEPBIN "$1" --extract-packs "$SCRATCH/packs") && [ -d "$PACKS" ]; then
    if ls "$PACKS"/*.codecpack/ >/dev/null 2>&1; then
        echo "[sweepboot] volume-hosted codecpacks extracted: $PACKS"
        export INVFS_CODECPACKS="$PACKS"
    else
        echo "[sweepboot] volume hosts no codecpacks; builtin codecs only"
    fi
else
    echo "[sweepboot] !! pack extraction failed — sweeping with builtin codecs"
fi

# 3. the maintenance pass itself
if ! $SWEEPBIN "$1" --seal; then
    echo "[sweepboot] !! SWEEP FAILED on $1 — falling through to NORMAL boot"
    umount "$SCRATCH" 2>/dev/null
    return 1
fi

# 4. back to the normal boot entry
echo "[sweepboot] sweep+seal done; rebooting into the normal entry"
sync
reboot -f
# reboot -f must not return; if it ever does, a normal boot is still the
# safe answer (the sweep committed, the volume is consistent)
echo "[sweepboot] !! reboot -f returned?! — falling through to NORMAL boot"
umount "$SCRATCH" 2>/dev/null
return 0
