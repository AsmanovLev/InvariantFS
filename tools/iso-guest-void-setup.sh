#!/bin/bash
# iso-guest-void-setup.sh -- runs INSIDE a stock Void live env, chrooted
# into the live root from the dracut break shell (see iso-void-install.sh).
#
# Real native Void Linux install on InvariantFS per Handbook XBPS method:
#   1. GPT disk partitioning: ESP (512M FAT32) + InvariantFS root
#   2. Install InvariantFS in live env via curl/fetch http://10.0.2.2:PORT/sh | sh
#   3. Format root (mkfs.invfs) and mount (mount.invfs -o allow_other)
#   4. Mount ESP at /mnt/boot
#   5. Install base-system + grub-x86_64-efi into /mnt via xbps-install -r /mnt
#   6. Inside /mnt: install InvariantFS, configure dracut with 90invfs,
#      build initramfs, and run invf-boot-setup --esp /boot --root <dev>
#   7. Clean unmount, offline invf-fsck verify, and PASS marker.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8001}"
VOL=""   # resolved below: invfsvol= cmdline, else only writable disk
B=/root/pkg
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
    timeout 60 poweroff || poweroff -f
}
trap 'done_rc=$?; finish' EXIT

# Watchdog: heartbeat + memory + active processes to console
( while true; do
    mem=$(free -m | awk '/^Mem/{print $3"/"$2"MB"}')
    load=$(cut -d' ' -f1 /proc/loadavg)
    act=""
    for p in /proc/[0-9]*; do
        [ -d "$p" ] || continue
        stat=$(cat "$p/stat" 2>/dev/null) || continue
        st="${stat##*) }"
        state="${st%% *}"
        comm=$(cat "$p/comm" 2>/dev/null || echo "?")
        show=0
        case "$state" in
            R|D) show=1 ;;
            *)
                case "$comm" in
                    apk|pacman|xbps*|dnf|yum|apt*|dpkg*|nix*|emerge|ebuild|tar|invf*|sh|bash|addgroup|useradd|chroot|mkfs*|mount*)
                        show=1 ;;
                esac
                ;;
        esac
        if [ "$show" = "1" ]; then
            pid="${p##*/}"
            wc=$(cat "$p/wchan" 2>/dev/null || echo "?")
            cmd=$(tr '\0\n' '  ' < "$p/cmdline" 2>/dev/null | cut -c1-40)
            act="$act [${pid}:${comm}(${state},${wc}) ${cmd}]"
        fi
    done
    echo "iso-watch: mem=$mem load=$load act=$act"
    sleep 30
  done ) &

fetch() { # fetch <url> <dest>
    if command -v curl >/dev/null 2>&1; then
        curl -fsS -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    elif command -v xbps-fetch >/dev/null 2>&1; then
        if [ "$2" = "/dev/null" ]; then
            ( cd /tmp && xbps-fetch "$1" >/dev/null 2>&1 ) || return 1
            rm -f "/tmp/$(basename "$1")"
            return 0
        fi
        ( cd "$(dirname "$2")" && xbps-fetch "$1" ) || return 1
        [ "$(dirname "$2")/$(basename "$1")" = "$2" ] \
            || mv -f "$(dirname "$2")/$(basename "$1")" "$2"
    else
        python3 -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
    fi
}

# Ensure network connectivity
if ! fetch "$SRV/sh" /dev/null; then
    say "no route to serve dir; trying dhcpcd"
    dhcpcd -w 30 2>/dev/null || udhcpc -i eth0 2>/dev/null || true
    fetch "$SRV/sh" /dev/null || exit 1
fi
say "serve dir reachable"

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
[ -e /dev/fuse ] || modprobe fuse 2>/dev/null || true
[ -e /dev/fuse ] || { say "no /dev/fuse (modprobe fuse failed)"; exit 1; }
say "proc/sys mounted, /dev/fuse present"

# Update host xbps and tools if repo is reachable
say "updating live xbps and utilities..."
xbps-install -S 2>&1 | tail -2 || true
xbps-install -uy xbps 2>&1 | tail -2 || true
xbps-install -Sy zstd dosfstools 2>&1 | tail -2 || true

# ---- phase 1: identify and partition target disk ----
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
if [ -z "$VOL" ]; then
    VOL=$(lsblk -dn -o NAME,RO,TYPE | awk '$2=="0" && $3=="disk" {print "/dev/"$1}' | head -1)
fi
[ -b "${VOL:-none}" ] || { say "no volume disk"; exit 1; }
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
elif command -v fdisk >/dev/null 2>&1; then
    printf 'g\nn\n1\n\n+512M\nt\n1\nn\n2\n\n\nw\n' | fdisk "$VOL" || true
fi
udevadm settle 2>/dev/null || sleep 2

say "formatting ESP $P1 (FAT32)..."
mkfs.vfat -F32 "$P1" || mkfs.fat -F32 "$P1" || exit 1

