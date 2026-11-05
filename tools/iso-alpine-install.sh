#!/bin/bash
# iso-alpine-install.sh -- Stock Alpine Linux virt ISO install onto InvariantFS.
#
# Driven via event-based serial triggers and real-time streaming console:
#   1. Downloads & verifies stock alpine-virt ISO
#   2. Extracts kernel+initramfs from ISO
#   3. Serves release tarball, bootstrap.sh and iso-guest-alpine-setup.sh locally
#   4. Boots stock Alpine in QEMU with bidirectional serial pipe
#   5. Event trigger: detects login prompt -> sends root login
#   6. Runs guest setup script, streams progress with timestamps in real-time
#   7. Detects PASS/FAIL marker instantly, verifies clean shutdown
set -eu
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"

ISO_VER="3.24.4"
ISO_URL="https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/alpine-virt-$ISO_VER-x86_64.iso"
ISO_KERNEL="boot/vmlinuz-virt"
ISO_INITRD="boot/initramfs-virt"
PORT="${INVFS_ISO_PORT:-8003}"
MEM="${INVFS_ISO_MEM:-4G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-120}"
ACCEL="${INVFS_ACCEL:-auto}"

ISO="$WORK/alpine-virt-$ISO_VER-x86_64.iso"
VOL="$WORK/vol.img"
SER="$WORK/serial.log"
SPIPE="$WORK/serial.pipe"
SLOG="$WORK/serve.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"

fail() { echo "iso-alpine-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-alpine-install: $*"; }

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"

# Cleanup stray processes
P=$(ps -eo pid,args | grep "[a]lpineisosearch" | awk '{print $1}' || true)
[ -n "$P" ] && { kill -KILL $P 2>/dev/null; note "killed stray qemu: $P"; }
Q=$(ps -eo pid,args | grep "[i]nvfs-serve.sh.*$PORT" | awk '{print $1}' || true)
[ -n "$Q" ] && { kill -KILL $Q 2>/dev/null; note "killed stray server: $Q"; }
rm -f "$SPIPE.in" "$SPIPE.out"
sleep 1

# Download ISO
if [ ! -f "$ISO" ]; then
    note "downloading $ISO_URL"
    curl -fsSL -o "$ISO" "$ISO_URL" || fail "ISO download failed"
    curl -fsSL -o "$ISO.sha256" "$ISO_URL.sha256" 2>/dev/null || true
fi
if [ -f "$ISO.sha256" ]; then
    ( cd "$WORK" && sha256sum -c "alpine-virt-$ISO_VER-x86_64.iso.sha256" ) || fail "ISO checksum mismatch"
    note "ISO verified against upstream sha256"
fi

note "extracting kernel+initramfs from the ISO"
mkdir -p "$MNT"
sudo mount -o loop,ro "$ISO" "$MNT" || fail "loop-mount failed"
sudo cp "$MNT/$ISO_KERNEL" "$WORK/vmlinuz-alpine" || { sudo umount "$MNT"; fail "no $ISO_KERNEL in ISO"; }
sudo cp "$MNT/$ISO_INITRD" "$WORK/initramfs-alpine.img" || { sudo umount "$MNT"; fail "no $ISO_INITRD in ISO"; }
sudo umount "$MNT"
sudo chown "$(id -u):$(id -g)" "$WORK/vmlinuz-alpine" "$WORK/initramfs-alpine.img"

note "building the release artifact for the guest"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls -t "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
ART_GZ="${ART%.tar.zst}.tar.gz"
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/packaging/bootstrap.sh" "$REPO/tools/iso-guest-alpine-setup.sh" "$SERVEDIR/"
zstd -dc "$ART" | gzip -9 > "$SERVEDIR/$(basename "$ART_GZ")"
( cd "$SERVEDIR" && sha256sum "$(basename "$ART_GZ")" >> SHA256SUMS )

cat > "$SERVEDIR/sh" <<'SH_EOF'
#!/bin/sh
set -eu
SRV="${INVFS_SERVER:-http://10.0.2.2:8003}"
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
sh bootstrap.sh --file "$ART" --yes --prefix /usr
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

# Real-time streaming reader (tee guarantees unbuffered character output to $SER
# even when the prompt has no trailing newline)
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
note "starting qemu (accel=$ACCEL, stock Alpine kernel+initramfs)"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-alpine" -initrd "$WORK/initramfs-alpine.img" \
  -append "modules=loop,squashfs,sd-mod,usb-storage quiet console=ttyS0,115200 invfsvol=/dev/vda" \
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
# Trigger 1: Wait for login prompt
note "waiting for Alpine boot / login prompt..."
TIMEOUT_SEC=$(( TIMEOUT_MIN * 60 ))
serial_wait "login:" "$TIMEOUT_SEC" || fail "login prompt never appeared (see $SER)"
note "login prompt detected; sending root login"
sleep 1
serial_send "root"

# Trigger 2: Wait for root shell prompt (#)
note "waiting for root shell prompt..."
serial_wait "#" 30 || {
    note "retrying root login..."
    serial_send "root"
    serial_wait "#" 30 || fail "shell prompt never appeared after login (see $SER)"
}

sleep 1
# Trigger 3: Launch guest setup script (bring up network interface, then udhcpc)
note "root shell ready; bringing up network and launching iso-guest-alpine-setup.sh"
serial_send "ip link set eth0 up && udhcpc -i eth0"
sleep 4
serial_send "wget -q -O - http://10.0.2.2:$PORT/iso-guest-alpine-setup.sh | sh"

# Trigger 3: Stream progress and wait for setup marker
note "monitoring live guest installation..."
serial_wait "INVFS-ISO-SETUP:" "$TIMEOUT_SEC" || fail "guest setup timed out or died (see $SER)"

if grep -aq "INVFS-ISO-SETUP: PASS" "$SER" 2>/dev/null; then
    note "Alpine ISO install: PASS"
    for _ in $(seq 1 30); do
        kill -0 "$QPIDE" 2>/dev/null || break
        sleep 1
    done
    exit 0
else
    fail "Alpine ISO install: FAIL (see $SER)"
fi
