#!/usr/bin/bash
# dracut module 90invfs: mount an InvariantFS volume as the root filesystem.
#
# Stages invf-fuse (+ the offline tools and the system codecpack dir) into
# the initramfs and installs a pre-mount hook that mounts
#   rootfstype=invfs root=<dev>      (or the alias  root=invfs:<dev>)
# with the FUSE daemon instead of a kernel mount.
#
# The hook mounts $NEWROOT in pre-mount; 80base/init.sh sees the mounted
# $NEWROOT before its mount-hook loop and proceeds straight to usable_root
# (the same shape as 74virtiofs).

# called by dracut
check() {
    # no daemon on the host -> nothing to stage
    require_binaries invf-fuse || return 1

    # hostonly: only when this host plausibly boots from invfs. A FUSE
    # mount's fstype in /proc is just "fuse", so trust the source field
    # convention (fsname=invfs[...]) and an explicit rootfstype=invfs.
    [[ $hostonly ]] || [[ $mount_needs ]] && {
        for fs in "${host_fs_types[@]}"; do
            [[ $fs == *invfs* ]] && return 0
        done
        grep -qw "rootfstype=invfs" /proc/cmdline && return 0
        findmnt -n -o SOURCE / 2>/dev/null | grep -q "^invfs\[" && return 0
        return 255
    }

    return 0
}

# called by dracut
depends() {
    echo base fs-lib
    return 0
}

# called by dracut
cmdline() {
    # fuse must be in the kernel before the pre-mount hook runs
    printf " rd.driver.pre=fuse"
}

# called by dracut
installkernel() {
    hostonly='' instmods fuse
    printf "%s\n" "$(cmdline)" > "${initdir}/etc/cmdline.d/00-invfs.conf"
}

# called by dracut
install() {
    # the daemon + the offline tools (fsck/verify rescue in the initramfs,
    # invf-sweep for maintenance boots)
    inst_multiple invf-fuse invf-sweep invf-verify invf-fsck invf-rollback

    # the system codecpack dir, staged wholesale: the sweep/read paths
    # resolve packs via /usr/lib/invfs/codecpacks inside the initramfs too.
    # Compiled helpers (<pack>/bin/*) go through inst_binary so their
    # shared libraries ride along; manifests and scripts are data (a
    # script whose interpreter is not in the initramfs simply probes
    # absent and its content waits RAW -- the builtin codecs still work).
    local pack f
    for pack in "${dracutsysrootdir-}"/usr/lib/invfs/codecpacks/*.codecpack; do
        [[ -d $pack ]] || continue
        while IFS= read -r f; do
            case $f in
                */bin/*) inst_binary "${f#"${dracutsysrootdir-}"}" ;;
                *)       inst "${f#"${dracutsysrootdir-}"}" ;;
            esac
        done < <(find "$pack" -type f)
    done

    inst_hook pre-mount 90 "$moddir/invfs-mount.sh"
}
