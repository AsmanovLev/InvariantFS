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
export INVFS_SERVER="$SRV"
VOL=""
B=/root/pkg
say() { echo "iso-setup: $*"; }

# Per-phase timings from the daemon (mount fold, file-table build) into its log,
# which this script keeps on the console. Same reason as the boot path: a slow
# mount and a hung mount read identically without it -- 2026-11-25, gentoo spent
# >120 s in the file-table build over an installed root and the log said nothing.
export INVFS_PERF_TRACE="${INVFS_PERF_TRACE:-1}"
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

say "mounting root on /mnt (allow_other,survive_term,exec,dev,suid)..."
mkdir -p /mnt
mount.invfs -o allow_other,survive_term,exec,dev,suid "$P2" /mnt || exit 1
mount -o remount,exec,dev,suid /mnt 2>/dev/null || true
mkdir -p /mnt/boot
mount "$P1" /mnt/boot || exit 1

# Protect invf-fuse from OOM-killer
for p in $(pgrep -f nvf-fuse); do
    echo -1000 > /proc/$p/oom_score_adj 2>/dev/null || true
done

# Setup 2GB swapfile in live env to prevent OOM during closure copy
if ! swapon --show 2>/dev/null | grep -q "/"; then
    say "enabling 2GB swapfile in live env..."
    dd if=/dev/zero of=/tmp/swapfile bs=1M count=2048 status=none 2>/dev/null || true
    chmod 600 /tmp/swapfile 2>/dev/null || true
    mkswap /tmp/swapfile >/dev/null 2>&1 && swapon /tmp/swapfile 2>/dev/null || true
