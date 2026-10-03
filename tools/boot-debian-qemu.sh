#!/bin/bash
# boot-debian-qemu.sh — KI3b: does systemd, as PID 1 on a FUSE root, come up?
#
# Fully non-interactive, mirroring tools/boot-void-qemu.sh:
#   1. mkfs + offline invf-import of the debootstrapped Debian root
#   2. boot it under QEMU (q35/kvm, host kernel + vm/initramfs.cpio.gz)
#   3. assert from the serial log AND over SSH on hostfwd:
#        PID 1 is systemd -- NOT runit (this is the whole point of this script)
#        systemd reached multi-user.target
#        ' / ' in /proc/mounts is a fuse mount mentioning invfs
#   4. poweroff over SSH and assert the guest actually leaves
#   5. offline: bit-exact invf-cat of representative files, then fsck CLEAN
#
# WHY DEBIAN, and why the init is named on the cmdline. Debian is the one
# readily available distro whose init is systemd as PID 1, and
# INCIDENTS.md's H1 says exactly what that costs: "early-mount units fail and
# daemon startup hangs" without /run and cgroup2. The initramfs already
# provides both (initramfs-init.sh:295-300, "WP66 H1") and takes the init from
# the cmdline (:320, get_opt invfs.init), so this harness names it explicitly
# rather than relying on /sbin/init resolution inside the root.
#
# Run from the repo root after `make` and `tools/mkinitramfs.sh`:
#   INVFS_DEB_STAGE=/mnt/invfs-scratch/debroot \
#     bash tools/boot-debian-qemu.sh
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B="$REPO/bin"
WORK="${INVFS_DEB_WORK:-/var/tmp/invfs-deb-boot}"
LOGDIR="${INVFS_DEB_LOGDIR:-$WORK-logs}"
STAGE="${INVFS_DEB_STAGE:-}"
PORT="${INVFS_DEB_PORT:-2426}"
MEM="${INVFS_DEB_MEM:-3072}"
KERNEL="${INVFS_DEB_KERNEL:-/boot/vmlinuz-$(uname -r)}"
INITRD="${INVFS_DEB_INITRD:-$REPO/vm/initramfs.cpio.gz}"
BOOT_TIMEOUT="${INVFS_DEB_BOOT_TIMEOUT:-420}"
MODE="${INVFS_DEB_MODE:-single}"

[ -n "$STAGE" ] || { echo "INVFS_DEB_STAGE must point at a provisioned Debian root"; exit 2; }
[ -d "$STAGE" ] || { echo "no such staged root: $STAGE"; exit 2; }
[ -x "$B/invf-mkfs" ] || { echo "run make first"; exit 2; }
[ -f "$INITRD" ] || { echo "no initramfs at $INITRD -- run tools/mkinitramfs.sh"; exit 2; }

mkdir -p "$WORK" "$LOGDIR"
RAW="$WORK/deb.img"; SHADOW="$WORK/deb-shadow.img"
SERIAL="$LOGDIR/serial.log"; IMG="$RAW"

say()  { echo "== $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- volume ----------------------------------------------------------------
if [ "$MODE" = multi ]; then
    say "mkfs (two-device)"
    "$B/invf-mkfs" "$RAW" 15 "$SHADOW" 20 > "$LOGDIR/mkfs.log" 2>&1 || {
        cat "$LOGDIR/mkfs.log"; fail "mkfs"; }
    grep -q 'devices:.*2' "$LOGDIR/mkfs.log" \
        || fail "two-device mkfs did not report devices: 2"
    export INVFS_DEV1="$SHADOW"
else
    say "mkfs (single)"
    "$B/invf-mkfs" "$RAW" 4 > "$LOGDIR/mkfs.log" 2>&1 || {
        cat "$LOGDIR/mkfs.log"; fail "mkfs"; }
fi

say "import the Debian root (systemd + dbus: heavier than Void's runit root)"
"$B/invf-import" "$RAW" "$STAGE" > "$LOGDIR/import.log" 2>&1 || {
    tail -5 "$LOGDIR/import.log"; fail "invf-import"; }
grep -o 'imported:.*' "$LOGDIR/import.log"

# ---- boot ------------------------------------------------------------------
# invfs.init is named EXPLICITLY so the assertion below is about systemd
# starting, not about /sbin/init happening to resolve.
CMDLINE="console=ttyS0,115200 invfs.init=/lib/systemd/systemd"
# Accel: KVM when the runner actually has it, TCG otherwise. A host that has
# /dev/kvm but refuses to open it (a container without the device mapped, or
# without the supplementary group) otherwise fails with "failed to initialize
# kvm", which reads like a QEMU problem rather than a host capability one --
# the same shape as the qcow2/qemu-img gate in the unit tier.
ACCEL="${INVFS_ACCEL:-auto}"
if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        ACCEL=kvm; CPU=host
    else
        ACCEL=tcg; CPU=max          # tcg has no host passthrough
        echo "note: /dev/kvm not usable here; falling back to TCG (slower)"
    fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}

QOPTS=(-machine "q35,accel=$ACCEL" -cpu "$CPU" -m "$MEM" -smp 2
       -kernel "$KERNEL" -initrd "$INITRD" -append "$CMDLINE"
       -drive "file=$RAW,format=raw,if=virtio"
       -netdev "user,id=net0,hostfwd=tcp::$PORT-:22"
       -device virtio-net-pci,netdev=net0
       -display none -serial "file:$SERIAL" -monitor none -no-reboot)
[ "$MODE" = multi ] && QOPTS+=(-drive "file=$SHADOW,format=raw,if=virtio")

