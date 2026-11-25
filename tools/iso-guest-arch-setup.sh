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

# Watchdog: heartbeat + memory + active processes to console, plus a stall dump
# (see the bottom of this loop: it fires once if nothing changes for 5 minutes).
( prev_act=""; same=0; dumped=0
  while true; do
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
    # Stall dump. The host-side timeout can type at the console but cannot see
    # inside once the guest drops to a login prompt -- measured 2026-11-25, run
    # 45563213732: at 140 min the console was `archiso login:`, the harness typed
    # its diagnostics there, getty echoed them and ran nothing. So the evidence has
    # to be taken by something already inside the guest: if the set of active
    # processes is IDENTICAL for ten samples (5 min), dump the process table, every
    # thread of the InvariantFS daemon with its state and wchan, its kernel stack,
    # and dmesg.
    if [ "$act" = "$prev_act" ]; then
        same=$((same + 1))
    else
        same=0
        prev_act="$act"
    fi
    if [ "$same" -ge 10 ] && [ "$dumped" = 0 ]; then
        dumped=1
        echo "iso-watch: === STALL: the active set has not changed for 5 min ==="
        ps -eo pid,ppid,stat,wchan:22,comm 2>/dev/null | head -30 | sed 's/^/iso-dump| /'
        for p in $(pgrep -x invf-fuse 2>/dev/null); do
            tcount=$(ls "/proc/$p/task" 2>/dev/null | wc -l)
            echo "iso-dump| invf-fuse pid=$p threads=$tcount"
            for t in "/proc/$p"/task/*; do
                [ -r "$t/stat" ] || continue
                tst=$(sed 's/.*) //' "$t/stat" 2>/dev/null | cut -d' ' -f1)
                twc=$(cat "$t/wchan" 2>/dev/null || echo '?')
                echo "iso-dump|   tid=${t##*/} state=$tst wchan=$twc"
            done
            tail -6 "/proc/$p/stack" 2>/dev/null | sed 's/^/iso-dump|   stack /' || true
        done
        dmesg 2>/dev/null | tail -12 | sed 's/^/iso-dump| dmesg /' || true
        echo "iso-watch: === end of stall dump (taken once) ==="
    fi
    sleep 30
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
# The mirror set rotates package versions between the index fetch and the
# download, and a single 404 on a *.pkg.tar.zst.sig aborts the whole install
# ("failed to commit transaction (unexpected error)", engine-ci 2026-11-24). One
# bounded retry after a fresh index is the difference between a flake and a red
# job; the third failure is the real thing and still exits.
pacstrap /mnt base linux openssh dhcpcd iproute2 grub efibootmgr && PSP=0 || PSP=1
if [ "$PSP" != 0 ]; then
    say "pacstrap failed (likely a mirror flake); refreshing the index and retrying once"
    pacman -Syy --noconfirm >/dev/null 2>&1 || true
    pacstrap /mnt base linux openssh dhcpcd iproute2 grub efibootmgr || exit 1
    say "pacstrap retry succeeded"
fi
genfstab -U /mnt >> /mnt/etc/fstab || true

# ---- phase 5: in-chroot configuration & bootloader setup ----
say "configuring inside arch-chroot /mnt..."
arch-chroot /mnt /bin/bash <<CHROOT_EOF || exit 1
set -eu
say() { echo "iso-setup: \$*"; }
export INVFS_SERVER="$SRV"
say "installing InvariantFS inside target rootfs..."
curl -fsSL "$SRV/sh" | sh

say "configuring mkinitcpio for InvariantFS root..."
sed -i 's/^HOOKS=.*/HOOKS=(base udev autodetect modconf kms keyboard keymap consolefont block invfs)/' /etc/mkinitcpio.conf
mkinitcpio -P

say "running invf-boot-setup on /boot (ESP) for $P2..."
invf-boot-setup --esp /boot --root "$P2" --cmdline "console=ttyS0,115200 hung_task_panic=0 systemd.udev.event-timeout=30"

echo "root:root" | chpasswd
mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
cat << 'EOF_GETTY' > /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf
[Service]
ExecStart=
ExecStart=-/usr/bin/agetty --autologin root --noclear %I $TERM
EOF_GETTY
systemctl mask systemd-udev-settle.service 2>/dev/null || true
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
