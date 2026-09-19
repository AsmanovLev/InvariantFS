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
for p in /modules/failover.ko /modules/net_failover.ko /modules/virtio_net.ko; do
    [ -f "$p" ] && busybox insmod "$p" 2>/dev/null
done

# ---- cmdline options -----------------------------------------------------
get_opt() {  # $1 = key
    for w in $(cat /proc/cmdline 2>/dev/null); do
        case "$w" in
            "$1"=*) echo "${w#*=}"; return;;
        esac
    done
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

if [ -z "$INVFS_RAW" ]; then
    echo "ERROR: no InvariantFS volume found on any block device"
    sh
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
    sh
fi

grep "InvariantFS mounted" /tmp/fuse.log

# Debug emergency shell on ttyS0 (background; chroot still happens).
busybox sh -i < /dev/ttyS0 > /dev/ttyS0 2>&1 &

# rbind essential filesystems into the new root BEFORE chroot (Ersei's
# fuse-root recipe): OpenRC needs /proc live for its boot detection and
# /dev alive for console nodes.
mount --rbind /proc /mnt/invfs/proc 2>/dev/null
mount --rbind /sys  /mnt/invfs/sys  2>/dev/null
mount --rbind /dev  /mnt/invfs/dev  2>/dev/null

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
# Optional alternate init (documented cmdline option; used by the Arch
# bring-up because systemd is not usable as PID1 on a FUSE root -- see
# docs/ARCH-INSTALL.md). Defaults to /sbin/init.
INIT=$(get_opt invfs.init)
[ -n "$INIT" ] || INIT=/sbin/init
exec chroot /mnt/invfs "$INIT"
