#!/bin/sh
# Initramfs init script for InvariantFS
#
# Boot strategy: mount InvFS via FUSE at /mnt/invfs, wait until the file
# table is built ("InvariantFS mounted" in /tmp/fuse.log), then chroot
# into it and hand off to /sbin/init. switch_root is NOT used because the
# new root is a FUSE mount, not a tmpfs/ramfs -- busybox switch_root
# rejects it (prints usage and kills PID1).
#
# Device selection is by VOLUME, not by kernel name: every block device is
# probed for the InvariantFS superblock magic ("InvariFS") and its 16-byte
# uuid (sb offsets 0x00 and 0x08). Kernel names (sda/sdb/vda/...) are not
# stable and, worse, a stale mknod could make a name exist without a real
# device. Optional kernel cmdline:
#   invfs.raw_uuid=<hex>   force the raw volume
#   invfs.dev1_uuid=<hex>  force the second (shadow/mirror) volume
#   invfs.sweepboot        maintenance boot (WP23): sweep engine-side and
#                          reboot, never FUSE-mounting the volume
#   invfs.no_allow_other   mount WITHOUT allow_other, reproducing the WP225-era
#                          guest state on demand (see FUSE_OPTS below)
# Without them the first InvFS device is RAW; a second distinct one is DEV1.

export PATH=/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin

# The initramfs ships only bin/busybox + bin/sh; install the remaining
# applet symlinks (mount, sleep, grep, insmod, chroot, ...). Without this
# /init dies at "mount: not found" before it can even probe block devices.
/bin/busybox --install -s /bin 2>/dev/null || true

# Mount essential filesystems
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
sleep 1

# Kernel modules. Stock distro kernels ship FUSE as a module
# (CONFIG_FUSE_FS=m); mkinitramfs.sh bundles fuse.ko built for the exact
# kernel in use. Without this, invf-fuse fails with "fuse: device not
# found" and we drop to a shell after the 120s wait below. The NIC modules
# let the guest reach the network after chroot.
if [ -f /fuse.ko ]; then
    busybox insmod /fuse.ko 2>/dev/null || insmod /fuse.ko 2>/dev/null || true
fi
for m in /lib/modules/fuse.ko /modules/fuse.ko; do
    [ -f "$m" ] && busybox insmod "$m" 2>/dev/null || true
done
[ -e /dev/fuse ] || busybox mknod /dev/fuse c 10 229 2>/dev/null || true
[ -e /dev/fuse ] && chmod 666 /dev/fuse 2>/dev/null
# virtio_blk FIRST, then the NIC. It is not a nicety: on a distro kernel
# CONFIG_VIRTIO_BLK=m, so a virtio-attached volume does not exist as a block
# device until this loads, and the probe below then reports "no InvariantFS
# volume found on any block device" -- which reads like a lost volume and is
# actually an invisible disk. Measured on 6.12.107+deb13-amd64, the image
# built before this listed only the NIC modules and booted to exactly that
# error with a perfectly good volume on the bus.
#
# The path is /lib/modules, where mkinitramfs.sh stages them (its `mkdir -p
# lib/modules`). This loop used to read /modules, which exists but is empty,
# so every staged module was skipped by a `[ -f ]` guard that hid it. Both
# halves of that were silent.
#
# Order matters: insmod resolves nothing. Each module is staged in dependency
# order by the builder; a failure is reported rather than swallowed, because a
# rescue medium that silently cannot see the disk is the worst possible
# failure mode for this script.
# virtio.ko and virtio_ring.ko come FIRST. They are =y on the Debian kernel the
# original list was written against, so staging them was unnecessary there -- and
# on NixOS's kernel they are =m, which is why that guest printed
#   virtio_blk: Unknown symbol register_virtio_driver (err -2)
# (register_virtio_driver lives in virtio.ko) and then "no InvariantFS volume
# found on any block device". insmod resolves no symbols, so the dependency has to
# be a separate, earlier entry. Missing files are skipped, so a kernel with them
# builtin is unaffected.
# the two virtio_pci halves ride between virtio_ring and virtio_pci:
# vp_legacy_*/vp_modern_* live in them, and virtio_pci fails to load
# ("Unknown symbol vp_legacy_get_features") without both. Measured on nixos
# 2026-11-24, right after virtio/virtio_ring were staged.
for p in /lib/modules/virtio.ko /lib/modules/virtio_ring.ko \
         /lib/modules/virtio_pci_legacy_dev.ko \
         /lib/modules/virtio_pci_modern_dev.ko \
         /lib/modules/virtio_pci.ko /lib/modules/virtio_blk.ko \
         /lib/modules/failover.ko /lib/modules/net_failover.ko \
         /lib/modules/virtio_net.ko; do
    if [ -f "$p" ]; then
        if ! busybox insmod "$p" 2>/dev/null; then
            echo "initramfs: could not load $p" >&2
        fi
    else
        echo "initramfs: $p is NOT in this image (kernel built it in, or the" \
             "builder did not stage it)" >&2
    fi
