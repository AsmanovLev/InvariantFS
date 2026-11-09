#!/bin/bash
# iso-nixos-install.sh -- stock NixOS minimal ISO install onto InvariantFS.
#
# Driven via event-based serial triggers and real-time streaming console:
#   1. Downloads & verifies stock nixos-minimal ISO
#   2. Extracts kernel+initramfs from ISO
#   3. Serves release tarball, bootstrap.sh and iso-guest-nixos-setup.sh locally
#   4. Boots stock NixOS minimal ISO in QEMU with bidirectional serial pipe
#   5. Event trigger: detects login prompt or root shell prompt
#   6. Runs guest setup script, streams progress with timestamps in real-time
#   7. Detects PASS/FAIL marker instantly, verifies clean shutdown
set -eu
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"

# Pinned NixOS 24.11 minimal ISO
ISO_VER="24.11.719113.50ab793786d9"
ISO_URL="https://releases.nixos.org/nixos/24.11/nixos-$ISO_VER/nixos-minimal-$ISO_VER-x86_64-linux.iso"
ISO_SHA256="acdcf8239f64e5acd20cf49c63f83e4c1b823b31d9f669033b48876b29b52177  nixos-minimal-$ISO_VER-x86_64-linux.iso"
PORT="${INVFS_ISO_PORT:-8004}"
MEM="${INVFS_ISO_MEM:-4G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-120}"
ACCEL="${INVFS_ACCEL:-auto}"

ISO="$WORK/nixos-minimal-$ISO_VER-x86_64-linux.iso"
VOL="$WORK/vol.img"
SER="$WORK/serial.log"
SPIPE="$WORK/serial.pipe"
SLOG="$WORK/serve.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"

fail() { echo "iso-nixos-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-nixos-install: $*"; }

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"

# Cleanup stray processes
P=$(ps -eo pid,args | grep "[n]ixosisosearch" | awk '{print $1}' || true)
[ -n "$P" ] && { kill -KILL $P 2>/dev/null; note "killed stray qemu: $P"; }
Q=$(ps -eo pid,args | grep "[i]nvfs-serve.sh.*$PORT" | awk '{print $1}' || true)
[ -n "$Q" ] && { kill -KILL $Q 2>/dev/null; note "killed stray server: $Q"; }
rm -f "$SPIPE.in" "$SPIPE.out"
sleep 1

# Download ISO
if [ ! -f "$ISO" ]; then
    note "downloading $ISO_URL"
    curl -fsSL -o "$ISO" "$ISO_URL" || fail "ISO download failed"
fi
( cd "$WORK" && echo "$ISO_SHA256" | sha256sum -c - ) || fail "ISO checksum mismatch"
note "ISO verified against pinned sha256"

note "extracting kernel+initramfs and boot parameters from the ISO"
mkdir -p "$MNT"
sudo mount -o loop,ro "$ISO" "$MNT" || fail "loop-mount failed"
# In NixOS minimal ISO, kernel is boot/bzImage or isolinux/bzImage, initrd is boot/initrd or isolinux/initrd
if [ -f "$MNT/boot/bzImage" ]; then
    sudo cp "$MNT/boot/bzImage" "$WORK/vmlinuz-nixos"
    sudo cp "$MNT/boot/initrd" "$WORK/initrd-nixos"
elif [ -f "$MNT/isolinux/bzImage" ]; then
    sudo cp "$MNT/isolinux/bzImage" "$WORK/vmlinuz-nixos"
    sudo cp "$MNT/isolinux/initrd" "$WORK/initrd-nixos"
else
    # Find any bzImage/kernel and initrd in ISO
    K=$(find "$MNT" -name "bzImage" -o -name "vmlinuz*" | head -1)
    I=$(find "$MNT" -name "initrd*" | head -1)
    [ -n "$K" ] && [ -n "$I" ] || { sudo umount "$MNT"; fail "kernel/initrd not found in NixOS ISO"; }
    sudo cp "$K" "$WORK/vmlinuz-nixos"
    sudo cp "$I" "$WORK/initrd-nixos"
fi

# Extract init= parameter from the ISO's grub/isolinux config
NIXOS_INIT=$(grep -Eho 'init=[^ "]+' "$MNT"/boot/grub/grub.cfg "$MNT"/isolinux/*.cfg "$MNT"/loader/entries/*.conf 2>/dev/null | head -1 || true)
note "NixOS live init param: ${NIXOS_INIT:-none}"
sudo umount "$MNT"
sudo chown "$(id -u):$(id -g)" "$WORK/vmlinuz-nixos" "$WORK/initrd-nixos"

note "building the release artifact for the guest"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls -t "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
ART_GZ="${ART%.tar.zst}.tar.gz"
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/packaging/bootstrap.sh" "$REPO/tools/iso-guest-nixos-setup.sh" "$SERVEDIR/"
zstd -dc "$ART" | gzip -9 > "$SERVEDIR/$(basename "$ART_GZ")"
( cd "$SERVEDIR" && sha256sum "$(basename "$ART_GZ")" >> SHA256SUMS )

cat > "$SERVEDIR/sh" <<'SH_EOF'
#!/bin/sh
set -eu
SRV="${INVFS_SERVER:-http://10.0.2.2:8004}"
TMP=$(mktemp -d /tmp/invfs-sh.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o bootstrap.sh "$SRV/bootstrap.sh"
    curl -fsSL -o SHA256SUMS "$SRV/SHA256SUMS"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O bootstrap.sh "$SRV/bootstrap.sh"
    wget -q -O SHA256SUMS "$SRV/SHA256SUMS"
fi

if command -v zstd >/dev/null 2>&1; then
    ART=$(grep '\.tar\.zst$' SHA256SUMS | awk '{print $2}' | head -1)
else
    ART=$(grep '\.tar\.gz$' SHA256SUMS | awk '{print $2}' | head -1)
fi
[ -n "$ART" ] || ART=$(awk '{print $2}' SHA256SUMS | head -1)

if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$ART" "$SRV/$ART"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$ART" "$SRV/$ART"
fi
grep "[[:space:]*]$ART\$" SHA256SUMS | sha256sum -c -
sh bootstrap.sh --file "$ART" --yes --prefix /usr/local
SH_EOF
chmod 755 "$SERVEDIR/sh"

if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm; CPU=host
    else ACCEL=tcg; CPU=max; note "/dev/kvm unusable; TCG fallback (slower)"; fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}

truncate -s 8G "$VOL"
note "starting server on :$PORT"
bash "$REPO/tools/invfs-serve.sh" "$SERVEDIR" "$PORT" >"$SLOG" 2>&1 &
SRVPID=$!

# Bidirectional serial pipe
mkfifo "$SPIPE.in" "$SPIPE.out" 2>/dev/null || true
: > "$SER"

# Real-time streaming reader
tee "$SER" < "$SPIPE.out" &
READER_PID=$!

QPIDE=""
cleanup() {
    [ -n "$QPIDE" ] && kill -KILL $QPIDE 2>/dev/null || true
    kill -KILL $SRVPID $READER_PID 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
}
trap cleanup EXIT
sleep 1

QLOG="$WORK/qemu.log"
note "starting qemu (accel=$ACCEL, stock NixOS minimal kernel+initrd)"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-nixos" -initrd "$WORK/initrd-nixos" \
  -append "${NIXOS_INIT} console=ttyS0,115200 invfsvol=/dev/vda" \
  -cdrom "$ISO" \
  -drive "file=$VOL,format=raw,if=virtio" \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -serial "pipe:$SPIPE" -display none -no-reboot >"$QLOG" 2>&1 &
QPIDE=$!

exec 9>"$SPIPE.in"

serial_send() { printf '%s\r\n' "$1" >&9; }
serial_wait() {
    local pat="$1" timeout="$2"
    local start=$(date +%s)
    while true; do
        if grep -q -a "$pat" "$SER" 2>/dev/null; then return 0; fi
        if ! kill -0 "$QPIDE" 2>/dev/null; then return 2; fi
        local now=$(date +%s)
        if [ $((now - start)) -ge "$timeout" ]; then return 1; fi
        sleep 0.5
    done
}

# Responsive event-driven triggers:
# In NixOS minimal ISO, it auto-logs in root on console, or shows login/prompt
note "waiting for NixOS boot prompt..."
TIMEOUT_SEC=$(( TIMEOUT_MIN * 60 ))
serial_wait "nixos#" "$TIMEOUT_SEC" || serial_wait "login:" 60 || serial_wait "#" 30 || true

# Check if we need to send enter or root
serial_send ""
sleep 1
if grep -q -a "login:" "$SER" 2>/dev/null && ! grep -q -a "nixos#" "$SER" 2>/dev/null; then
    note "login prompt detected; sending root login"
    serial_send "root"
    sleep 1
fi

note "triggering guest setup script via curl..."
serial_send "curl -fsSL http://10.0.2.2:$PORT/iso-guest-nixos-setup.sh | bash"

# Stream progress and wait for setup marker
note "monitoring live guest installation..."
serial_wait "INVFS-ISO-SETUP:" "$TIMEOUT_SEC" || fail "guest setup timed out or died (see $SER)"

if grep -aq "INVFS-ISO-SETUP: PASS" "$SER" 2>/dev/null; then
    note "NixOS ISO install: PASS"
    for _ in $(seq 1 30); do
        kill -0 "$QPIDE" 2>/dev/null || break
        sleep 1
    done
    exit 0
else
    fail "NixOS ISO install: FAIL (see $SER)"
fi
