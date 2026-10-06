#!/bin/bash
# iso-void-install.sh -- stock-Void-ISO install onto InvariantFS (host side).
#
# Same shape as tools/iso-arch-install.sh, adapted to Void's live media:
# the ISO boots PRISTINE (checksum-verified against the pinned hash below)
# and the guest side is automated WITHOUT any installer TUI -- Void's
# Handbook documents the install as CLI already (XBPS method: mount the
# target, `xbps-install -r`). The automation hook differs from Arch:
# archiso runs script= for us, Void's dracut initramfs has no such hook,
# so the driver boots with rd.break=pre-pivot and drives the debug shell
# over a bidirectional serial pipe (the repo's wait_marker idea, extended
# to serial *input*), then chroots into the live root and runs the single
# guest command. Everything InvFS-specific comes from the local server.
#
#   WORK=/path/to/work bash tools/iso-void-install.sh
#
# Needs: qemu-system-x86_64 (KVM, else TCG fallback), sudo (ISO loop-mount),
# ~12 GB scratch, network. Prints INVFS-ISO-SETUP: PASS/FAIL from the guest
# serial and exits 0/1, so CI can gate on it directly.
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"
# Pinned ISO (policy: reproducible inputs; bump with checksum together).
ISO_VER="20250202"
ISO_URL="https://repo-default.voidlinux.org/live/current/void-live-x86_64-$ISO_VER-base.iso"
# Upstream publishes BSD-style "SHA256 (file) = hash"; normalized to GNU here.
ISO_SHA256="0f7439f500740f62dd18972cae448cec7d8a85032c7eb8f1bf946100d9a92161  void-live-x86_64-$ISO_VER-base.iso"
# Kernel+initramfs paths INSIDE the ISO (confirmed against 20250202).
ISO_KERNEL="boot/vmlinuz"
ISO_INITRD="boot/initrd"
# Break prompt: dracut's emergency_shell prints 'Dropping to debug shell.'
# and sets PS1 to '<name>:${PWD}# ' (here 'pre-pivot:/# '). Match the
# banner, not the prompt: the prompt also matches our own command echo.
BREAK_BANNER="Dropping to debug shell."
# dracut live root lookup (confirmed: volume label of the ISO).
ISO_LABEL="VOID_LIVE"
PORT="${INVFS_ISO_PORT:-8001}"
MEM="${INVFS_ISO_MEM:-6G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-100}"
ACCEL="${INVFS_ACCEL:-auto}"

ISO="$WORK/void-live-x86_64-$ISO_VER-base.iso"
VOL="$WORK/vol.img"
SER="$WORK/serial.log"
SPIPE="$WORK/serial.pipe"
SLOG="$WORK/serve.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"
fail() { echo "iso-void-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-void-install: $*"; }

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"

# Strays from a killed run hold the volume lock, the port, or the pipes.
P=$(ps -eo pid,args | grep "[v]oidisosearchuuid" | awk '{print $1}' || true)
[ -n "$P" ] && { kill -KILL $P 2>/dev/null; note "killed stray qemu: $P"; }
Q=$(ps -eo pid,args | grep "[i]nvfs-serve.sh" | awk '{print $1}' || true)
[ -n "$Q" ] && { kill -KILL $Q 2>/dev/null; note "killed stray server: $Q"; }
rm -f "$SPIPE.in" "$SPIPE.out"
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
cp "$MNT/$ISO_KERNEL" "$WORK/vmlinuz-void" || { sudo umount "$MNT"; fail "no $ISO_KERNEL in ISO"; }
cp "$MNT/$ISO_INITRD" "$WORK/initramfs-void.img" || { sudo umount "$MNT"; fail "no $ISO_INITRD in ISO"; }
sudo umount "$MNT"

note "building the release artifact the guest will install"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/tools/iso-guest-void-setup.sh" "$SERVEDIR/"
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
cleanup() { kill $SRVPID 2>/dev/null; exec 9>&- 2>/dev/null; }
trap cleanup EXIT
sleep 1

# Bidirectional serial: qemu pipe: creates .in (driver->guest) and
# .out (guest->driver). Open order matters: the reader first (backgrounded
# cat blocks until qemu opens .out), qemu next, and only then the writer
# (opening .in O_WRONLY blocks until qemu opens it for reading -- opening
# it before qemu starts deadlocks the driver).
mkfifo "$SPIPE.in" "$SPIPE.out" 2>/dev/null || true
: > "$SER"
cat "$SPIPE.out" >>"$SER" &
serial_send() { printf '%s\n' "$1" >&9; }
serial_wait() { # serial_wait <pattern> <timeout-10s-units> ; 0 on match
    local pat=$1 n=$2 i
    for i in $(seq 1 "$n"); do
        sleep 10
        grep -aq "$pat" "$SER" 2>/dev/null && return 0
    done
    return 1
}

note "starting qemu (accel=$ACCEL, stock ISO kernel+initramfs, rd.break hook)"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-void" -initrd "$WORK/initramfs-void.img" \
  -append "root=live:CDLABEL=$ISO_LABEL rd.live.image ip=dhcp rd.break=pre-pivot console=ttyS0,115200 voidisosearchuuid=$ISO_LABEL" \
  -cdrom "$ISO" \
  -drive "file=$VOL,format=raw,if=virtio" \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -serial "pipe:$SPIPE" -display none -no-reboot &
QPIDE=$!
# Writer only now that qemu has both pipe ends open (see comment above).
exec 9>"$SPIPE.in"
note "qemu pid $QPIDE; waiting for dracut break (up to $TIMEOUT_MIN min)"
N=$(( TIMEOUT_MIN * 6 ))
serial_wait "$BREAK_BANNER" "$N" || fail "dracut break prompt never appeared (see $SER)"
note "break shell up; entering live root and running guest"
# One chrooted command, no nested prompts: the driver only ever waits for
# echo-markers and the final PASS, never for a shell prompt (prompt text
# also matches our own echo, so matching it proves nothing).
serial_send "chroot /sysroot /bin/bash -c 'curl -fsS http://10.0.2.2:$PORT/iso-guest-void-setup.sh | bash'"
note "guest launched; waiting for marker (up to $TIMEOUT_MIN min)"
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