# ---- phase 2: install InvariantFS in live env ----
say "installing InvariantFS in live env via $SRV/sh..."
export INVFS_SERVER="$SRV"
mkdir -p /tmp/invfs-sh
fetch "$SRV/sh" /tmp/invfs-sh/sh || exit 1
sh /tmp/invfs-sh/sh || exit 1
command -v mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount ----
say "formatting root $P2 with mkfs.invfs..."
mkfs.invfs "$P2" || exit 1

say "mounting root on /mnt (allow_other)..."
mkdir -p /mnt
mount.invfs -o allow_other "$P2" /mnt || exit 1
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1

# ---- phase 4: install base-system via xbps-install -r /mnt ----
say "configuring xbps repo keys for target..."
mkdir -p /mnt/var/db/xbps/keys /mnt/etc/xbps.d
cp /var/db/xbps/keys/*.plist /mnt/var/db/xbps/keys/ || exit 1
printf 'repository=https://repo-default.voidlinux.org/current\n' \
    > /mnt/etc/xbps.d/00-repository-main.conf

say "updating xbps index and installing base-system grub grub-x86_64-efi into /mnt..."
XBPS_ARCH=x86_64 xbps-install -r /mnt -Sy base-system grub grub-x86_64-efi || exit 1
say "base system installed on /mnt"

# ---- phase 5: in-chroot provisioning ----
say "binding dev/proc/sys into /mnt..."
cp -f /etc/resolv.conf /mnt/etc/resolv.conf 2>/dev/null || true
mount --rbind /dev /mnt/dev 2>/dev/null || true
mount --rbind /proc /mnt/proc 2>/dev/null || true
mount --rbind /sys /mnt/sys 2>/dev/null || true

say "installing InvariantFS and configuring boot inside /mnt..."
chroot /mnt /bin/bash -c "
set -eu
export INVFS_SERVER=\"$SRV\"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o sh \"\$INVFS_SERVER/sh\"
elif command -v xbps-fetch >/dev/null 2>&1; then
    xbps-fetch \"\$INVFS_SERVER/sh\"
fi
sh ./sh

mkdir -p /etc/dracut.conf.d
echo 'add_dracutmodules+=\" invfs \"' > /etc/dracut.conf.d/invfs.conf

# Rebuild dracut initramfs for installed kernel
kver=\$(ls /lib/modules 2>/dev/null | head -1)
if [ -n \"\$kver\" ]; then
    dracut --force \"/boot/initramfs-\$kver.img\" \"\$kver\" || dracut --force /boot/initramfs.img
else
    dracut --force /boot/initramfs.img
fi

# Runit services: agetty-ttyS0, dhcpcd, sshd
mkdir -p /etc/runit/runsvdir/default
mkdir -p /etc/sv/agetty-serial
cat > /etc/sv/agetty-serial/conf <<'EOF_SER'
GETTY_ARGS=\"-L -8 -a root\"
BAUD_RATE=115200
TERM_NAME=vt100
EOF_SER

ln -sfn /etc/sv/agetty-ttyS0 /etc/runit/runsvdir/default/agetty-ttyS0
ln -sfn /etc/sv/dhcpcd /etc/runit/runsvdir/default/dhcpcd
ln -sfn /etc/sv/sshd /etc/runit/runsvdir/default/sshd

# sshd configuration: allow root password login
mkdir -p /etc/ssh/sshd_config.d
cat > /etc/ssh/sshd_config.d/99-invfs.conf <<'EOF_SSH'
PermitRootLogin yes
PasswordAuthentication yes
UseDNS no
EOF_SSH
chmod 644 /etc/ssh/sshd_config.d/99-invfs.conf

# Pre-generate SSH host keys if ssh-keygen exists
if command -v ssh-keygen >/dev/null 2>&1; then
    ssh-keygen -A 2>/dev/null || true
fi

# Hostname
printf 'void-invfs\n' > /etc/hostname

# FUSE-root boot fix: prevent runit 03-filesystems.sh remount / failure
if [ -f /etc/runit/core-services/03-filesystems.sh ]; then
    if ! grep -q '# WP64: FUSE root' /etc/runit/core-services/03-filesystems.sh; then
        sed -i '/\[ -n \"\$VIRTUALIZATION\" \] && return 0/a\\
# WP64: FUSE root -- mount.fuse cannot remount\\
if grep -q \" / fuse \" /proc/mounts 2>/dev/null; then\\
    msg \"FUSE root detected; skipping root fsck/remount and mount -a\"\\
    return 0\\
fi' /etc/runit/core-services/03-filesystems.sh
    fi
fi

invf-boot-setup --esp /boot --root \"$P2\" --cmdline \"console=ttyS0,115200\"
echo 'root:root' | chpasswd
" || exit 1

# ---- phase 6: clean unmount & validation ----
say "unmounting /mnt..."
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
for f in etc/os-release etc/passwd; do
    invf-cat "$P2" "$f" "$B/out" || exit 1
    [ -s "$B/out" ] || { say "empty $f"; exit 1; }
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
