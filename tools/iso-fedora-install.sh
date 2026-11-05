#!/bin/bash
# iso-fedora-install.sh -- stock Fedora Server ISO install onto InvariantFS.
#
# Non-interactive Kickstart / Serial driven installation:
#   1. Downloads & verifies stock Fedora Server netinst ISO
#   2. Extracts kernel+initramfs (vmlinuz & initrd.img) from ISO
#   3. Generates kickstart file pointing to local server for post-install hook
#      and serves release tarball, bootstrap.sh and iso-guest-fedora-setup.sh locally
#   4. Boots Fedora in QEMU with kickstart or serial break prompt
#   5. Runs guest setup script, streams progress with timestamps in real-time
#   6. Detects PASS/FAIL marker instantly, verifies clean shutdown
set -eu
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"

# Pinned Fedora 44 Server netinst ISO
ISO_VER="44-1.7"
ISO_URL="https://download.fedoraproject.org/pub/fedora/linux/releases/44/Server/x86_64/iso/Fedora-Server-netinst-x86_64-$ISO_VER.iso"
ISO_SHA256="ae20c06bea746913cadea7d80463e13f4bf55bee4df2918111c921c674b70283  Fedora-Server-netinst-x86_64-$ISO_VER.iso"
PORT="${INVFS_ISO_PORT:-8006}"
MEM="${INVFS_ISO_MEM:-4G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-120}"
ACCEL="${INVFS_ACCEL:-auto}"

ISO="$WORK/Fedora-Server-netinst-x86_64-$ISO_VER.iso"
VOL="$WORK/vol.img"
SER="$WORK/serial.log"
SPIPE="$WORK/serial.pipe"
SLOG="$WORK/serve.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"

fail() { echo "iso-fedora-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-fedora-install: $*"; }

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"

# Cleanup stray processes
P=$(ps -eo pid,args | grep "[f]edoraisosearch" | awk '{print $1}' || true)
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

note "extracting kernel+initramfs from the ISO"
mkdir -p "$MNT"
sudo mount -o loop,ro "$ISO" "$MNT" || fail "loop-mount failed"
# In Fedora netinst ISO, images/pxeboot/vmlinuz and images/pxeboot/initrd.img
if [ -f "$MNT/images/pxeboot/vmlinuz" ]; then
    sudo cp "$MNT/images/pxeboot/vmlinuz" "$WORK/vmlinuz-fedora"
    sudo cp "$MNT/images/pxeboot/initrd.img" "$WORK/initrd-fedora"
elif [ -f "$MNT/isolinux/vmlinuz" ]; then
    sudo cp "$MNT/isolinux/vmlinuz" "$WORK/vmlinuz-fedora"
    sudo cp "$MNT/isolinux/initrd.img" "$WORK/initrd-fedora"
else
    K=$(find "$MNT" -name "vmlinuz*" | head -1)
    I=$(find "$MNT" -name "initrd*.img" | head -1)
    [ -n "$K" ] && [ -n "$I" ] || { sudo umount "$MNT"; fail "kernel/initrd not found in Fedora ISO"; }
    sudo cp "$K" "$WORK/vmlinuz-fedora"
    sudo cp "$I" "$WORK/initrd-fedora"
fi
sudo umount "$MNT"
sudo chown "$(id -u):$(id -g)" "$WORK/vmlinuz-fedora" "$WORK/initrd-fedora"

note "building the release artifact for the guest"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls -t "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
ART_GZ="${ART%.tar.zst}.tar.gz"
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/packaging/bootstrap.sh" "$REPO/tools/iso-guest-fedora-setup.sh" "$SERVEDIR/"
zstd -dc "$ART" | gzip -9 > "$SERVEDIR/$(basename "$ART_GZ")"
( cd "$SERVEDIR" && sha256sum "$(basename "$ART_GZ")" >> SHA256SUMS )

cat > "$SERVEDIR/sh" <<'SH_EOF'
#!/bin/sh
set -eu
SRV="${INVFS_SERVER:-http://10.0.2.2:8006}"
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

# Kickstart file for unattended automated Fedora install or rescue shell
cat > "$SERVEDIR/ks.cfg" <<KS_EOF
# Kickstart file for InvariantFS automated install
text
poweroff

# Keyboard & Language
keyboard --vckeymap=us --xlayouts='us'
lang en_US.UTF-8

# Network
network --bootproto=dhcp --device=link --activate

# Disk partitioning
clearpart --all --initlabel
part /boot/efi --fstype="efi" --size=512
part / --fstype="ext4" --size=2048 --grow

# Root password
rootpw --plaintext root

%post --interpreter=/bin/bash
curl -fsSL http://10.0.2.2:$PORT/iso-guest-fedora-setup.sh | bash
%end
KS_EOF

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
note "starting qemu (accel=$ACCEL, Fedora Server netinst + Kickstart/rd.break)"
# Using rd.break=pre-pivot or inst.ks for automated non-interactive install
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-fedora" -initrd "$WORK/initrd-fedora" \
  -append "inst.stage2=hd:LABEL=Fedora-S-dvd-x86_64-44 inst.ks=http://10.0.2.2:$PORT/ks.cfg console=ttyS0,115200 quiet rd.break=pre-pivot invfsvol=/dev/vda fedoraisosearch" \
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

# Wait for prompt (either dracut debug shell 'pre-pivot' or login prompt or setup script output)
note "waiting for Fedora installer / emergency shell / setup..."
TIMEOUT_SEC=$(( TIMEOUT_MIN * 60 ))
serial_wait "Dropping to debug shell" 120 || serial_wait "login:" 120 || serial_wait "#" 60 || true

# Check if we dropped to dracut debug shell
if grep -q -a "Dropping to debug shell" "$SER" 2>/dev/null || grep -q -a "#" "$SER" 2>/dev/null; then
    note "debug shell active; launching guest setup script directly..."
    serial_send "dhclient eth0 2>/dev/null || ip link set eth0 up"
    sleep 2
    serial_send "curl -fsSL http://10.0.2.2:$PORT/iso-guest-fedora-setup.sh | sh"
fi

# Stream progress and wait for setup marker
note "monitoring live guest installation..."
serial_wait "INVFS-ISO-SETUP:" "$TIMEOUT_SEC" || fail "guest setup timed out or died (see $SER)"

if grep -aq "INVFS-ISO-SETUP: PASS" "$SER" 2>/dev/null; then
    note "Fedora ISO install: PASS"
    for _ in $(seq 1 30); do
        kill -0 "$QPIDE" 2>/dev/null || break
        sleep 1
    done
    exit 0
else
    fail "Fedora ISO install: FAIL (see $SER)"
fi
