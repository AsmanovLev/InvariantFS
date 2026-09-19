#!/bin/bash
# tools/boot-gentoo-qemu.sh — scripted, reproducible clean Gentoo-on-InvariantFS
# boot under QEMU (WP62 regression re-run).
#
# Automates the "Verified Bootstrap Path" / "Single-device volume" recipe in
# docs/GENTOO-INSTALL.md end to end:
#
#   truncate 15G sparse image -> INVFS_META_FRAC=16 invf-mkfs
#   -> offline invf-import of an already-extracted stage3 tree
#      (NEVER tar through FUSE)
#   -> invf-fuse mount + tools/configure-guest.sh + unmount
#   -> tools/mkinitramfs.sh
#   -> qemu-system-x86_64 direct-kernel boot (q35/KVM, -serial file:<log>)
#   -> assert the log has "InvariantFS mounted", runlevel 3 and a login prompt,
#      then log in over SSH on host port 2222
#   -> kill qemu and remove the image (the console log is kept).
#
# Usage:
#   tools/boot-gentoo-qemu.sh [--keep-image] [--help]
#
# Environment overrides (defaults match this host's WP62 layout):
#   WP62_DIR          work dir for image + log   /run/media/.../wp62
#   WP62_STAGE3       extracted stage3 root      /run/media/.../stage3-root
#   WP62_KERNEL       guest kernel               /boot/vmlinuz-$(uname -r)
#   WP62_LOG          console log path           $WP62_DIR/gentoo-boot.log
#   WP62_SSH_PORT     hostfwd/ssh port           2222
#   WP62_SIZE_GB      volume size                15
#   WP62_BOOT_TIMEOUT seconds per boot marker    240
#   WP62_SSH_TIMEOUT  seconds waiting for ssh     180
#   WP62_KEEP_IMAGE=1 keep the image (or pass --keep-image)
#
# Exit code 0 iff the full boot + ssh assertions pass.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/bin"
DISK=/run/media/user/a3277147-c47a-4b90-a3ec-6536c5d8c724

WP62_DIR=${WP62_DIR:-$DISK/wp62}
WP62_STAGE3=${WP62_STAGE3:-$DISK/stage3-root}
WP62_KERNEL=${WP62_KERNEL:-/boot/vmlinuz-$(uname -r)}
WP62_LOG=${WP62_LOG:-$WP62_DIR/gentoo-boot.log}
WP62_SSH_PORT=${WP62_SSH_PORT:-2222}
WP62_SIZE_GB=${WP62_SIZE_GB:-15}
WP62_BOOT_TIMEOUT=${WP62_BOOT_TIMEOUT:-240}
WP62_SSH_TIMEOUT=${WP62_SSH_TIMEOUT:-180}
KEEP_IMAGE=${WP62_KEEP_IMAGE:-0}

IMG="$WP62_DIR/root.img"
MNT="$WP62_DIR/mnt"
IMPORT_LOG="$WP62_DIR/import.log"
SSH_KEY="$WP62_DIR/id_ed25519"
SSH_OUT="$WP62_DIR/ssh.out"

while [ $# -gt 0 ]; do
    case "$1" in
        --keep-image) KEEP_IMAGE=1 ;;
        --help|-h) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

log() { printf '== %s\n' "$*"; }
die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