done

# ---- cmdline options -----------------------------------------------------
get_opt() {  # $1 = key
    for w in $(cat /proc/cmdline 2>/dev/null); do
        case "$w" in
            "$1"=*) echo "${w#*=}"; return;;
        esac
    done
}
has_opt() {  # $1 = bare flag, e.g. invfs.selftest
    # get_opt matches "key=value" only, so a bare flag read through it ALWAYS
    # comes back empty -- which is how `invfs.selftest` first did nothing and
    # the boot looked like it had simply not run the check. The sweepboot
    # flag at the bottom of this file has its own bare-key loop for the same
    # reason; this is that loop, named.
    for w in $(cat /proc/cmdline 2>/dev/null); do
        case "$w" in
            "$1"|"$1"=*) return 0;;
        esac
    done
    return 1
}
RAW_UUID=$(get_opt invfs.raw_uuid)
DEV1_UUID=$(get_opt invfs.dev1_uuid)

# ---- find InvFS volumes by superblock magic + uuid -----------------------
# Uses invf-fuse's own parser (the initramfs busybox has no dd/od); prints
# "dev uuid" lines for every block device carrying an InvFS superblock.
invfs_probe() {
    for d in /dev/sd? /dev/sd?? /dev/vd? /dev/vd?? \
             /dev/sd?[0-9] /dev/sd??[0-9] /dev/vd?[0-9] /dev/vd??[0-9]; do
        [ -b "$d" ] || continue
        uuid=$(invf-fuse --probe-uuid "$d" 2>/dev/null) || continue
        [ -n "$uuid" ] && echo "$d $uuid"
    done
}

INVFS_RAW=""
INVFS_DEV1=""
PROBED=$(invfs_probe)

# pick RAW: explicit uuid match, else the first probed device
while read -r dev uuid; do
    [ -n "$dev" ] || continue
    if [ -n "$RAW_UUID" ] && [ "$uuid" = "$RAW_UUID" ]; then
        INVFS_RAW="$dev"
    elif [ -z "$RAW_UUID" ] && [ -z "$INVFS_RAW" ]; then
        INVFS_RAW="$dev"
    fi
done <<EOF
$PROBED
EOF

# pick DEV1: explicit uuid match, else the second distinct device
while read -r dev uuid; do
    [ -n "$dev" ] || continue
    [ "$dev" = "$INVFS_RAW" ] && continue
    if [ -n "$DEV1_UUID" ] && [ "$uuid" = "$DEV1_UUID" ]; then
        INVFS_DEV1="$dev"
    elif [ -z "$DEV1_UUID" ] && [ -z "$INVFS_DEV1" ]; then
        INVFS_DEV1="$dev"
    fi
done <<EOF
$PROBED
EOF

echo "INVFS probe: [$PROBED]"
echo "INVFS_RAW=$INVFS_RAW DEV1=$INVFS_DEV1"

