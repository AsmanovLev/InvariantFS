#!/bin/bash
# iso-arch-install.sh -- stock-Arch-ISO install onto InvariantFS (host side).
#
# Least-invasive by construction: the ISO boots PRISTINE (checksum-verified
# against the pinned hash below) and fetches everything InvFS-specific from
# the local server this script runs -- the recipe, the release artifact, the
# guest setup script. See tools/invfs-serve.sh and, in the guest,
# tools/iso-guest-arch-setup.sh (which doubles as the curl|bash form).
#
#   WORK=/path/to/work bash tools/iso-arch-install.sh
#
# Needs: qemu-system-x86_64 (KVM, else TCG fallback), sudo (ISO loop-mount),
# ~12 GB scratch, network. Prints INVFS-ISO-SETUP: PASS/FAIL from the guest
# serial and exits 0/1, so CI can gate on it directly.
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"
# Pinned ISO (policy: reproducible inputs; bump with checksum together).
ISO_VER="2026.10.01"
ISO_URL="https://geo.mirror.pkgbuild.com/iso/$ISO_VER/archlinux-$ISO_VER-x86_64.iso"
ISO_SHA256="684ded26c63240ff4a41e8c25ee84ea6da233f557364821f13d12c2b0a9059a5  archlinux-$ISO_VER-x86_64.iso"
ISO_UUID="2026-10-01-14-49-03-00"
PORT="${INVFS_ISO_PORT:-8000}"
MEM="${INVFS_ISO_MEM:-4G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-100}"
ACCEL="${INVFS_ACCEL:-auto}"

ISO="$WORK/archlinux-$ISO_VER-x86_64.iso"
VOL="$WORK/vol.img"
SER="$WORK/serial.log"
SLOG="$WORK/serve.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"
fail() { echo "iso-arch-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-arch-install: $*"; }

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"

# Strays from a killed run hold the volume lock and the port. Bracketed
# patterns ([a]rch...): a match on its own command line kills the harness
# itself -- learned twice the hard way. (The server's bash wrapper execs
# python but keeps its argv, so it still matches invfs-serve.sh.)
P=$(ps -eo pid,args | grep "[a]rchisosearchuuid" | awk '{print $1}' || true)
[ -n "$P" ] && { kill -KILL $P 2>/dev/null; note "killed stray qemu: $P"; }
Q=$(ps -eo pid,args | grep "[i]nvfs-serve.sh" | awk '{print $1}' || true)
[ -n "$Q" ] && { kill -KILL $Q 2>/dev/null; note "killed stray server: $Q"; }
sleep 1

if [ ! -f "$ISO" ]; then
    note "downloading $ISO_URL"
    curl -fsSL -o "$ISO" "$ISO_URL" || fail "ISO download failed"
fi
( cd "$WORK" && echo "$ISO_SHA256" | sha256sum -c - ) || fail "ISO checksum mismatch"
note "ISO verified: $(basename "$ISO")"

note "extracting kernel+initramfs from the ISO"
mkdir -p "$MNT"
sudo mount -o loop,ro "$ISO" "$MNT" || fail "loop-mount failed"
cp "$MNT/arch/boot/x86_64/vmlinuz-linux" "$WORK/vmlinuz-archiso"
cp "$MNT/arch/boot/x86_64/initramfs-linux.img" "$WORK/initramfs-archiso.img"
sudo umount "$MNT"

note "building the release artifact the guest will install"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/packaging/PKGBUILD" "$REPO/tools/iso-guest-arch-setup.sh" "$REPO/bin/busybox-static" "$SERVEDIR/"
note "serving: $(ls "$SERVEDIR" | tr '\n' ' ')"

if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm; CPU=host
    else ACCEL=tcg; CPU=max; note "/dev/kvm unusable; TCG fallback (slower)"; fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}

truncate -s 8G "$VOL"
note "starting server on :$PORT"
bash "$REPO/tools/invfs-serve.sh" "$SERVEDIR" "$PORT" >"$SLOG" 2>&1 &
SRVPID=$!
cleanup() { kill $SRVPID 2>/dev/null; }
trap cleanup EXIT
sleep 1
note "starting qemu (accel=$ACCEL, stock ISO, script= hook)"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-archiso" -initrd "$WORK/initramfs-archiso.img" \
  -append "archisobasedir=arch archisosearchuuid=$ISO_UUID console=ttyS0,115200 cow_spacesize=3G invfsvol=/dev/vda script=http://10.0.2.2:$PORT/iso-guest-arch-setup.sh" \
  -cdrom "$ISO" \
  -drive "file=$VOL,format=raw,if=virtio" \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0 \
  -serial "file:$SER" -display none -no-reboot &
QPIDE=$!
note "qemu pid $QPIDE; waiting for marker (up to $TIMEOUT_MIN min)"
N=$(( TIMEOUT_MIN * 6 ))
for _ in $(seq 1 "$N"); do
  sleep 10
  grep -aq "INVFS-ISO-SETUP:" "$SER" 2>/dev/null && break
  kill -0 $QPIDE 2>/dev/null || { note "qemu exited early"; break; }
done
grep -a "INVFS-ISO-SETUP:" "$SER" 2>/dev/null | tail -2
if grep -aq "INVFS-ISO-SETUP: PASS" "$SER" 2>/dev/null; then
    note "volume left at $VOL"
    exit 0
else
    fail "no PASS marker (see $SER)"
fi
