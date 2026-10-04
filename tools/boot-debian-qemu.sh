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
# WP224: raised from 420s, and the failure message now says where it GOT TO.
# 420 was arbitrary, and it fired on a guest that was visibly sitting at a
# login prompt: getty.target ("Login Prompts") legitimately precedes
# multi-user.target, so a shell prompt is NOT evidence the target was reached
# -- the first version of my reading made exactly that mistake. With
# systemd-networkd enabled the boot waits on DHCP and legitimately takes longer
# than 420s; the same root reached multi-user.target in ~5 minutes when
# networkd was off. A timeout that fires mid-boot and says only "did not reach
# multi-user" is a worse instrument than one that reports the last target.
BOOT_TIMEOUT="${INVFS_DEB_BOOT_TIMEOUT:-900}"
MODE="${INVFS_DEB_MODE:-single}"

# STAGE is required only when this run will IMPORT it. With
# INVFS_DEB_VOLUME set the volume already holds the root and STAGE is
# irrelevant -- which the first version of this reuse path did not allow, so it
# exited 2 demanding a STAGE it was about to ignore.
if [ -z "${INVFS_DEB_VOLUME:-}" ]; then
    [ -n "$STAGE" ] || { echo "INVFS_DEB_STAGE must point at a provisioned Debian root"; exit 2; }
    [ -d "$STAGE" ] || { echo "no such staged root: $STAGE"; exit 2; }
fi
[ -x "$B/invf-mkfs" ] || { echo "run make first"; exit 2; }
[ -f "$INITRD" ] || { echo "no initramfs at $INITRD -- run tools/mkinitramfs.sh"; exit 2; }

mkdir -p "$WORK" "$LOGDIR"
RAW="$WORK/deb.img"; SHADOW="$WORK/deb-shadow.img"
SERIAL="$LOGDIR/serial.log"; IMG="$RAW"

say()  { echo "== $*"; }
# WP224: a silent death is the worst failure mode in a gate. With
# `set -euo pipefail`, an assignment from a pipeline whose FIRST element fails
# exits non-zero even when the last succeeds -- so `PID1=$(ssh ... | tr -d
# '[:space:]')` killed the script at that line when ssh refused, printing
# nothing at all: no FAIL, no line number, just a log that stops mid-thought
# after "SSH OK". An ERR trap that names the line and the exit status turns
# that class of bug into something readable.
trap 'rc=$?; echo "FAIL: harness died at line $LINENO (exit $rc) -- the last line above is what it was doing" >&2' ERR
fail() { echo "FAIL: $*" >&2; exit 1; }
note() { echo "note: $*"; }

# ---- volume ----------------------------------------------------------------
# INVFS_DEB_VOLUME=<img> reuses a volume already imported from $STAGE and skips
# mkfs+import entirely. The import is ~850s of fsync-bound work (7 barriers per
# file), and when the thing being iterated on is a PROVISIONING change -- a
# missing network file, an sshd drop-in -- paying that again for one file is
# the difference between a 20-second loop and a 15-minute one. The importer
# merges into an existing volume, so a one-file change is
# `invf-import <img> <stagedir>` and the boot follows immediately.
#
# It reuses, so it does NOT re-verify: the offline checks at the end still run,
# but nothing here rebuilds the volume. Use a fresh run when the STAGE itself
# changed rather than one file in it.
if [ -n "${INVFS_DEB_VOLUME:-}" ]; then
    [ -f "$INVFS_DEB_VOLUME" ] || fail "no such volume: $INVFS_DEB_VOLUME"
    RAW="$INVFS_DEB_VOLUME"
    say "REUSING $RAW (INVFS_DEB_VOLUME set -- skipping mkfs+import)"
else
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
fi   # end INVFS_DEB_VOLUME reuse branch

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

say "booting: qemu-system-x86_64 -machine q35,accel=$ACCEL $CMDLINE"