# WP125: when the root volume will not come up, the box is only rescueable if
# the repair tools are reachable from THIS shell. mkinitramfs.sh stages
# invf-verify/invf-fsck/invf-cat/invf-ls/invf-stat for exactly this reason.
rescue_shell() {
    cat <<EOF

--------------------------------------------------------------------------
InvariantFS root did not come up. This initramfs carries the repair tools,
so the volume can be inspected and fixed from here -- no rescue media needed.

  # what is on the volume (works on an UNMOUNTED volume, unlike the mount):
  invf-ls     $INVFS_RAW
  invf-cat    $INVFS_RAW /path/to/file /tmp/out

  # is it damaged?  NOTE: trust the EXIT CODE, not the word OK on stdout --
  # invf-verify prints "OK" for the superblock pass BEFORE --deep runs.
  invf-verify --deep $INVFS_RAW; echo "exit=\$?"

  # repair, then re-check:
  invf-fsck -f $INVFS_RAW
  invf-verify --deep $INVFS_RAW; echo "exit=\$?"

  # reclaim space (offline; the volume must NOT be mounted):
  invf-sweep $INVFS_RAW

Mounting is impossible while any of the above hold the volume open, so
unmount first if something is still attached.
--------------------------------------------------------------------------
EOF
    sh
}

if [ -z "$INVFS_RAW" ]; then
    echo "ERROR: no InvariantFS volume found on any block device"
    rescue_shell
fi

# ---- WP23 sweepboot: maintenance boot --------------------------------------
# `invfs.sweepboot` on the cmdline turns this boot into a maintenance pass:
# the sweep runs ENGINE-SIDE (the volume is never FUSE-mounted here), then
# reboot -f hands back to the normal entry. /sweepboot-init.sh is staged by
# mkinitramfs.sh and NEVER blocks the fall-through to a normal boot.
SWEEPBOOT=0
for a in $(cat /proc/cmdline 2>/dev/null); do
    case "$a" in invfs.sweepboot) SWEEPBOOT=1 ;; esac
done
if [ "$SWEEPBOOT" = 1 ] && [ -f /sweepboot-init.sh ]; then
    echo "[init] invfs.sweepboot: maintenance boot for $INVFS_RAW"
    . /sweepboot-init.sh "$INVFS_RAW"
    echo "[init] sweepboot returned without rebooting; NORMAL boot continues"
fi

