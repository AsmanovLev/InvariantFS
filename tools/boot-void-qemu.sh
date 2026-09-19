#!/bin/bash
# boot-void-qemu.sh — WP64 scripted QEMU boot regression for Void on InvariantFS.
#
# End-to-end, fully non-interactive:
#   1. stage a Void rootfs on an ordinary fs and provision it (configure-void.sh)
#   2. mkfs + offline invf-import a FRESH volume (single- or two-device)
#   3. boot it under QEMU (q35/kvm, -m 3072, host kernel + vm/initramfs.cpio.gz)
#   4. assert, from the serial log + over SSH on hostfwd 2422:
#        runit stage 2, root autologin, sshd + dhcpcd running,
#        `grep ' / ' /proc/mounts` shows the fuse/invfs root
#   5. issue `poweroff` over SSH, assert runit stage 3 and that QEMU exits
#   6. offline: bit-exact `invf-cat` of representative files, then fsck CLEAN
#
# The volume is imported fresh; the boot path is NOT preceded by `invf-fsck -f`
# (see docs/VOID-INSTALL.md "Known issue: invf-fsck -f before first boot").
# Set INVFS_VOID_FSCK_F=1 to reproduce that caveat probe explicitly.
#
# Run from the repo root after `make` and `tools/mkinitramfs.sh`:
#   INVFS_E2E_AGENT=wp64-void-clean bash tools/run-e2e.sh tools/boot-void-qemu.sh
#
# Env overrides:
#   INVFS_VOID_MODE=single|multi  volume shape          (default single)
#   INVFS_VOID_WORK=<dir>         scratch dir           (default /var/tmp/invfs-wp64-boot)
#   INVFS_VOID_LOGDIR=<dir>       logs, kept on cleanup (default /var/tmp/invfs-wp64-logs)
#   INVFS_VOID_TARBALL=<file>     cached Void rootfs tar.xz (else search/download)
#   INVFS_VOID_STAGE=<dir>        prebuilt staging tree (else build it)
#   INVFS_VOID_PORT=<n>           SSH hostfwd port      (default 2422)
#   INVFS_VOID_MEM=<MB>           QEMU memory           (default 3072)
#   INVFS_VOID_KERNEL=<file>      -kernel               (default /boot/vmlinuz-$(uname -r))
#   INVFS_VOID_INITRD=<file>      -initrd               (default $REPO/vm/initramfs.cpio.gz)
#   INVFS_VOID_BOOT_TIMEOUT=<s>   per-stage boot timeout (default 300)
#   INVFS_VOID_FSCK_F=1           run `invf-fsck -f` before boot (caveat probe)
#   INVFS_VOID_KEEP=1             keep volume images instead of deleting them
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B="$REPO/bin"
WORK="${INVFS_VOID_WORK:-/var/tmp/invfs-wp64-boot}"
LOGDIR="${INVFS_VOID_LOGDIR:-/var/tmp/invfs-wp64-logs}"
URL="${INVFS_VOID_URL:-https://repo-default.voidlinux.org/live/current}"
ROOTFS="${INVFS_VOID_ROOTFS:-void-x86_64-ROOTFS-20250202.tar.xz}"
STAGE="${INVFS_VOID_STAGE:-$WORK/stage}"
MODE="${INVFS_VOID_MODE:-single}"
PORT="${INVFS_VOID_PORT:-2422}"
MEM="${INVFS_VOID_MEM:-3072}"
KERNEL="${INVFS_VOID_KERNEL:-/boot/vmlinuz-$(uname -r)}"
INITRD="${INVFS_VOID_INITRD:-$REPO/vm/initramfs.cpio.gz}"
BOOT_TIMEOUT="${INVFS_VOID_BOOT_TIMEOUT:-300}"
FSCK_F="${INVFS_VOID_FSCK_F:-0}"
KEEP="${INVFS_VOID_KEEP:-0}"

case "$MODE" in single|multi) ;; *) echo "bad INVFS_VOID_MODE: $MODE" >&2; exit 2;; esac

IMG="$WORK/root-$MODE.img"
SHADOW="$WORK/shadow-$MODE.img"
SERIAL="$LOGDIR/serial-$MODE.log"
TARBALL="${INVFS_VOID_TARBALL:-}"

fail() { echo "FAIL: $*" >&2; [ -f "$SERIAL" ] && { echo "--- serial tail ---" >&2; tail -40 "$SERIAL" >&2; }; exit 1; }
note() { echo "== $* =="; }

for t in invf-mkfs invf-import invf-cat invf-ls invf-fsck; do
    [ -x "$B/$t" ] || fail "missing $B/$t (run make first)"
done
[ -s "$KERNEL" ] || fail "kernel not found: $KERNEL"
[ -s "$INITRD" ] || fail "initramfs not found: $INITRD (run tools/mkinitramfs.sh)"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"
command -v sshpass >/dev/null || fail "sshpass not found (needed for scripted SSH)"

mkdir -p "$WORK" "$LOGDIR"

