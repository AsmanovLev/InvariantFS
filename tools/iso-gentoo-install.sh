#!/bin/bash
# iso-gentoo-install.sh -- stock-Gentoo-minimal-ISO install onto InvariantFS.
#
# Same shape as tools/iso-arch-install.sh: the ISO boots PRISTINE
# (checksum-verified against the pinned hash below) and everything
# InvFS-specific comes from the local server. The automation hook is the
# Handbook's own unattended-install pair: dosshd + passwd= on the kernel
# cmdline. The driver then SSHes in (sshpass, throwaway CI-only password)
# and runs one guest command -- no serial input driving, no installer
# (Gentoo has no installer TUI; the Handbook install IS the CLI flow).
#
#   WORK=/path/to/work bash tools/iso-gentoo-install.sh
#
# Needs: qemu-system-x86_64 (KVM, else TCG fallback), sudo (ISO loop-mount),
# sshpass, ~16 GB scratch, network. Prints INVFS-ISO-SETUP: PASS/FAIL from
# the guest serial and exits 0/1, so CI can gate on it directly.
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"
# Pinned ISO (policy: reproducible inputs; bump with checksum together).
ISO_VER="20260913T163055Z"
ISO_URL="https://distfiles.gentoo.org/releases/amd64/autobuilds/current-install-amd64-minimal/install-amd64-minimal-$ISO_VER.iso"
# Upstream publishes SHA512 in the .DIGESTS file; verified with sha512sum.
ISO_SHA512="7bc150d92d330d90135683e8b430e03fde7f7284e8d868923ebf66b0f94c2b77a0ae9db51696e51704ed0a05769c31939bf26cf7af96a3829608ab2479519047  install-amd64-minimal-$ISO_VER.iso"
# Kernel+initramfs paths INSIDE the ISO (confirmed against 20260913).
ISO_KERNEL="boot/gentoo"
ISO_INITRD="boot/gentoo.igz"
# dracut live root lookup (confirmed: grub.cfg root=live:CDLABEL=...).
ISO_LABEL="Gentoo-amd64-20260913"
# Throwaway root password for the dosshd CI guest. Not a credential to
# anything real; the guest is NATed, short-lived, and destroyed after.
GUEST_PW="invfs-live-install"
PORT="${INVFS_ISO_PORT:-8002}"
SSHPORT="${INVFS_ISO_SSHPORT:-2223}"
MEM="${INVFS_ISO_MEM:-8G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-100}"
ACCEL="${INVFS_ACCEL:-auto}"

ISO="$WORK/install-amd64-minimal-$ISO_VER.iso"
VOL="$WORK/vol.img"
STAGEDISK="$WORK/stage.img"
SER="$WORK/serial.log"
SPIPE="$WORK/serial.pipe"
SLOG="$WORK/serve.log"
SSHLOG="$WORK/ssh.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"
fail() { echo "iso-gentoo-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-gentoo-install: $*"; }

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"
command -v sshpass >/dev/null 2>&1 || fail "sshpass not installed"

# Strays from a killed run hold the volume lock, the port, or the pipes.
P=$(ps -eo pid,args | grep "[g]entooisosearchuuid" | awk '{print $1}' || true)
[ -n "$P" ] && { kill -KILL $P 2>/dev/null; note "killed stray qemu: $P"; }
Q=$(ps -eo pid,args | grep "[i]nvfs-serve.sh" | awk '{print $1}' || true)
[ -n "$Q" ] && { kill -KILL $Q 2>/dev/null; note "killed stray server: $Q"; }
sleep 1

if [ ! -f "$ISO" ]; then
    note "downloading $ISO_URL"
    curl -fsSL -o "$ISO" "$ISO_URL" || fail "ISO download failed"
fi
( cd "$WORK" && echo "$ISO_SHA512" | sha512sum -c - ) || fail "ISO checksum mismatch"
note "ISO verified: $(basename "$ISO")"

note "extracting kernel+initramfs from the ISO"
mkdir -p "$MNT"
sudo mount -o loop,ro "$ISO" "$MNT" || fail "loop-mount failed"
# The ISO's files are root-owned and not user-readable; copy as root,
# then take ownership (qemu runs as the user and must read them).
sudo cp "$MNT/$ISO_KERNEL" "$WORK/vmlinuz-gentoo" || { sudo umount "$MNT"; fail "no $ISO_KERNEL in ISO"; }
sudo cp "$MNT/$ISO_INITRD" "$WORK/initramfs-gentoo.img" || { sudo umount "$MNT"; fail "no $ISO_INITRD in ISO"; }
sudo umount "$MNT"
sudo chown "$(id -u):$(id -g)" "$WORK/vmlinuz-gentoo" "$WORK/initramfs-gentoo.img"

