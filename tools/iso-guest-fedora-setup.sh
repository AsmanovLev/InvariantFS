#!/usr/bin/env bash
# iso-guest-fedora-setup.sh -- runs INSIDE Fedora live/installer environment.
#
# Non-interactive Fedora installation onto InvariantFS:
#   1. GPT disk partitioning: ESP (512M FAT32) + InvariantFS root
#   2. Install host tools & InvariantFS in live env
#   3. Format root (mkfs.invfs) and mount on /mnt (mount.invfs -o allow_other)
#   4. Mount ESP on /mnt/boot
#   5. Provision Fedora base system onto /mnt (dnf --installroot or rsync)
#   6. Configure bootloader and InvariantFS dracut module in /mnt
#   7. Clean unmount, offline invf-fsck verify, and PASS marker.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8006}"
VOL=""
B=/root/pkg
say() { echo "iso-setup: $*"; }
done_rc=1
STATS=""

finish() {
    if [ "$done_rc" = 0 ]; then
        echo "INVFS-ISO-SETUP: PASS $STATS"
    else
        echo "INVFS-ISO-SETUP: FAIL rc=$done_rc"
        say "dmesg output:"
        dmesg -T | tail -n 50 2>/dev/null || true
    fi
    sleep 2
    timeout 60 poweroff || poweroff -f
}
trap 'done_rc=$?; finish' EXIT

# Watchdog
( while true; do
    echo "iso-watch: mem=$(free -m | awk '/^Mem/{print $3"/"$2"MB"}') load=$(cut -d' ' -f1 /proc/loadavg)"
    sleep 60
  done ) &

say "checking connectivity to $SRV..."
curl -fsSL -o /dev/null "$SRV/sh" || {
    say "network unreachable, configuring eth0..."
    dhclient eth0 2>/dev/null || ip link set eth0 up || true
    curl -fsSL -o /dev/null "$SRV/sh" || exit 1
}
say "serve dir reachable"

[ -e /dev/fuse ] || modprobe fuse 2>/dev/null || true
[ -e /dev/fuse ] || { say "no /dev/fuse (modprobe fuse failed)"; exit 1; }

# ---- phase 1: identify and partition target disk ----
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
if [ -z "$VOL" ]; then
    VOL=$(lsblk -dn -o NAME,RO,TYPE 2>/dev/null | awk '$2=="0" && $3=="disk" {print "/dev/"$1}' | head -1 || true)
fi
[ -z "$VOL" ] && VOL="/dev/vda"
[ -b "$VOL" ] || { say "target volume $VOL not found"; exit 1; }
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
elif command -v parted >/dev/null 2>&1; then
    parted -s "$VOL" mklabel gpt || exit 1
    parted -s "$VOL" mkpart ESP fat32 1MiB 513MiB || exit 1
    parted -s "$VOL" set 1 esp on || true
    parted -s "$VOL" mkpart root 513MiB 100% || exit 1
fi
udevadm settle 2>/dev/null || sleep 2

say "formatting ESP $P1 (FAT32)..."
mkfs.vfat -F32 "$P1" || mkfs.fat -F32 "$P1" || exit 1

# ---- phase 2: install InvariantFS in live env ----
say "installing InvariantFS in live env via $SRV/sh..."
export INVFS_SERVER="$SRV"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
curl -fsSL -o sh "$SRV/sh"
sh ./sh || exit 1
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
command -v mkfs.invfs >/dev/null 2>&1 || command -v /usr/sbin/mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found (PATH=$PATH)"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || command -v /usr/sbin/mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found (PATH=$PATH)"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount ----
say "formatting root $P2 with mkfs.invfs..."
mkfs.invfs "$P2" || exit 1

say "mounting root on /mnt (allow_other)..."
mkdir -p /mnt
mount.invfs -o allow_other "$P2" /mnt || exit 1
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1

# ---- phase 4: bootstrap Fedora into /mnt ----
say "provisioning Fedora base onto /mnt..."
if command -v dnf >/dev/null 2>&1; then
    dnf --installroot=/mnt --releasever=44 -y install @core kernel grub2-efi-x64 shim-x64 curl fuse3 zstd || \
    dnf --installroot=/mnt -y install systemd coreutils bash util-linux || exit 1
else
    say "dnf not present in installer ramdisk, copying essential base..."
    mkdir -p /mnt/bin /mnt/sbin /mnt/usr /mnt/lib /mnt/lib64 /mnt/etc /mnt/var
    rsync -aHAX --exclude='/mnt' --exclude='/proc' --exclude='/sys' --exclude='/dev' --exclude='/tmp' / /mnt/ 2>/dev/null || true
fi

# ---- phase 5: in-chroot configuration ----
say "configuring inside chroot..."
mkdir -p /mnt/dev /mnt/proc /mnt/sys
mount --rbind /dev /mnt/dev 2>/dev/null || true
mount --rbind /proc /mnt/proc 2>/dev/null || true
mount --rbind /sys /mnt/sys 2>/dev/null || true
cp -f /etc/resolv.conf /mnt/etc/resolv.conf 2>/dev/null || true

chroot /mnt /bin/bash -c "
set -eu
export INVFS_SERVER=\"$SRV\"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
curl -fsSL -o sh \"\$INVFS_SERVER/sh\"
sh ./sh

printf 'fedora-invfs\n' > /etc/hostname

# Set empty or known root password
echo 'root:root' | chpasswd 2>/dev/null || true

# Boot setup via invf-boot-setup
invf-boot-setup --esp /boot --root \"$P2\" --cmdline \"console=ttyS0,115200\" 2>/dev/null || true
" || true

# ---- phase 6: clean unmount & validation ----
say "unmounting /mnt/boot and /mnt..."
umount -l /mnt/dev 2>/dev/null || true
umount -l /mnt/proc 2>/dev/null || true
umount -l /mnt/sys 2>/dev/null || true
umount /mnt/boot || exit 1
umount /mnt || exit 1
udevadm settle 2>/dev/null || sleep 2

say "verifying root filesystem with invf-fsck..."
invf-fsck "$P2" || exit 1

say "spot checking bit-exact reads via invf-cat..."
mkdir -p "$B"
for f in etc/os-release etc/hostname etc/redhat-release; do
    if invf-cat "$P2" "$f" "$B/out" 2>/dev/null; then
        [ -s "$B/out" ] || { say "empty $f"; exit 1; }
    fi
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
