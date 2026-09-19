#!/bin/bash
# mkdisk-arch.sh — assemble a bootable GPT disk image for Arch Linux on
# InvariantFS.  The bootloader (GRUB/EFI) reads kernel + initramfs from an
# ESP/FAT32 partition; the InvFS root is a separate partition reached by
# the initramfs hook (WP67).
#
# Usage:
#   tools/mkdisk-arch.sh --volume VOL [options]
#
# Options:
#   --volume   PATH   InvariantFS volume image (required)
#   --kernel   PATH   vmlinuz to copy to ESP   (default: /boot/vmlinuz-linux)
#   --initrd   PATH   initramfs to copy to ESP (default: /boot/initramfs-linux.img)
#   --esp-size MB     ESP size                 (default: 512)
#   --output   PATH   output disk image         (default: vm/disk-arch.img)
#   --grub-cfg PATH   custom grub.cfg          (auto-generated if omitted)
#   --grub-mkimage PATH  grub-mkimage binary   (default: grub-mkimage)
#   --dry-run         print plan, do not write
#
# Disk layout (GPT):
#   p1  ESP          FAT32   $ESP_SIZE MiB   (GRUB EFI + kernel + initramfs)
#   p2  InvFS root   raw     remainder       (invf-mkfs'd volume)
#
# The script does NOT run grub-install (it fails on FUSE mounts). Instead
# it calls grub-mkimage to produce a standalone EFI binary and places it
# at EFI/BOOT/BOOTX64.EFI on the ESP with a hand-crafted grub.cfg.
set -euo pipefail
cd "$(dirname "$0")/.."

# -- defaults ----------------------------------------------------------------
VOL=""
KERNEL="/boot/vmlinuz-linux"
INITRD="/boot/initramfs-linux.img"
ESP_MB=512
DISK="vm/disk-arch.img"
GRUB_CFG=""
GRUB_MKIMAGE="grub-mkimage"
DRY_RUN=0

# -- parse args --------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --volume)       VOL="$2";            shift 2;;
        --kernel)       KERNEL="$2";         shift 2;;
        --initrd)       INITRD="$2";         shift 2;;
        --esp-size)     ESP_MB="$2";         shift 2;;
        --output)       DISK="$2";           shift 2;;
        --grub-cfg)     GRUB_CFG="$2";       shift 2;;
        --grub-mkimage) GRUB_MKIMAGE="$2";   shift 2;;
        --dry-run)      DRY_RUN=1;           shift;;
        -h|--help)
            sed -n '2,/^set -euo/p' "$0" | head -n -1 | sed 's/^# \?//'
            exit 0;;
        *) echo "unknown option: $1" >&2; exit 2;;
    esac
done

[ -n "$VOL" ] || { echo "FAIL: --volume is required" >&2; exit 1; }
[ -f "$VOL" ] || { echo "FAIL: volume not found: $VOL" >&2; exit 1; }

# -- locate GRUB modules directory ------------------------------------------
GRUB_MODDIR=""
if command -v "$GRUB_MKIMAGE" >/dev/null 2>&1; then
    _bindir=$(dirname "$(command -v "$GRUB_MKIMAGE")")
    for d in "$_bindir"/../lib/grub/x86_64-efi \
             "$_bindir"/../share/grub/x86_64-efi \
             /usr/lib/grub/x86_64-efi \
             /usr/share/grub/x86_64-efi; do
        if [ -d "$d" ]; then GRUB_MODDIR="$d"; break; fi
    done
fi

# -- size calculation --------------------------------------------------------
VOL_SZ=$(stat -Lc%s "$VOL")
DISK_MB=$(( (VOL_SZ + 1048575) / 1048576 + ESP_MB + 2 ))
INVFS_OFF_MB=$(( ESP_MB + 1 ))

echo "=== mkdisk-arch ==="
echo "  volume:    $VOL ($(( VOL_SZ / 1048576 )) MiB)"
echo "  kernel:    $KERNEL"
echo "  initrd:    $INITRD"
echo "  ESP:       ${ESP_MB} MiB"
echo "  disk:      $DISK (${DISK_MB} MiB)"

if [ "$DRY_RUN" = 1 ]; then
    echo "(dry-run -- no changes)"
    exit 0
fi

# -- create disk image -------------------------------------------------------
mkdir -p "$(dirname "$DISK")"
rm -f "$DISK"
truncate -s "${DISK_MB}M" "$DISK"

# -- partition ---------------------------------------------------------------
parted -s "$DISK" mklabel gpt \
    mkpart ESP fat32 1MiB "${ESP_MB}MiB" \
    set 1 esp on \
    mkpart root "${INVFS_OFF_MB}MiB" 100%

echo ">> copying volume into partition 2 (sparse)"
dd if="$VOL" of="$DISK" bs=1M seek=$INVFS_OFF_MB conv=sparse,notrunc status=none

# -- loop + format -----------------------------------------------------------
LOOP=$(sudo losetup -fP --show "$DISK")
trap 'sudo losetup -d "$LOOP" 2>/dev/null || true' EXIT

