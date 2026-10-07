#!/bin/bash
# boot-arch-qemu.sh — scripted QEMU boot of an Arch-on-InvariantFS root.
#
# Boots a (freshly imported, NOT fsck -f'd — see docs/VOID-INSTALL.md:237)
# InvariantFS volume with the host kernel + vm/initramfs.cpio.gz, waits for
# the serial markers, then asserts over SSH that Arch Linux is running with
# the volume mounted as "/". Single- and two-device (INVFS_DEV1) variants.
#
# Usage:
#   tools/boot-arch-qemu.sh --single <root.img>
#   tools/boot-arch-qemu.sh --multi  <root.img> <shadow.img>
#   tools/boot-arch-qemu.sh --bootloader <disk.img>     # OVMF + GRUB from ESP
#
# The --bootloader mode uses OVMF firmware and boots a GPT disk image built
# by mkdisk-arch.sh (ESP with GRUB + kernel + initramfs, InvFS partition).
# The default --single/--multi modes use -kernel/-initrd (host kernel).
#
# Env overrides:
#   ARCH_PORT      host ssh forward port        (default 2322)
#   ARCH_RAM       guest RAM in MB              (default 3072)
#   ARCH_KERNEL    host kernel                  (default /boot/vmlinuz-$(uname -r))
#   ARCH_INITRD    initramfs                    (default <repo>/vm/initramfs.cpio.gz)
#   ARCH_LOG       serial log path              (default /tmp/invfs-arch-boot-<mode>.log)
#   ARCH_SSH_LOG   ssh transcript path          (default ${ARCH_LOG%.log}.ssh.log)
#   ARCH_SSH_PASS  root password                (default root)
#   ARCH_TIMEOUT   seconds to wait for markers  (default 300)
#   ARCH_REBOOT_WAIT seconds to wait for qemu   (default 30)
#   ARCH_OVMF      OVMF firmware path           (auto-detected if unset)
#   ARCH_DISK      disk image for --bootloader  (overrides positional arg)
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# Positional flags, or ARCH_MODE/ARCH_IMG/ARCH_SHADOW from the environment
# (run-e2e.sh --bg cannot forward arguments, only the environment).
MODE="${ARCH_MODE:-}"; IMG="${ARCH_IMG:-}"; SHADOW="${ARCH_SHADOW:-}"

BOOTLOADER=0
OVMF=""
DISK_IMG="${ARCH_DISK:-}"

if [ "$#" -gt 0 ]; then
    case "$1" in
      --single) MODE=single; IMG="${2:-}"; shift 2 2>/dev/null || true;;
      --multi)  MODE=multi;  IMG="${2:-}"; SHADOW="${3:-}"; shift 3 2>/dev/null || true;;
      --bootloader) MODE=bootloader; BOOTLOADER=1; DISK_IMG="${DISK_IMG:-${2:-}}"; shift 2 2>/dev/null || true;;
      -h|--help) sed -n '2,30p' "$0"; exit 0;;
      *) echo "usage: $0 --single <root.img> | --multi <root.img> <shadow.img> | --bootloader <disk.img>"; exit 2;;
    esac
fi
[ -n "$MODE" ] || MODE=single

PORT="${ARCH_PORT:-2322}"
RAM="${ARCH_RAM:-3072}"
KERNEL="${ARCH_KERNEL:-/boot/vmlinuz-$(uname -r)}"
INITRD="${ARCH_INITRD:-$REPO/vm/initramfs.cpio.gz}"
LOG="${ARCH_LOG:-/tmp/invfs-arch-boot-$MODE.log}"
SSHLOG="${ARCH_SSH_LOG:-${LOG%.log}.ssh.log}"
PASS="${ARCH_SSH_PASS:-root}"
TIMEOUT="${ARCH_TIMEOUT:-300}"
REBOOT_WAIT="${ARCH_REBOOT_WAIT:-30}"
SSH_BASE="ssh -p $PORT -o StrictHostKeyChecking=no \
     -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10"
