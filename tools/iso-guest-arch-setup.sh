#!/bin/bash
# iso-guest-arch-setup.sh -- runs INSIDE a stock Arch live env.
#
# Two front ends, one script (no $0 tricks, no prompts, config from env):
#   archiso hook:  script=http://10.0.2.2:8000/iso-guest-arch-setup.sh
#   by hand / TTY: curl -fsS http://10.0.2.2:8000/iso-guest-arch-setup.sh | bash
#                  (or download whole, verify out-of-band, then run)
# SRV and VOL override the defaults below when exported first.
#
# Realistic install: makepkg the recipe + pacman -U (not loose binaries),
# then image with the INSTALLED tools. All output to serial; the host
# greps for the INVFS-ISO-SETUP marker. Ends by powering the guest off.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8000}"
VOL=""   # resolved in phase 2: invfsvol= cmdline, else only writable disk
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
    # Graceful first (flushes the imaged volume), forced fallback: a bare
    # `systemctl poweroff` has hung from this hook, leaving a stale guest
    # holding the volume lock and confusing the next attempt.
    timeout 30 systemctl poweroff || poweroff -f
}
trap 'done_rc=$?; finish' EXIT

# ---- phase 1: build + install the package, like a user would ----
# Watchdog: heartbeat + memory to console every 60 s. If the guest dies
# silently (OOM takes the shell: no trap, no marker), the serial shows
# exactly when the heart stopped and what pressure preceded it.
( while true; do
    echo "iso-watch: mem=$(free -m | awk '/^Mem/{print $3"/"$2"MB"}') load=$(cut -d' ' -f1 /proc/loadavg)"
    sleep 60
  done ) &
id builder >/dev/null 2>&1 || useradd -m builder
mkdir -p "$B"
for f in PKGBUILD invfs-v0.6.0-x86_64.tar.zst SHA256SUMS; do
    curl -fsS -o "$B/$f" "$SRV/$f" || exit 1
done
say "recipe + artifact fetched"
# Provenance FIRST: the artifact must match the served checksums before
# anything trusts it. (The published chain adds a signature on top;
# test staging cannot sign, so the checksum file is the root here.)
( cd "$B" && sha256sum -c SHA256SUMS ) || exit 1
say "artifact matches SHA256SUMS"
# Test-staging pin: the recipe pins the PUBLISHED hash, but this artifact
# was built from HEAD minutes ago. Re-pin this COPY to the verified hash so
# makepkg enforces a real (matching) checksum instead of --skipchecksums.
H=$(cut -d' ' -f1 < "$B/SHA256SUMS")
sed -i "s/'[0-9a-f]\{64\}'/'$H'/" "$B/PKGBUILD"
grep -q "'  " "$B/PKGBUILD" && { say "PKGBUILD sums array has a non-hash entry"; exit 1; }
# Build tooling, minimal: makepkg needs fakeroot (ownership) but, with
# options=('!strip' '!debug') in the recipe, no binutils/base-devel. The live
# ISO root is a RAM overlay -- a full base-devel does not fit next to the
# pacstrap target.
# Rolling-release race first: the ISO's keyring predates the mirror's
# packages, so a fresh package can arrive signed by a key the ISO never
# saw ('unknown trust', CI archiso-validation). Refresh from the mirror
# before trusting it for anything -- including the fakeroot install below
# and pacstrap later (both -Sy against the same mirror).
# (The keyring package is signed by master keys the ISO already trusts.)
pacman -Sy --needed --noconfirm archlinux-keyring || exit 1
# --populate needs a local secret key; a current keyring package SKIPS the
# install that used to create it implicitly, exposing an uninitialized
# gnupg home ('no secret key ... use pacman-key --init'). Init explicitly;
# virtio-rng (driver) feeds the keygen under TCG so it can't stall on
# entropy.
pacman-key --init || exit 1
pacman-key --populate archlinux || exit 1
command -v fakeroot >/dev/null 2>&1 || pacman -Sy --needed --noconfirm fakeroot || exit 1
chown -R builder:builder "$B"
su builder -c "cd '$B' && makepkg --noconfirm" || exit 1
PKG=$(ls "$B"/invfs-*.pkg.tar.zst | head -1)
pacman -U --noconfirm "$PKG" || exit 1
pacman -Q invfs || exit 1
pacman -Qk invfs 2>&1 | tail -2
say "installed: $(pacman -Q invfs)"

# ---- phase 2: image with the INSTALLED tools ----
# The volume disk: named on the kernel cmdline (invfsvol=/dev/vda) so the
# recipe never guesses wrong; falls back to the only writable disk (the
# ISO itself is read-only).
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
if [ -z "$VOL" ]; then
    VOL=$(lsblk -dn -o NAME,RO,TYPE | awk '$2=="0" && $3=="disk" {print "/dev/"$1}' | head -1)