# ---- mount -----------------------------------------------------------------
mkdir -p /tmp /mnt/invfs
# WP212's reason to ignore TERM on this mount is now one of two dispositions,
# chosen per guest init by the WP630 block below; see it for both measurements.
#
# The daemon is backgrounded and the guest's /sbin/init is exec'd below, so
# the daemon is a process in the GUEST's pid namespace. Void's shutdown runs
# "Sending TERM signal to processes..." over that namespace, which reaches
# the daemon serving the guest's root -- and then the guest cannot run its
# own shutdown scripts, because 70-pkill.sh and the `sleep`/`pkill` it execs
# live on that root. Measured on 2 of 2 Void shutdowns: ENOTCONN from exec(2)
# plus a SIGBUS, every time. The kernel removes the mount at poweroff
# regardless, so nothing is lost by ignoring TERM here.
# allow_other is NOT optional here, and its absence is invisible to every
# boot test ever written for this tree.
#
# A FUSE mount without allow_other is accessible ONLY to the uid that mounted
# it. This mount is made from the initramfs as root, so the guest's root
# filesystem belongs to uid 0 and every OTHER uid in the guest gets EACCES on
# every file. Measured on a Debian trixie guest: dbus.service fails every boot
# with status=203/EXEC because systemd runs it as User=messagebus and cannot
# exec /usr/bin/dbus-daemon, while root execs the same binary fine;
# `runuser -u messagebus -- sh` fails the same way. A guest in that state is
# unusable for any non-root process -- no package manager, no unprivileged ssh
# login, no services.
#
# Every boot harness in this repo asserts as root over SSH, so this class of
# bug cannot fail a test; it is invisible until something runs unprivileged.
# tools/test-acl.sh already requires user_allow_other for HOST mounts and
# bench-gate-b.sh / prepare-livecd.sh already pass -o allow_other, so this was
# an inconsistency between the host and initramfs paths rather than a decision.
#
# Root may always request allow_other without /etc/fuse.conf, which the
# initramfs has no reason to carry. INVFS_NO_ALLOW_OTHER=1 exists only so the
# regression can be demonstrated by turning it back off.
# WP630: not mounted with -o survive_term, and the reason is measured.
# survive_term rewrites argv[0] to '@', which makes systemd's killall() skip
# the daemon -- but systemd-shutdown's broadcast_signal fences EVERY process
# with kill(-1, SIGSTOP) before killall() runs, and it does that to the '@'-
# marked daemon too. killall() then walks /proc, and for a FUSE root that walk
# needs the daemon it just froze: PID 1 blocks in the shutdown path, the
# paired kill(-1, SIGCONT) is never reached, and the guest never powers off
# (measured: no daemon heartbeat for 300 s; systemd PR #22875 is the same
# deadlock). Without the option the daemon takes the broadcast SIGTERM,
# unmounts and exits, its FUSE connection aborts, and PID 1's lookups fail
# fast with ENOTCONN -- measured as "Failed to open /proc/: Transport endpoint
# is not connected" followed by "reboot: Power down" in ~1 s.
#
# WP212 measured the opposite disposition on the other init system: with the
# daemon gone, Void's runit stage 3 cannot run its own shutdown scripts,
# because 70-pkill.sh and the sleep/pkill it execs live on this root --
# ENOTCONN from exec(2) plus a SIGBUS, 2 of 2 runs. So the disposition follows
# the guest's init: a systemd guest must let the root go at the end, a runit/
# OpenRC guest must keep serving it. The guest is not asked -- that would need
# its pid namespace -- it is read offline from the volume, before the daemon
# starts, with the initramfs rescue tool (read-only, no mount, no daemon).
#
# The option itself stays; tools/test-sigterm-exit.sh leg C pins its semantics.
if invf-cat "$INVFS_RAW" /lib/systemd/systemd >/dev/null 2>&1 ||
   invf-cat "$INVFS_RAW" /usr/lib/systemd/systemd >/dev/null 2>&1; then
    SURVIVE_TERM=""
else
    SURVIVE_TERM="survive_term"
fi
if [ "${INVFS_NO_ALLOW_OTHER:-0}" = 1 ] || has_opt invfs.no_allow_other; then
    ALLOW_OTHER=""
else
    ALLOW_OTHER="allow_other"
fi
FUSE_OPTS="${SURVIVE_TERM}${SURVIVE_TERM:+${ALLOW_OTHER:+,}}${ALLOW_OTHER}"
# -o with an empty string is not worth the risk on every libfuse build; a
# no-op token keeps the invocation shape unchanged (only reachable with
# INVFS_NO_ALLOW_OTHER=1 on a systemd guest).
[ -n "$FUSE_OPTS" ] || FUSE_OPTS="rw"

if [ -n "$INVFS_DEV1" ]; then
    INVFS_DEV1="$INVFS_DEV1" invf-fuse -o "$FUSE_OPTS" -f "$INVFS_RAW" /mnt/invfs >/tmp/fuse.log 2>&1 &
else
    invf-fuse -o "$FUSE_OPTS" -f "$INVFS_RAW" /mnt/invfs >/tmp/fuse.log 2>&1 &
fi

READY=""
i=0
while [ "$i" -lt 120 ]; do
    if grep -q "InvariantFS mounted" /tmp/fuse.log 2>/dev/null; then
        READY=1
        break
    fi
    sleep 1
    i=$((i + 1))
done

if [ "$READY" != "1" ]; then
    echo "ERROR: invf-fuse did not mount in 120s:"
    cat /tmp/fuse.log
    rescue_shell
fi
grep "InvariantFS mounted" /tmp/fuse.log

