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
# Pre-flight: verify sufficient physical memory
_mem_kb=0
while read -r _k _v _u; do
    [ "$_k" = "MemTotal:" ] && { _mem_kb="$_v"; break; }
done < /proc/meminfo 2>/dev/null
if [ "$_mem_kb" -gt 0 ]; then
    _mem_mb=$((_mem_kb / 1024))
    if [ "$_mem_mb" -lt 768 ]; then
        warn "invfs: CRITICAL: INSUFFICIENT SYSTEM MEMORY (${_mem_mb} MB)!"
        warn "invfs: rootfs requires >= 1024 MB (recommended >= 2048 MB)."
        warn "invfs: if running in a virtual machine, disable dynamic memory ballooning and increase memory size (RAM >= 2048 MB)."
        getargbool 0 invfs.ignore_mem || die "insufficient memory (${_mem_mb} MB < 768 MB). Increase VM RAM or pass invfs.ignore_mem=1"
    fi
fi

# pre-mount check, warn-only: fsck.invfs -a never repairs (only -y does).
# A DAMAGED-but-readable volume still mounts; the verdict stays in the
# journal instead of silent.
if command -v fsck.invfs >/dev/null 2>&1; then
    if fsck.invfs -a "$rootdev" >/tmp/invfs-fsck.log 2>&1; then
        vinfo < /tmp/invfs-fsck.log
    else
        vinfo < /tmp/invfs-fsck.log
        info "invfs: WARNING: pre-mount check of $rootdev reported damage; mounting anyway"
    fi
fi
# WP630: a systemd root must let go on shutdown, never survive_term
if invf-cat "$rootdev" /usr/lib/systemd/systemd >/dev/null 2>&1 ||
   invf-cat "$rootdev" /lib/systemd/systemd >/dev/null 2>&1; then
    mopts="allow_other"
else
    mopts="allow_other,survive_term"
fi
if [ "$_mem_kb" -gt 0 ] && [ "$_mem_mb" -lt 1500 ]; then
    case "${rootflags:-}" in
        *arc_limit=*) ;;
        *) mopts="$mopts,arc_limit=64M" ;;
    esac
fi
[ -n "${rootflags:-}" ] && mopts="$mopts,$rootflags"
invf-fuse -o "$mopts" "$rootdev" "$NEWROOT" 2>&1 | vinfo

if ! ismounted "$NEWROOT"; then
    die "invfs: failed to mount root fs on $rootdev"
fi

info "invfs: root fs mounted ($rootdev)"

# a FUSE rootfs has no fsck-at-mount semantics here; silence the generic
# fsck triggers for the switch-root target
[ -f "$NEWROOT"/forcefsck ] && rm -f -- "$NEWROOT"/forcefsck 2>/dev/null
[ -f "$NEWROOT"/.autofsck ] && rm -f -- "$NEWROOT"/.autofsck 2>/dev/null
:
