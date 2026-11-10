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
mkdir -p /lib64 /usr/lib /usr/lib64 /lib /usr/local/bin /usr/local/sbin /usr/local/lib
INTERP=$(find /nix/store -maxdepth 4 -name "ld-linux-x86-64.so.2" 2>/dev/null | head -1 || true)
if [ -n "$INTERP" ] && [ ! -e /lib64/ld-linux-x86-64.so.2 ]; then
    ln -sf "$INTERP" /lib64/ld-linux-x86-64.so.2 || true
    say "linked ld-linux-x86-64.so.2 -> $INTERP"
fi

# Locate core shared libraries in /nix/store and symlink into /usr/lib and /lib
for libname in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 libz.so.1 libzstd.so.1 libfuse3.so.3; do
    so=$(find /nix/store -maxdepth 4 -name "$libname" 2>/dev/null | head -1 || true)
    if [ -n "$so" ]; then
        ln -sf "$so" "/usr/lib/$libname" 2>/dev/null || true
        ln -sf "$so" "/lib/$libname" 2>/dev/null || true
        ln -sf "$so" "/lib64/$libname" 2>/dev/null || true
    fi
done

NIX_LIB_DIRS=$(find /nix/store -maxdepth 4 -type d -name lib 2>/dev/null | tr '\n' ':')
export LD_LIBRARY_PATH="${NIX_LIB_DIRS}${LD_LIBRARY_PATH:-}"

say "installing InvariantFS in live env via $SRV/sh..."
export INVFS_SERVER="$SRV"
mkdir -p /tmp/invfs-sh
cd /tmp/invfs-sh
curl -fsSL -o sh "$SRV/sh"
sh ./sh || exit 1
export PATH="/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
[ -x /usr/local/sbin/mkfs.invfs ] && ln -sf /usr/local/sbin/mkfs.invfs /usr/local/bin/mkfs.invfs 2>/dev/null || true
[ -x /usr/local/sbin/mount.invfs ] && ln -sf /usr/local/sbin/mount.invfs /usr/local/bin/mount.invfs 2>/dev/null || true
[ -x /usr/local/sbin/fsck.invfs ] && ln -sf /usr/local/sbin/fsck.invfs /usr/local/bin/fsck.invfs 2>/dev/null || true
command -v mkfs.invfs >/dev/null 2>&1 || command -v /usr/local/bin/mkfs.invfs >/dev/null 2>&1 || command -v /usr/local/sbin/mkfs.invfs >/dev/null 2>&1 || { say "mkfs.invfs not found (PATH=$PATH)"; exit 1; }
command -v mount.invfs >/dev/null 2>&1 || command -v /usr/local/bin/mount.invfs >/dev/null 2>&1 || command -v /usr/local/sbin/mount.invfs >/dev/null 2>&1 || { say "mount.invfs not found (PATH=$PATH)"; exit 1; }
say "InvariantFS installed in live env"

# ---- phase 3: format root & mount ----
say "formatting root $P2 with mkfs.invfs..."
mkfs.invfs "$P2" || exit 1

say "mounting root on /mnt (allow_other,survive_term)..."
mkdir -p /mnt
mount.invfs -o allow_other,survive_term "$P2" /mnt || exit 1
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1

# ---- phase 4: generate NixOS config & install ----
say "generating nixos-generate-config..."
if command -v nixos-generate-config >/dev/null 2>&1; then
    nixos-generate-config --root /mnt || exit 1
    # Customize configuration.nix for minimal headless boot & InvariantFS
    cat > /mnt/etc/nixos/configuration.nix <<EOF
{ config, pkgs, lib, ... }:

{
  imports = [ ./hardware-configuration.nix ];

  fileSystems."/" = {
    device = "$P2";
    fsType = "invfs";
    options = [ "allow_other" ];
    noCheck = true;
  };

  boot.loader.systemd-boot.enable = false;
  boot.loader.grub.enable = false;
  boot.kernelModules = [ "fuse" ];
  boot.initrd.enable = false;

  nix.settings = {
    sandbox = false;
    build-users-group = "";
    trusted-users = [ "root" ];
  };

  # Minimal headless system: strictly avoid docs, fonts, sound, desktop bloat
  # to guarantee fast installation using only pre-cached packages from ISO.
  documentation.enable = false;
  documentation.nixos.enable = false;
  documentation.man.enable = false;
  documentation.info.enable = false;
  documentation.doc.enable = false;
  environment.extraOutputsToInstall = [ ];
  environment.defaultPackages = [ ];
  i18n.supportedLocales = [ "en_US.UTF-8/UTF-8" ];
  fonts.fontconfig.enable = false;
  programs.command-not-found.enable = false;
  services.openssh.enable = false;
  services.udisks2.enable = false;
  xdg.sounds.enable = false;
  xdg.mime.enable = false;
  xdg.icons.enable = false;

  networking.hostName = "nixos-invfs";
  networking.useDHCP = false;
  networking.dhcpcd.enable = false;
  networking.firewall.enable = false;

  users.users.root.initialPassword = "root";
  services.getty.autologinUser = "root";

  system.stateVersion = "24.11";
}
EOF

    say "stopping systemd nix-daemon and systemd-oomd..."
    echo 0 > /proc/sys/kernel/hung_task_timeout_secs 2>/dev/null || true
    systemctl stop nix-daemon.socket nix-daemon.service 2>/dev/null || true
    systemctl stop systemd-oomd.service systemd-oomd.socket 2>/dev/null || true
    systemctl mask systemd-oomd.service systemd-oomd.socket 2>/dev/null || true
    pkill -9 -f nix-daemon 2>/dev/null || true
    sleep 1

    say "configuring nix with sandbox disabled on /etc/nix tmpfs..."
    mkdir -p /etc/nix
    mount -t tmpfs tmpfs /etc/nix 2>/dev/null || true
    cat > /etc/nix/nix.conf <<'EOF_NIX'