# WP212: put the daemon's own log where an operator can read it WITHOUT a
# rescue shell. It used to live only in this initramfs's /tmp -- i.e. on the
# tmpfs that disappears at poweroff -- so a boot that failed AFTER the mount
# (which is every interesting failure) left no trace anywhere: the serial
# console showed only the guest's side of events. The mount line and any
# error the daemon printed are cheap, bounded, and are what a bug report
# needs first. The full log stays in /tmp/fuse.log for the rescue shell,
# which can still cat it.
echo "--- invf-fuse log (WP212) ---"
cat /tmp/fuse.log
echo "--- end invf-fuse log ---"

# ---- self-test (invfs.selftest on the cmdline) ------------------------------
# A rescue medium is only as good as what an operator can do with it, and
# "it booted" is not that. This runs the check that answers the question
# directly: is the volume mounted, and does it serve back the exact bytes it
# was given? The output goes to the console and nowhere else, so it can be
# read from a serial log with no one typing anything. The expected sum is
# baked in only when the builder is given INVFS_SELFTEST_SUM, so a normal
# image carries no test data and the branch is simply skipped.
#
# It exists as a cmdline flag rather than a test-only script because typing
# into this console turned out to be unusable for automation: the guest tty
# drops characters (a character-at-a-time sender with a 12 ms gap still
# delivered "ls /mnt/invfs" as "l/ntnf"), so a harness that types commands
# measures the tty, not the filesystem.
if has_opt invfs.selftest; then
    echo "SELFTEST: begin"
    echo "SELFTEST: device=$INVFS_RAW dev1=$INVFS_DEV1"
    echo "SELFTEST: files at the mount point:"
    ls /mnt/invfs 2>&1 | sed 's/^/SELFTEST:   /'
    N=$(ls /mnt/invfs 2>/dev/null | grep -vc '^\(proc\|sys\|dev\)$')
    echo "SELFTEST: file_count=$N"
    # Byte fidelity, read back THROUGH the mount. A mount that lists names
    # but serves wrong bytes is precisely the failure this filesystem exists
    # to prevent, and this is the only place it can be seen end to end.
    if [ -f /mnt/invfs/invfs-selftest.txt ]; then
        SUM=$(cat /mnt/invfs/invfs-selftest.txt 2>/dev/null | md5sum | cut -d" " -f1)
        WANT=$(cut -d" " -f1 /selftest.sha 2>/dev/null)
        echo "SELFTEST: readback_md5=$SUM"
        echo "SELFTEST: expected_md5=$WANT"
        if [ -n "$WANT" ] && [ "$SUM" = "$WANT" ]; then
            echo "SELFTEST: BYTES_MATCH"
        else
            echo "SELFTEST: BYTES_DIFFER"
        fi
    else
        echo "SELFTEST: no invfs-selftest.txt on the volume; skipping the byte check"
    fi
    # The offline tools want the volume UNMOUNTED and will say so here. A
    # refusal is reported as a refusal, never as a pass.
    echo "SELFTEST: invf-verify on the mounted raw device:"
    invf-verify "$INVFS_RAW" 2>&1 | tail -3 | sed 's/^/SELFTEST:   /'
    echo "SELFTEST: end"
fi

# Debug emergency shell on ttyS0 (background; chroot still happens).
busybox sh -i < /dev/ttyS0 > /dev/ttyS0 2>&1 &

# rbind essential filesystems into the new root BEFORE chroot (Ersei's
# fuse-root recipe): OpenRC needs /proc live for its boot detection and
# /dev alive for console nodes.
mount --rbind /proc /mnt/invfs/proc 2>/dev/null
mount --rbind /sys  /mnt/invfs/sys  2>/dev/null
mount --rbind /dev  /mnt/invfs/dev  2>/dev/null

# WP66 H1: systemd expects /run as tmpfs and cgroup2 before PID1 starts.
# Without these, early-mount units fail and daemon startup hangs.
mkdir -p /mnt/invfs/run
mount -t tmpfs tmpfs /mnt/invfs/run 2>/dev/null
mkdir -p /mnt/invfs/sys/fs/cgroup
mount -t cgroup2 cgroup2 /mnt/invfs/sys/fs/cgroup 2>/dev/null || true

