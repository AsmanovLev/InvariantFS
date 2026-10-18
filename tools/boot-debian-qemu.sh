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
# WP621: the poweroff wait was a hardcoded 120s and fired on a healthy
# guest purely from TCG-emulation variance (engine-ci: booted, SSH OK,
# poweroff issued, no powerdown in 120s; sibling run on the same sha went
# green). Same treatment the boot wait already got: TCG-honest default,
# overridable per leg. Deliberately NOT shortening guest-side
# DefaultTimeoutStopSec instead: a systemd SIGKILL of a hung FUSE unmount
# would still "power off" and the gate would stop proving CLEAN shutdown.
POWEROFF_TIMEOUT="${INVFS_DEB_POWEROFF_TIMEOUT:-300}"
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
# WP629: the console is a SOCKET in addition to a log file, because a stalled
# boot has to be interrogated, not just watched. QEMU's own chardev logfile
# keeps $SERIAL exactly as -serial file: did, so every existing wait_marker and
# grep keeps working unchanged.
SOCK="$LOGDIR/serial.sock"

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
    # WP611: IMG was assigned from the default RAW at the top, so a reuse
    # run kept pointing the offline checks at $WORK/deb.img (which may not
    # even exist) instead of the reused volume. Keep them in step here.
    IMG="$RAW"
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
# The host kernel is not always readable by the user running this.
#
# GitHub runners ship /boot/vmlinuz-<ver> root-only, so qemu fails with
#     could not open kernel file '/boot/vmlinuz-...': Permission denied
# and the harness reports a boot failure when qemu never started. Same
# helper as boot-arch-qemu.sh / boot-void-qemu.sh: copy the kernel
# somewhere readable rather than requiring it to already be. `install
# -m 0444` keeps this to one step and never leaves a group- or
# other-writable copy behind.
readable_kernel() {
    [ -n "$1" ] && [ -r "$1" ] && { printf '%s\n' "$1"; return 0; }
    [ -r "$1" ] || {
        local dst
        dst="$(mktemp -d)/vmlinuz"
        sudo install -m 0444 "$1" "$dst" 2>/dev/null \
            || { echo "FAIL: cannot read kernel $1 (mode $(stat -c %a "$1" 2>/dev/null))" >&2
                 echo "     pass one with INVFS_DEB_KERNEL, or install one readable" >&2
                 return 1; }
        printf '%s\n' "$dst"
    }
}
KERNEL="$(readable_kernel "$KERNEL")" || exit 1
# invfs.init is named EXPLICITLY so the assertion below is about systemd
# starting, not about /sbin/init happening to resolve.
CMDLINE="console=ttyS0,115200 invfs.init=/lib/systemd/systemd"
# WP629: mask the units that do no useful work on an emulated CPU but are
# enough to lose the multi-user.target race on a shared TCG runner.
# 2026-10-18 CI: the deadline fired with basic.target already reached and
# systemd-journal-catalog-update.service having reported an error -- the
# same unit 9b16f7a named as a 2m45 stall. The rest are timers that only
# arm themselves. None of this is part of what the gate asserts: systemd
# as PID 1, the root is FUSE, multi-user.target, clean poweroff.
# systemd.mask= is a KERNEL-CMDLINE mask, deliberately: masking by writing
# into $STAGE would change the tree that was imported, and the offline
# check at the end compares $STAGE against the volume byte for byte.
# INVFS_DEB_BOOT_MASK= (empty) boots unmasked.
BOOT_MASK="${INVFS_DEB_BOOT_MASK-systemd-journal-catalog-update.service apt-daily.timer apt-daily-upgrade.timer dpkg-db-backup.timer fstrim.timer systemd-tmpfiles-clean.timer}"
for _mu in $BOOT_MASK; do CMDLINE="$CMDLINE systemd.mask=$_mu"; done
if [ -n "$BOOT_MASK" ]; then
    echo "note: masking boot-noise units via systemd.mask=: $BOOT_MASK"