KEY="${ARCH_SSH_KEY:-$HOME/.ssh/id_ed25519}"
if [ "${ARCH_SSH_MODE:-}" = password ] || [ ! -r "$KEY" ]; then
    command -v sshpass >/dev/null || { echo "FAIL: need $KEY or sshpass"; exit 1; }
    SSH="sshpass -p $PASS $SSH_BASE -o PreferredAuthentications=password"
else
    SSH="$SSH_BASE -i $KEY -o IdentitiesOnly=yes -o PreferredAuthentications=publickey"
fi

# -- OVMF firmware (only needed for --bootloader) ----------------------------
if [ "$BOOTLOADER" = 1 ]; then
    if [ -z "$OVMF" ]; then
        for p in /usr/share/OVMF/OVMF_CODE.fd \
                 /usr/share/edk2/ovmf/OVMF_CODE.fd \
                 /usr/share/edk2/OVMF_CODE.fd \
                 /usr/share/qemu/OVMF_CODE.fd; do
            [ -r "$p" ] && { OVMF="$p"; break; }
        done
    fi
    [ -n "$OVMF" ] || { echo "FAIL: OVMF firmware not found (set ARCH_OVMF)"; exit 1; }
    [ -n "$DISK_IMG" ] || { echo "FAIL: --bootloader requires a disk image (set ARCH_DISK or pass arg)"; exit 1; }
    [ -f "$DISK_IMG" ] || { echo "FAIL: disk image not found: $DISK_IMG"; exit 1; }
fi

fail() { echo "FAIL($MODE): $*"; FAILED=$((FAILED+1)); }
note() { echo ":: $*"; }

FAILED=0
if [ "$BOOTLOADER" = 1 ]; then
    [ -f "$DISK_IMG" ] || { echo "FAIL: no such disk: $DISK_IMG"; exit 1; }
else
    [ -f "$IMG" ] || { echo "FAIL: no such volume: $IMG"; exit 1; }
    if [ "$MODE" = multi ]; then
        [ -f "$SHADOW" ] || { echo "FAIL: no such shadow volume: $SHADOW"; exit 1; }
    fi
    # -e, not -r: readable_kernel() is what makes an unreadable kernel
      # usable, and it runs LATER in this script. Requiring readability here
      # made the fix unreachable -- the Arch step died with
      #     FAIL: no kernel: /boot/vmlinuz-6.8.0-1064-azure
      # before the code that solves that exact problem was ever reached.
      [ -e "$KERNEL" ] || { echo "FAIL: no kernel: $KERNEL"; exit 1; }
    [ -r "$INITRD" ] || { echo "FAIL: no initramfs: $INITRD"; exit 1; }
fi
# The host kernel is not always readable by the user running this.
#
# GitHub runners ship /boot/vmlinuz-<ver> root-only, so qemu fails with
#     could not open kernel file '/boot/vmlinuz-...': Permission denied
# and the harness reports "did not reach runit stage 2 within 300s" -- which
# reads as a boot failure and is not one: qemu never started. The guest never
# got a kernel.
#
# Copy the kernel somewhere readable rather than requiring it to already be.
# `install -m 0444` keeps this to one step and never leaves a group- or
# other-writable copy behind.
readable_kernel() {
    [ -n "$1" ] && [ -r "$1" ] && { printf '%s\n' "$1"; return 0; }
    [ -r "$1" ] || {
        local dst
        dst="$(mktemp -d)/vmlinuz"
        sudo install -m 0444 "$1" "$dst" 2>/dev/null \
            || { echo "FAIL: cannot read kernel $1 (mode $(stat -c %a "$1" 2>/dev/null))" >&2
                 echo "     pass one with INVFS_*_KERNEL, or install one readable" >&2
                 return 1; }
        printf '%s\n' "$dst"
    }
}

command -v qemu-system-x86_64 >/dev/null || { echo "FAIL: qemu not found"; exit 1; }