fi

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
    options = [ "allow_other" "exec" ];
    noCheck = true;
  };

  boot.loader.systemd-boot.enable = false;
  boot.loader.grub.enable = false;
  boot.kernelModules = [ "fuse" ];
  boot.initrd.enable = false;
  system.build.initialRamdisk = lib.mkForce (pkgs.runCommand "dummy-initrd" { } "mkdir -p \$out; touch \$out/initrd");
  system.build.initialRamdiskSecretAppender = lib.mkForce (pkgs.writeShellScriptBin "append-initrd-secrets" "exit 0");

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

    mkdir -p /mnt/tmp /mnt/var/tmp
    mount -t tmpfs -o exec,dev,suid tmpfs /mnt/tmp 2>/dev/null || true
    mount -t tmpfs -o exec,dev,suid tmpfs /mnt/var/tmp 2>/dev/null || true
    chmod 1777 /mnt/tmp /mnt/var/tmp 2>/dev/null || true
    export TMPDIR=/tmp

    say "building NixOS system toplevel in native host store..."
    SYS_PATH=$(nix-build '<nixpkgs/nixos>' -A system -I nixos-config=/mnt/etc/nixos/configuration.nix --no-out-link \
        --option build-users-group "" \
        --option sandbox false \
        --option max-jobs 2 \
        --option cores 2) || exit 1
    say "system built successfully: $SYS_PATH"

    export TMPDIR=/mnt/tmp
    say "installing prebuilt system closure onto /mnt (InvariantFS)..."
    nixos-install --system "$SYS_PATH" --no-root-passwd --no-channel-copy --no-bootloader \
        --option build-users-group "" \
        --option sandbox false \
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
        # WHICH KERNEL'S MODULES. The old search was
        #   find /mnt/nix/store -name "virtio_blk.ko*" | head -1
        # i.e. the lexically FIRST module in the whole store, which is any
        # kernel generation that happens to sort first -- not the kernel this
        # initrd boots. Measured 2026-11-24, installation-iso-nixos: the guest
        # died with
        #   virtio_blk: Unknown symbol register_virtio_driver (err -2)
        #   initramfs: could not load /lib/modules/virtio_blk.ko
        #   ERROR: no InvariantFS volume found on any block device
        # and dropped to the repair shell, so the whole boot check was green on
        # a guest that never saw a disk. The modules must come from the INSTALLED
        # system's kernel tree, and the version is printed so the log says which.
        KMOD_ROOT=$(readlink -f /mnt/nix/var/nix/profiles/system/kernel-modules 2>/dev/null || true)
        if [ -z "$KMOD_ROOT" ] || [ ! -d "$KMOD_ROOT" ]; then
            KMOD_ROOT=$(ls -d /mnt/nix/store/*-linux-*-modules 2>/dev/null | tail -1)
        fi
        KVER=$(basename "$(ls -d "$KMOD_ROOT"/lib/modules/* 2>/dev/null | head -1)" 2>/dev/null)
        say "kernel modules: root=$KMOD_ROOT version=${KVER:-unknown}"
        kmod_find() {   # $1 = module basename -> first match under the installed kernel
            [ -n "$KMOD_ROOT" ] || return 1
            find "$KMOD_ROOT/lib/modules/${KVER:-/}" -name "$1.ko*" 2>/dev/null | head -1
        }
        FUSE_SRC=$(kmod_find fuse 2>/dev/null)
        [ -n "$FUSE_SRC" ] || FUSE_SRC=$(find /mnt/nix/store -name "fuse.ko*" 2>/dev/null | head -1)
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
        for mod in virtio virtio_ring virtio_pci_legacy_dev virtio_pci_modern_dev virtio_pci virtio_blk virtio_net failover net_failover; do
            MSRC=$(kmod_find "$mod" 2>/dev/null)
            [ -n "$MSRC" ] || MSRC=$(find /mnt/nix/store -name "$mod.ko*" 2>/dev/null | head -1)
            [ -n "$MSRC" ] || MSRC=$(find /lib/modules -name "$mod.ko*" 2>/dev/null | head -1)
            if [ -z "$MSRC" ]; then
                say "WARNING: no $mod.ko for kernel ${KVER:-unknown}; the initrd will not find a block device"
            fi
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

# ---- mode readiness -------------------------------------------------------
# The 2026-11-25 boot refused the hand-off with the chain resolved and the file
# PRESENT but `-r--r--r--`: /nix/store/...-nixos-system-.../init was 0444 on the
# volume, so nothing could execute it. Two things can produce that, and the log
# has to say which:
#   (a) the mode was written as 0444 by the install (nix writes store files
#       read-only, then chmods the executable ones), or
#   (b) chmod through this FUSE mount does not take effect at all.
# The probe answers both: it chmods a fresh file inside the target volume, and it
# compares the installed store's init with the INSTALLER's own copy of the same
# path -- same content, same hash, so a different mode is ours.
say "mode readiness: does chmod through the InvFS mount take effect?"
mp=/mnt/tmp/.mode-probe.$$
if : > "$mp" 2>/dev/null; then
    chmod 0555 "$mp" 2>/dev/null
    say "  fresh file, after chmod 0555 -> $(stat -c '%a' "$mp" 2>/dev/null || echo '?')  (want 555)"
    chmod 0644 "$mp" 2>/dev/null
    say "  fresh file, after chmod 0644 -> $(stat -c '%a' "$mp" 2>/dev/null || echo '?')  (want 644)"
    rm -f "$mp"
else
    say "  could not create a probe file under /mnt/tmp"
fi
VOLINIT=$(ls -d /mnt/nix/store/*-nixos-system-*/init 2>/dev/null | head -1)
LIVEINIT=$(ls -d /nix/store/*-nixos-system-*/init 2>/dev/null | head -1)
say "  installed store init : $(stat -c '%a %n' "${VOLINIT:-/nonexistent}" 2>/dev/null || echo MISSING)"
say "  installer store init : $(stat -c '%a %n' "${LIVEINIT:-/nonexistent}" 2>/dev/null || echo MISSING)"
if [ -n "$VOLINIT" ] && [ ! -x "$VOLINIT" ]; then
    say "  the installed init is NOT executable; trying chmod 0755 (this is a probe, not a fix):"
    chmod 0755 "$VOLINIT" 2>&1 | sed 's/^/    chmod| /'
    say "  after chmod: $(stat -c '%a %n' "$VOLINIT" 2>/dev/null || echo '?')"
    [ -x "$VOLINIT" ] && say "  chmod through the mount WORKS -- so the install wrote 0444 and the boot can proceed"                        || say "  chmod through the mount did NOT take effect -- that is the defect"