echo ">> formatting ESP (${LOOP}p1)"
sudo mkfs.vfat -F32 -n INVFS_ESP "${LOOP}p1" >/dev/null

# -- mount ESP ---------------------------------------------------------------
MNT=/tmp/invfs-mkdisk-esp
sudo mkdir -p "$MNT"
sudo mount "${LOOP}p1" "$MNT"
trap 'sudo umount "$MNT" 2>/dev/null; sudo losetup -d "$LOOP" 2>/dev/null || true' EXIT

# -- copy kernel + initramfs to ESP /boot ------------------------------------
# These must live on a FAT32 filesystem because the bootloader cannot read
# InvFS.  The initramfs hook (WP67) copies them into the running root.
echo ">> copying kernel + initramfs to ESP"
sudo mkdir -p "$MNT/boot"
if [ -r "$KERNEL" ]; then
    sudo cp "$KERNEL" "$MNT/boot/vmlinuz-linux"
else
    echo "WARN: kernel not found at $KERNEL -- skipping" >&2
fi
if [ -r "$INITRD" ]; then
    sudo cp "$INITRD" "$MNT/boot/initramfs-linux.img"
else
    echo "WARN: initramfs not found at $INITRD -- skipping" >&2
fi

# -- grub.cfg ----------------------------------------------------------------
# Auto-generate unless the caller supplied one.
if [ -n "$GRUB_CFG" ] && [ -r "$GRUB_CFG" ]; then
    echo ">> using custom grub.cfg: $GRUB_CFG"
    sudo cp "$GRUB_CFG" "$MNT/boot/grub/grub.cfg"
else
    echo ">> generating grub.cfg"
    # The root= parameter points at the raw partition.  The initramfs hook
    # (WP67) uses this to locate and mount the InvFS volume.
    sudo mkdir -p "$MNT/boot/grub"
    sudo tee "$MNT/boot/grub/grub.cfg" >/dev/null <<'GRUBCFG'
set default=0
set timeout=3
set gfxpayload=keep

menuentry "Arch Linux (InvariantFS)" {
    insmod part_gpt
    insmod fat
    search --no-floppy --fs-uuid --set=root ${ESP_UUID}
    linux /boot/vmlinuz-linux console=ttyS0,115200 rootfstype=invfs root=/dev/sda2 rw
    initrd /boot/initramfs-linux.img
}

menuentry "Arch Linux (InvariantFS, fallback)" {
    insmod part_gpt
    insmod fat
    search --no-floppy --fs-uuid --set=root ${ESP_UUID}
    linux /boot/vmlinuz-linux console=ttyS0,115200 rootfstype=invfs root=/dev/sda2 rw init=/bin/invfs-init
    initrd /boot/initramfs-linux.img
}
GRUBCFG
fi

# Also place grub.cfg in the EFI fallback location so UEFI firmware finds it.
sudo mkdir -p "$MNT/EFI/BOOT"
sudo cp "$MNT/boot/grub/grub.cfg" "$MNT/EFI/BOOT/grub.cfg"

# -- grub-mkimage (standalone EFI binary) -----------------------------------
# grub-install fails on FUSE; we build a standalone EFI binary that embeds
# its prefix (/boot/grub) so it finds grub.cfg on the ESP FAT32 partition.
if command -v "$GRUB_MKIMAGE" >/dev/null 2>&1; then
    echo ">> building GRUB EFI binary via grub-mkimage"
    sudo mkdir -p "$MNT/EFI/BOOT"

    # Core modules: filesystem + partition drivers GRUB needs to read the ESP.
    GRUB_MODULES="fat part_gpt part_msdos normal linux configfile search search_label search_fs_uuid reboot echo test all_video loadenv"

    if [ -n "$GRUB_MODDIR" ]; then
        sudo "$GRUB_MKIMAGE" -O x86_64-efi \
            -o "$MNT/EFI/BOOT/BOOTX64.EFI" \
            -p /boot/grub \
            -d "$GRUB_MODDIR" \
            $GRUB_MODULES
    else
        sudo "$GRUB_MKIMAGE" -O x86_64-efi \
            -o "$MNT/EFI/BOOT/BOOTX64.EFI" \
            -p /boot/grub \
            $GRUB_MODULES
    fi
    echo ">> EFI binary: $MNT/EFI/BOOT/BOOTX64.EFI"
else
    echo "WARN: grub-mkimage not found -- ESP has kernel/initrd + grub.cfg" >&2
    echo "      but no EFI binary.  Install grub to build the bootloader." >&2
fi

# -- cleanup -----------------------------------------------------------------
echo ">> unmounting ESP"
sync
sudo umount "$MNT" 2>/dev/null || true
sudo losetup -d "$LOOP" 2>/dev/null || true
trap - EXIT

echo ">> done: $DISK"
echo "   Layout: p1=ESP(FAT32) p2=InvFS raw @ ${INVFS_OFF_MB}MiB"
