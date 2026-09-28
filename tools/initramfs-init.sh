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
if [ ! -e /dev/fuse ]; then
    busybox insmod /fuse.ko 2>/dev/null || true
    [ -e /dev/fuse ] || busybox mknod /dev/fuse c 10 229 2>/dev/null || true
    [ -e /dev/fuse ] && chmod 666 /dev/fuse 2>/dev/null
fi
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
for p in /lib/modules/virtio_blk.ko /lib/modules/failover.ko \
         /lib/modules/net_failover.ko /lib/modules/virtio_net.ko; do
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
if [ -n "$INVFS_DEV1" ]; then
    INVFS_DEV1="$INVFS_DEV1" invf-fuse -f "$INVFS_RAW" /mnt/invfs >/tmp/fuse.log 2>&1 &
else
    invf-fuse -f "$INVFS_RAW" /mnt/invfs >/tmp/fuse.log 2>&1 &
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
if [ ! -x "/mnt/invfs$INIT" ]; then
    echo "The volume mounted, but $INIT is not there (checked /mnt/invfs$INIT)."
    if [ -e "/mnt/invfs$INIT" ]; then
        echo "It exists but is not executable -- wrong permissions, or it is a"
        echo "directory or a dangling symlink."
    else
        echo "This volume has no root filesystem in it: it may be a data-only"
        echo "volume, a half-restored one, or not the volume you meant."
    fi
    echo "Staying in the repair shell instead of handing off to a rootfs."
    rescue_shell
fi
exec chroot /mnt/invfs "$INIT"