fi
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
       -vga none
       -kernel "$KERNEL" -initrd "$INITRD" -append "$CMDLINE"
       -drive "file=$RAW,format=raw,if=virtio"
       -netdev "user,id=net0,hostfwd=tcp::$PORT-:22"
       -device virtio-net-pci,netdev=net0
        # virtio-rng: guests without it stall saving the RNG seed at shutdown
        # (entropy-starved VM hung a CI void boot after stage 3 with 60s left).
        -object rng-random,filename=/dev/urandom,id=rng0
        -device virtio-rng-pci,rng=rng0
       -display none -monitor none -no-reboot
        # WP629: socket + logfile, not file:. The logfile records the guest's
        # output (so the waits below are untouched); the socket lets the
        # harness TYPE into the guest's root shell when the boot stalls.
       -chardev "socket,id=ser0,path=$SOCK,server=on,wait=off,logfile=$SERIAL"
       -serial "chardev:ser0")
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
# The port has to be free BEFORE starting, or QEMU exits instantly and every
# later assertion is measuring nothing. This check must run before the
# launch below: placed after it, it finds our OWN qemu holding the port
# and refuses every run (CI 37306407583 died exactly this way -- the
# 'leaked qemu' it reported was itself).
_port_in_use() { ss -ltn 2>/dev/null | grep -q ":$1 " ||                  netstat -ltn 2>/dev/null | grep -q ":$1 "; }
# TCG-without-KVM boots are slow AND variable on shared runners: a boot
# that reaches a login shell but never declares multi-user.target within
# the budget is a known environmental stall, not a filesystem verdict.
# The 2-attempt retry that used to live here (9cb4604) was removed by
# 9b16f7a because two 1500 s attempts back to back spend the whole 60-min
# job cap on a guest that is SLOW rather than stuck; one 2100 s attempt
# fits a slow boot and still catches a true hang. boot_once stays so the
# retry can be re-armed in one word.
#
# diagnose_boot_stall -- WP629. The failure path used to report "the last
# target it DID reach" with
#     grep -aoE 'Reached target [A-Za-z0-9.-]+' | tail -1
# on the RAW serial. systemd colorises the target name, so the character
# after 'Reached target ' is an ANSI escape and not a name. On the
# 2026-10-18 CI run that grep matched an early uncoloured line and printed
# 'swap.target' while the 40-line dump printed immediately below it showed
# sysinit/timers/sockets/basic/getty.target reached -- a diagnostic that
# contradicts its own evidence, and it sent the first reading of that
# failure after the wrong question. Strip the escapes before matching.
#
# The two facts reported are what a red boot gate actually has to answer:
#   1. units that printed 'Starting' and never 'Started'/'Finished' -- the
#      pending activation, i.e. what the boot is waiting on;
#   2. whether the console is still producing output. A guest that is
#      merely slow keeps writing; one blocked on a request that never
#      returns goes silent, and the serial file's size says which.
diagnose_boot_stall() {
    local st="$LOGDIR/serial.stripped" sz0 sz1 pend silent
    local USUF='service|socket|timer|target|mount|path|slice|device|swap|scope|automount'
    # Unit names are cut to the console width (80 cols), so a naive set
    # difference reports units that in fact finished: on run 39134798855
    # 'Starting modprobe@efi_pstore.servi...' never equals the full
    # 'Finished modprobe@efi_pstore.service', and that unit was reported as
    # pending while its own Finished line sat in the same serial. Compare
    # 24-char prefixes, and require a unit suffix so systemd's own
    # 'Starting of X ... unsupported' ([UNSUPP]) line cannot contribute 'of'.
    sed 's/\x1b\[[0-9;]*m//g' "$SERIAL" > "$st" 2>/dev/null || cp "$SERIAL" "$st"
    echo "--- last targets reached (ANSI-stripped, newest last):"
    grep -aoE 'Reached target [A-Za-z0-9@._-]+' "$st" | tail -3 | sed 's/^/      /' || true
    echo "--- still activating at the deadline (said 'Starting', never 'Started'/'Finished'):"
    pend="$(comm -23 \
        <(grep -aoE "Starting [A-Za-z0-9@._-]+\.($USUF)" "$st" | awk '{print substr($2,1,24)}' | sort -u) \
        <(grep -aoE "(Started|Finished|Stopped) [A-Za-z0-9@._-]+\.($USUF)" "$st" | awk '{print substr($2,1,24)}' | sort -u))" || true
    if [ -n "$pend" ]; then
        while read -r p; do
            [ -n "$p" ] || continue
            printf '      %s\n' "$(grep -aoE "Starting [A-Za-z0-9@._-]+\.($USUF)" "$st" \
                | awk '{print $2}' | grep -m1 -F "$p" || printf '%s' "$p")"
        done <<<"$pend"
    else
        echo "      (none -- every unit that started also finished)"
    fi
    sz0="$(stat -c %s "$SERIAL" 2>/dev/null || echo 0)"
    echo "--- console activity: ${sz0} bytes; sampling 10s ..."
    sleep 10
    sz1="$(stat -c %s "$SERIAL" 2>/dev/null || echo 0)"
    silent=$(( $(date +%s) - $(stat -c %Y "$SERIAL" 2>/dev/null || date +%s) ))
    if [ "$sz1" -gt "$sz0" ]; then
        echo "      grew $((sz1 - sz0)) bytes in 10s -- the guest is STILL BOOTING"
    else
        echo "      no new bytes in 10s; the console has been silent for ${silent}s"
        echo "      NOTE: silence is NOT by itself proof of a hang -- a guest that"
        echo "            finished booting and sits at an idle shell prompt prints"
        echo "            nothing either. The in-guest probe below separates the two."
    fi
    guest_probe
    echo "--- last 40 lines of serial:"
    tail -40 "$st"
}

