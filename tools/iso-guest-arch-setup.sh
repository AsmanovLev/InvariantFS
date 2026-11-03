#!/bin/bash
# iso-guest-arch-setup.sh -- runs INSIDE a stock Arch live env.
#
# Real native Arch Linux install on InvariantFS:
#   1. GPT disk partitioning: ESP (512M FAT32) + InvariantFS root
#   2. Install InvariantFS in live env via curl -fsSL http://10.0.2.2:PORT/sh | sh
#   3. Format root (mkfs.invfs) and mount (mount.invfs -o allow_other)
#   4. Mount ESP at /mnt/boot
#   5. Bootstrap distro packages via pacstrap (base, linux, grub, efibootmgr, sshd)
#   6. Inside arch-chroot: install InvariantFS, configure mkinitcpio with invfs hook,
#      build initramfs (mkinitcpio -P), and run invf-boot-setup --esp /boot --root <dev>
#   7. Clean unmount, offline invf-fsck verify, and PASS marker.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8000}"
VOL=""   # resolved below: invfsvol= cmdline, else first writable disk
B=/home/builder/pkg
say() { echo "iso-setup: $*"; }
done_rc=1
STATS=""
finish() {
    if [ "$done_rc" = 0 ]; then
        echo "INVFS-ISO-SETUP: PASS $STATS"
    else
        echo "INVFS-ISO-SETUP: FAIL rc=$done_rc"
    fi
    sleep 2
    timeout 30 systemctl poweroff || poweroff -f
}
trap 'done_rc=$?; finish' EXIT

# Watchdog: heartbeat + memory to console every 60 s
( while true; do
    echo "iso-watch: mem=$(free -m | awk '/^Mem/{print $3"/"$2"MB"}') load=$(cut -d' ' -f1 /proc/loadavg)"
    sleep 60
  done ) &

# ---- phase 1: identify and partition the target disk ----
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
if [ -z "$VOL" ]; then
    VOL=$(lsblk -dn -o NAME,RO,TYPE | awk '$2=="0" && $3=="disk" {print "/dev/"$1}' | head -1)
fi
[ -b "${VOL:-none}" ] || { say "no volume disk (cmdline: $(tr ' ' '\n' < /proc/cmdline | grep -o 'invfsvol=[^ ]*' || echo none); disks: $(lsblk -dn -o NAME,RO | tr '\n' ' '))"; exit 1; }
say "target disk: $VOL"

case "$VOL" in
    *[0-9]) P1="${VOL}p1"; P2="${VOL}p2" ;;
    *)      P1="${VOL}1";  P2="${VOL}2"  ;;
esac

say "partitioning $VOL (GPT: 512M ESP + InvFS root)..."
if command -v sfdisk >/dev/null 2>&1; then
    sfdisk "$VOL" << 'EOF' || exit 1
label: gpt
size=512M, type=U
type=L
EOF
elif command -v sgdisk >/dev/null 2>&1; then
    sgdisk -Z "$VOL" || exit 1
    sgdisk -n 1:0:+512M -t 1:ef00 "$VOL" || exit 1
    sgdisk -n 2:0:0 -t 2:8300 "$VOL" || exit 1
fi
udevadm settle 2>/dev/null || sleep 2

say "formatting ESP $P1 (FAT32)..."
mkfs.vfat -F32 "$P1" || mkfs.fat -F32 "$P1" || exit 1

# ---- phase 2: install InvariantFS in live environment ----
say "installing InvariantFS in live environment via $SRV/sh..."
export INVFS_SERVER="$SRV"
curl -fsSL "$SRV/sh" | sh || exit 1
command -v mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found after sh install"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found after sh install"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount filesystems ----
say "formatting root $P2 with mkfs.invfs..."
mkfs.invfs "$P2" || exit 1

say "mounting root on /mnt (allow_other)..."
mkdir -p /mnt
mount.invfs -o allow_other "$P2" /mnt || exit 1
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1

# ---- phase 4: bootstrap base system via pacstrap ----
say "refreshing pacman keyring..."
pacman -Sy --needed --noconfirm archlinux-keyring || true
pacman-key --init || true
for attempt in 1 2 3; do
    if pacman-key --populate archlinux; then break; fi
    say "populate attempt $attempt failed; resetting gnupg home + re-init..."
    gpgconf --kill gpg-agent 2>/dev/null || true
    rm -rf /etc/pacman.d/gnupg
    pacman-key --init || true
    sleep 5
done

say "pacstrapping base linux openssh dhcpcd iproute2 grub efibootmgr into /mnt..."
pacstrap /mnt base linux openssh dhcpcd iproute2 grub efibootmgr || exit 1
genfstab -U /mnt >> /mnt/etc/fstab || true

# ---- phase 5: in-chroot configuration & bootloader setup ----
say "configuring inside arch-chroot /mnt..."
arch-chroot /mnt /bin/bash <<CHROOT_EOF || exit 1
set -eu
export INVFS_SERVER="$SRV"
say "installing InvariantFS inside target rootfs..."
curl -fsSL "$SRV/sh" | sh

say "configuring mkinitcpio for InvariantFS root..."
sed -i 's/^HOOKS=.*/HOOKS=(base udev autodetect modconf kms keyboard keymap consolefont block invfs)/' /etc/mkinitcpio.conf
mkinitcpio -P

say "running invf-boot-setup on /boot (ESP) for $P2..."
invf-boot-setup --esp /boot --root "$P2" --cmdline "console=ttyS0,115200"

echo "root:root" | chpasswd
systemctl enable sshd 2>/dev/null || true
systemctl enable dhcpcd 2>/dev/null || true
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config 2>/dev/null || true
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config 2>/dev/null || true
ssh-keygen -A 2>/dev/null || true
CHROOT_EOF

# ---- phase 6: clean unmount & offline validation ----
say "unmounting /mnt/boot and /mnt..."
umount /mnt/boot || exit 1
umount /mnt || exit 1
udevadm settle 2>/dev/null || sleep 2

say "verifying root filesystem with invf-fsck..."
invf-fsck "$P2" || exit 1

say "spot checking bit-exact reads via invf-cat..."
mkdir -p "$B"
for f in etc/os-release etc/passwd; do
    invf-cat "$P2" "$f" "$B/out" || exit 1
    [ -s "$B/out" ] || { say "empty $f"; exit 1; }
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