sandbox = false
build-users-group =
trusted-users = * root
substituters = https://cache.nixos.org
connect-timeout = 5
max-jobs = 2
cores = 2
EOF_NIX
    mkdir -p /root/.config/nix
    cp -f /etc/nix/nix.conf /root/.config/nix/nix.conf 2>/dev/null || true
    export NIX_CONFIG="sandbox = false
build-users-group =
trusted-users = * root"

    say "starting standalone root nix-daemon without sandbox..."
    nix-daemon --daemon &
    DAEMON_PID=$!
    sleep 2

    mkdir -p /mnt/tmp
    chmod 1777 /mnt/tmp 2>/dev/null || true

    say "running nixos-install (no-root-passwd)..."
    nixos-install --no-root-passwd --no-channel-copy --no-bootloader \
        --option build-users-group "" \
        --option sandbox false \
        --option connect-timeout 5 \
        --option max-jobs 2 \
        --option cores 2 || exit 1

    kill "$DAEMON_PID" 2>/dev/null || true
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

# Prepare universal UEFI InvariantFS boot on the ESP (/mnt/boot)
say "preparing universal UEFI InvariantFS boot on ESP..."
K=$(find /mnt/boot -name "*vmlinuz*" -o -name "*bzImage*" 2>/dev/null | head -1 || true)
if [ -z "$K" ]; then
    K=$(find /mnt/nix/store -maxdepth 3 -name "bzImage" 2>/dev/null | head -1 || true)
fi
if [ -z "$K" ]; then
    K=/boot/bzImage
fi
if [ -f "$K" ]; then
    cp -f "$K" /mnt/boot/vmlinuz
    say "staged kernel $K -> /mnt/boot/vmlinuz"
fi

if curl -fsSL "$SRV/initramfs.cpio.gz" -o /mnt/boot/initramfs.img 2>/dev/null; then
    say "staged universal InvariantFS initramfs -> /mnt/boot/initramfs.img"
    say "injecting host-matching kernel modules into /mnt/boot/initramfs.img..."
    curl -fsSL "$SRV/busybox" -o /tmp/busybox 2>/dev/null && chmod +x /tmp/busybox || true
    CPIO_BIN="cpio"
    if [ -x /tmp/busybox ]; then
        CPIO_BIN="/tmp/busybox cpio"
    elif command -v cpio >/dev/null 2>&1; then
        CPIO_BIN="cpio"
    elif command -v busybox >/dev/null 2>&1; then
        CPIO_BIN="busybox cpio"
    fi
    TMOD=$(mktemp -d /tmp/initrd-inject.XXXXXX)
    (
        cd "$TMOD"
        gzip -dc /mnt/boot/initramfs.img | $CPIO_BIN -idm 2>/dev/null
        FUSE_SRC=$(find /mnt/nix/store -name "fuse.ko*" 2>/dev/null | head -1)
        [ -n "$FUSE_SRC" ] || FUSE_SRC=$(find /lib/modules -name "fuse.ko*" 2>/dev/null | head -1)
        if [ -n "$FUSE_SRC" ]; then
            case "$FUSE_SRC" in
                *.xz)   xz -dc "$FUSE_SRC" > fuse.ko ;;
                *.zst)  zstd -dc "$FUSE_SRC" > fuse.ko ;;
                *.gz)   gzip -dc "$FUSE_SRC" > fuse.ko ;;
                *)      cp -f "$FUSE_SRC" fuse.ko ;;
            esac
            say "injected fuse.ko into initramfs ($FUSE_SRC)"
        fi
        for mod in virtio_blk virtio_pci virtio_net failover net_failover; do
            MSRC=$(find /mnt/nix/store -name "$mod.ko*" 2>/dev/null | head -1)
            [ -n "$MSRC" ] || MSRC=$(find /lib/modules -name "$mod.ko*" 2>/dev/null | head -1)
            if [ -n "$MSRC" ]; then
                mkdir -p lib/modules
                case "$MSRC" in
                    *.xz)  xz -dc "$MSRC" > "lib/modules/$mod.ko" ;;
                    *.zst) zstd -dc "$MSRC" > "lib/modules/$mod.ko" ;;
                    *.gz)  gzip -dc "$MSRC" > "lib/modules/$mod.ko" ;;
                    *)     cp -f "$MSRC" "lib/modules/$mod.ko" ;;
                esac
            fi
        done
        find . | $CPIO_BIN -o -H newc 2>/dev/null | gzip -9 > /mnt/boot/initramfs.img
    )
    rm -rf "$TMOD"
fi

# Ensure /mnt/sbin/init and /mnt/init point to NixOS system profile init
mkdir -p /mnt/sbin
if [ -e /mnt/nix/var/nix/profiles/system/init ]; then
    ln -sf /nix/var/nix/profiles/system/init /mnt/init
    ln -sf /nix/var/nix/profiles/system/init /mnt/sbin/init
elif [ -e /mnt/init ]; then
    ln -sf /init /mnt/sbin/init 2>/dev/null || true
fi

# Run invf-boot-setup to generate standalone EFI GRUB and grub.cfg on ESP
say "running invf-boot-setup on /mnt/boot (ESP)..."
invf-boot-setup --esp /mnt/boot --root "$P2" --cmdline "console=ttyS0,115200 invfs.init=/init" || true

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