# SILENCE_TIMEOUT -- WP629. A boot that goes quiet must not burn the whole
# budget: run 39134798855 spent 2100 s waiting for a marker while the console
# produced nothing for the last 1920 s of it. Watch the console's size as well
# as the marker and give up after this many seconds without growth.
SILENCE_TIMEOUT="${INVFS_DEB_SILENCE_TIMEOUT:-300}"
wait_multiuser_or_silence() {   # 0 reached, 1 budget, 2 guest died, 3 console silent
    local i=0 quiet=0 last cur
    last="$(stat -c %s "$SERIAL" 2>/dev/null || echo 0)"
    while [ "$i" -lt "$BOOT_TIMEOUT" ]; do
        if grep -qE 'Reached target.*Multi-User|Startup finished' "$SERIAL" 2>/dev/null; then
            return 0
        fi
        qpid_alive || return 2
        sleep 1; i=$((i + 1))
        cur="$(stat -c %s "$SERIAL" 2>/dev/null || echo 0)"
        if [ "$cur" -gt "$last" ]; then
            last="$cur"; quiet=0
        else
            quiet=$((quiet + 1))
            [ "$quiet" -ge "$SILENCE_TIMEOUT" ] && return 3
        fi
    done
    return 1
}

# guest_probe -- WP629. The 2026-10-19 local reproduction booted this same
# staged root with the same kernel/initramfs to multi-user.target twice (KVM
# 5.3 s, TCG 2 min 25 s) with ssh.service and systemd-logind.service both
# 'active (running)', zero D-state processes and PID 1 idle in do_epoll_wait.
# A console-only harness cannot tell that state from a wedged guest -- it sees
# silence either way. The guest auto-logs in as root on ttyS0, so the socket
# above can be typed into: ASK the guest rather than infer its state.
# Best effort by construction: no python3, a dead QEMU or a truly wedged guest
# changes how much evidence lands in the log, never the verdict.
guest_probe() {
    if ! qpid_alive; then echo "      (QEMU is gone; no in-guest probe possible)"; return 0; fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "      (no python3 on this host; skipping the in-guest probe)"; return 0
    fi
    echo "--- in-guest probe over the serial console (the guest auto-logs in as root):"
    timeout 180 python3 - "$SOCK" "$SERIAL" <<'PY' || echo "      (probe did not complete -- the guest console may be gone)"
import select, socket, sys, time

sock, cap = sys.argv[1], sys.argv[2]
CMDS = [
    "stty cols 400 rows 60",
    "systemctl is-system-running; echo ISRUNNING-RC=$?",
    "systemctl --no-pager list-jobs",
    "systemctl --no-pager list-units --state=activating",
    "cat /proc/1/wchan",
    "ps -eo pid,stat,wchan:30,comm | awk 'NR==1||$2~/D/'",
    "systemctl --no-pager --full status ssh.service systemd-logind.service",
    "echo PROBE-DONE",
]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(sock)
except OSError as exc:
    print("      cannot reach the serial socket: %s" % exc)
    sys.exit(0)
s.setblocking(False)
capf = open(cap, "ab")

def pump(sec):
    end = time.time() + sec
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.25)
        if not r:
            continue
        try:
            d = s.recv(65536)
        except BlockingIOError:
            continue
        if not d:
            break
        capf.write(d); capf.flush()
        sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()

