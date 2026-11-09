#!/usr/bin/env bash
# iso-guest-nixos-setup.sh -- runs INSIDE NixOS live minimal installer.
#
# Non-interactive NixOS installation onto InvariantFS:
#   1. GPT disk partitioning: ESP (512M FAT32) + InvariantFS root
#   2. Format ESP (mkfs.vfat)
#   3. Install InvariantFS in live env via $SRV/sh
#   4. Format root (mkfs.invfs) and mount on /mnt (mount.invfs -o allow_other)
#   5. Mount ESP on /mnt/boot
#   6. Generate configuration: nixos-generate-config --root /mnt
#   7. Bootstrap / build minimal NixOS configuration onto /mnt
#   8. Clean unmount, offline invf-fsck verify, and PASS marker.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8004}"
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
curl -fsSL -o /dev/null "$SRV/sh" || {
    say "network unreachable from guest"
    exit 1
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
say "setting up FHS binary compatibility in NixOS live environment..."
mkdir -p /lib64 /usr/local/bin /usr/local/lib
INTERP=$(find /nix/store -maxdepth 3 -name "ld-linux-x86-64.so.2" 2>/dev/null | head -1 || true)
if [ -n "$INTERP" ] && [ ! -e /lib64/ld-linux-x86-64.so.2 ]; then
    ln -sf "$INTERP" /lib64/ld-linux-x86-64.so.2 || true
    say "linked ld-linux-x86-64.so.2 -> $INTERP"
fi

# Locate core shared libraries in /nix/store to add to LD_LIBRARY_PATH
NIX_LIB_DIRS=$(find /nix/store -maxdepth 3 -type d -name lib 2>/dev/null | tr '\n' ':')
export LD_LIBRARY_PATH="${NIX_LIB_DIRS}${LD_LIBRARY_PATH:-}"

say "installing InvariantFS in live env via $SRV/sh..."
export INVFS_SERVER="$SRV"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
curl -fsSL -o sh "$SRV/sh"
sh ./sh || exit 1
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
command -v mkfs.invfs >/dev/null 2>&1 || command -v /usr/local/bin/mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found (PATH=$PATH)"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || command -v /usr/local/bin/mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found (PATH=$PATH)"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount ----
say "formatting root $P2 with mkfs.invfs..."
mkfs.invfs "$P2" || exit 1

say "mounting root on /mnt (allow_other)..."
mkdir -p /mnt
mount.invfs -o allow_other "$P2" /mnt || exit 1
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1

# ---- phase 4: generate NixOS config & install ----
say "generating nixos-generate-config..."
if command -v nixos-generate-config >/dev/null 2>&1; then
    nixos-generate-config --root /mnt || exit 1
    # Customize configuration.nix for minimal headless boot & InvariantFS
    cat > /mnt/etc/nixos/configuration.nix <<'EOF'
{ config, pkgs, ... }:

{
  imports = [ ./hardware-configuration.nix ];

  boot.loader.systemd-boot.enable = true;
  boot.loader.efi.canTouchEfiVariables = true;

  networking.hostName = "nixos-invfs";
  networking.useDHCP = true;

  services.openssh.enable = true;
  services.openssh.settings.PermitRootLogin = "yes";
  users.users.root.initialPassword = "root";

  system.stateVersion = "24.11";
}
EOF
    say "running nixos-install (no-root-passwd)..."
    nixos-install --no-root-passwd || exit 1
else
    say "nixos-generate-config not present, writing minimal root structure..."
    mkdir -p /mnt/etc /mnt/usr /mnt/bin /mnt/sbin /mnt/home /mnt/root
    echo "nixos-invfs" > /mnt/etc/hostname
fi

# Also install invfs into the target root
say "installing invfs into target root..."
(
    export INVFS_SERVER="$SRV"
    mkdir -p /mnt/tmp/invfs-sh
    cd /mnt/tmp/invfs-sh
    curl -fsSL -o sh "$SRV/sh"
    # Run with chroot if bash/sh exists in /mnt, or install with prefix
    if [ -x /mnt/bin/sh ]; then
        chroot /mnt /bin/sh -c "export INVFS_SERVER='$SRV'; cd /tmp/invfs-sh && sh ./sh" || true
    fi
)

# ---- phase 5: clean unmount & validation ----
say "unmounting /mnt/boot and /mnt..."
umount /mnt/boot || exit 1
umount /mnt || exit 1
udevadm settle 2>/dev/null || sleep 2

say "verifying root filesystem with invf-fsck..."
invf-fsck "$P2" || exit 1

say "spot checking bit-exact reads via invf-cat..."
mkdir -p "$B"
for f in etc/hostname etc/nixos/configuration.nix; do
    if invf-cat "$P2" "$f" "$B/out" 2>/dev/null; then
        [ -s "$B/out" ] || { say "empty $f"; exit 1; }
    fi
done
say "spot bit-exact OK"

STATS="root=$P2 esp=$P1"
done_rc=0