say "booting: qemu-system-x86_64 -machine q35,accel=kvm $CMDLINE"
setsid qemu-system-x86_64 "${QOPTS[@]}" >/dev/null 2>&1 &
QPID=$!
qpid_alive() { kill -0 "$QPID" 2>/dev/null; }

wait_marker() {   # $1 = pattern, $2 = seconds
    local i=0
    while [ "$i" -lt "${2:-60}" ]; do
        grep -qE "$1" "$SERIAL" 2>/dev/null && return 0
        qpid_alive || return 1
        sleep 1; i=$((i + 1))
    done
    return 1
}

say "waiting for the FUSE root to be mounted and handed off"
wait_marker 'InvariantFS mounted' 180 || {
    tail -20 "$SERIAL" 2>/dev/null; fail "initramfs never mounted the volume"; }
wait_marker 'Chrooting to InvFS root' 60 || fail "no chroot into the volume"

say "waiting for systemd (this is the KI3b assertion)"
wait_marker 'systemd\[1\]|Starting systemd|systemd v2' "$BOOT_TIMEOUT" || {
    echo "--- last 30 lines of serial:"; tail -30 "$SERIAL"; \
    fail "systemd never announced itself within ${BOOT_TIMEOUT}s -- \
this is H1: 'daemon startup hangs'"; }
qpid_alive || { tail -30 "$SERIAL"; fail "guest died while systemd was starting"; }

say "waiting for multi-user.target"
wait_marker 'Reached target.*Multi-User|Startup finished' "$BOOT_TIMEOUT" || {
    echo "--- last 40 lines of serial:"; tail -40 "$SERIAL"; \
    fail "systemd did not reach multi-user.target within ${BOOT_TIMEOUT}s"; }
say "systemd reached multi-user.target"

# ---- assert over SSH -------------------------------------------------------
SSH_OPTS=(-p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o ConnectTimeout=20 -o LogLevel=ERROR)
ssh_guest()  { sshpass -p root ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"; }
ssh_ready()  { timeout 30 sshpass -p root ssh "${SSH_OPTS[@]}" root@127.0.0.1 true \
                   >>"$LOGDIR/ssh-probe.log" 2>&1; }

say "waiting for sshd"
deadline=$((SECONDS + BOOT_TIMEOUT))
until ssh_ready; do
    [ "$SECONDS" -lt "$deadline" ] || fail "sshd never became reachable"
    qpid_alive || fail "QEMU exited before sshd came up"
    sleep 3
done
say "SSH OK"

# THE KI3b ASSERTION, stated positively rather than inferred from the absence
# of runit: ask the guest what its PID 1 is.
PID1=$(ssh_guest "ps -p 1 -o comm=" 2>/dev/null | tr -d '[:space:]')
say "guest PID 1: ${PID1:-<unknown>}"
case "$PID1" in
    systemd) : ;;
    *) fail "PID 1 is '${PID1:-unknown}', not systemd -- this run does not \
exercise KI3b" ;;
esac

MOUNTS=$(ssh_guest "grep ' / ' /proc/mounts") || fail "mount grep over SSH"
echo "$MOUNTS"
printf '%s\n' "$MOUNTS" | grep -q 'fuse'  || fail "' / ' is not a fuse mount"
printf '%s\n' "$MOUNTS" | grep -q 'invfs' || fail "' / ' does not mention invfs"
say "' / ' is an invfs fuse mount"

SYSTEMD_STATE=$(ssh_guest "systemctl is-system-running" 2>/dev/null || true)
say "systemctl is-system-running: ${SYSTEMD_STATE:-<unknown>}"

say "poweroff over SSH"
ssh_guest "systemctl poweroff" 2>/dev/null || true
wait_marker 'reboot: (Power down|System Powered Off|Reached target.*(Power-Off|Reboot|Shutdown))' 120 \
    || fail "guest did not shut down within 120s of systemctl poweroff"
qpid_alive && { sleep 20; qpid_alive && fail "QEMU still running 20s after poweroff"; }
say "poweroff OK (QEMU exited)"

# ---- offline --------------------------------------------------------------
say "offline bit-exact invf-cat + fsck"
mkdir -p "$WORK/out"
for f in etc/hostname etc/passwd lib/systemd/systemd usr/bin/systemctl etc/os-release; do
    [ -f "$STAGE/$f" ] || continue
    "$B/invf-cat" "$IMG" "$f" "$WORK/out/$(basename "$f")" >/dev/null 2>&1 \
        || fail "invf-cat $f"
    cmp -s "$STAGE/$f" "$WORK/out/$(basename "$f")" \
        || fail "$f not bit-exact after the systemd boot"
    say "  $f bit-exact"
done

"$B/invf-fsck" "$IMG" > "$LOGDIR/fsck.log" 2>&1 || true
if ! grep -q '^OK$' "$LOGDIR/fsck.log"; then
    "$B/invf-fsck" -f "$IMG" > "$LOGDIR/fsck-f.log" 2>&1 || true
    "$B/invf-fsck" "$IMG" > "$LOGDIR/fsck.log" 2>&1 || true
fi
grep -q '^OK$' "$LOGDIR/fsck.log" || { cat "$LOGDIR/fsck.log"; fail "fsck not clean"; }
say "fsck CLEAN: $(tail -1 "$LOGDIR/fsck.log")"

echo
echo "PASS: Debian boot on InvariantFS -- PID 1 was systemd, multi-user.target"
echo "      reached, root is an invfs fuse mount, clean poweroff, bit-exact, fsck CLEAN"