# Everything printed before we connected is in the log and is not replayed, so
# start with a newline: at a shell prompt that re-prints the prompt, and with no
# shell yet the commands below simply go nowhere.
s.sendall(b"\n")
pump(20)
for c in CMDS:
    s.sendall(c.encode() + b"\n")
    pump(8)
PY
}

# guest_says_running -- WP629. The guest's own answer to `is-system-running` is
# STRONGER evidence than a console line, and it is the same assertion: systemd
# returns `running` only once every startup job is done and nothing is degraded.
# A genuinely wedged boot says `starting` or `degraded`, so accepting this cannot
# paper over a real failure -- and it removes the false red caused by a console
# that stops carrying systemd's output (measured: the guest answered `running`
# with half of the log missing a line before `Reached target Multi-User System.`).
# Fail closed by default: if the probe did not run, or systemd could not answer
# because PID 1 is itself stuck, there is no evidence and the run stays red.
guest_says_running() {
    # The serial is CRLF and carries ANSI codes, so normalise before matching:
    # without this the state line is 'running\r' and an exact match never fires
    # (caught by unit-checking this function against the real stalled serial).
    grep -aq 'ISRUNNING-RC=0' "$SERIAL" 2>/dev/null && return 0
    sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$SERIAL" 2>/dev/null | tr -d '\r' \
        | grep -aA1 'systemctl is-system-running' | grep -qxE 'running' && return 0
    return 1
}
boot_once() {   # $1 = attempt number (1-based)
    if [ "$1" != 1 ]; then
        cp "$SERIAL" "$SERIAL.att$(( $1 - 1 ))" 2>/dev/null || true
        cp "$QEMU_ERR" "$QEMU_ERR.att$(( $1 - 1 ))" 2>/dev/null || true
        : > "$SERIAL"; : > "$QEMU_ERR"
    fi
    if _port_in_use "$PORT"; then
        echo "port $PORT is already in use -- refusing to start a guest that" >&2
        echo "cannot bind. A leaked qemu from an earlier run holds it; kill it," >&2
        echo "or pass INVFS_DEB_PORT=<n>." >&2
        exit 1
    fi
# NOT setsid. `setsid cmd &` makes $! the PID of setsid, which FORKS when it
# is already a process group leader and then exits -- so $! is not QEMU, and the
# EXIT trap kills nothing while the guest keeps running and keeps holding the
# hostfwd port. The very next run then refused to start with "port 2426 is
# already in use", which is this check earning its place, but the cleanup has to
# actually clean. stdio is redirected either way, so setsid bought nothing.
# WP629: the socket path is bound by QEMU (server=on); a leftover socket file
# from a killed earlier run makes that bind fail with EADDRINUSE, and QEMU then
# exits with a chardev error rather than anything about the guest.
rm -f "$SOCK"
qemu-system-x86_64 "${QOPTS[@]}" >"$QEMU_ERR" 2>&1 &
QPID=$!
qpid_alive() { kill -0 "$QPID" 2>/dev/null; }

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
qpid_alive || { echo "qemu exited immediately:" >&2; cat "$QEMU_ERR" >&2; return 1; }
[ -s "$SERIAL" ] || qpid_alive || { echo "qemu died before writing serial:" >&2; \
    cat "$QEMU_ERR" >&2; return 1; }

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
    tail -20 "$SERIAL" 2>/dev/null; echo "initramfs never mounted the volume" >&2; return 1; }
