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
# Alpine's apk branch directory is v<major>.<minor>, and it is not the same
# string as the release: this file used to write `/.24` (a lost `v3`), apk then
# answered `gcompat (no such package)` from a repository that does not exist,
# gcompat/fuse3/zstd/zlib never installed, and the glibc release binaries died
# as "not found" (a missing ELF interpreter reads exactly like a missing file)
# at mkfs.invfs -- engine-ci 2026-11-23, installation-iso-alpine, nine minutes
# into the job. The live system knows its own release; ask it.
ALPINE_BRANCH="v$(cut -d. -f1-2 /etc/alpine-release 2>/dev/null || echo 3.24)"

say "updating live apk and installing prerequisites (gcompat, dosfstools, sfdisk)..."
# In stock Alpine live ISO, repositories may need community for some packages
grep -q community /etc/apk/repositories 2>/dev/null || {
    sed -i 's/main/community/' /etc/apk/repositories 2>/dev/null || true
    echo "https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/main" >> /etc/apk/repositories
    echo "https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/community" >> /etc/apk/repositories
}
apk update || true
# gcompat, fuse3, zstd and zlib are REQUIRED: without them the release tarball's
# binaries do not run on musl at all. This used to be `|| true`, which turned the
# 404 above into "mkfs.invfs: ... /usr/bin/invf-mkfs: not found" four steps later.
apk add sfdisk util-linux dosfstools e2fsprogs curl || true
apk add gcompat fuse3 zstd zlib     || { say "required packages did not install (repositories: $(tr '
' ' ' < /etc/apk/repositories))"; exit 1; }

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
# Unquoted heredoc on purpose: the branch comes from the live system above, so
# the target root and the installer cannot disagree about which repository set
# the base system comes from.
cat > /mnt/etc/apk/repositories <<EOF
https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/main
https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/community
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

# inittab serial autologin / console
if [ -f /etc/inittab ]; then
    grep -v 'ttyS0' /etc/inittab > /etc/inittab.tmp || true
    echo 'ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100' >> /etc/inittab.tmp
    cat /etc/inittab.tmp > /etc/inittab
    rm -f /etc/inittab.tmp
fi

# empty root password
if [ -f /etc/shadow ]; then
    awk -F: 'BEGIN{OFS=\":\"} \$1==\"root\"{\$2=\"\"} {print}' /etc/shadow > /etc/shadow.tmp
    cat /etc/shadow.tmp > /etc/shadow
    rm -f /etc/shadow.tmp
fi

# Ensure generic symlinks or copies exist on /boot
if [ -f /boot/vmlinuz-virt ] && [ ! -e /boot/vmlinuz ]; then
    ln -sf vmlinuz-virt /boot/vmlinuz 2>/dev/null || cp -f /boot/vmlinuz-virt /boot/vmlinuz 2>/dev/null || true
fi
if [ -f /boot/initramfs-virt ] && [ ! -e /boot/initramfs.img ]; then
    ln -sf initramfs-virt /boot/initramfs.img 2>/dev/null || cp -f /boot/initramfs-virt /boot/initramfs.img 2>/dev/null || true
fi

# Configure mkinitfs with InvariantFS support
echo \"configuring mkinitfs for InvariantFS root...\"
mkdir -p /etc/mkinitfs/features.d
cat > /etc/mkinitfs/features.d/invfs.modules << 'M_EOF'
kernel/fs/fuse/*
M_EOF

cat > /etc/mkinitfs/features.d/invfs.files << 'F_EOF'
/usr/bin/invf-fuse
/usr/sbin/invf-fuse
/lib/ld-linux-x86-64.so.2
/usr/lib/libgcompat.so*
/lib/libgcompat.so*
/usr/lib/libfuse3.so*
/lib/libfuse3.so*
/usr/lib/libzstd.so*
/lib/libzstd.so*
/lib/libz.so*
/usr/lib/libz.so*
F_EOF

if [ -f /etc/mkinitfs/mkinitfs.conf ]; then
    if ! grep -qw \"invfs\" /etc/mkinitfs/mkinitfs.conf; then
        sed -i 's/^features=\"/features=\"invfs /' /etc/mkinitfs/mkinitfs.conf 2>/dev/null || \
            echo 'features=\"\$features invfs\"' >> /etc/mkinitfs/mkinitfs.conf
    fi
fi

if [ -f /usr/share/mkinitfs/initramfs-init ] && ! grep -q \"invf-fuse\" /usr/share/mkinitfs/initramfs-init; then
    awk '/if \\[ \"\\\$rootfstype\" = \"zfs\" \\]; then/{
        print \"\\t\\tif [ \\\"\\\$rootfstype\\\" = \\\"invfs\\\" ]; then\"
        print \"\\t\\t\\tmodprobe fuse 2>/dev/null || true\"
        print \"\\t\\t\\t[ -e /dev/fuse ] || mknod /dev/fuse c 10 229 2>/dev/null || true\"
        print \"\\t\\t\\t[ -e /dev/fuse ] && chmod 666 /dev/fuse 2>/dev/null || true\"
        print \"\\t\\t\\topts=\\\"allow_other,survive_term\\\"\"
        print \"\\t\\t\\t[ -n \\\"\\\$KOPT_rootflags\\\" ] && opts=\\\"\\\$opts,\\\$KOPT_rootflags\\\"\"
        print \"\\t\\t\\tinvf-fuse -o \\\"\\\$opts\\\" \\\"\\\$KOPT_root\\\" \\\"\\\$sysroot\\\"\"
        print \"\\t\\t\\tmount --rbind /proc \\\"\\\$sysroot\\\"/proc 2>/dev/null || true\"
        print \"\\t\\t\\tmount --rbind /sys \\\"\\\$sysroot\\\"/sys 2>/dev/null || true\"
        print \"\\t\\t\\tmount --rbind /dev \\\"\\\$sysroot\\\"/dev 2>/dev/null || true\"
        print \"\\t\\t\\tmkdir -p \\\"\\\$sysroot\\\"/run 2>/dev/null || true\"
        print \"\\t\\t\\tmount -t tmpfs tmpfs \\\"\\\$sysroot\\\"/run 2>/dev/null || true\"
        print \"\\t\\t\\texec chroot \\\"\\\$sysroot\\\" \\\"\\\${KOPT_init:-/sbin/init}\\\"\"
        print \"\\t\\tfi\"
    }
    { print }' /usr/share/mkinitfs/initramfs-init > /usr/share/mkinitfs/initramfs-init.tmp && \
    mv /usr/share/mkinitfs/initramfs-init.tmp /usr/share/mkinitfs/initramfs-init
fi

kver=\$(ls /lib/modules 2>/dev/null | tail -1 || true)
if [ -n \"\$kver\" ]; then
    mkinitfs -o /boot/initramfs-virt \"\$kver\" || true
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

# A FUSE daemon is not a mount: `umount` detaches the mount point and returns,
# the invf-fuse that served it exits a moment later, and until it does it still
# holds the volume's flock -- vol_open() then answers
#     "image is in use by another process (flock: Resource temporarily unavailable)"
# Measured 2026-11-23 in engine-ci on void/arch/debian/gentoo: invf-fsck ran
# three seconds after a successful umount and was refused with exactly that
# line, so the installation was fine and the VERIFICATION failed.
#
# Kill it by NAME and wait for it to go. Not `fuser -km <mountpoint>`: after a
# successful umount the mount point is detached, so fuser no longer attributes
# the daemon's open files to it and finds nothing -- checked locally, a live
# process named invf-fuse survives `fuser -km` on a path it does not hold open.
# Not a fixed sleep either: the runner's load decides how long the exit takes,
# and the three seconds this setup used to give it were not enough.
pkill -x invf-fuse 2>/dev/null || true
for _i in $(seq 1 10); do
    pgrep -x invf-fuse >/dev/null 2>&1 || break
    sleep 0.2
done
if pgrep -x invf-fuse >/dev/null 2>&1; then
    pkill -9 -x invf-fuse 2>/dev/null || true
    for _i in $(seq 1 10); do
        pgrep -x invf-fuse >/dev/null 2>&1 || break
        sleep 0.2
    done
fi
if pgrep -x invf-fuse >/dev/null 2>&1; then
    say "invf-fuse survived SIGTERM and SIGKILL: $(pgrep -x invf-fuse | tr '
' ' ')"
fi

say "verifying root filesystem with invf-fsck..."
# Capture and PRINT the verdict. Measured 2026-11-24 (installation-iso-alpine):
# this line failed with rc=1 and the log carried no output at all -- the `say`
# above it was the last thing written, so the log said "the setup failed" and
# nothing about why. A refusal that prints nothing is the shape the project
# polices everywhere else; a harness must not have it either.
if ! invf-fsck "$P2" > /tmp/invfs-fsck-final.log 2>&1; then
    say "invf-fsck on $P2 FAILED; its output follows:"
    while IFS= read -r _l; do say "  fsck| $_l"; done < /tmp/invfs-fsck-final.log
    exit 1
fi
say "root filesystem clean"

say "spot checking bit-exact reads via invf-cat..."
mkdir -p "$B"
for f in etc/os-release etc/inittab etc/hostname; do
    invf-cat --follow "$P2" "$f" "$B/out" || exit 1
    [ -s "$B/out" ] || { say "empty $f"; exit 1; }
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