note "building the release artifact the guest will install"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/tools/iso-guest-gentoo-setup.sh" "$SERVEDIR/"
note "serving: $(ls "$SERVEDIR" | tr '\n' ' ')"

if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm; CPU=host
    else ACCEL=tcg; CPU=max; note "/dev/kvm unusable; TCG fallback (slower)"; fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}

truncate -s 12G "$VOL"
# Staging disk: ext4, LABEL=stage (same RAM-overlay reasoning as the
# void harness: tmpfs caps at half of RAM, a stage3 + cache overflows
# it; disk-backed staging removes RAM sizing from the equation).
truncate -s 8G "$STAGEDISK"
mkfs.ext4 -F -q -L stage "$STAGEDISK" || fail "mkfs.ext4 stage failed"
# Serial doubles as evidence AND as the fallback eyes: if SSH never comes
# up, the serial log is the only diagnosis. Pipe form keeps .out readable
# while running (file: only flushes at qemu exit).
rm -f "$SPIPE.in" "$SPIPE.out"
mkfifo "$SPIPE.in" "$SPIPE.out" 2>/dev/null || true
: > "$SER"
cat "$SPIPE.out" >>"$SER" &
# Dummy writer: qemu opens .in O_RDONLY (blocks with no writer) and .out
# O_WRONLY (blocks with no reader). Reader is above; this unblocks .in.
# Nothing is ever sent -- SSH is the control channel, serial is evidence.
exec 9>"$SPIPE.in"
note "starting server on :$PORT"
bash "$REPO/tools/invfs-serve.sh" "$SERVEDIR" "$PORT" >"$SLOG" 2>&1 &
SRVPID=$!
cleanup() { kill $SRVPID 2>/dev/null; }
trap cleanup EXIT
sleep 1

note "starting qemu (accel=$ACCEL, dosshd, passwd set)"
# qemu's own stdout goes to a file, NOT the driver's: it inherits the
# driver's stdout pipe, and a live qemu would hold it open forever,
# hanging any downstream `| tail` (or CI log capture) past the run.
QLOG="$WORK/qemu.log"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-gentoo" -initrd "$WORK/initramfs-gentoo.img" \
  -append "root=live:CDLABEL=$ISO_LABEL rd.live.dir=/ rd.live.squashimg=image.squashfs cdroot dosshd passwd=$GUEST_PW console=ttyS0,115200 ip=dhcp gentooisosearchuuid=$ISO_LABEL invfsvol=/dev/vda" \
  -cdrom "$ISO" \
  -drive "file=$VOL,format=raw,if=virtio" \
  -drive "file=$STAGEDISK,format=raw,if=virtio" \
  -netdev "user,id=net0,hostfwd=tcp::$SSHPORT-:22" -device virtio-net-pci,netdev=net0 \
  -vga none -serial "pipe:$SPIPE" -display none -no-reboot >"$QLOG" 2>&1 &
QPIDE=$!
# The trap must kill qemu too: on FAIL the driver exits while the guest
# (a live shell) would otherwise idle forever, holding locks.
cleanup() { kill $QPIDE $SRVPID 2>/dev/null; }
note "qemu pid $QPIDE; waiting for SSH (up to 30 min)"
ssh_up() {
    sshpass -p "$GUEST_PW" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 -o BatchMode=no -p "$SSHPORT" root@localhost "echo SSHUP" 2>/dev/null | grep -q SSHUP
}
N=$(( 30 * 6 ))
up=0
for _ in $(seq 1 "$N"); do
    sleep 10
    kill -0 $QPIDE 2>/dev/null || { note "qemu exited early"; break; }
    if ssh_up; then up=1; break; fi
done
[ "$up" = 1 ] || fail "SSH never came up (see $SER)"
note "SSH up; running guest (up to $TIMEOUT_MIN min)"
sshpass -p "$GUEST_PW" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ServerAliveInterval=60 -p "$SSHPORT" root@localhost \
    "curl -fsS http://10.0.2.2:$PORT/iso-guest-gentoo-setup.sh | bash" >"$SSHLOG" 2>&1 &
SSHPID=$!
M=$(( TIMEOUT_MIN * 6 ))
for _ in $(seq 1 "$M"); do
    sleep 10
    grep -aq "INVFS-ISO-SETUP:" "$SER" 2>/dev/null && break
    kill -0 $QPIDE 2>/dev/null || { note "qemu exited early"; break; }
done
wait $SSHPID 2>/dev/null || true
grep -a "INVFS-ISO-SETUP:" "$SER" 2>/dev/null | tail -2
if grep -aq "INVFS-ISO-SETUP: PASS" "$SER" 2>/dev/null; then
    note "volume left at $VOL"
    exit 0
else
    fail "no PASS marker (see $SER $SSHLOG)"
fi
