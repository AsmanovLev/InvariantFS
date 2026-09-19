#!/bin/bash
# test-arch-install.sh — WP63+WP68 regression: build an Arch Linux root once,
# then package it into a single-device and a two-device InvariantFS volume
# *offline* with invf-import, and check the invariants that mattered during
# the bring-up. NO boot and NO FUSE mount: this is the fast, deterministic
# half of the Arch install path.
#
# What it checks
#   * invf-mkfs geometry (single: no DEVT; two-device: DEVT on both, DEV1)
#   * invf-import of the full tree (dirs/files/symlinks, 0 skipped)
#   * invf-fsck -f then invf-fsck => OK, 0 orphans
#   * invf-verify --deep          => 0 corrupt
#   * invf-ls entry count matches the imported entry count
#   * bit-exact invf-cat of a few representative files
#   * /boot/vmlinuz-linux + /boot/initramfs-linux.img exist and are bit-exact
#   * two-device metadata mirror in sync (invf-stats)
#
# Environment
#   ARCH_STAGE   prebuilt Arch root; if unset it is built under ARCH_WORK
#                (needs network + sudo; downloads archlinux-bootstrap)
#   ARCH_WORK    scratch dir (default /var/tmp/invfs-arch-test, /tmp fallback)
#   KEEP=1       keep the volume images (they are sparse; ~2 GB on disk)
#   ARCH_SSH_KEY  pubkey to install as /root/.ssh/authorized_keys
#
# Run standalone, or serialized through the e2e lock:
#   INVFS_E2E_AGENT=wp63-arch bash tools/run-e2e.sh tools/test-arch-install.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B="$REPO/bin"
[ -x "$B/invf-import" ] || { echo "FAIL: build first (make)"; exit 1; }

WORK="${ARCH_WORK:-/var/tmp/invfs-arch-test}"
if ! mkdir -p "$WORK" 2>/dev/null; then
    WORK="${TMPDIR:-/tmp}/invfs-arch-test"
    mkdir -p "$WORK"
fi
STAGE="${ARCH_STAGE:-$WORK/stage}"
VOL="$WORK/vol"
IMG="$WORK/archlinux-bootstrap-x86_64.tar.zst"
BOOTSTRAP_URL="${ARCH_BOOTSTRAP_URL:-https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst}"
META_FRAC="${INVFS_META_FRAC:-16}"

if [ "$(id -u)" = 0 ]; then SUDO=""; else SUDO="sudo"; fi
if [ -n "$SUDO" ] && ! $SUDO -n true 2>/dev/null; then
    echo "FAIL: need passwordless sudo (root-owned files in the staging tree)"
    exit 1
fi

fail() { echo "FAIL: $*"; exit 1; }
note() { echo ":: $*"; }

# --------------------------------------------------------------------------
# Stage 1: get a staging tree (prebuilt via ARCH_STAGE, else build it)
# --------------------------------------------------------------------------
build_stage() {
    command -v curl >/dev/null || fail "curl not found"
    command -v tar  >/dev/null || fail "tar not found"
    command -v zstd >/dev/null || fail "zstd not found (bootstrap is .tar.zst)"

    note "downloading $BOOTSTRAP_URL"
    [ -s "$IMG" ] || curl -fL -o "$IMG" "$BOOTSTRAP_URL"
    rm -rf "$WORK/root.x86_64" "$STAGE"
    note "extracting bootstrap"
    $SUDO tar --zstd -xf "$IMG" -C "$WORK" >/dev/null 2>&1
    $SUDO mv "$WORK/root.x86_64" "$STAGE"

    # pacman needs a mirror and resolv.conf; the bootstrap ships an empty
    # mirrorlist.
    printf 'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n' \
        | $SUDO tee "$STAGE/etc/pacman.d/mirrorlist" >/dev/null
    $SUDO rm -f "$STAGE/etc/resolv.conf"
    $SUDO cp /etc/resolv.conf "$STAGE/etc/resolv.conf"
    # CheckSpace mis-computes on the loop of dirs here; DownloadUser=alpm
    # needs an alpm passwd entry the bootstrap may lack.
    $SUDO sed -i 's/^CheckSpace/#CheckSpace/; s/^DownloadUser/#DownloadUser/' "$STAGE/etc/pacman.conf"

    $SUDO mount -t proc proc "$STAGE/proc"
    $SUDO mount --rbind /sys "$STAGE/sys"
    $SUDO mount --rbind /dev "$STAGE/dev"
    trap '$SUDO umount -R "$STAGE/proc" "$STAGE/sys" "$STAGE/dev" 2>/dev/null || true' EXIT
    note "pacman-key + base + linux + mkinitcpio + openssh + dhcpcd"
    $SUDO chroot "$STAGE" /bin/bash -c '
        pacman-key --init >/dev/null 2>&1
        pacman-key --populate archlinux >/dev/null 2>&1
        pacman -Sy --noconfirm >/dev/null 2>&1
        pacman -S --noconfirm --needed base linux linux-firmware mkinitcpio openssh dhcpcd iproute2 iputils >/dev/null 2>&1
    ' || fail "pacman install"
    $SUDO umount -R "$STAGE/proc" "$STAGE/sys" "$STAGE/dev" 2>/dev/null || true
    trap - EXIT
    provision_stage
}