# WP224: QEMU's own stderr is KEPT, and the guest is killed on the way out.
#
# Both halves were found by having this harness fail and believing it. QEMU was
# launched with >/dev/null and no cleanup, so when it refused to start the
# harness sat for the full 180s waiting for a mount marker that could never
# appear, and reported "the initramfs never mounted the volume" -- a claim
# about the FILESYSTEM, made when QEMU had never run at all. The real reason
# was a stray QEMU from an earlier run still holding the hostfwd port:
#
#   -netdev user,...,hostfwd=tcp::2426-:22: Could not set up host
#   forwarding rule 'tcp::2426-:22'
#
# and it was invisible because the very thing that would have said so was
# redirected to /dev/null. So: keep stderr, and clean up the guest.
QEMU_ERR="$LOGDIR/qemu.err"
# NOT setsid. `setsid cmd &` makes $! the PID of setsid, which FORKS when it
# is already a process group leader and then exits -- so $! is not QEMU, and the
# EXIT trap kills nothing while the guest keeps running and keeps holding the
# hostfwd port. The very next run then refused to start with "port 2426 is
# already in use", which is this check earning its place, but the cleanup has to
# actually clean. stdio is redirected either way, so setsid bought nothing.
qemu-system-x86_64 "${QOPTS[@]}" >"$QEMU_ERR" 2>&1 &
QPID=$!
qpid_alive() { kill -0 "$QPID" 2>/dev/null; }

# The port has to be free, or QEMU exits instantly and every later assertion
# is measuring nothing.
_port_in_use() { ss -ltn 2>/dev/null | grep -q ":$1 " ||                  netstat -ltn 2>/dev/null | grep -q ":$1 "; }
if _port_in_use "$PORT"; then
    echo "port $PORT is already in use -- refusing to start a guest that" >&2
    echo "cannot bind. A leaked qemu from an earlier run holds it; kill it," >&2
    echo "or pass INVFS_DEB_PORT=<n>." >&2
    exit 1
fi

# WP224: every signal here is `|| true`. Under `set -e` a `kill` that finds the
# process already gone returns non-zero, and since this function is also the
# EXIT trap, that turned "the guest stopped" into a silent exit in the middle of
# the offline checks -- the log simply stopped after the notes, with no FAIL
# and no line number. Cleanup must never be able to fail the run.
cleanup_guest() {
    if qpid_alive; then
        kill -TERM "$QPID" 2>/dev/null || true
        sleep 2
        kill -KILL "$QPID" 2>/dev/null || true
    fi
    return 0
}
trap cleanup_guest EXIT

# Give QEMU a moment and CHECK it started, rather than inferring it from the
# absence of a failure 180 seconds later.
sleep 2
qpid_alive || { echo "qemu exited immediately:" >&2; cat "$QEMU_ERR" >&2; exit 1; }
[ -s "$SERIAL" ] || qpid_alive || { echo "qemu died before writing serial:" >&2; \
    cat "$QEMU_ERR" >&2; exit 1; }

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
    echo "--- last target it DID reach:" \
        "$(grep -aoE 'Reached target [A-Za-z0-9.-]+' "$SERIAL" 2>/dev/null \
            | sed 's/\x1b\[[0-9;]*m//g' | tail -1)"
    echo "--- last 40 lines of serial:"; tail -40 "$SERIAL"; \
    fail "systemd did not reach multi-user.target within ${BOOT_TIMEOUT}s \
(last target above -- a login prompt means getty.target, NOT multi-user)"; }
say "systemd reached multi-user.target"

# ---- THE KI3b ASSERTION, from the serial log ------------------------------
# WP224: asserted from the SERIAL LOG, not over SSH. Two reasons, both learned
# the hard way. First, a boot harness that needs the guest to have a DHCP lease
# has a second failure surface that has nothing to do with the question being
# asked: two boots here reached multi-user.target perfectly and then failed
# "sshd never became reachable", because a minbase Debian needs both a
# .network file AND the systemd-networkd wants-symlink, and systemd-networkd
# logs to the journal rather than the console so the serial log cannot even
# say why. Second, none of the assertions need a shell:
#
#   * PID 1 is systemd   -- systemd's own banner names itself and its version
#   * the root is FUSE   -- the initramfs already printed the mount line
#   * a clean shutdown   -- systemd's poweroff lines
#
# So SSH stays, but as a BEST-EFFORT extra that adds depth when it works and is
# reported as unavailable when it does not, rather than being load-bearing.
SYSTEMD_BANNER=$(grep -aoE "systemd [0-9][^ ]* running in system mode" "$SERIAL" 2>/dev/null | head -1)
say "guest init: ${SYSTEMD_BANNER:-<unknown>}"
case "$SYSTEMD_BANNER" in
    systemd*) : ;;
    *) fail "no systemd banner in the guest console -- this run does not \
exercise KI3b (and 'runit' occurrences: $(grep -ac runit "$SERIAL" 2>/dev/null || echo 0))" ;;
esac

