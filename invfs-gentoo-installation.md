# Gentoo Installation on InvariantFS — VM Testing Guide

## Overview

This documents the process of installing Gentoo Linux in a QEMU VM using the InvariantFS kernel (7.3-rc2) with FUSE root support. The goal is to reproduce and verify the complete Gentoo installation workflow before doing it on real hardware.

## Prerequisites

- QEMU with KVM support (`qemu-system-x86_64`)
- Gentoo stage3 amd64 OpenRC tarball
- InvariantFS kernel (7.3-rc2) built with:
  - `CONFIG_EFI_STUB=y`
  - `CONFIG_FUSE_FS=y`
  - `CONFIG_R8169=y` (or your NIC driver)
  - `CONFIG_REALTEK_PHY=y`
  - All drivers built-in (=y, not as modules)

## VM Setup

### 1. Create VM Image

```bash
# Create a 10GB raw image (can convert to qcow2 later)
qemu-img create -f raw gentoo.raw 10G

# Create ext4 filesystem
mkfs.ext4 -F gentoo.raw
```

### 2. Extract Stage3

```bash
# Mount the image
sudo mount -o loop gentoo.raw /mnt/gentoo-mount

# Extract stage3 (amd64 OpenRC)
sudo tar xf stage3-amd64-openrc-20260906T170102Z.tar.xz -C /mnt/gentoo-mount --numeric-owner

# Create essential device nodes
sudo mknod /mnt/gentoo-mount/dev/null c 1 3
sudo mknod /mnt/gentoo-mount/dev/console c 5 1

# Unmount when done
sudo umount /mnt/gentoo-mount
```

### 3. Configure Base System

Inside the chroot:

```bash
# Set hostname
echo 'vm-gentoo' > /etc/hostname

# Configure SSH
sed -i 's/#*PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config
sed -i 's/#*PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Set root password
echo 'root:YourPassword' | chpasswd

# Create fstab
cat > /etc/fstab << 'EOF'
/dev/sda1   /        ext4   noatime  0 1
none        /proc    proc   defaults 0 0
none        /sys     sysfs  defaults 0 0
none        /dev/shm tmpfs  defaults 0 0
EOF
```

## Kernel Configuration

The InvariantFS kernel must have drivers **built-in** (=y), not as modules (=m):

```
CONFIG_FUSE_FS=y          # FUSE support
CONFIG_EFI_STUB=y         # Boot directly as EFI application
CONFIG_R8169=y            # Realtek NIC driver
CONFIG_REALTEK_PHY=y      # Realtek PHY driver (required for R8169)
CONFIG_MII=y              # MII bus driver
```

**Critical**: If drivers are built as modules (=m), they must be present in the initramfs. With InvariantFS root, loading modules from FUSE doesn't work due to module version mismatch and FUSE limitations.

### Building the Kernel

```bash
cd /path/to/linux-source
cp /path/to/existing/.config .config
make olddefconfig

# Ensure drivers are built-in
sed -i 's/CONFIG_R8169=m/CONFIG_R8169=y/' .config
sed -i 's/CONFIG_REALTEK_PHY=m/CONFIG_REALTEK_PHY=y/' .config
sed -i 's/CONFIG_FUSE_FS=m/CONFIG_FUSE_FS=y/' .config

make -j$(nproc)
```

## Initramfs for FUSE Root

Since InvariantFS root requires FUSE before the root filesystem is available, an initramfs is needed to:
1. Mount the FUSE InvariantFS volume
2. Load any required kernel modules (if not built-in)
3. Switch to the real root

### Minimal Initramfs Contents

```
/init                           # Main init script
/bin/busybox                    # For shell commands
/sbin/switch_root               # From util-linux (NOT busybox applet!)
/lib64/ld-linux-x86-64.so.2    # Dynamic linker
/lib64/libc.so.6               # C library
/usr/bin/invf-fuse             # InvariantFS binary
/lib64/libfuse3.so.4           # FUSE library
/lib64/libzstd.so.1            # Compression library
/lib64/libz.so.1               # Zlib library
/dev/null, /dev/zero, /dev/console  # Device nodes
/dev/c1-c6                     # InvariantFS internal device nodes
/dev/sda, /dev/sda3            # Block devices
```

### Init Script Example

```sh
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/lib64:/usr/lib64

echo "=== InvFS initramfs ==="

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# Check for second InvariantFS device (1.8TB drive)
if [ -b /dev/sdb1 ]; then
    echo "sdb1 present - using dev1"
    INVFS_DEV1=/dev/sdb1 /usr/bin/invf-fuse /dev/sda3 /mnt/newroot -o allow_other,dev
else
    echo "Single device mode"
    /usr/bin/invf-fuse /dev/sda3 /mnt/newroot -o allow_other,dev
fi

if [ $? -ne 0 ] || [ ! -d "/mnt/newroot/sbin" ]; then
    echo "ERROR: invf-fuse failed"
    exec /bin/sh
fi

echo "=== switch_root ==="
exec switch_root /mnt/newroot /sbin/init
```

**Important**: `/sbin/switch_root` must be the **util-linux** version, NOT the busybox applet. Busybox's switch_root may not handle FUSE root correctly.

