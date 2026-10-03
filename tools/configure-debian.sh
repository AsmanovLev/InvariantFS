#!/bin/bash
# configure-debian.sh -- provision a debootstrapped Debian root for an
# InvariantFS boot. Modeled on tools/configure-void.sh, which is the same job
# for a runit system.
#
# WHY THIS EXISTS. docs/INSTALL-MATRIX.md §7 records the gap this closes:
#
#   "There is no Debian install guide at all, and no Debian boot script.
#    Debian is the one target whose *package* is verified here; its *rootfs on
#    InvariantFS* is entirely unexplored."
#
# Debian is the right target for that gap for three reasons: its package is
# already verified here (INSTALL-MATRIX §3), it is what CI's container base is,
# and -- the actual point -- it is the only readily available distro whose init
# is systemd as PID 1. The initramfs was already written for that case:
# src/../tools/initramfs-init.sh:295-300 mounts /run as tmpfs and cgroup2
# before handing off, with the comment
#
#   WP66 H1: systemd expects /run as tmpfs and cgroup2 before PID1 starts.
#   Without these, early-mount units fail and daemon startup hangs.
#
# so the harness side exists; there has simply never been a systemd root to
# point it at. That is why this is a provisioning script and not a rewrite.
#
# Usage: bash tools/configure-debian.sh <staged-root> [sshd]
set -eu

E=${1:?usage: configure-debian.sh <staged-root> [sshd]}
WITH_SSHD=${2:-sshd}
[ -d "$E" ] || { echo "no such root: $E" >&2; exit 1; }
[ -x "$E/sbin/init" ] || [ -d "$E/lib/systemd" ] \
    || { echo "$E does not look like a Debian root" >&2; exit 1; }

note() { echo "configure-debian: $*"; }

# --- fstab ------------------------------------------------------------------
# The FUSE root cannot be remounted and is already mounted; only the
# pseudo-filesystems and tmpfs scratch belong here. Same reasoning as the Void
# provisioner: on a FUSE root systemd's own root-fs handling is skipped, so a
# /tmp entry is what guarantees the mount exists.
cat > "$E/etc/fstab" <<'EOF'
proc	/proc	proc	defaults	0 0
sysfs	/sys	sysfs	defaults	0 0
tmpfs	/tmp	tmpfs	defaults,noatime,mode=1777,size=1G	0 0
tmpfs	/var/tmp	tmpfs	defaults,noatime,mode=1777,size=512M	0 0
EOF

# --- serial console: autologin root on ttyS0 -------------------------------
# systemd-getty-generator spawns a getty per active console; the kernel cmdline
# console=ttyS0 is what makes ttyS0 one. Autologin is a unit drop-in, and it
# must not fight the login prompt the generator sets up.
mkdir -p "$E/etc/systemd/system/serial-getty@ttyS0.service.d"
cat > "$E/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I 115200 vt220
EOF

# --- first-boot noise that would otherwise stall a serial console -----------
# Without these, systemd prints a boot splash and a login prompt over the
# serial line, and the harness's marker greps land in the middle of a progress
# bar. configure-void.sh does the same job for runit.
mkdir -p "$E/etc/systemd/system"
cat > "$E/etc/systemd/system/serial-getty@ttyS0.service.d/nologin-message.conf" 2>/dev/null <<'EOF'
[Service]
TTYVTDisallocate=no
EOF
mkdir -p "$E/etc/issue.d"
: > "$E/etc/issue"
# No splash, no animations on a 115200 baud console.
mkdir -p "$E/etc/systemd/system/systemd-quiet.service.d"
cat > "$E/etc/default/locale" <<'EOF'
LANG=C.UTF-8
EOF

# --- sshd (optional; the harness uses it to assert the root is alive) -------
if [ "$WITH_SSHD" = "sshd" ]; then
    if [ ! -e "$E/etc/ssh/ssh_host_rsa_key" ]; then
        # Host keys must be generated OFF the volume, for the same reason
        # configure-void.sh does it before import: key generation wants
        # randomness and permissions that a FUSE-backed root does not owe it.
        ssh-keygen -q -t ed25519 -f /tmp/_invfs_deb_ssh -N '' 2>/dev/null \
            && mkdir -p "$E/etc/ssh" \
            && mv /tmp/_invfs_deb_ssh "$E/etc/ssh/ssh_host_ed25519_key" \
            && mv /tmp/_invfs_deb_ssh.pub "$E/etc/ssh/ssh_host_ed25519_key.pub" \
            || note "WARN: no sshd host key (need ssh-keygen on the HOST)"
    fi
    mkdir -p "$E/etc/ssh/sshd_config.d"
    cat > "$E/etc/ssh/sshd_config.d/99-invfs.conf" <<'EOF'
PermitRootLogin yes
PasswordAuthentication yes
EOF
    # root must have a password the harness can use: sshpass -p root
    sed -i 's/^root:[^:]*:/root:root:/' "$E/etc/shadow" 2>/dev/null || true
fi

# --- the one thing that makes this root a filesystem test -------------------
note "staged at $E"
note "init candidates:"
for i in /lib/systemd/systemd /usr/lib/systemd/systemd /sbin/init; do
    [ -e "$E$i" ] && note "  present: $i"
done
note "cgroup2 present in the initramfs handoff (initramfs-init.sh:300)"