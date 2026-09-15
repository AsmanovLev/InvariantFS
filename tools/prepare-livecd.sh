#!/bin/bash
# prepare-livecd.sh — Prepare a Gentoo live CD for InvariantFS work
# Usage: ./tools/prepare-livecd.sh <ip> [password]
set -euo pipefail

IP="${1:?Usage: $0 <live-cd-ip> [password]}"
PASS="${2:-0000}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$SCRIPT_DIR/../bin"

run() { sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@"$IP" "$@"; }
copy() { sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$@"; }

echo "=== Preparing live CD at $IP ==="

echo "[1/7] Setting root password..."
run "echo 'root:$PASS' | chpasswd"

echo "[2/7] Configuring and starting SSH..."
run '
  mkdir -p /run/sshd &&
  sed -i "s/^#*PermitRootLogin.*/PermitRootLogin yes/" /etc/ssh/sshd_config 2>/dev/null || true &&
  sed -i "s/^#*PasswordAuthentication.*/PasswordAuthentication yes/" /etc/ssh/sshd_config 2>/dev/null || true &&
  sed -i "s/^#*ChallengeResponseAuthentication.*/ChallengeResponseAuthentication no/" /etc/ssh/sshd_config 2>/dev/null || true &&
  /usr/sbin/sshd 2>/dev/null || true
'

echo "[3/7] Bringing up network via DHCP on all interfaces..."
run '
  for iface in /sys/class/net/*/; do
    name=$(basename "$iface")
    [ "$name" = "lo" ] && continue
    ip link set "$name" up 2>/dev/null || true
    dhcpcd -q "$name" 2>/dev/null || true
  done
'

echo "[4/7] Waiting for network..."
sleep 3
run 'ip addr show'

echo "[5/7] Uploading InvFS binaries..."
run 'mkdir -p /usr/local/bin'
for tool in "$BIN_DIR"/invf-*; do
    copy "$tool" "root@$IP:/usr/local/bin/$(basename "$tool")" 2>/dev/null || true
done
run 'chmod +x /usr/local/bin/invf-* && ls /usr/local/bin/invf-* | wc -l'

echo "[6/7] Mounting EFI partition..."
run 'mkdir -p /mnt/efi && mount /dev/sda1 /mnt/efi 2>/dev/null; echo ok'

echo "[7/7] Checking and mounting InvFS..."
run 'pkill -9 invf-fuse 2>/dev/null; fusermount3 -uz /mnt/invfs 2>/dev/null; sleep 1; invf-fsck -f /dev/sda3 2>&1 | tail -8'
run 'mkdir -p /mnt/invfs && invf-fuse /dev/sda3 /mnt/invfs -o allow_other 2>&1 | grep -v "^\["'
run 'mount -t proc proc /mnt/invfs/proc 2>/dev/null || true'
run 'mount -t sysfs sysfs /mnt/invfs/sys 2>/dev/null || true'
run 'mount --rslave /dev /mnt/invfs/dev 2>/dev/null || true'
run 'cp /etc/resolv.conf /mnt/invfs/etc/ 2>/dev/null || true'

IP_ACTUAL=$(run 'hostname -I | awk "{print \$1}"')
echo ""
echo "=== Ready ==="
echo "  IP:     $IP_ACTUAL"
echo "  SSH:    sshpass -p '$PASS' ssh root@$IP_ACTUAL"
echo "  chroot: sshpass -p '$PASS' ssh root@$IP_ACTUAL 'chroot /mnt/invfs /bin/bash'"
echo "  efi:    sshpass -p '$PASS' ssh root@$IP_ACTUAL 'ls -lh /mnt/efi/EFI/Gentoo/'"