## Booting with QEMU

```bash
qemu-system-x86_64 \
  -m 4G \
  -hda gentoo.raw \
  -kernel vmlinuz \
  -initrd initramfs.img \
  -append "root=/dev/sda1 rootfstype=ext4 net.ifnames=0 console=ttyS0" \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device e1000,netdev=net0 \
  -display none \
  -serial mon:stdio
```

## Known Issues

### Module Version Mismatch

If you see errors like:
```
module realtek: .gnu.linkonce.this_module section size must match the kernel's built struct module size at run time
```

This means the .ko modules were built against a different kernel version than the running one. **Fix**: Build all required drivers into the kernel (=y) instead of as modules (=m).

### FUSE Duplicate Symbol

```
fuse: exports duplicate symbol fuse_chan_abort (owned by kernel)
```

This happens when FUSE is both built into the kernel AND loaded as a module. **Fix**: Ensure `CONFIG_FUSE_FS=y` (built-in) and don't include `fuse.ko` in the initramfs.

### /sbin/init Points to Busybox

OpenRC requires `/sbin/init` to be its init binary, not busybox:
```bash
# Wrong:
/sbin/init -> busybox

# Correct:
/sbin/init -> openrc-init
```

Fix: `cp /sbin/openrc-init /sbin/init`

### /sbin/switch_root Points to Busybox

The util-linux `switch_root` is required for proper FUSE root switching:
```bash
# Wrong:
/sbin/switch_root -> busybox

# Correct:
cp /usr/sbin/switch_root /sbin/switch_root
```

## Partition Layout (Target Machine)

| Partition | Size | Filesystem | Purpose |
|-----------|------|------------|---------|
| sda1 | 1.6G | FAT32 | EFI System Partition |
| sda3 | ~124G | InvariantFS | Root filesystem |
| sda4 | 2G | swap | Swap |
| sdb1 | 1.8T | InvariantFS | InvariantFS dev1 (second device) |

## EFISTUB Boot Entry

For booting without GRUB using EFISTUB:

```bash
efibootmgr --create \
  --disk /dev/sda \
  --part 1 \
  --label "Gentoo EFISTUB" \
  --loader '\EFI\Gentoo\vmlinuz' \
  --unicode "rootfstype=invfs root=/dev/sda3 net.ifnames=0 initrd=\\EFI\\Gentoo\\initrd.img"
```

Kernel command line:
```
rootfstype=invfs root=/dev/sda3 net.ifnames=0 initrd=\EFI\Gentoo\initrd.img
```

## Testing Checklist

- [x] Kernel boots without panic (tested with QEMU VM)
- [x] Initramfs runs and mounts InvariantFS (tested with QEMU VM)
- [x] OpenRC starts and reaches login prompt (tested with QEMU VM)
- [ ] switch_root succeeds (requires InvFS root, not ext4)
- [ ] Network interface (eth0) appears and gets DHCP IP
- [ ] SSH connection works with password authentication
- [ ] InvariantFS is writable

## Current Status (Sep 15 2026)

### Volume Setup
- `/mnt/sde/invfs-uki/invfs-raw.img` (9.6 GB) - InvFS raw device
- `/mnt/sde/invfs-uki/invfs-shadow.img` (15 GB) - InvFS shadow (mirror)
- Volume size: 23 GB total, ~24% full after fsck
- Two-device mode: works with single dev0, two-device (:dev1) requires Administrator

### Compression Settings
- Default: ZSTD level 19 (slow, high ratio)
- `INVFS_PROFILE=fastest` → LZ4 (fast, good ratio)
- `INVFS_RAW_ADAPT=0` → disables adaptive ZSTD at high fill levels
- **NOTE**: `INVFS_PROFILE=fastest` does NOT disable adaptive ZSTD writes when RAW fill ≥80%

### Copy Progress
- Gentoo stage3 extracted to `/tmp/gentoo-stage/` (~1.1GB, 54026 files)
- InvFS mounted at `/tmp/invfs-clean/` (4470 existing files before copy)
- **Issue**: rsync and cp both fail to write most files via FUSE
  - rsync: `mkstemp` fails with EROFS for most files
  - cp: Creates empty directories but cannot write file content
  - Root cause: invf-fuse doesn't properly support certain FUSE operations
- Files that DID copy: ~5200 files (mostly small files)
- Volume fill increased but inode area became full during sweep

### FUSE Write Issues (INVFS Issue)
invf-fuse has problems with:
1. `mkstemp()` - rsync uses this to create temp files before rename
2. Large file writes - creates directory entry but content fails to write
3. Directory timestamps - reports success but doesn't actually update

Workaround: Use direct block device access via NBD or character device,
not FUSE mount, for bulk data operations.

## Useful Commands

```bash
# Check kernel command line
cat /proc/cmdline

# Check network interfaces
ip link show
ip addr show

# Check mounted filesystems
mount

# Check dmesg
dmesg | tail -50

# Check OpenRC services
rc-status

# Check SSH
ps aux | grep sshd
netstat -tlnp | grep :22
```