# Same precondition as boot-void-qemu.sh: the scripted-SSH path uses sshpass,
# which drives the password prompt through a pseudo-terminal, so it needs
# /dev/pts on THIS machine. Without it the guest is reported as unreachable
# when in fact it booted fine and the client could not allocate a pty.
if [ -z "$KEY" ] && { [ ! -e /dev/ptmx ] || ! mountpoint -q /dev/pts 2>/dev/null; }; then
    if sudo mountpoint -q /dev/pts 2>/dev/null || sudo mount -t devpts devpts /dev/pts 2>/dev/null; then
        echo "   mounted /dev/pts (sshpass needs a pty; it was not available)"
    else
        echo "FAIL: no /dev/pts and no ARCH_SSH_KEY -- scripted SSH cannot work." >&2
        echo "     This is the machine running the harness, not the guest." >&2
        echo "     Fix:  sudo mount -t devpts devpts /dev/pts   or   export ARCH_SSH_KEY=<file>" >&2
        exit 1
    fi
fi

rm -f "$LOG" "$SSHLOG"
: > "$LOG"      # so early log_grep[] reads do not race qemu -serial file:
: > "$SSHLOG"

# -- build QEMU arguments ----------------------------------------------------
# KVM if the host offers it, TCG otherwise (runners have no /dev/kvm;
# hardcoded accel=kvm died instantly there -- the Arch boot that had
# never succeeded anywhere). Same shape as boot-void-qemu.sh.
ACCEL="${INVFS_ACCEL:-auto}"
if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm
    else ACCEL=tcg; echo "note: /dev/kvm not usable here; falling back to TCG (slower)"; fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}
if [ "$BOOTLOADER" = 1 ]; then
    # --bootloader: OVMF + GRUB reading the ESP from the GPT disk image.
    # The disk image has p1=ESP(FAT32) with GRUB+kernel+initramfs,
    # p2=InvFS raw volume.
    note "booting via OVMF+GRUB from disk: $DISK_IMG"
    note "OVMF=$OVMF ram=${RAM}M port=$PORT"
    QEMU_ARGS=(
        -machine q35,accel=$ACCEL -cpu "$CPU" -m "$RAM" -smp 2
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF"
        -drive "id=root,file=$DISK_IMG,format=raw,if=virtio"
        -append "console=ttyS0,115200"
        -netdev user,id=net0,hostfwd=tcp::${PORT}-:22
        -device virtio-net-pci,netdev=net0
        # virtio-rng: guests without it stall saving the RNG seed at shutdown
        # (entropy-starved VM hung a CI void boot after stage 3 with 60s left).
        -object rng-random,filename=/dev/urandom,id=rng0
        -device virtio-rng-pci,rng=rng0
        -display none -serial "file:$LOG" -monitor none -no-reboot
    )
else
    # --single/--multi: direct -kernel/-initrd boot (host kernel).
    DRIVES=(-drive "id=root,file=$IMG,format=raw,if=virtio")
    if [ "$MODE" = multi ]; then
        DRIVES+=(-drive "id=shadow,file=$SHADOW,format=raw,if=virtio")
    fi
    note "booting $MODE volume(s): IMG=$IMG${SHADOW:+ SHADOW=$SHADOW}"
    note "kernel=$KERNEL initrd=$INITRD ram=${RAM}M port=$PORT"
    QEMU_ARGS=(
        -machine q35,accel=$ACCEL -cpu "$CPU" -m "$RAM" -smp 2
        -vga none
        -kernel "$KERNEL" -initrd "$INITRD"
        -append "console=ttyS0,115200 invfs.init=/bin/invfs-init"
        "${DRIVES[@]}"
        -netdev user,id=net0,hostfwd=tcp::${PORT}-:22
        -device virtio-net-pci,netdev=net0
        # virtio-rng: guests without it stall saving the RNG seed at shutdown
        # (entropy-starved VM hung a CI void boot after stage 3 with 60s left).
        -object rng-random,filename=/dev/urandom,id=rng0
        -device virtio-rng-pci,rng=rng0
        -display none -serial "file:$LOG" -monitor none -no-reboot
    )
fi

KERNEL="$(readable_kernel "$KERNEL")" || exit 1
# QEMU_ARGS was built ABOVE, and `QEMU_ARGS=( ... -kernel "$KERNEL" ... )`
# expands $KERNEL INTO THE ARRAY at that moment. Reassigning KERNEL here
# changes nothing, so QEMU was handed the original root-only
# /boot/vmlinuz-$(uname -r) that runners ship, failed to read it, and exited
# in about a second with an empty serial log and five assertions and no cause.
#
# e190ca0 fixed the -r/-e half of this and missed the array half. Void has
# always been right: it substitutes into the array after building it. Done the
# same way here rather than relying on ordering, so the two harnesses cannot
# drift apart in this way again.
for _i in "${!QEMU_ARGS[@]}"; do
    [ "${QEMU_ARGS[$_i]}" = "-kernel" ] && QEMU_ARGS[$((_i + 1))]="$KERNEL"