fi
[ -b "${VOL:-none}" ] || { say "no volume disk (cmdline: $(tr ' ' '\n' < /proc/cmdline | grep -o 'invfsvol=[^ ]*' || echo none); disks: $(lsblk -dn -o NAME,RO | tr '\n' ' '))"; exit 1; }
say "pacstrap base + linux to /mnt (native Arch install)"
# openssh/dhcpcd/iproute2/iputils: not in base, required for the
# post-install BOOT phase (rc.invfs runs dhcpcd + sshd; boot-arch
# asserts over SSH). linux-firmware stays out (no hardware to init).
pacstrap /mnt base linux openssh dhcpcd iproute2 iputils || exit 1
say "pacstrap done: $(du -sh /mnt | cut -f1) in /mnt"
# ---- boot provisioning: make the imaged root bootable the same way
# test-arch-install.sh does (busybox-init: systemd is not usable as PID1
# on a FUSE root). Sourced from there -- keep the two in sync.
BB=$B/busybox-static
curl -fsS -o "$BB" "$SRV/busybox-static" || exit 1
cp "$BB" /mnt/bin/busybox && chmod 755 /mnt/bin/busybox
cat > /mnt/bin/invfs-init <<'EOF'
#!/bin/sh
# PID1 entry for the busybox-init fallback (invfs.init=/bin/invfs-init).
exec /bin/busybox init
EOF
chmod 755 /mnt/bin/invfs-init
cat > /mnt/etc/inittab <<'EOF'
::sysinit:/etc/rc.invfs
ttyS0::respawn:/usr/bin/agetty --autologin root --noclear -L 115200 ttyS0 vt100
::ctrlaltdel:/usr/bin/reboot
::shutdown:/bin/umount -a -r
EOF
cat > /mnt/etc/rc.invfs <<'EOF'
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
chmod 755 /mnt/etc/rc.invfs
ln -sf /run/systemd/resolve/stub-resolv.conf /mnt/etc/resolv.conf
arch-chroot /mnt bash -c 'echo root:root | chpasswd; ssh-keygen -A >/dev/null 2>&1 || true' || exit 1
awk 'BEGIN{OFS=" "}
/^#?PermitRootLogin/ {print "PermitRootLogin yes"; next}
/^#?PasswordAuthentication/ {print "PasswordAuthentication yes"; next}
{print}' /mnt/etc/ssh/sshd_config > "$B/sshd_config" || exit 1
cp "$B/sshd_config" /mnt/etc/ssh/sshd_config
chmod 600 /mnt/etc/ssh/sshd_config
find /mnt/etc/ssh -maxdepth 1 -type f \( -name 'ssh_host_*_key' -o -name 'id_*' \) -exec chmod 0600 {} + 2>/dev/null || true
say "boot provisioning done (busybox-init, root pw, host keys, sshd)"
invf-mkfs "$VOL" 8 || exit 1
# Bulk-import profile from docs/benchmarks/commit-policy.md: the default
# commit intervals are conservative (live-root safe); an install may batch
# aggressively because the whole tree is verified right after.
export INVFS_COMMIT_BYTES=16 INVFS_COMMIT_MS=5000 INVFS_COMMIT_IDLE_MS=1000
say "import /mnt -> $VOL (batched)"
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
invf-import "$VOL" /mnt || exit 1
unset INVFS_COMMIT_BYTES INVFS_COMMIT_MS INVFS_COMMIT_IDLE_MS
# Lock-holder snapshot: if fsck below reports 'in use by another process',
# this names the holder.
say "holders of $VOL:"
ls -l /proc/[0-9]*/fd 2>/dev/null | grep -a "$(basename "$VOL")$" || say "(none visible)"
invf-fsck "$VOL" || exit 1

# ---- phase 3: asserts ----
# Paths are slashless by repo convention (all harnesses call invf-cat with
# etc/passwd, not /etc/passwd); a leading slash reads nothing and cmp then
# reports a MISMATCH that is really a lookup miss.
# Fully synchronous reads: invf-cat writes a temp FILE and exits (lock
# released) before cmp starts. The old process-substitution form let a
# mismatch-exited cmp orphan a still-running cat, whose held lock failed
# the NEXT try with 'in use by another process'.
# Retries stay (a genuine transient still shouldn't fail the install)
# and no invf-cat stderr is ever discarded.
for f in etc/pacman.conf boot/vmlinuz-linux; do
    ok=0
    for r in 1 2 3; do
        if invf-cat "$VOL" "$f" "$B/out" 2>"$B/cat-$r.err" && cmp -s "/mnt/$f" "$B/out"; then
            ok=$r; break
        fi
        say "try $r FAILED for $f: $(head -c 200 "$B/cat-$r.err" | tr '\n' '|')"
        sleep 2
    done
    if [ "$ok" != 0 ] && [ "$ok" != 1 ]; then say "FLAKY-READ: $f passed on try $ok (read-path finding, install continues)"; fi
    if [ "$ok" = 0 ]; then
        say "MISMATCH: $f (3 tries, first errors above)"
        say "source: $(md5sum < "/mnt/$f")"
        exit 1
    fi
done
say "spot bit-exact OK"
NFILES=$(find /mnt -type f | wc -l)
STATS="pkg=$(pacman -Q invfs) files=$NFILES src=$(du -sb /mnt | cut -f1)B"
done_rc=0