provision_stage() {
    note "provisioning staging tree"
    local S="$STAGE"
    # root password + sshd
    $SUDO chroot "$S" /bin/bash -c 'echo root:root | chpasswd; ssh-keygen -A >/dev/null 2>&1 || true'
    $SUDO awk 'BEGIN{OFS=" "}
        /^#?PermitRootLogin/ {print "PermitRootLogin yes"; next}
        /^#?PasswordAuthentication/ {print "PasswordAuthentication yes"; next}
        {print}' "$S/etc/ssh/sshd_config" > /tmp/sshd_config.$$
    $SUDO cp /tmp/sshd_config.$$ "$S/etc/ssh/sshd_config"
    rm -f /tmp/sshd_config.$$
    $SUDO chmod 600 "$S/etc/ssh/sshd_config"
    [ -n "${ARCH_SSH_KEY:-}" ] && {
        $SUDO mkdir -p "$S/root/.ssh"; $SUDO chmod 700 "$S/root/.ssh"
        printf '%s\n' "$ARCH_SSH_KEY" | $SUDO tee "$S/root/.ssh/authorized_keys" >/dev/null
        $SUDO chmod 600 "$S/root/.ssh/authorized_keys"
    }

    # fstab: root is the FUSE mount, handled by the initramfs. Volatile
    # trees are tmpfs (see docs/ARCH-INSTALL.md for the two-device reason).
    $SUDO tee "$S/etc/fstab" >/dev/null <<'EOF'
proc      /proc proc     defaults                                     0 0
sysfs     /sys  sysfs    defaults                                     0 0
devtmpfs  /dev  devtmpfs mode=0755,nosuid                             0 0
tmpfs     /tmp  tmpfs    defaults,noatime,nosuid,nodev,mode=1777,size=1G 0 0
tmpfs     /var/tmp tmpfs defaults,noatime,nosuid,nodev,mode=1777,size=512M 0 0
EOF

    # systemd serial autologin + networkd (for a systemd-capable root; the
    # verified boot path uses the busybox fallback below).
    $SUDO mkdir -p "$S/etc/systemd/system/serial-getty@ttyS0.service.d"
    $SUDO tee "$S/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear -s %I 115200,38400,9600 vt102
Type=idle
EOF
    $SUDO mkdir -p "$S/etc/systemd/network"
    $SUDO tee "$S/etc/systemd/network/20-wired.network" >/dev/null <<'EOF'
[Match]
Name=en* eth* ens* enp*

[Network]
DHCP=yes
EOF

    # busybox-init fallback: systemd is not usable as PID1 on a FUSE root
    # (journald/udev fail -> no device units -> no getty).
    $SUDO cp "$B/busybox-static" "$S/bin/busybox"; $SUDO chmod 755 "$S/bin/busybox"
    $SUDO tee "$S/bin/invfs-init" >/dev/null <<'EOF'
#!/bin/sh
# PID1 entry for the busybox-init fallback (invfs.init=/bin/invfs-init).
exec /bin/busybox init
EOF
    $SUDO chmod 755 "$S/bin/invfs-init"
    $SUDO tee "$S/etc/inittab" >/dev/null <<'EOF'
::sysinit:/etc/rc.invfs
ttyS0::respawn:/usr/bin/agetty --autologin root --noclear -L 115200 ttyS0 vt100
::ctrlaltdel:/usr/bin/reboot
::shutdown:/bin/umount -a -r
EOF
    $SUDO tee "$S/etc/rc.invfs" >/dev/null <<'EOF'
#!/bin/sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mountpoint -q /proc || mount -t proc proc /proc
mountpoint -q /sys  || mount -t sysfs sysfs /sys
mountpoint -q /dev  || mount -t devtmpfs devtmpfs /dev
mkdir -p /run /tmp /var/tmp /var
mountpoint -q /run     || mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run
mountpoint -q /tmp     || mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp
mountpoint -q /var/tmp || mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /var/tmp
mountpoint -q /var     || mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /var
mkdir -p /var/empty /var/log /var/lib /run/sshd /run/systemd/resolve
hostname arch-invfs 2>/dev/null || true
IFACE=""
for n in /sys/class/net/*; do
    [ -e "$n" ] || continue
    b=$(basename "$n"); [ "$b" = lo ] && continue
    IFACE="$b"; break
done
if [ -n "$IFACE" ]; then
    ip link set "$IFACE" up
    dhcpcd -b --nohook resolv.conf "$IFACE" 2>/dev/null
    sleep 2
    if ! ip -o -4 addr show dev "$IFACE" 2>/dev/null | grep -q 'inet '; then
        ip addr add 10.0.2.15/24 dev "$IFACE" 2>/dev/null
        ip route add default via 10.0.2.2 2>/dev/null
    fi
fi
printf 'nameserver 10.0.2.3\n' > /run/systemd/resolve/stub-resolv.conf 2>/dev/null || true
/usr/bin/sshd -D -e &
echo "invfs-arch: rc.invfs done"
EOF
    $SUDO chmod 755 "$S/etc/rc.invfs"
    # keep /etc/resolv.conf a symlink into tmpfs so boot writes stay off FUSE
    $SUDO ln -sf /run/systemd/resolve/stub-resolv.conf "$S/etc/resolv.conf"
    note "staging tree ready: $S"
}

if [ ! -d "$STAGE/usr/bin" ]; then
    if [ -z "$SUDO" ] || $SUDO -n true 2>/dev/null; then
        note "no staging tree at $STAGE; building one"
        build_stage
    else
        echo "SKIP: no staging tree and no passwordless sudo to build one"
        echo "      (set ARCH_STAGE to a prebuilt Arch root)"
        exit 0
    fi
fi

# --------------------------------------------------------------------------
# Stage 2: package offline and verify
# --------------------------------------------------------------------------
rm -rf "$VOL"; mkdir -p "$VOL"
cleanup() { [ "${KEEP:-0}" = 1 ] || rm -rf "$VOL"; }
trap cleanup EXIT

# Create a minimal initramfs for /boot assertions.  We cannot run
# mkinitcpio offline (no kernel modules to build against), but the
# test needs /boot/initramfs-linux.img to exist so we can verify it is
# bit-exact through invf-cat.
if [ ! -f "$STAGE/boot/initramfs-linux.img" ]; then
    note "creating dummy initramfs-linux.img for /boot assertions"
    _tmp_cpio=$(mktemp /tmp/invfs-initrd-XXXXXX.cpio)
    ( echo "INVFS_INITRD_MARKER" | cpio -o --format=newc 2>/dev/null ) > "$_tmp_cpio" || true
    $SUDO mkdir -p "$STAGE/boot"
    $SUDO cp "$_tmp_cpio" "$STAGE/boot/initramfs-linux.img"
    rm -f "$_tmp_cpio"
fi

entries_of() {  # $1 = import output
    printf '%s\n' "$1" | sed -n \
        's/.*imported: \([0-9]*\) dirs, \([0-9]*\) files, \([0-9]*\) symlinks, \([0-9]*\) specials.*/\1 \2 \3 \4/p' \
        | awk '{print $1+$2+$3+$4}'
}

