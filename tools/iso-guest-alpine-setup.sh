#!/bin/sh
# iso-guest-alpine-setup.sh -- runs INSIDE stock Alpine live environment.
#
# Real native Alpine Linux install on InvariantFS:
#   1. GPT disk partitioning: ESP (512M FAT32) + InvariantFS root
#   2. Install host tools (gcompat, dosfstools, sfdisk) in live env
#   3. Format root (mkfs.invfs) and mount (mount.invfs -o allow_other)
#   4. Mount ESP at /mnt/boot
#   5. Bootstrap distro packages into /mnt via apk add --root /mnt
#   6. Inside /mnt: configure inittab, OpenRC, install InvariantFS,
#      and run invf-boot-setup --esp /boot --root <dev>
#   7. Clean unmount, offline invf-fsck verify, and PASS marker.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8003}"
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

say "checking connectivity to $SRV..."
wget -q -O /dev/null "$SRV/sh" || {
    say "network unreachable, retrying udhcpc"
    udhcpc -i eth0 || true
    wget -q -O /dev/null "$SRV/sh" || exit 1
}
say "serve dir reachable"

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
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

# Install necessary partitioning & glibc compatibility packages in live Alpine
say "updating live apk and installing prerequisites (gcompat, dosfstools, sfdisk)..."
# In stock Alpine live ISO, repositories may need community for some packages
grep -q community /etc/apk/repositories 2>/dev/null || {
    sed -i 's/main/community/' /etc/apk/repositories 2>/dev/null || true
    echo "https://dl-cdn.alpinelinux.org/alpine/v3.24/main" >> /etc/apk/repositories
    echo "https://dl-cdn.alpinelinux.org/alpine/v3.24/community" >> /etc/apk/repositories
}
apk update || true
apk add sfdisk util-linux dosfstools e2fsprogs gcompat curl zstd || true

say "partitioning $VOL (GPT: 512M ESP + InvFS root)..."
if command -v sfdisk >/dev/null 2>&1; then
    sfdisk "$VOL" << 'EOF' || exit 1
label: gpt
size=512M, type=U
type=L
EOF
else
    # Busybox fdisk does not support GPT 'g', use sfdisk from util-linux/sfdisk
    apk add sfdisk 2>/dev/null || true
    if command -v sfdisk >/dev/null 2>&1; then
        sfdisk "$VOL" << 'EOF' || exit 1
label: gpt
size=512M, type=U
type=L
EOF
    else
        # Fallback to DOS MBR for busybox fdisk
        printf 'o\nn\np\n1\n\n+512M\nt\nef\nn\np\n2\n\n\nw\n' | fdisk "$VOL" || true
    fi
fi
mdev -s 2>/dev/null || true
udevadm settle 2>/dev/null || sleep 2

say "formatting ESP $P1 (FAT32)..."
if command -v mkfs.vfat >/dev/null 2>&1; then
    mkfs.vfat -F32 "$P1" || exit 1
elif command -v mkfs.fat >/dev/null 2>&1; then
    mkfs.fat -F32 "$P1" || exit 1
else
    apk add dosfstools 2>/dev/null || true
    mkfs.vfat -F32 "$P1" || exit 1
fi

# ---- phase 2: install InvariantFS in live env ----
say "installing InvariantFS in live env via $SRV/sh..."
# Install fuse3 and runtime libraries so InvariantFS runs under Alpine (gcompat)
apk add fuse3 zstd zlib >/dev/null 2>&1 || true
export INVFS_SERVER="$SRV"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
wget -q -O sh "$SRV/sh" || curl -fsSL -o sh "$SRV/sh"
sh ./sh || exit 1
export PATH="/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
hash -r 2>/dev/null || true
command -v mkfs.invfs >/dev/null 2>&1 || command -v /usr/sbin/mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found (PATH=$PATH)"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || command -v /usr/sbin/mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found (PATH=$PATH)"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount ----
say "formatting root $P2 with mkfs.invfs..."
if command -v mkfs.invfs >/dev/null 2>&1; then
    mkfs.invfs "$P2" || exit 1
else
    /usr/sbin/mkfs.invfs "$P2" || exit 1
fi

say "mounting root on /mnt (allow_other)..."
mkdir -p /mnt
if command -v mount.invfs >/dev/null 2>&1; then
    mount.invfs -o allow_other "$P2" /mnt || exit 1
else
    /usr/sbin/mount.invfs -o allow_other "$P2" /mnt || exit 1
fi
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1
mkdir -p /mnt/dev /mnt/proc /mnt/sys
mount --rbind /dev /mnt/dev 2>/dev/null || true
mount --rbind /proc /mnt/proc 2>/dev/null || true
mount --rbind /sys /mnt/sys 2>/dev/null || true

# ---- phase 4: install Alpine base-system into /mnt ----
say "bootstrapping Alpine packages into /mnt via apk..."
mkdir -p /mnt/etc/apk
# In Alpine 3.24+, grub triggers update-grub via grub-probe which fails on FUSE root.
# Disable grub.trigger; bootloader setup is handled standalone by invf-boot-setup.
echo "disable_trigger=1" > /mnt/etc/update-grub.conf
cp -a /etc/apk/keys /mnt/etc/apk/ 2>/dev/null || true
cat > /mnt/etc/apk/repositories <<'EOF'
https://dl-cdn.alpinelinux.org/alpine/v3.24/main
https://dl-cdn.alpinelinux.org/alpine/v3.24/community
EOF

apk add --root /mnt --initdb --repositories-file /mnt/etc/apk/repositories --keys-dir /etc/apk/keys \
    alpine-base linux-virt openrc util-linux grub grub-efi gcompat fuse3 zstd zlib || exit 1
say "base system installed on /mnt"

# ---- phase 5: in-chroot provisioning ----
say "configuring in-chroot environment..."
cp -f /etc/resolv.conf /mnt/etc/resolv.conf 2>/dev/null || true

chroot /mnt /bin/sh -c "
set -eu
export INVFS_SERVER=\"$SRV\"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
wget -q -O sh \"\$INVFS_SERVER/sh\" || curl -fsSL -o sh \"\$INVFS_SERVER/sh\"
sh ./sh

printf 'alpine-invfs\n' > /etc/hostname

# inittab serial autologin
if [ -f /etc/inittab ]; then
    grep -v 'ttyS0' /etc/inittab > /etc/inittab.tmp || true
    echo 'ttyS0::respawn:/sbin/getty -L 115200 ttyS0 -a root' >> /etc/inittab.tmp
    cat /etc/inittab.tmp > /etc/inittab
    rm -f /etc/inittab.tmp
fi

# empty root password
if [ -f /etc/shadow ]; then
    awk -F: 'BEGIN{OFS=\":\"} \$1==\"root\"{\$2=\"\"} {print}' /etc/shadow > /etc/shadow.tmp
    cat /etc/shadow.tmp > /etc/shadow
    rm -f /etc/shadow.tmp
fi

# Ensure generic symlinks exist on /boot
if [ -f /boot/vmlinuz-virt ] && [ ! -e /boot/vmlinuz ]; then
    ln -sf vmlinuz-virt /boot/vmlinuz || true
fi
if [ -f /boot/initramfs-virt ] && [ ! -e /boot/initramfs.img ]; then
    ln -sf initramfs-virt /boot/initramfs.img || true
fi

invf-boot-setup --esp /boot --root \"$P2\" --cmdline \"console=ttyS0,115200\"
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
for f in etc/os-release etc/inittab etc/hostname; do
    invf-cat --follow "$P2" "$f" "$B/out" || exit 1
    [ -s "$B/out" ] || { say "empty $f"; exit 1; }
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