QPID=""
cleanup() {
    if [ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$QPID" 2>/dev/null || break
            sleep 0.5
        done
        kill -9 "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    if mountpoint -q "$MNT" 2>/dev/null; then
        fusermount3 -u "$MNT" 2>/dev/null || true
    fi
    if [ "$KEEP_IMAGE" = 1 ]; then
        log "kept image: $IMG"
    else
        rm -f "$IMG"
    fi
    log "console log kept: $WP62_LOG"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 0. Preconditions
# ---------------------------------------------------------------------------
[ -x "$BIN/invf-mkfs" ] || die "build first: make ($BIN/invf-mkfs missing)"
for t in truncate:coreutils qemu-system-x86_64:qemu ssh:openssh fusermount3:fuse3 \
         ssh-keygen:openssh; do
    cmd=${t%%:*}; pkg=${t##*:}
    command -v "$cmd" >/dev/null 2>&1 || die "$cmd not found (install $pkg)"
done
[ -d "$WP62_STAGE3" ] || die "stage3 tree not found: $WP62_STAGE3"
[ -f "$WP62_KERNEL" ] || die "kernel not found: $WP62_KERNEL"
[ -f "$ROOT/vm/initramfs/init" ] || log "WARN: vm/initramfs/init missing; mkinitramfs.sh will recreate"

mkdir -p "$WP62_DIR"
rm -f "$IMG" "$WP62_LOG" "$SSH_OUT"
[ -d "$MNT" ] && { fusermount3 -u "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true; }
mkdir -p "$MNT"

# ---------------------------------------------------------------------------
# 1. Format (15G sparse, rootfs-sized metadata zone)
# ---------------------------------------------------------------------------
log "stage 1: mkfs $IMG (${WP62_SIZE_GB}G sparse, INVFS_META_FRAC=16)"
truncate -s "${WP62_SIZE_GB}G" "$IMG"
INVFS_META_FRAC=16 "$BIN/invf-mkfs" "$IMG" "$WP62_SIZE_GB" | tee "$WP62_DIR/mkfs.log"

# ---------------------------------------------------------------------------
# 2. Offline import of the stage3 tree (never tar through FUSE)
# ---------------------------------------------------------------------------
log "stage 2: invf-import $WP62_STAGE3 -> $IMG"
"$BIN/invf-import" "$IMG" "$WP62_STAGE3" 2>&1 | tee "$IMPORT_LOG"
grep -q "0 skipped" "$IMPORT_LOG" || die "import skipped entries (see $IMPORT_LOG)"
STAGE_N=$(find "$WP62_STAGE3" -mindepth 1 | wc -l)
IMP_N=$(sed -n 's/.*imported: \([0-9]*\) dirs, \([0-9]*\) files, \([0-9]*\) symlinks, \([0-9]*\) specials.*/\1 \2 \3 \4/p' \
        "$IMPORT_LOG" | tail -1 | awk '{print $1+$2+$3+$4}')
[ -n "$IMP_N" ] || die "cannot parse import counts"
[ "$IMP_N" = "$STAGE_N" ] || die "imported $IMP_N != staged $STAGE_N"
log "import verified: $IMP_N objects (== find count)"
LS_N=$("$BIN/invf-ls" "$IMG" 2>/dev/null | wc -l)
log "invf-ls emitted $LS_N lines"

# ---------------------------------------------------------------------------
# 3. FUSE mount + configure-guest.sh + ssh key + unmount
# ---------------------------------------------------------------------------
log "stage 3: FUSE mount + configure-guest.sh"
"$BIN/invf-fuse" "$IMG" "$MNT"
for _ in $(seq 1 30); do mountpoint -q "$MNT" && break; sleep 1; done
mountpoint -q "$MNT" || die "invf-fuse did not mount $MNT"

"$ROOT/tools/configure-guest.sh" "$MNT"

# Ephemeral key so the SSH assertion is deterministic (the guest also
# accepts an empty root password, but key auth avoids PAM/empty-pass
# edge cases).  Files created through FUSE come back uid 1000, so chown.
rm -f "$SSH_KEY" "$SSH_KEY.pub"
ssh-keygen -q -t ed25519 -N '' -f "$SSH_KEY"
mkdir -p "$MNT/root/.ssh"
cat "$SSH_KEY.pub" > "$MNT/root/.ssh/authorized_keys"
chown -R root:root "$MNT/root/.ssh" 2>/dev/null || true
chmod 700 "$MNT/root/.ssh" 2>/dev/null || true
chmod 600 "$MNT/root/.ssh/authorized_keys" 2>/dev/null || true

sync
fusermount3 -u "$MNT"
for _ in $(seq 1 20); do mountpoint -q "$MNT" || break; sleep 1; done
mountpoint -q "$MNT" && die "could not unmount $MNT"
log "guest provisioned and volume unmounted"

# ---------------------------------------------------------------------------
# 4. Initramfs
# ---------------------------------------------------------------------------
log "stage 4: tools/mkinitramfs.sh"
"$ROOT/tools/mkinitramfs.sh" | tee "$WP62_DIR/mkinitramfs.log"
INITRD="$ROOT/vm/initramfs.cpio.gz"
[ -s "$INITRD" ] || die "no initramfs at $INITRD"

# ---------------------------------------------------------------------------
# 5. Boot QEMU (direct kernel, single device)
# ---------------------------------------------------------------------------
log "stage 5: qemu boot (m=3072, q35/kvm, ssh hostfwd :$WP62_SSH_PORT -> :22)"
qemu-system-x86_64 \
    -machine q35,accel=kvm -cpu host -m 3072 -smp 2 \
    -kernel "$WP62_KERNEL" -initrd "$INITRD" \
    -append 'console=ttyS0,115200' \
    -drive file="$IMG",format=raw,if=ide \
    -netdev "user,id=net0,hostfwd=tcp::${WP62_SSH_PORT}-:22" \
    -device virtio-net-pci,netdev=net0 \
    -display none -serial file:"$WP62_LOG" -monitor none -no-reboot &
QPID=$!

wait_log() { # <fixed-string> <timeout-s>
    local pat="$1" to="$2" i=0
    while [ "$i" -lt "$to" ]; do
        grep -qF -- "$pat" "$WP62_LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 3
        sleep 1; i=$((i + 1))
    done
    return 1
}

fatal_boot() {
    echo "--- tail of $WP62_LOG ---" >&2
    tail -40 "$WP62_LOG" 2>/dev/null >&2 || true
    die "$1"
}

wait_log "InvariantFS mounted" "$WP62_BOOT_TIMEOUT" \
    || fatal_boot "no 'InvariantFS mounted' within ${WP62_BOOT_TIMEOUT}s"
log "marker OK: InvariantFS mounted"

wait_log "Entering runlevel: 3" "$WP62_BOOT_TIMEOUT" \
    || fatal_boot "no runlevel 3 within ${WP62_BOOT_TIMEOUT}s"
log "marker OK: Entering runlevel: 3"

wait_log "login:" "$WP62_BOOT_TIMEOUT" \
    || fatal_boot "no login prompt within ${WP62_BOOT_TIMEOUT}s"
log "marker OK: login prompt"

# ---------------------------------------------------------------------------
# 6. SSH assertion (host port -> guest :22)
# ---------------------------------------------------------------------------
SSH_OPTS=(-i "$SSH_KEY" -p "$WP62_SSH_PORT" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=5 -o BatchMode=yes)
ssh_ok=0
for _ in $(seq 1 "$WP62_SSH_TIMEOUT"); do
    if ssh "${SSH_OPTS[@]}" root@127.0.0.1 \
            'echo INVFS_SSH_OK; uname -r; hostname; mount | grep " / "' \
            >"$SSH_OUT" 2>/dev/null; then
        ssh_ok=1; break
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 2
done
if [ "$ssh_ok" != 1 ]; then
    echo "--- ssh attempt output ---" >&2
    cat "$SSH_OUT" 2>/dev/null >&2 || true
    fatal_boot "ssh root@127.0.0.1 -p $WP62_SSH_PORT did not succeed"
fi
grep -qF "INVFS_SSH_OK" "$SSH_OUT" || die "ssh ran but produced no marker"
log "SSH login OK on port $WP62_SSH_PORT:"
sed 's/^/   /' "$SSH_OUT"

log "PASS: clean Gentoo boot on InvariantFS (scripted)"