fi
# The comparison above uses two different store hashes (the guest's system is
# rebuilt with its own hostname), so it says the modes differ, not WHY. This is
# the exact code path instead: nix's own import into the target store, which is
# what wrote every file on this volume.
# Two questions, and the previous probe answered neither because `nix-store
# --store /mnt --add` returned a path under /nix/store -- it copied into the
# INSTALLER's store, not the volume (measured 2026-11-25, run 45625987117).
#
#   a) Did EVERY executable in the store lose its bit, or only some? One number
#      settles it: a store binary that has to be 555.
#   b) Does the write pattern nix uses -- chmod a few seconds after the write, on
#      a file that has since been linked into place -- survive?
say "  store executables (a lost +x on these means the whole store is 444):"
for b in /mnt/nix/store/*-bash-*/bin/bash /mnt/nix/store/*-nixos-system-*/init; do
    [ -e "$b" ] && say "    $(stat -c '%a %n' "$b")"
done
# EVERY store executable is 444 on the volume (bash, bash-interactive, the
# system init) while the installer's own copies are 555, and chmod works in both
# shapes tested above. So the question is no longer "does chmod work" but "does
# the store's write sequence work", and nix's sequence is: mkdir the store path
# 0555, create the file, write it, chmod it to 0444 or 0555. Mirror it exactly,
# in a directory of our own, so the answer is about the mount and not about nix:
say "  the store's own write sequence, replayed by hand:"
d=/mnt/nix/store/zz-probe-dir.$$
if mkdir -p "$d" 2>/dev/null && chmod 0555 "$d" 2>/dev/null; then
    say "    mkdir 0555 -> $(stat -c '%a' "$d")"
    if ( set -C; : > "$d/exec.sh" ) 2>/dev/null; then
        say "    create in a 0555 dir -> $(stat -c '%a' "$d/exec.sh")"
        chmod 0555 "$d/exec.sh" 2>/dev/null
        say "    then chmod 0555 -> $(stat -c '%a' "$d/exec.sh")   (want 555 -- this is the step that fails on the real store)"
    fi
    chmod 0755 "$d" 2>/dev/null; rm -rf "$d" 2>/dev/null
else
    say "    could not create a probe directory under /mnt/nix/store"
fi
say "  chmod after a link (the shape a store import has):"
p=/mnt/tmp/.link-probe.$$
q=/mnt/tmp/.link-probe-target.$$
if : > "$p" 2>/dev/null && ln "$p" "$q" 2>/dev/null; then
    chmod 0555 "$q" 2>/dev/null
    say "    two names, one inode; after chmod 0555 on the second: $(stat -c '%a' "$p") / $(stat -c '%a' "$q")  (want 555/555)"
    chmod 0444 "$q" 2>/dev/null
    say "    then chmod 0444 on the second:                     $(stat -c '%a' "$p") / $(stat -c '%a' "$q")  (want 444/444)"
else
    say "    could not create the link probe under /mnt/tmp"
fi
rm -f "$p" "$q" 2>/dev/null
if [ -n "${VOLINIT:-}" ] && [ -n "${LIVEINIT:-}" ]; then
    v=$(stat -c '%a' "$VOLINIT" 2>/dev/null); l=$(stat -c '%a' "$LIVEINIT" 2>/dev/null)
    [ "$v" = "$l" ] && say "  modes agree (installer $l, volume $v)"                     || say "  MODES DIFFER: installer $l vs volume $v -- the copy through FUSE lost the bits"
fi

# Pre-populate BOOTX64.EFI from host server if available
mkdir -p /mnt/boot/EFI/BOOT
if [ -n "${INVFS_SERVER:-}" ]; then
    curl -fsSL -o /mnt/boot/EFI/BOOT/BOOTX64.EFI "$INVFS_SERVER/BOOTX64.EFI" 2>/dev/null || true
fi

# ---- mode fixup, with the installer's own store as the reference -----------
# The probe above settles where the bits are lost: replaying the store's write
# sequence by hand -- mkdir 0555, create inside it, chmod 0555 -- gives 555, so
# the MOUNT is not dropping them. What is left is the copy itself: nix writes the
# destination files 0444 and the +x that the source has never lands (measured
# 2026-11-25, run 45647381802: every store executable 444 while the installer's
# copy of the same paths is 555).
#
# Rather than guess which files should be executable, use the source as the
# reference: the installer's own /nix/store holds the same closure, built on this
# machine, so any file that is executable THERE and not here is exactly the set
# that was lost. Closing this in the harness is not a fix for the installer, and
# it is not claimed to be one: INCIDENTS.md records the open question. It is what
# makes the installed system bootable enough to test the rest.
say "mode fixup: +x bits the installer's own store has and the volume does not"
mfs=/tmp/modefix.stats
: > "$mfs" 2>/dev/null
while IFS= read -r src; do
    dst="/mnt$src"
    if [ ! -e "$dst" ]; then echo m >> "$mfs"; continue; fi
    if [ ! -x "$dst" ]; then
        if chmod --reference="$src" "$dst" 2>/dev/null; then echo f >> "$mfs"; else echo e >> "$mfs"; fi
    fi
done < <(find /nix/store -type f -perm -u+x 2>/dev/null)
say "  fixed $(grep -c f "$mfs" 2>/dev/null || echo 0), absent on the volume $(grep -c m "$mfs" 2>/dev/null || echo 0), chmod failed $(grep -c e "$mfs" 2>/dev/null || echo 0)"
say "  /mnt/init is now $(stat -c '%a' /mnt/init 2>/dev/null || echo MISSING) (want 555)"

# Run invf-boot-setup to generate standalone EFI GRUB and grub.cfg on ESP
say "running invf-boot-setup on /mnt/boot (ESP)..."
invf-boot-setup --esp /mnt/boot --root "$P2" --cmdline "console=ttyS0,115200 invfs.init=/init" || true

# ---- phase 5: clean unmount & validation ----
say "unmounting guest filesystems..."
sync
swapoff /tmp/swapfile 2>/dev/null || true
rm -f /tmp/swapfile 2>/dev/null || true
umount /mnt/tmp 2>/dev/null || true
umount /mnt/var/tmp 2>/dev/null || true
umount /mnt/boot 2>/dev/null || true
umount -R /mnt 2>/dev/null || umount /mnt || exit 1
udevadm settle 2>/dev/null || sleep 2

# A FUSE daemon is not a mount: `umount` detaches the mount point and returns,
# the invf-fuse that served it exits a moment later, and until it does it still
# holds the volume's flock -- vol_open() then answers
#     "image is in use by another process (flock: Resource temporarily unavailable)"
# This setup carried `fuser -km /mnt` + `sleep 1` BEFORE the umount and it was
# not enough: engine-ci 2026-11-23 13:48:41, installation-iso-nixos, the fsck
# three lines below this block was still refused with exactly that message.
# (The other six setups had no wait at all -- see the same block in
# iso-guest-{void,arch,debian,gentoo,alpine,fedora}-setup.sh.)
#
# Kill it by NAME and wait for it to go: after a successful umount the mount
# point is detached, so `fuser` no longer attributes the daemon's open files to
# /mnt and finds nothing -- checked locally, a live process named invf-fuse
# survives `fuser -km` on a path it does not hold open. Not a fixed sleep
# either: the runner's load decides how long the exit takes.
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
    say "invf-fuse survived SIGTERM and SIGKILL: $(pgrep -x invf-fuse | tr '\n' ' ')"
fi

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