# Kernel modules: the guest kernel has virtio-net as a module; initramfs
# carries net_failover + virtio_net built from the same tree. insmod BEFORE
# the chroot owns the network devices (idempotent if already loaded near
# the top). Accept either layout (mkinitramfs.sh writes /lib/modules;
# the maintained skeleton also stages them under /modules).
for mod in failover net_failover virtio_net; do
    for p in "/lib/modules/$mod.ko" "/modules/$mod.ko"; do
        if [ -f "$p" ]; then
            busybox insmod "$p" 2>/dev/null && break
        fi
    done
done

echo "Chrooting to InvFS root..."
export INVFS_DEV1
# Optional alternate init (documented cmdline option; can be used by the
# Arch bring-up -- systemd may need additional fixes beyond WP66's H1;
# see docs/ARCH-INSTALL.md). Defaults to /sbin/init.
INIT=$(get_opt invfs.init)
[ -n "$INIT" ] || INIT=/sbin/init
# Do NOT exec a chroot whose target does not exist. `exec` replaces PID 1, so
# a failing chroot does not return to a shell -- the kernel panics with
# "Attempted to kill init!", measured on a volume that mounted cleanly and
# simply had no rootfs in it:
#   Chrooting to InvFS root...
#   chroot: can't execute '/sbin/init': No such file or directory
#   Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000132
# That is the wrong answer for a RESCUE medium. "The volume mounts but has no
# init" is an ordinary state -- a data-only volume, a half-restored one, the
# wrong volume, a rootfs whose init is missing -- and it is precisely the case
# an operator boots rescue media to fix. A medium that panics there has
# removed the only way to reach the repair tools, which are sitting right here.
# Say WHAT the hand-off is about to run and why it might refuse: a guest that
# mounts and then lands in this shell with no explanation is undebuggable from a
# serial log (measured on nixos 2026-11-24: "InvariantFS mounted: 40296 files",
# then `sh: can't access tty` and a prompt, no message from this file).
# Always, and cheap: one line before the hand-off. An initramfs that hands off
# (or refuses to) without saying which path it used leaves a serial log that
# cannot be debugged -- measured on nixos 2026-11-24, which mounted the root
# ("InvariantFS mounted: 40296 files") and then appeared at a shell with no
# message from this file at all.
echo "[init] INIT='$INIT' chroot target='/mnt/invfs$INIT'"
ls -l "/mnt/invfs$INIT" 2>&1 | sed 's/^/[init] /'
[ -x "/mnt/invfs$INIT" ] && echo "[init] -x: yes" || echo "[init] -x: NO"
# An ABSOLUTE symlink cannot be tested from the initramfs: the kernel resolves
# /mnt/invfs/init -> /nix/... in THIS root, where /nix does not exist, so `-x`
# answers NO for a link that is perfectly fine after chroot. Measured on nixos
# 2026-11-25:
#   [init] lrwxrwxrwx 1 0 0 33 ... /mnt/invfs/init -> /nix/var/nix/profiles/system/init
#   [init] -x: NO
#   [init] INIT was '/init' ... Staying in the repair shell
# i.e. the volume was mounted (40296 files) and the init refused to hand off to
# a root that was ready. Test it where the kernel will run it: inside the mount.
# `-e` and `-x` FOLLOW the link, so neither can answer for NixOS's /init:
#   /init -> /nix/var/nix/profiles/system/init      (absolute)
# Resolved from the initramfs that target is /nix/... which does not exist here,
# so both tests say no for a root that is fine after chroot. Measured twice
# (2026-11-25, runs 45528526588 and 45534711912): "-x: NO" then "The volume
# mounted, but /init is not there" -- on a volume holding 40296 files. The first
# attempt guarded the in-chroot test with `[ -e ]`, which has the same flaw and
# never fired. Test unconditionally, and fall back to resolving the link by hand
# for the case where the in-chroot test itself cannot run.
# Resolve a path INSIDE the mount, following one symlink component at a time.
#
# `readlink` cannot cross a symlinked DIRECTORY: for
#   /nix/var/nix/profiles/system/init
# where `system` is itself a link into /nix/store, `readlink` on that whole path
# fails with ENOENT -- the kernel must resolve `system` first, and from the
# initramfs's root it cannot. Measured 2026-11-25, run 45556796872: the walker
# printed `hop1 /init -> /nix/var/nix/profiles/system/init` and then stopped,
# with 40296 files mounted and a NixOS root behind them.
#
# So walk the components: readlink each prefix, substitute its target (absolute
# targets restart from the mount root, relative ones stay in the current
# directory), and start over. Bounded, and it prints every substitution it makes.
# Path normalisation without external tools: drop empty and "." components, pop
# on "..". Needed because a relative link target is substituted textually and can
# carry both (measured on the first version: /d/e -> ../f came back as /d/../f).
normalize_path() {
    _in="$1"
    _out=""
    _ifs=$IFS
    IFS=/
    set -- $_in
    IFS=$_ifs
    for _c in "$@"; do
        case "$_c" in
            ''|.) ;;
            ..) _out=${_out%/*} ;;
            *) _out="$_out/$_c" ;;
        esac
    done
    printf '%s
' "${_out:-/}"
}

resolve_in_mount() {
    _p="$1"
    _guard=0
    while [ "$_guard" -lt 40 ]; do
        _guard=$((_guard + 1))
        _cur=""
        _rest="$_p"
        _changed=0
        while [ -n "$_rest" ]; do
            _comp=${_rest%%/*}
            if [ "$_comp" = "$_rest" ]; then _rest=""; else _rest=${_rest#*/}; fi
            [ -z "$_comp" ] && continue
            _next="$_cur/$_comp"
            _tgt=$(readlink "/mnt/invfs$_next" 2>/dev/null || true)
            if [ -n "$_tgt" ]; then
                case "$_tgt" in
                    /*) _cur="$_tgt" ;;
                    *)  _cur="$_cur/$_tgt" ;;
                esac
                _cur=$(normalize_path "$_cur")
                if [ -n "$_rest" ]; then _p="$_cur/$_rest"; else _p="$_cur"; fi
                echo "[init] link $_next -> $_tgt" >&2
                _changed=1
                break
            fi
            _cur="$_next"
        done
        if [ "$_changed" = 0 ]; then
            normalize_path "$_cur"
            return 0
        fi
    done
    return 1
}

HAVE_INIT=0
if [ -x "/mnt/invfs$INIT" ]; then
    HAVE_INIT=1
else
    _final=$(resolve_in_mount "$INIT" || true)
    if [ -n "$_final" ] && [ -x "/mnt/invfs$_final" ]; then
        HAVE_INIT=1
        echo "[init] $INIT resolves inside the volume to $_final"
    fi
fi
if [ "$HAVE_INIT" != 1 ]; then
    echo "The volume mounted, but $INIT is not executable there (checked /mnt/invfs$INIT)."
    _target=$(readlink "/mnt/invfs$INIT" 2>/dev/null || true)
    if [ -n "$_target" ]; then
        echo "It is a symlink to '$_target'; inside the volume that target is"
        if [ -e "/mnt/invfs$_target" ]; then
            echo "present but not executable -- wrong mode, or it is a directory."
        else
            echo "absent, so the link dangles inside the volume too."
        fi
    elif [ -e "/mnt/invfs$INIT" ]; then
        echo "It exists but is not executable -- wrong permissions, or it is a"
        echo "directory."
    else
        echo "This volume has no root filesystem in it: it may be a data-only"
        echo "volume, a half-restored one, or not the volume you meant."
    fi
    echo "[init] INIT was '$INIT' (chroot target /mnt/invfs$INIT)"
    echo "Staying in the repair shell instead of handing off to a rootfs."
    rescue_shell
fi
echo "[init] exec chroot /mnt/invfs '$INIT'"
exec chroot /mnt/invfs "$INIT"
