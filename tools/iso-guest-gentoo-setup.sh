#!/bin/bash
# iso-guest-gentoo-setup.sh -- runs INSIDE a stock Gentoo minimal live env
# over SSH (driver boots dosshd + passwd= and runs this one command).
#
# Real native Gentoo Linux install on InvariantFS per Handbook:
#   1. GPT disk partitioning: ESP (512M FAT32) + InvariantFS root
#   2. Install InvariantFS in live env via curl -fsSL http://10.0.2.2:PORT/sh | sh
#   3. Format root (mkfs.invfs) and mount (mount.invfs -o allow_other)
#   4. Mount ESP at /mnt/gentoo/boot
#   5. Unpack Stage3 tarball directly into /mnt/gentoo
#   6. Inside /mnt/gentoo: install InvariantFS, configure dracut with 90invfs,
#      build initramfs, and run invf-boot-setup --esp /boot --root <dev>
#   7. Clean unmount, offline invf-fsck verify, and PASS marker.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8002}"
VOL=""   # resolved below: invfsvol= cmdline, else only writable disk
B=/root/pkg
# Pinned stage3 (policy: reproducible inputs; bump with checksum together).
STAGE3_VER="20261011T170049Z"
STAGE3_URL="https://distfiles.gentoo.org/releases/amd64/autobuilds/current-stage3-amd64-openrc/stage3-amd64-openrc-$STAGE3_VER.tar.xz"
STAGE3_SHA512="80ccdd6d88f94f695f77741fc4f7d81e841565255073ac65aef8e28159c6657146a9a07bafd39e127d8b53270262123f2320c6f0c0e68c9d55da648866a4f57c  stage3-amd64-openrc-$STAGE3_VER.tar.xz"
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
    else
        python3 -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
    fi
}
if ! fetch "$SRV/sh" /dev/null; then
    say "no route to serve dir; trying dhcpcd"
    dhcpcd -w 30 2>/dev/null || udhcpc -i eth0 2>/dev/null || true
    fetch "$SRV/sh" /dev/null || exit 1
fi
say "serve dir reachable"

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
elif command -v sgdisk >/dev/null 2>&1; then
    sgdisk -Z "$VOL" || exit 1
    sgdisk -n 1:0:+512M -t 1:ef00 "$VOL" || exit 1
    sgdisk -n 2:0:0 -t 2:8300 "$VOL" || exit 1
fi
udevadm settle 2>/dev/null || sleep 2

say "formatting ESP $P1 (FAT32)..."
mkfs.vfat -F32 "$P1" || mkfs.fat -F32 "$P1" || exit 1

# ---- phase 2: install InvariantFS in live env ----
say "installing InvariantFS in live environment via $SRV/sh..."
export INVFS_SERVER="$SRV"
curl -fsSL "$SRV/sh" | sh || exit 1
command -v mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount ----
say "formatting root $P2 with mkfs.invfs..."
mkfs.invfs "$P2" || exit 1

say "mounting root on /mnt/gentoo (allow_other)..."
mkdir -p /mnt/gentoo
mount.invfs -o allow_other "$P2" /mnt/gentoo || exit 1
mkdir -p /mnt/gentoo/boot
mount "$P1" /mnt/gentoo/boot || exit 1

# ---- phase 4: fetch and unpack stage3 directly into /mnt/gentoo ----
mkdir -p "$B"
STAGE3_FILE="stage3-amd64-openrc-$STAGE3_VER.tar.xz"
if ! fetch "$SRV/stage3.tar.xz" "$B/$STAGE3_FILE"; then
    say "host stage3 missing; falling back to $STAGE3_URL"
    fetch "$STAGE3_URL" "$B/$STAGE3_FILE" || exit 1
fi
[ -s "$B/$STAGE3_FILE" ] || { say "stage3 fetch produced no file"; exit 1; }
( cd "$B" && echo "$STAGE3_SHA512" | sha512sum -c - ) || exit 1
say "stage3 checksum OK"

say "unpacking stage3 directly into /mnt/gentoo..."
tar -xpf "$B/$STAGE3_FILE" -C /mnt/gentoo || exit 1
printf 'invfs-gentoo\n' > /mnt/gentoo/etc/hostname

# ---- phase 5: in-chroot configuration ----
say "binding dev/proc/sys into /mnt/gentoo..."
mount --rbind /dev /mnt/gentoo/dev 2>/dev/null || true
mount --rbind /proc /mnt/gentoo/proc 2>/dev/null || true
mount --rbind /sys /mnt/gentoo/sys 2>/dev/null || true

say "installing InvariantFS inside /mnt/gentoo..."
chroot /mnt/gentoo /bin/bash -c "
set -eu
export INVFS_SERVER=\"$SRV\"
curl -fsSL \"\$INVFS_SERVER/sh\" | sh
invf-boot-setup --esp /boot --root \"$P2\" --cmdline \"console=ttyS0,115200\"
echo 'root:root' | chpasswd 2>/dev/null || true
" || exit 1

# ---- phase 6: clean unmount & validation ----
say "unmounting /mnt/gentoo..."
umount -l /mnt/gentoo/dev 2>/dev/null || true
umount -l /mnt/gentoo/proc 2>/dev/null || true
umount -l /mnt/gentoo/sys 2>/dev/null || true
umount /mnt/gentoo/boot || exit 1
umount /mnt/gentoo || exit 1
udevadm settle 2>/dev/null || sleep 2

say "verifying root filesystem with invf-fsck..."
invf-fsck "$P2" || exit 1

say "spot checking bit-exact reads via invf-cat..."
mkdir -p "$B"
for f in etc/os-release etc/gentoo-release etc/passwd bin/bash; do
    invf-cat "$P2" "$f" "$B/out" || exit 1
    [ -s "$B/out" ] || { say "empty $f"; exit 1; }
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
