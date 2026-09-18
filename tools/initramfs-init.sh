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

# Mount essential filesystems
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
sleep 1

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
# the chroot owns the network devices.
busybox insmod /lib/modules/failover.ko 2>/dev/null
busybox insmod /lib/modules/net_failover.ko 2>/dev/null
busybox insmod /lib/modules/virtio_net.ko 2>/dev/null

echo "Chrooting to InvFS root..."
export INVFS_DEV1
exec chroot /mnt/invfs /sbin/init
