#!/bin/bash
# Boot the InvariantFS rescue medium for real and use it.
#
# "The initramfs exists" is not the same claim as "the initramfs works". This
# builds a volume, builds the initramfs, boots it under QEMU with the volume
# on the bus, and asserts the only things that matter:
#
#   1. the volume is FOUND on the bus (so the block driver is present),
#   2. it MOUNTS,
#   3. it serves back the EXACT bytes that were written (bit-exactness, read
#      through a real FUSE mount inside a guest),
#   4. invf-verify on the raw device reports CLEAN,
#   5. the guest did not panic.
#
# The check runs from inside the guest via the `invfs.selftest` cmdline flag
# rather than by typing at the console. That is not a convenience: this
# console DROPS CHARACTERS. A character-at-a-time sender with a 12 ms gap
# still delivered "ls /mnt/invfs" as "l/ntnf", so a harness that types
# commands measures the tty and then reports a filesystem failure. Three
# runs were lost to that before the flag existed.
#
# Requires: qemu-system-x86_64, KVM (sudo -n), a kernel with a matching
# /lib/modules tree. Deliberately NOT part of `make e2e` (AGENTS.md 2.2 puts
# the slow environment-dependent legs out of band): it needs sudo, takes
# minutes, and wants memory. Run it directly or from `make e2e-slow`.
set -u
cd "$(dirname "$0")/.." || exit 2

W="${INVFS_BOOT_W:-/srv/bench/invfs-boot}"
KERNEL="${INVFS_BOOT_KERNEL:-/boot/vmlinuz-$(uname -r)}"
MEM="${INVFS_BOOT_MEM:-2048}"
BOOT_TIMEOUT="${INVFS_BOOT_TIMEOUT:-240}"
SERIAL="$W/serial.log"
VOL="$W/rescue.img"
RC=0

fail() { echo "FAIL: $*" >&2; RC=1; }
note() { echo "  $*"; }

cleanup() {
    # Kill the PROCESS GROUP, not the process we spawned. qemu runs under
    # `sudo`, so killing the wrapper leaves the real qemu alive holding the
    # image lock, and every later run then dies with "Failed to get write
    # lock" -- which reads like a filesystem problem. This cost three runs.
    if [ -n "${QPGID:-}" ]; then
        kill -9 -- "-$QPGID" 2>/dev/null
    fi
    # Belt and braces: anything still holding this image is ours.
    for p in $(ps -eo pid,comm 2>/dev/null | awk '/qemu-system/{print $1}'); do
        sudo -n kill -9 "$p" 2>/dev/null
    done
}
trap cleanup EXIT

rm -rf "$W"; mkdir -p "$W/src" || exit 2
note "workdir $W"

# ---- preconditions ---------------------------------------------------------
[ -x bin/invf-mkfs ] || { echo "run make first" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { fail "qemu-system-x86_64 not installed"; exit 1; }
[ -r "$KERNEL" ] || { fail "kernel not readable: $KERNEL"; exit 1; }
if [ ! -e /dev/kvm ]; then
    fail "/dev/kvm absent; this needs KVM (sudo -n qemu ... accel=kvm)"
    exit 1
fi
sudo -n true 2>/dev/null || { fail "sudo -n does not work; cannot start qemu as root"; exit 1; }

# ---- the payload -----------------------------------------------------------
# A file whose exact bytes are the assertion. Not a random blob: a named,
# readable file is what an operator would check, and its text appears in the
# failure output when the check fails.
printf 'InvariantFS rescue medium self-test payload\nsecond line: bit-exactness or nothing\n' \
    > "$W/src/invfs-selftest.txt"
for i in 1 2 3; do head -c 65536 /dev/urandom > "$W/src/blob.$i.bin"; done
# A symlink, because the fsck gate at the end of this file was passing for
# the wrong reason. That gate is `invf-fsck "$VOL" | grep -q "^OK"`, and
# the volume carried no symlink -- so it could not fire for a checker that
# treats a symlink's content as an AST recipe. It did not, and every
# healthy rootfs volume was reported DAMAGED: /bin, /sbin, /lib, /lib64,
# /usr/bin/sh and the ld.so are all symlinks, and a bootable Linux root
# filesystem cannot exist without them. A gate that cannot fail is the same
# class of defect as the bug it missed. The target is a file that is in
# this same payload, so the link resolves and the guest can follow it.
LINKTGT='invfs-selftest.txt'
ln -s "$LINKTGT" "$W/src/invfs-selftest.link"
WANT=$(md5sum "$W/src/invfs-selftest.txt" | cut -d' ' -f1)
note "self-test md5 $WANT"

INVFS_V3=1 bin/invf-mkfs "$VOL" 0.3 >"$W/mkfs.log" 2>&1 \
    || { cat "$W/mkfs.log"; fail "mkfs"; exit 1; }
bin/invf-import "$VOL" "$W/src" >"$W/import.log" 2>&1 \
    || { tail -5 "$W/import.log"; fail "import"; exit 1; }
# Read it back on the HOST first. If the host round trip is wrong there is
# no point booting: the failure would be ours, not the guest's, and saying
# "the rescue medium lost the bytes" would be a lie.
GOT=$(bin/invf-cat "$VOL" invfs-selftest.txt 2>/dev/null | md5sum | cut -d' ' -f1)
[ "$GOT" = "$WANT" ] || { fail "host round trip already differs ($GOT); refusing to boot"; exit 1; }
note "host round trip byte-identical before boot"
# The symlink, byte-compared. `invf-verify --deep` is NOT the oracle here: it
# checks readability and length only, because invfs_ast_block_entry carries
# a pba and no content hash. cmp against the literal target string is.
# If this is empty the import silently skipped the link and every fsck
# assertion below is vacuous again, so it is a hard failure of its own.
bin/invf-cat "$VOL" invfs-selftest.link >"$W/link.out" 2>/dev/null
printf '%s' "$LINKTGT" | cmp -s - "$W/link.out" \
    || { fail "the imported symlink's target does not read back byte-exact"; exit 1; }
note "imported symlink target reads back byte-exact"

# ---- the medium ------------------------------------------------------------
INVFS_SELFTEST_SUM="$WANT" INVFS_INITRAMFS_OUT="$W/initramfs.cpio.gz" \
    bash tools/mkinitramfs.sh >"$W/build.log" 2>&1 \
    || { tail -8 "$W/build.log"; fail "initramfs build"; exit 1; }
grep -q "^self-test: expected md5" "$W/build.log" \
    || fail "the self-test sum was not baked into the image"
note "initramfs built with the expected sum staged in it"

# ---- boot ------------------------------------------------------------------
setsid sudo -n qemu-system-x86_64 \
    -machine q35,accel=kvm -cpu host -m "$MEM" -smp 2 -display none -no-reboot \
    -kernel "$KERNEL" -initrd "$W/initramfs.cpio.gz" \
    -append "console=ttyS0,115200 panic=-1 invfs.selftest" \
    -drive "file=$VOL,format=raw,if=virtio" \
    -serial "file:$SERIAL" -monitor none >"$W/qemu.log" 2>&1 &
sleep 1
QPGID=$(ps -eo pid,pgid,comm 2>/dev/null | awk '/qemu-system/{print $2; exit}')
note "qemu started (pgid ${QPGID:-?})"

# Wait for the self-test to finish, not for a shell and not for a boot banner.
# A banner is not a reliable marker: QEMU's serial file gets everything, but
# any marker inside it is a race against how fast the guest boots.
i=0
while [ "$i" -lt "$BOOT_TIMEOUT" ]; do
    grep -qa "SELFTEST: end" "$SERIAL" 2>/dev/null && break
    grep -qa "Kernel panic" "$SERIAL" 2>/dev/null && break
    sleep 2
    i=$((i + 2))
done

echo "=== what the guest said ==="
grep -aE "SELFTEST|virtio_blk|INVFS probe|INVFS_RAW|InvariantFS mounted|Kernel panic" \
    "$SERIAL" 2>/dev/null | sed 's/^/  /' | head -30
echo "==========================="

# ---- the assertions --------------------------------------------------------
if ! grep -qa "SELFTEST: end" "$SERIAL" 2>/dev/null; then
    if grep -qa "Kernel panic" "$SERIAL" 2>/dev/null; then
        fail "the guest PANICKED -- see $SERIAL"
    else
        fail "the self-test never completed within ${BOOT_TIMEOUT}s -- see $SERIAL"
    fi
fi
grep -qa "Kernel panic" "$SERIAL" 2>/dev/null && fail "the guest kernel panicked"

# The block driver. Without virtio_blk the disk does not exist as a block
# device and the init reports "no InvariantFS volume found on any block
# device", which is indistinguishable from a lost volume. Measured: the
# builder shipped only NIC modules, so a perfectly good volume was invisible.
grep -qa "virtio_blk virtio0: \[vda\]" "$SERIAL" 2>/dev/null \
    || fail "no virtio_blk disk in the guest -- the initramfs cannot see the volume"

grep -qa "SELFTEST: device=/dev/vd" "$SERIAL" 2>/dev/null \
    || fail "the guest did not identify the volume as its RAW device"
grep -qa "InvariantFS mounted" "$SERIAL" 2>/dev/null \
    || fail "the volume did not mount"

# The bit-exactness assertion, read back through a real FUSE mount.
grep -qa "SELFTEST: BYTES_MATCH" "$SERIAL" 2>/dev/null \
    || fail "the bytes read back in the guest differ from the bytes written"

grep -qa "state: CLEAN" "$SERIAL" 2>/dev/null \
    || fail "invf-verify did not report the volume CLEAN in the guest"

# The rescue medium must survive a volume with no rootfs in it. Measured
# before this check existed: it mounted, ran `exec chroot`, chroot failed
# because /sbin/init was absent, and because exec had REPLACED pid 1 the
# kernel panicked -- taking the repair tools with it.
grep -qa "Kernel panic" "$SERIAL" 2>/dev/null \
    && fail "the medium panicked instead of staying in the repair shell"
grep -qa "Staying in the repair shell" "$SERIAL" 2>/dev/null \
    || note "no 'Staying in the repair shell' line (expected only when the volume has no rootfs)"

# The volume must survive being mounted read-write by a guest.
if [ "$RC" = 0 ]; then
    cleanup
    sleep 2
    HOST=$(bin/invf-cat "$VOL" invfs-selftest.txt 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$HOST" = "$WANT" ] \
        || fail "the volume's bytes changed after the guest ran ($HOST)"
    # Same for the symlink, same reason: byte compare, not --deep.
    bin/invf-cat "$VOL" invfs-selftest.link >"$W/link.after" 2>/dev/null
    printf '%s' "$LINKTGT" | cmp -s - "$W/link.after" \
        || fail "the symlink target changed after the guest ran"
    # THE gate. It now runs on a volume that HAS a symlink, so it can
    # actually fail: before this payload gained one, an fsck that called
    # every healthy rootfs volume DAMAGED still passed here.
    bin/invf-fsck "$VOL" 2>&1 | grep -q "^OK" \
        || fail "invf-fsck is not OK on the volume after the guest ran"
    note "host re-read byte-identical and fsck OK after the guest ran"
fi

if [ "$RC" = 0 ]; then
    echo "PASS: the rescue medium finds the volume, mounts it, serves the exact bytes, verifies CLEAN, and survives a volume with no rootfs"
    echo "      serial log: $SERIAL"
fi
exit $RC
