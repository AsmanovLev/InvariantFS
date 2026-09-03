#!/usr/bin/sh
# 90invfs pre-mount hook: mount an InvariantFS rootfs with invf-fuse.
#
# Triggered by either cmdline spelling:
#   rootfstype=invfs root=<device>
#   root=invfs:<device>
# (rd.driver.pre=fuse, emitted by the module's cmdline(), already loaded
# fuse.ko; load_fstype is the belt-and-braces retry.)

command -v getarg > /dev/null || . /lib/dracut-lib.sh
command -v load_fstype > /dev/null || . /lib/fs-lib.sh

[ "${fstype}" = "invfs" ] || [ "${root%%:*}" = "invfs" ] || return 0

rootdev="${root#invfs:}"
rootdev="${rootdev#block:}"

if [ -z "$rootdev" ]; then
    die "invfs: rootfstype=invfs needs root=<device> (or root=invfs:<device>)"
fi

if ! load_fstype fuse; then
    die "invfs: the fuse kernel module is required but not available"
fi

# the initqueue has settled by pre-mount, but a short poll covers slow
# virtio/usb probes
_n=0
while [ ! -b "$rootdev" ]; do
    _n=$((_n + 1))
    [ "$_n" -gt 20 ] && die "invfs: root device $rootdev not found"
    sleep 0.25
done

info "invfs: mounting InvariantFS volume $rootdev on $NEWROOT"
invf-fuse "$rootdev" "$NEWROOT" 2>&1 | vinfo

if ! ismounted "$NEWROOT"; then
    die "invfs: failed to mount root fs on $rootdev"
fi

info "invfs: root fs mounted ($rootdev)"

# a FUSE rootfs has no fsck-at-mount semantics here; silence the generic
# fsck triggers for the switch-root target
[ -f "$NEWROOT"/forcefsck ] && rm -f -- "$NEWROOT"/forcefsck 2>/dev/null
[ -f "$NEWROOT"/.autofsck ] && rm -f -- "$NEWROOT"/.autofsck 2>/dev/null
:
