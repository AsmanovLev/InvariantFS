# Gentoo on InvariantFS — Install Guide

This guide walks through creating a bootable Gentoo/OpenRC system
with its entire root filesystem on an InvariantFS volume.

## Prerequisites

- A running Linux host with FUSE 3, `parted`, `grub2-install` (or
  `grub-install`), and a Gentoo stage3 tarball.
- InvariantFS binaries (`bin/invf-*`) built from this tree.
- Sufficient disk space for the volume image (~10 GiB recommended).

## Step 1: Create and format the volume

```bash
# Create a 10 GiB sparse InvariantFS image
bin/invf-mkfs vol-root.img 10

# Mount it via FUSE
mkdir /mnt/root
bin/invf-fuse vol-root.img /mnt/root
```

## Step 2: Extract Gentoo stage3

```bash
# Download stage3 from https://www.gentoo.org/downloads/
tar xJpf stage3-amd64-openrc-*.tar.xz -C /mnt/root
```

## Step 3: Configure the guest

The helper script `tools/configure-guest.sh` sets up everything
needed for a minimal bootable Gentoo system:

```bash
tools/configure-guest.sh /mnt/root
```

This configures:
- Binary package host (binhost) with GPG stub for the VM
- `/etc/fstab` with `invfs` root mount
- Serial console getty on `ttyS0`
- `sshd` with root login (test VM only)
- Default runlevels (sshd, dhcpcd)
- Portage `locks.py` patch for InvariantFS dcache ghosts
- `getuto` stubs (real gpg needs mmap support)
- `installkernel` with dracut backend

## Step 4: Build kernel and initramfs

Inside the FUSE-mounted root (or via chroot):

```bash
# Chroot into the volume
mount -t proc proc /mnt/root/proc
mount -t sysfs sysfs /mnt/root/sys
mount -rbind /dev /mnt/root/dev
chroot /mnt/root /bin/bash

# Build kernel (uses dracut for initramfs)
emerge gentoo-sources
make menuconfig  # ensure FUSE, virtio, ext4 modules
make -j$(nproc) && make modules_install
emerge dracut
dracut --hostonly /boot/initramfs.img
exit
```

## Step 5: Build the InvariantFS initramfs

On the host (not chroot):

```bash
tools/mkinitramfs.sh
# Produces vm/initramfs.cpio.gz
```

This creates a minimal initramfs containing:
- `busybox-static`
- `invf-fuse` + `invf-sweep`
- `fuse.ko` (decompressed from the host kernel modules)
- `virtio_net.ko`, `failover.ko`, `net_failover.ko` (guest NIC)
- `sweepboot-init.sh` (WP23 maintenance boot)

## Step 6: Assemble the boot disk

```bash
tools/mkdisk.sh vol-root.img disk.img
```

This creates a GPT disk with:
- **Partition 1**: 256 MiB ESP (FAT32) with GRUB EFI (removable),
  kernel, and initramfs
- **Partition 2**: InvariantFS volume (byte-copied from `vol-root.img`)

## Step 7: Boot with QEMU

```bash
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -nographic \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vm/OVMF_VARS.fd \
  -drive file=vm/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0
```

The boot chain is:

1. OVMF (UEFI firmware) loads GRUB from the ESP
2. GRUB loads the kernel and InvariantFS initramfs
3. The initramfs `init` script loads `fuse.ko`, mounts `/dev/vda2`
   via `invf-fuse`, and `switch_root` into Gentoo

## Boot Options (kernel cmdline)

| Parameter | Effect |
|-----------|--------|
| `root=/dev/vda2` | Root device (default: `/dev/vda`) |
| `invfs.sweepboot` | Maintenance boot: runs sweep engine-side without FUSE mount, then reboots |
| `invfs.init=shell` | Drop to a rescue shell instead of `/sbin/init` |
| `invfs.init=<path>` | Run a custom init binary instead of `/sbin/init` |

## Maintenance: Background Sweep

To trigger a manual sweep on a running system:

```bash
# From inside the guest
/usr/local/bin/invf-sweep
```

Or from the host (via SSH):

```bash
ssh root@localhost -p 2222 'kill -USR1 $(pidof invf-fuse)'
```

The sweep runs transparently; files remain readable throughout.

## Troubleshooting

### Mount fails in initramfs

If the initramfs drops to a shell, check:
- `/dev/vda2` exists (`ls /dev/vda*`)
- `fuse.ko` loaded (`lsmod | grep fuse`)
- `invf-fuse` runs manually (`/usr/local/bin/invf-fuse /dev/vda2 /newroot`)

### Kernel panics at boot

Ensure the kernel includes:
- `CONFIG_FUSE_FS=m`
- `CONFIG_VIRTIO_PCI=m` (for virtio disks)
- `CONFIG_VIRTIO_NET=m` (for guest networking)

### Portage EEXIST errors

The `configure-guest.sh` script patches `portage/locks.py` to handle
InvariantFS dcache ghosts from recycled PIDs. If you see
`OSError: [Errno 17] File exists` during `emerge`, re-run the patch
or manually add the `EEXIST` handling as shown in the script.

### Read-only root at boot

If the volume mounts read-only, the superblock may be DIRTY from a
previous crash. The auto-recovery mechanism (`INVFS_AUTO_RECOVER=1`,
the default) handles this at the next mount. To force:

```bash
invf-fsck -f /dev/vda2
```