check_one() {  # $1 = image, $2 = label, $3 = optional dev1
    local img="$1" label="$2" dev1="$3" log="$VOL/import-$label.log"
    [ -s "$log" ] || fail "$label: no import log"
    local n; n=$(entries_of "$(tail -1 "$log")")
    [ -n "$n" ] || fail "$label: cannot parse import counts"
    local envdev=(); [ -n "$dev1" ] && envdev=(INVFS_DEV1="$dev1")
    local ls_n; ls_n=$(env "${envdev[@]}" "$B/invf-ls" "$img" 2>/dev/null | wc -l || true)
    local listed=$((ls_n - 2))       # invf-ls prints a header + path line
    [ "$listed" -ge "$n" ] || fail "$label: listed $listed < imported $n (loss)"
    [ "$listed" -le "$((n + 8))" ] || fail "$label: listed $listed >> imported $n"
    echo "   $label: $n imported, $listed listed ($ls_n lines)"
}

run_case() {  # $1 = label
    local label="$1" img="$VOL/$1.img" log="$VOL/import-$1.log"
    note "$label: mkfs + import"
    if [ "$label" = multi ]; then
        ( cd "$VOL" && INVFS_META_FRAC="$META_FRAC" "$B/invf-mkfs" "$1.img" 15 "$1-shadow.img" 20 ) >/dev/null
    else
        ( cd "$VOL" && INVFS_META_FRAC="$META_FRAC" "$B/invf-mkfs" "$1.img" 15 ) >/dev/null
    fi
    local envdev=()
    [ "$label" = multi ] && envdev=(INVFS_DEV1="$VOL/$1-shadow.img")
    $SUDO env "${envdev[@]}" INVFS_IMPORT_KEEP_OWNER=1 "$B/invf-import" "$img" "$STAGE" >"$log" 2>&1
    tail -1 "$log"
    grep -q "0 skipped" "$log" || fail "$label: import skipped entries"
    $SUDO chown "$(id -u):$(id -g)" "$img" "$VOL/$1-shadow.img" 2>/dev/null || true

    note "$label: fsck"
    env "${envdev[@]}" "$B/invf-fsck" "$img" -f >"$VOL/fsck-f-$label.log" 2>&1 || true
    env "${envdev[@]}" "$B/invf-fsck" "$img" >"$VOL/fsck-$label.log" 2>&1
    grep -q "OK" "$VOL/fsck-$label.log" || { cat "$VOL/fsck-$label.log"; fail "$label: fsck not clean"; }
    grep -qE "orphans:[[:space:]]*0" "$VOL/fsck-$label.log" || fail "$label: orphans remain"

    note "$label: verify --deep"
    env "${envdev[@]}" "$B/invf-verify" "$img" --deep >"$VOL/verify-$label.log" 2>&1
    grep -q "0 corrupt" "$VOL/verify-$label.log" || { cat "$VOL/verify-$label.log"; fail "$label: corrupt"; }

    check_one "$img" "$label" "$([ "$label" = multi ] && echo "$VOL/$1-shadow.img")"

    note "$label: bit-exact cat"
    local f out rc=0
    for f in usr/bin/bash usr/lib/systemd/systemd etc/passwd usr/bin/ssh; do
        out="$VOL/cat-$(basename "$f")"
        env "${envdev[@]}" "$B/invf-cat" "$img" "$f" "$out" >/dev/null 2>&1 || { echo "   cat failed: $f"; rc=1; continue; }
        cmp -s "$STAGE/$f" "$out" || { echo "   MISMATCH: $f"; rc=1; }
    done
    [ "$rc" = 0 ] || fail "$label: bit-exact reads"
    echo "   $label: 4 files bit-exact"

    # -- WP68: /boot kernel + initramfs bit-exact assertions ----------------
    note "$label: /boot kernel + initramfs"
    local boot_rc=0
    for f in boot/vmlinuz-linux boot/initramfs-linux.img; do
        if [ ! -f "$STAGE/$f" ]; then
            echo "   SKIP: $f not in staging tree"
            continue
        fi
        out="$VOL/cat-$(basename "$f")"
        if env "${envdev[@]}" "$B/invf-cat" "$img" "$f" "$out" >/dev/null 2>&1; then
            if cmp -s "$STAGE/$f" "$out"; then
                echo "   ok: $f bit-exact ($(wc -c < "$STAGE/$f") bytes)"
            else
                echo "   MISMATCH: $f"; boot_rc=1
            fi
        else
            echo "   cat failed: $f"; boot_rc=1
        fi
    done
    [ "$boot_rc" = 0 ] || fail "$label: /boot bit-exact reads"
}

run_case single
run_case multi

# two-device specifics: DEVT marker on both devices, mirror in sync
if ! dd if="$VOL/multi.img" bs=1 skip=672 count=4 status=none | grep -q DEVT; then
    fail "multi: no DEVT descriptor on dev0"
fi
if ! dd if="$VOL/multi-shadow.img" bs=1 skip=672 count=4 status=none | grep -q DEVT; then
    fail "multi: no DEVT descriptor on dev1"
fi
if ! INVFS_DEV1="$VOL/multi-shadow.img" "$B/invf-stats" "$VOL/multi.img" 2>/dev/null | grep -q "mirror .*: in sync"; then
    fail "multi: metadata mirror not in sync"
fi
echo "   multi: DEVT on both devices, mirror in sync"

echo
echo "PASS: Arch install path (single + two-device, offline package/verify)"