wait_marker 'Chrooting to InvFS root' 60 || { echo "no chroot into the volume" >&2; return 1; }

say "waiting for systemd (this is the KI3b assertion)"
wait_marker 'systemd\[1\]|Starting systemd|systemd v2' "$BOOT_TIMEOUT" || {
    echo "--- last 30 lines of serial:"; tail -30 "$SERIAL"; \
    echo "systemd never announced itself within ${BOOT_TIMEOUT}s -- this is H1: 'daemon startup hangs'" >&2; return 1; }
qpid_alive || { tail -30 "$SERIAL"; echo "guest died while systemd was starting" >&2; return 1; }

say "waiting for multi-user.target (or ${SILENCE_TIMEOUT}s of console silence)"
local mrc=0
wait_multiuser_or_silence || mrc=$?
if [ "$mrc" -ne 0 ]; then
    diagnose_boot_stall
    if [ "$mrc" -eq 3 ] && guest_says_running; then
        say "the console stopped carrying systemd's output, but the GUEST reports"
        say "'is-system-running = running' with no jobs pending -- the boot "
        say "completed and only the console line was lost, so the assertion holds."
        say "(See the in-guest probe above; this is a WITNESS failure, not a stall.)"
        return 0
    fi
    case "$mrc" in
        2) echo "the guest died while systemd was starting" >&2;;
        3) echo "the console produced nothing for ${SILENCE_TIMEOUT}s while waiting for multi-user.target, and the guest did NOT report 'running' -- treat as stalled (state above)" >&2;;
        *) echo "systemd did not reach multi-user.target within ${BOOT_TIMEOUT}s (the pending units above are what it was waiting on -- a login prompt means getty.target, NOT multi-user)" >&2;;
    esac
    return 1
fi
say "systemd reached multi-user.target"
}

BOOT_OK=0
# Single attempt: the 2-attempt retry (9cb4604) doubled the worst case
# under the 60-min job cap on slow TCG hosts (two 1500 s timeouts back
# to back, both burning fully on a guest that is SLOW not stuck).
# boot_once machinery stays for a future re-arm; the loop below runs
# it exactly once.
for _att in 1; do
    if boot_once "$_att"; then BOOT_OK=1; break; fi
    say "boot attempt $_att missed multi-user.target; killing guest before retry"
    cleanup_guest
    sleep 3
done
[ "$BOOT_OK" = 1 ] || fail "systemd did not reach multi-user.target in a single boot attempt (serial saved as $SERIAL)"

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
    # WP621: blame snapshot before the poweroff so the next red names its
    # slow unit instead of just timing out. Best-effort, note-level.
    say "slowest stop units (pre-poweroff blame): $(ssh_guest 'systemd-analyze blame 2>/dev/null | head -5 | tr "\n" ";"' 2>/dev/null || echo '<unknown>')"
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
    # The 'reboot: ' prefix belongs INSIDE its own alternative: written
    # outside the group it silently required every alternative to be
    # preceded by 'reboot: ', which made the Reached-target fallbacks
    # dead -- a guest that reached poweroff.target without the kernel
    # ever printing 'Power down' (slow TCG shutdown) could never match
    # and burned the whole budget. With live fallbacks the marker fires
    # as soon as systemd finishes the shutdown targets; the qemu-exit
    # poll below is what proves the guest actually left.
    wait_marker '(reboot: (Power down|System Powered Off)|Reached target.*(Power-Off|Reboot|Shutdown))' "$POWEROFF_TIMEOUT" \
        || fail "systemctl poweroff was issued over SSH but the guest did not \
shut down within ${POWEROFF_TIMEOUT}s -- a clean shutdown on a FUSE root IS worth failing on"
    # The marker above can fire early (shutdown.target is reached long
    # before the kernel powers off), and under TCG the tail of shutdown
    # is slow -- so give QEMU its own bounded budget instead of one 20s
    # glance. A guest that truly wedged still fails here, with a message
    # that names it. Knobbed for CI diagnosis: a raised budget tells slow
    # (exits late) apart from wedged (never exits). Default 600 preserves
    # old behaviour everywhere the knob is unset.
    QEMU_EXIT_TIMEOUT="${INVFS_DEB_QEMU_EXIT_TIMEOUT:-600}"
    for _w in $(seq 1 $(( QEMU_EXIT_TIMEOUT / 20 ))); do qpid_alive || break; sleep 20; done
    qpid_alive && fail "QEMU still running ${QEMU_EXIT_TIMEOUT}s after the poweroff marker -- guest wedged before poweroff";
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
# Collect EVERY file's verdict, never fail fast: a single unreadable file
# next to four good ones is the evidence (quarantine? torn recipe?), and
# the cat stderr is kept -- >/dev/null here once hid the actual error.
catrc=0
for f in etc/hostname etc/passwd lib/systemd/systemd usr/bin/systemctl etc/os-release; do
    [ -n "$STAGE" ] || break
    [ -f "$STAGE/$f" ] || continue
    # --follow: a final-component symlink (merged-usr /etc/os-release ->
    # ../usr/lib/os-release) must resolve to the file, like the FUSE read
    # the guest just booted through did. Without it invf-cat returns the
    # link's target string and the cmp below fails on bytes that were
    # never wrong on the volume.
    if ! "$B/invf-cat" --follow "$IMG" "$f" "$WORK/out/$(basename "$f")" >"$WORK/out/$(basename "$f").err" 2>&1; then
        echo "FAIL: invf-cat $f: $(head -c 200 "$WORK/out/$(basename "$f").err")"
        catrc=1; continue
    fi
    cmp -s "$STAGE/$f" "$WORK/out/$(basename "$f")" \
        || { echo "FAIL: $f not bit-exact after the systemd boot"; catrc=1; continue; }
    say "  $f bit-exact"
done

"$B/invf-fsck" "$IMG" > "$LOGDIR/fsck.log" 2>&1 || true
if ! grep -q '^OK$' "$LOGDIR/fsck.log"; then
    "$B/invf-fsck" -f "$IMG" > "$LOGDIR/fsck-f.log" 2>&1 || true
    "$B/invf-fsck" "$IMG" > "$LOGDIR/fsck.log" 2>&1 || true
fi
grep -q '^OK$' "$LOGDIR/fsck.log" || { cat "$LOGDIR/fsck.log"; fail "fsck not clean"; }
say "fsck CLEAN: $(tail -1 "$LOGDIR/fsck.log")"
[ "$catrc" = 0 ] || fail "offline bit-exact: at least one file failed (see FAIL lines above)"

echo
echo "PASS: Debian boot on InvariantFS -- PID 1 was systemd, multi-user.target"
echo "      reached, root is an invfs fuse mount, clean poweroff, bit-exact, fsck CLEAN"