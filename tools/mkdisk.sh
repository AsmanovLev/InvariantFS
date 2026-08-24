#!/bin/bash
# Assemble the single-drive M3 boot disk:
#   GPT: p1 ESP (GRUB EFI --removable + kernel + initramfs)
#        p2 InvariantFS root volume (byte-copy of var/tmp/vol-root.img (or pass path as $1))
# Usage: tools/mkdisk.sh [volume-img] [disk-img]
set -euo pipefail
cd "$(dirname "$0")/.."

VOL=${1:-vm/vol-gentoo.img}
DISK=${2:-vm/disk.img}
ESP_OFF_MB=257          # 1 MiB align + 256 MiB ESP
KERNEL=/boot/vmlinuz-6.19.10-300.fc44.x86_64
INITRD=vm/initramfs.cpio.gz
SIZE_MB=$(( $(stat -Lc%s "$VOL") / 1048576 + ESP_OFF_MB + 8 ))

[ -f "$VOL" ] || { echo "no volume $VOL" >&2; exit 1; }
[ -f "$INITRD" ] || tools/mkinitramfs.sh >/dev/null

echo ">> $DISK: ${SIZE_MB}MiB (GPT ESP ${ESP_OFF_MB}MiB + InvariantFS)"
rm -f "$DISK"
truncate -s "${SIZE_MB}M" "$DISK"

parted -s "$DISK" mklabel gpt \
    mkpart ESP fat32 1MiB "${ESP_OFF_MB}MiB" \
    set 1 esp on \
    mkpart root 257MiB 100%

echo ">> copying volume into partition 2 (sparse)"
dd if="$VOL" of="$DISK" bs=1M seek=$ESP_OFF_MB conv=sparse,notrunc status=none

LOOP=$(sudo losetup -fP --show "$DISK")
trap "sudo losetup -d $LOOP" EXIT

echo ">> formatting ESP ($LOOP-p1)"
sudo mkfs.vfat -F32 -n INVFS_ESP "${LOOP}p1" >/dev/null
sudo mkdir -p /mnt/invfs-esp
sudo mount "${LOOP}p1" /mnt/invfs-esp
trap "sudo umount /mnt/invfs-esp; sudo losetup -d $LOOP" EXIT

echo ">> grub2-install --removable (x86_64-efi)"
sudo grub2-install --force --removable --target=x86_64-efi \
    --efi-directory=/mnt/invfs-esp \
    --boot-directory=/mnt/invfs-esp/boot \
    --no-nvram >/dev/null

sudo mkdir -p /mnt/invfs-esp/boot
sudo cp "$KERNEL" /mnt/invfs-esp/boot/vmlinuz
sudo cp "$INITRD" /mnt/invfs-esp/boot/initramfs.cpio.gz
cat <<'EOF' > /tmp/opencode/invfs-grub.cfg
set timeout=0
set default=0
menuentry "InvariantFS Gentoo" {
    linux /boot/vmlinuz console=ttyS0 root=/dev/vda2 rw
    initrd /boot/initramfs.cpio.gz
}
EOF
# Fedora's grub2-install embeds prefix .../boot/grub2; stock GRUB uses
# boot/grub; the removable BOOTX64.EFI also probes its own directory.
for d in /mnt/invfs-esp/boot/grub /mnt/invfs-esp/boot/grub2 \
         /mnt/invfs-esp/EFI/BOOT; do
    sudo mkdir -p "$d"
    sudo cp /tmp/opencode/invfs-grub.cfg "$d/grub.cfg"
done
sync
echo ">> done: $DISK"