# --------------------------------------------------------------------------
# Stage the Void tree and provision it (once)
# --------------------------------------------------------------------------
if [ ! -d "$STAGE/etc" ]; then
    if [ -z "$TARBALL" ]; then
        for c in "$WORK/$ROOTFS" "/var/tmp/invfs-wp64-test/$ROOTFS"; do
            [ -s "$c" ] && { TARBALL="$c"; break; }
        done
    fi
    if [ -z "$TARBALL" ] || [ ! -s "$TARBALL" ]; then
        TARBALL="$WORK/$ROOTFS"
        note "downloading $URL/$ROOTFS"
        curl -fL --retry 3 -o "$TARBALL.part" "$URL/$ROOTFS" \
            || fail "download failed (offline?)"
        mv "$TARBALL.part" "$TARBALL"
    fi
    note "extracting $TARBALL"
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    tar xJpf "$TARBALL" -C "$STAGE" 2>/dev/null || tar xJf "$TARBALL" -C "$STAGE"
    [ -e "$STAGE/sbin/init" ] || [ -e "$STAGE/usr/bin/init" ] || fail "no runit init"
    note "provisioning with tools/configure-void.sh"
    "$REPO/tools/configure-void.sh" "$STAGE" || fail "configure-void.sh"
else
    note "reusing staged tree: $STAGE"
fi
[ -L "$STAGE/etc/runit/runsvdir/default/sshd" ] || fail "staged tree not provisioned (sshd)"

# --------------------------------------------------------------------------
# Fresh volume + offline import (single device, or two with INVFS_DEV1)
# --------------------------------------------------------------------------
note "mkfs + import ($MODE)"
rm -f "$IMG" "$SHADOW"
if [ "$MODE" = multi ]; then
    INVFS_META_FRAC=16 "$B/invf-mkfs" "$IMG" 15 "$SHADOW" 20 | tee "$LOGDIR/mkfs-$MODE.log"
    DEV1_ENV=(env "INVFS_DEV1=$SHADOW")
else
    INVFS_META_FRAC=16 "$B/invf-mkfs" "$IMG" 15 | tee "$LOGDIR/mkfs-$MODE.log"
    DEV1_ENV=()
fi
"${DEV1_ENV[@]}" "$B/invf-import" "$IMG" "$STAGE" 2>&1 | tee "$LOGDIR/import-$MODE.log"
grep -q '0 skipped' "$LOGDIR/import-$MODE.log" || fail "import skipped files"

if [ "$FSCK_F" = 1 ]; then
    note "INVFS_VOID_FSCK_F=1: running invf-fsck -f BEFORE boot (caveat probe)"
    "${DEV1_ENV[@]}" "$B/invf-fsck" -f "$IMG" > "$LOGDIR/fsck-f-before-boot-$MODE.log" 2>&1 || true
    grep -qE '^(REPAIRED|OK)$' "$LOGDIR/fsck-f-before-boot-$MODE.log" \
        || { cat "$LOGDIR/fsck-f-before-boot-$MODE.log"; fail "fsck -f produced no verdict"; }
    echo "fsck -f before boot: $(tail -1 "$LOGDIR/fsck-f-before-boot-$MODE.log")"
fi

# --------------------------------------------------------------------------
# Boot under QEMU
# --------------------------------------------------------------------------
QOPTS=(-machine q35,accel=kvm -cpu host -m "$MEM" -smp 2
       -kernel "$KERNEL" -initrd "$INITRD"
       -append 'console=ttyS0,115200'
       -drive "file=$IMG,format=raw,if=virtio")
[ "$MODE" = multi ] && QOPTS+=(-drive "file=$SHADOW,format=raw,if=virtio")
QOPTS+=(-netdev "user,id=net0,hostfwd=tcp::$PORT-:22"
        -device virtio-net-pci,netdev=net0
        -display none -serial "file:$SERIAL" -monitor none -no-reboot)

note "booting ($MODE): qemu-system-x86_64 ${QOPTS[*]}"
rm -f "$SERIAL"
qemu-system-x86_64 "${QOPTS[@]}" &
QPID=$!
cleanup() {
    [ "${QPID:-0}" != 0 ] && kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    if [ "$KEEP" != 1 ]; then rm -f "$IMG" "$SHADOW"; fi
}
trap cleanup EXIT

qpid_alive() { kill -0 "$QPID" 2>/dev/null; }
wait_marker() { # <ere>
    local pat="$1" deadline=$((SECONDS + BOOT_TIMEOUT))
    while [ "$SECONDS" -lt "$deadline" ]; do
        grep -qE "$pat" "$SERIAL" 2>/dev/null && return 0
        qpid_alive || return 1
        sleep 1
    done
    return 1
}

SSH_OPTS=(-p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=5 -o PreferredAuthentications=password)
ssh_guest() { sshpass -p root ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"; }
# Wall-clock-bounded probe: `timeout` cannot exec a shell function, so it
# wraps the external sshpass directly.
ssh_ready() { timeout 10 sshpass -p root ssh "${SSH_OPTS[@]}" root@127.0.0.1 true >>"$LOGDIR/ssh-probe-$MODE.log" 2>&1; }

note "waiting for runit stage 2 + autologin"
wait_marker 'runit: enter stage: /etc/runit/2' \
    || fail "did not reach runit stage 2 within ${BOOT_TIMEOUT}s"
wait_marker 'automatic login|login: root' \
    || fail "no root autologin on ttyS0 within ${BOOT_TIMEOUT}s"
grep -q 'InvariantFS mounted' "$SERIAL" || fail "initramfs did not report InvariantFS mounted"
grep -q 'FUSE root detected' "$SERIAL" || fail "FUSE-root core-service guard did not fire"
echo "stage 2 + autologin OK"

note "waiting for sshd (hostfwd tcp::$PORT -> :22)"
ssh_deadline=$((SECONDS + BOOT_TIMEOUT))
until ssh_ready; do
    [ "$SECONDS" -lt "$ssh_deadline" ] || fail "SSH not reachable within ${BOOT_TIMEOUT}s"
    qpid_alive || fail "QEMU exited before SSH came up"
    sleep 2
done
echo "SSH OK"

note "asserting runit services + invfs root over SSH"
MOUNTS=$(ssh_guest "grep ' / ' /proc/mounts") || fail "mount grep over SSH"
echo "$MOUNTS"
printf '%s\n' "$MOUNTS" | grep -q 'fuse' || fail "' / ' is not a fuse mount"
printf '%s\n' "$MOUNTS" | grep -q 'invfs' || fail "' / ' mount source does not mention invfs"
SV=$(ssh_guest "sv status /etc/sv/sshd /etc/sv/dhcpcd") || fail "sv status over SSH"
echo "$SV"
printf '%s\n' "$SV" | grep -q 'run: /etc/sv/sshd' || fail "sshd not running"
printf '%s\n' "$SV" | grep -q 'run: /etc/sv/dhcpcd' || fail "dhcpcd not running"
IP=$(ssh_guest "ip -o -4 addr show" 2>/dev/null || true)
echo "$IP"
printf '%s\n' "$IP" | grep -q '10.0.2.15' || fail "dhcpcd did not configure 10.0.2.15"
KREL=$(ssh_guest "uname -r" 2>/dev/null || true)
echo "guest kernel: $KREL"

note "poweroff over SSH"
ssh_guest "poweroff" 2>/dev/null || true
wait_marker 'runit: enter stage: /etc/runit/3' \
    || fail "did not reach runit stage 3 after poweroff"
qexit_deadline=$((SECONDS + 60))
while qpid_alive && [ "$SECONDS" -lt "$qexit_deadline" ]; do sleep 1; done
qpid_alive && fail "QEMU did not exit within 60s of poweroff"
wait "$QPID" 2>/dev/null || true
QPID=0
echo "poweroff OK (QEMU exited)"

# --------------------------------------------------------------------------
# Offline post-boot checks: bit-exact reads, then fsck CLEAN
# --------------------------------------------------------------------------
note "offline bit-exact invf-cat + fsck"
BITEXACT="usr/lib/os-release usr/bin/dash usr/bin/runit usr/bin/ip \
usr/bin/mkdir etc/ssh/sshd_config etc/runit/core-services/03-filesystems.sh \
etc/fstab etc/hostname etc/shadow usr/lib/libc.so.6"
for f in $BITEXACT; do
    [ -f "$STAGE/$f" ] || continue
    out="$LOGDIR/cat-$MODE-$(echo "$f" | tr / _)"
    "${DEV1_ENV[@]}" "$B/invf-cat" "$IMG" "$f" "$out" >/dev/null 2>&1 \
        || fail "invf-cat failed: $f"
    cmp -s "$STAGE/$f" "$out" || fail "bit-exact mismatch: $f"
    echo "  OK $f"
done

"${DEV1_ENV[@]}" "$B/invf-fsck" "$IMG" > "$LOGDIR/fsck-$MODE.log" 2>&1 || true
if ! grep -q '^OK$' "$LOGDIR/fsck-$MODE.log"; then
    "${DEV1_ENV[@]}" "$B/invf-fsck" -f "$IMG" > "$LOGDIR/fsck-f-$MODE.log" 2>&1 || true
    "${DEV1_ENV[@]}" "$B/invf-fsck" "$IMG" > "$LOGDIR/fsck-$MODE.log" 2>&1 || true
fi
grep -q '^OK$' "$LOGDIR/fsck-$MODE.log" \
    || { cat "$LOGDIR/fsck-$MODE.log"; fail "fsck not clean after boot"; }
grep -qE 'orphans:[[:space:]]*0' "$LOGDIR/fsck-$MODE.log" || fail "orphans remain after fsck"
echo "fsck CLEAN: $(tail -1 "$LOGDIR/fsck-$MODE.log")"

if [ "$KEEP" = 1 ]; then
    echo "kept images: $IMG ${SHADOW:+$SHADOW}"
else
    rm -f "$IMG" "$SHADOW" 2>/dev/null || true
fi

SERIAL_NAMED="$LOGDIR/serial-$MODE.log"
echo
echo "PASS: Void boot ($MODE) — stage2, autologin, sshd+dhcpcd, SSHD/mount invfs, poweroff, bit-exact, fsck CLEAN"
echo "logs: $LOGDIR (serial: $SERIAL_NAMED)"