done
# QEMU's stderr is KEPT. It used to go to /dev/null, which is why an Arch boot
# that died in under a second produced a 0-byte serial log and five assertions
# with no reason anywhere: the one stream that said why was the one being
# thrown away. boot-void-qemu.sh has always kept it, which is why Void failures
# have always been diagnosable.
QERR="$(mktemp -t invfs-arch-qemu-err.XXXXXX)"
# Pre-launch environment dump: a qemu that dies in ~1s with an empty
# serial log AND empty stderr leaves nothing otherwise. Known unknowns
# this answers: wrong qemu, unreadable inputs, no memory.
qemu-system-x86_64 --version 2>&1 | head -1
free -m | head -2
ls -la "$KERNEL" "$INITRD" "$IMG" 2>&1
qemu-system-x86_64 "${QEMU_ARGS[@]}" 2>"$QERR" &
QPID=$!
cleanup() {
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    # Print whatever QEMU said on its way out. Cheap, and it is the whole
    # difference between "the guest did not boot" and a named cause.
    if [ -s "$QERR" ]; then
        echo "--- qemu stderr ---" >&2
        sed -n '1,20p' "$QERR" >&2
    else
        # Empty stderr + instant death = killed from outside (OOM) or
        # never execed. The kernel ring names OOM kills; sudo exists
        # on CI runners for exactly this.
        echo "--- qemu stderr empty; host dmesg tail (OOM?) ---" >&2
        sudo dmesg 2>/dev/null | grep -aiE "oom|killed process.*qemu|out of memory" | tail -5 >&2 || true
    fi
    rm -f "$QERR" 2>/dev/null
}
trap cleanup EXIT

# The serial console writes CRLF; strip CR before anything anchored to EOL.
log_grep() { tr -d '\r' < "$LOG" 2>/dev/null | grep -E "$@"; }

wait_marker() {  # $1 = regex, $2 = human label
    local i=0
    while [ "$i" -lt "$TIMEOUT" ]; do
        if log_grep -q "$1"; then
            echo "   ok: $2"
            return 0
        fi
        if ! kill -0 "$QPID" 2>/dev/null; then
            echo "   qemu exited before marker: $2"; return 1
        fi
        sleep 1; i=$((i+1))
    done
    echo "   timeout waiting for marker: $2"; return 1
}

note "waiting for serial markers"
wait_marker "InvariantFS mounted:" "invf-fuse mounted" || fail "no mount marker"
wait_marker "Chrooting to InvFS root" "chroot to InvFS root" || fail "no chroot marker"
wait_marker "invfs-arch: rc[.]invfs done" "rc.invfs finished" || fail "rc.invfs did not finish"

if [ "$MODE" = single ]; then
    # /dev/vda, NOT /dev/sda. b4473e7 changed the drives from if=ide to
    # if=virtio because q35 has no PIIX IDE controller and QEMU accepts if=ide
    # silently without ever giving the guest the disk -- that fix is correct and
    # the guest now enumerates virtio -- but these two assertions were left
    # asserting the old IDE name, so a perfectly healthy boot was reported as
    # "unexpected device selection". The serial line even prints the real value
    # in the failure branch, which is how it was spotted.
    log_grep -q "^INVFS_RAW=/dev/vda DEV1=$" \
        && echo "   ok: INVFS_RAW=/dev/vda DEV1=" \
        || { log_grep "^INVFS_RAW="; fail "unexpected device selection (single)"; }
else
    log_grep -q "^INVFS_RAW=/dev/vda DEV1=/dev/vdb$" \
        && echo "   ok: INVFS_RAW=/dev/vda DEV1=/dev/vdb" \
        || { log_grep "^INVFS_RAW="; fail "unexpected device selection (multi)"; }
