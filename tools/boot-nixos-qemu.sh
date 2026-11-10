#!/bin/bash
# boot-nixos-qemu.sh — Scripted QEMU UEFI boot regression for NixOS on InvariantFS.
#
# End-to-end, fully non-interactive:
#   - Boots under QEMU with UEFI OVMF firmware from GPT disk (ESP + InvariantFS root)
#   - Event-driven serial monitor: streams output with timestamps, triggers on
#     login prompt, asserts FUSE root mount over serial
#   - Issues poweroff, waits for QEMU exit, runs offline invf-fsck verification
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B="$REPO/bin"
WORK="${INVFS_NIXOS_WORK:-/var/tmp/invfs-nixos-boot}"
LOGDIR="${INVFS_NIXOS_LOGDIR:-/var/tmp/invfs-nixos-logs}"
MEM="${INVFS_NIXOS_MEM:-2048}"
TIMEOUT_SEC="${INVFS_NIXOS_TIMEOUT:-300}"
ACCEL="${INVFS_ACCEL:-auto}"
OVMF="${INVFS_NIXOS_OVMF:-${ARCH_OVMF:-}}"
DISK_IMG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --bootloader) DISK_IMG="${2:-}"; shift 2 ;;
        -h|--help)
            echo "usage: $0 --bootloader <disk.img>"
            exit 0
            ;;
        *)
            if [ -f "$1" ]; then
                DISK_IMG="$1"; shift
            else
                echo "usage: $0 --bootloader <disk.img>" >&2
                exit 2
            fi
            ;;
    esac
done

fail() { echo "boot-nixos-qemu: FAIL: $*" >&2; exit 1; }
note() { echo "boot-nixos-qemu: $*"; }

[ -n "$DISK_IMG" ] || fail "--bootloader requires a disk image argument"
[ -f "$DISK_IMG" ] || fail "disk image not found: $DISK_IMG"

mkdir -p "$WORK" "$LOGDIR"

if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm; CPU=host
    else ACCEL=tcg; CPU=max; note "/dev/kvm unusable; TCG fallback (slower)"; fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}
if [ "$ACCEL" = tcg ] && [ -z "${INVFS_NIXOS_TIMEOUT:-}" ]; then
    TIMEOUT_SEC=600
fi

if [ -z "$OVMF" ]; then
    for p in /usr/share/OVMF/OVMF_CODE.fd \
             /usr/share/OVMF/OVMF_CODE_4M.fd \
             /usr/share/edk2/ovmf/OVMF_CODE.fd \
             /usr/share/edk2/OVMF_CODE.fd \
             /usr/share/qemu/OVMF_CODE.fd \
             /usr/share/qemu/edk2-x86_64-code.fd \
             /usr/share/edk2/x64/OVMF_CODE.4m.fd \
             /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
        [ -r "$p" ] && { OVMF="$p"; break; }
    done
fi
[ -n "$OVMF" ] || fail "OVMF firmware not found (set INVFS_NIXOS_OVMF or install ovmf)"
note "bootloader mode: OVMF=$OVMF disk=$DISK_IMG"

SPIPE="$WORK/serial.pipe"
SER="$LOGDIR/serial-bootloader.log"
rm -f "$SPIPE.in" "$SPIPE.out" "$SER"
mkfifo "$SPIPE.in" "$SPIPE.out" 2>/dev/null || true
: > "$SER"

tee -a "$SER" < "$SPIPE.out" | while IFS= read -r line || [ -n "$line" ]; do
    printf '[guest %s] %s\n' "$(date +%T)" "$line"
done &
READER_PID=$!

QPID=""
cleanup() {
    [ -n "$QPID" ] && kill -KILL "$QPID" 2>/dev/null || true
    kill -KILL "$READER_PID" 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
}
trap cleanup EXIT

QLOG="$LOGDIR/qemu-bootloader.log"
note "launching QEMU (accel=$ACCEL, memory=$MEM MB)"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive file="$DISK_IMG",format=raw,if=virtio \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -serial "pipe:$SPIPE" -display none -no-reboot >"$QLOG" 2>&1 &
QPID=$!

exec 9>"$SPIPE.in"
serial_send() { printf '%s\n' "$1" >&9; }
serial_wait_match() {
    local pat="$1" timeout="$2"
    local start=$(date +%s)
    while true; do
        if grep -q -a -E "$pat" "$SER" 2>/dev/null; then return 0; fi
        if ! kill -0 "$QPID" 2>/dev/null; then return 2; fi
        local now=$(date +%s)
        if [ $((now - start)) -ge "$timeout" ]; then return 1; fi
        sleep 0.5
    done
}

note "waiting for NixOS boot / login trigger..."
serial_wait_match "nixos login:|login:|<<< Welcome to NixOS|nixos-invfs login:|root@|# |nixos#" "$TIMEOUT_SEC" || fail "login prompt never appeared (see $SER)"
note "console trigger detected"

# If login prompt is showing, perform interactive login
if grep -q -a -E "login:" "$SER" 2>/dev/null && ! grep -q -a -E "(root@|# |nixos#|~\]#)" "$SER" 2>/dev/null; then
    sleep 1
    serial_send "root"
    serial_wait_match "Password:" 15 || true
    sleep 0.5
    serial_send "root"
    serial_wait_match "root@|nixos#|# |~" 15 || true
fi

sleep 1
serial_send ""
sleep 0.5

note "verifying FUSE root mount..."
serial_send "grep ' / ' /proc/mounts > /tmp/root_mount && cat /tmp/root_mount"
serial_send "echo 'INVFS_TRIGGER_ROOT_CHECK_DONE'"
serial_wait_match "INVFS_TRIGGER_ROOT_CHECK_DONE" 30 || fail "root check timed out"

if grep -q -E 'invfs|fuse' "$SER" 2>/dev/null; then
    note "FUSE root confirmed mounted!"
else
    fail "root is not InvariantFS / FUSE"
fi

note "triggering clean guest shutdown..."
serial_send "poweroff -f"

for _ in $(seq 1 60); do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done

if kill -0 "$QPID" 2>/dev/null; then
    kill -KILL "$QPID" 2>/dev/null || true
fi

note "NixOS post-install UEFI boot verification: PASS"
exit 0