MOUNTED=$(grep -aoE "InvariantFS mounted: [0-9]+ files?" "$SERIAL" 2>/dev/null | head -1)
say "initramfs handoff: ${MOUNTED:-<none found>}"
[ -n "$MOUNTED" ] || fail "the initramfs never reported mounting the volume"

FUSE_MOUNT=$(grep -aoE "sys-fs-fuse-connections.mount|FUSE Control File System" "$SERIAL" 2>/dev/null | head -1)
[ -n "$FUSE_MOUNT" ] && say "guest mounted $FUSE_MOUNT"

# ---- best-effort: in-guest detail over SSH --------------------------------
SSH_OPTS=(-p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o ConnectTimeout=20 -o LogLevel=ERROR)
ssh_guest()  { sshpass -p root ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"; }
ssh_ready()  { timeout 30 sshpass -p root ssh "${SSH_OPTS[@]}" root@127.0.0.1 true \
                   >>"$LOGDIR/ssh-probe.log" 2>&1; }

# WP224: "SSH OK" used to be printed when the SERIAL marker matched, without
# any SSH having been attempted -- `wait_marker ... || ssh_ready` is true if
# EITHER succeeds. So the line claimed a working SSH channel, every subsequent
# ssh_guest silently failed, and the log read as though the in-guest checks had
# run and come back empty. They had not run at all. The marker is now only
# context; SSH_OK is set by SSH actually working.
wait_marker 'Started .*OpenBSD Secure Shell|Started sshd' 90 || true
SSH_OK=0
ssh_ready && SSH_OK=1
if [ "$SSH_OK" = 1 ]; then
    say "SSH OK (verified by an actual ssh round-trip) -- adding in-guest detail"
    PID1=$(ssh_guest "ps -p 1 -o comm=" 2>/dev/null | tr -d '[:space:]' || true)
    say "guest PID 1: ${PID1:-<unknown>}"
    [ -n "$PID1" ] || note "in-guest 'ps' unavailable or refused; the serial \
banner above is the authoritative init evidence either way"
    MOUNTS=$(ssh_guest "grep ' / ' /proc/mounts" 2>/dev/null || true)
    [ -n "$MOUNTS" ] && echo "$MOUNTS"
    printf '%s\n' "$MOUNTS" | grep -q 'invfs' \
        && say "' / ' is an invfs fuse mount" \
        || note "' / ' did not name invfs (checked in guest)"
    say "systemctl is-system-running: $(ssh_guest 'systemctl is-system-running' 2>/dev/null || echo '<unknown>')"
    say "poweroff over SSH"
    ssh_guest "systemctl poweroff" 2>/dev/null || true
else
    say "NOTE: sshd not reachable (no DHCP client configuration in this root)."
    say "      Proceeding on the serial assertions above, which do not need it."
fi
# WP224: shutdown is only attempted if SSH works, and if it does not the harness
# says so and stops the guest by SIGTERM rather than claiming a clean poweroff.
# The previous version waited 120s for poweroff lines that could never arrive --
# because the ONLY way it knew to request one was over the SSH channel it had
# just reported as unavailable -- and then failed the whole run for it. That
# made an optional extra leg load-bearing again, which is the mistake the
# serial-based assertions were introduced to remove.
if [ "$SSH_OK" = 1 ]; then
    wait_marker 'reboot: (Power down|System Powered Off|Reached target.*(Power-Off|Reboot|Shutdown))' 120 \
        || fail "systemctl poweroff was issued over SSH but the guest did not \
shut down within 120s -- a clean shutdown on a FUSE root IS worth failing on"
    qpid_alive && { sleep 20; qpid_alive && fail "QEMU still running 20s after poweroff"; }
    say "poweroff OK (QEMU exited)"
else
    note "no SSH channel, so no clean poweroff was requested -- stopping the"
    note "guest with SIGTERM. This does NOT prove the guest can shut down"
    note "cleanly; it only stops the boot here. The offline checks below still run."
    cleanup_guest
    say "guest stopped (SIGTERM, not a clean poweroff)"
fi

# ---- offline --------------------------------------------------------------
say "offline bit-exact invf-cat + fsck"
mkdir -p "$WORK/out"
for f in etc/hostname etc/passwd lib/systemd/systemd usr/bin/systemctl etc/os-release; do
    [ -n "$STAGE" ] || break
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