fi

# Wait until ssh actually completes a command. Early connections can be
# reset during sshd/login cold start, so retry a trivial echo.
note "waiting for sshd on host port $PORT"
ok=0; i=0
while [ "$i" -lt "$TIMEOUT" ]; do
    if [ "$($SSH root@127.0.0.1 'echo READY' 2>>"$SSHLOG")" = READY ]; then
        ok=1; break
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 2; i=$((i+2))
done
[ "$ok" = 1 ] || fail "ssh command did not succeed on :$PORT"

run_ssh() {  # $1 = label, $2 = remote command; retry until non-empty output
    local out="" i=0
    while [ "$i" -lt 6 ]; do
        out=$($SSH root@127.0.0.1 "$2" 2>>"$SSHLOG")
        [ -n "$out" ] && break
        sleep 2; i=$((i+1))
    done
    {
        echo "### $1"
        echo "$out"
    } >> "$SSHLOG"
    printf '%s\n' "$out"
}

if [ "$ok" = 1 ]; then
    out=$(run_ssh "os-release" "grep PRETTY_NAME /etc/os-release")
    case "$out" in
        *'Arch Linux'*) echo "   ok: os-release: $out";;
        *) fail "os-release not Arch Linux: [$out]";;
    esac

    out=$(run_ssh "mount /" "mount | grep ' / '")
    case "$out" in
        *invfs*type*fuse*) echo "   ok: mount | grep ' / ': $out";;
        *invfs*) echo "   ok: mount | grep ' / ': $out";;
        *) fail "root is not invfs: [$out]";;
    esac

    out=$(run_ssh "proc-mounts /" "grep ' / ' /proc/mounts")
    case "$out" in
        *invfs*) echo "   ok: /proc/mounts root: $out";;
        *) fail "proc mount root not invfs: [$out]";;
    esac

    out=$(run_ssh "hostname" "cat /proc/sys/kernel/hostname")
    echo "   info: hostname=$out"

    note "shutting guest down"
    # WP227: every other assertion in this harness runs as root, so none of them
    # can see a root filesystem that is unusable to unprivileged users. That is
    # not hypothetical -- it shipped from WP66 until 612d0fe. See
    # tools/lib-nonroot-gate.sh.
    note "WP227 guard: non-root access to /"
    . "$REPO/tools/lib-nonroot-gate.sh"
    nonroot_gate_ssh() { $SSH root@127.0.0.1 "$1"; }
    nonroot_gate nonroot_gate_ssh -- /bin/bash /bin/bash /etc/os-release /usr/bin \
        || die "WP227 non-root guard failed"
    [ "${nonroot_gate_skipped:-0}" = 1 ] && \
        note "WARNING: WP227 guard SKIPPED -- guest could not be checked, which is NOT a pass"

    $SSH root@127.0.0.1 "busybox poweroff -f" >>"$SSHLOG" 2>&1 || true
fi

i=0
while kill -0 "$QPID" 2>/dev/null && [ "$i" -lt "$REBOOT_WAIT" ]; do
    sleep 1; i=$((i+1))
done
if kill -0 "$QPID" 2>/dev/null; then
    note "qemu still alive after ${REBOOT_WAIT}s; terminating"
    kill "$QPID" 2>/dev/null
fi
wait "$QPID" 2>/dev/null

echo
if [ "$FAILED" = 0 ]; then
    if [ "$BOOTLOADER" = 1 ]; then
        echo "PASS: Arch-on-InvariantFS OVMF+GRUB boot ($DISK_IMG) -- serial + SSH + root mount ok"
    else
        echo "PASS: Arch-on-InvariantFS boot ($MODE) -- serial + SSH + root mount ok"
    fi
    exit 0
else
    if [ "$BOOTLOADER" = 1 ]; then
        echo "FAIL: Arch-on-InvariantFS OVMF+GRUB boot ($DISK_IMG): $FAILED assertion(s) failed"
    else
        echo "FAIL: Arch-on-InvariantFS boot ($MODE): $FAILED assertion(s) failed"
    fi
    echo "--- serial tail ($LOG) ---"
    tail -40 "$LOG" 2>/dev/null
    exit 1
fi
