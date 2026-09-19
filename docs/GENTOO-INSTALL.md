# Gentoo on InvariantFS — Install Guide

This guide covers three installation paths:
- **Verified bootstrap (v0.3)**: the currently proven path; two-device
  InvFS root under QEMU with a direct-kernel boot
  (see "Verified Bootstrap Path" below)
- **QEMU/VM legacy path** (Step 1–7): older virtual-machine recipe with
  OVMF + GRUB; superseded for VMs by the verified bootstrap — kept for
  historical reference only
- **Bare-metal** (Step 8): real hardware, SSH-based install, multi-device

## Prerequisites

- A running Linux host with FUSE 3, `parted`, and a Gentoo stage3 tarball.
- InvariantFS binaries (`bin/invf-*`) built from this tree.
- Sufficient disk space for the volume (~10 GiB recommended for VM).

## Verified Bootstrap Path (Sep 2026)

This is the path actually used for a full Gentoo boot on Sep 17–18 2026
(OpenRC reached runlevel 3, root autologin on ttyS0, SSH at host port
2222, `invfs` mounted as `/`, network devices enp0s2 UP). Use it; the
legacy QEMU steps below are superseded.

### Why this differs from the old path

- **Kernel boot is direct (`-kernel`/`-initrd`), NOT OVMF/UKI.** OVMF
  4M builds refuse a 25 MB UKI loaded from a FAT32 ESP (tested across
  virtio-blk/scsi/IDE/AHCI as well as raw FAT16/32 images); that path
  was abandoned. On real hardware with a mainstream UEFI the OVMF-only
  failure does not reproduce, so OVMF+GRUB remains listed for bare
  metal — for VMs the `-kernel` direct boot is the way in.
- **`switch_root` cannot enter a FUSE root.** The initramfs leaves the
  InvFS mount at `/mnt/invfs` and does `exec chroot /mnt/invfs /sbin/init`
  instead (Ersei's "fuse-root" recipe). PID 1 survives the transition,
  so OpenRC owns the console and reparents correctly.
- **Write metadata through `invf-import`, not tar through FUSE.**
  A tar extract onto a mounted InvFS volume died with
  `Cannot utime: No space left on device` + `Directory renamed before
  its status could be extracted` on a first-generation stage3 import.
  `invf-import` on the unmounted volume does the same 66k-object tree in
   ~35 s cleanly.

### Single-device volume (WP62 re-run, Sep 2026)

The two-device layout below is the general case; a **single-device** volume is
the minimal form (no `INVFS_DEV1`, no shadow disk) and is what the direct-boot
path was re-verified on. Exact sequence:

```bash
truncate -s 15G root.img
INVFS_META_FRAC=16 bin/invf-mkfs root.img 15
# extract stage3 into an ordinary directory, then import offline:
bin/invf-import root.img /path/to/stage3-root   # 3076 dirs, 54026 files, 8986 symlinks
bin/invf-fuse root.img /mnt/invfs
tools/configure-guest.sh /mnt/invfs
fusermount3 -u /mnt/invfs
tools/mkinitramfs.sh                            # -> vm/initramfs.cpio.gz
# direct boot, single volume, 3 GB RAM:
qemu-system-x86_64 -machine q35,accel=kvm -m 3G -cpu host -smp 2 \
  -kernel /boot/vmlinuz-$(uname -r) -initrd vm/initramfs.cpio.gz \
  -append console=ttyS0,115200 \
  -drive id=vol,file=root.img,format=raw,if=ide \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
  -display none -serial stdio -monitor none -no-reboot
```

The initramfs carries `fuse.ko` plus the guest NIC modules. `tools/mkinitramfs.sh`
writes the latter under **`/lib/modules/`**, and `tools/initramfs-init.sh` must
(a) install the busybox applet symlinks (`/bin/busybox --install -s /bin`) and
(b) `insmod /fuse.ko` before `invf-fuse` opens `/dev/fuse` — a distro kernel
ships FUSE as a module (`CONFIG_FUSE_FS=m`). `switch_root` is still not used:
the init `chroot`s into the FUSE mount.

**Do not run `invf-fsck -f` on a freshly imported volume** — see the pitfalls
table; the repair path can drop the live top-level usr-merge symlinks.

### Step V1: Format the volume (two devices)

```bash
truncate -s 15G /mnt/sde/invfs-root.img
truncate -s 20G /mnt/sde/invfs-shadow.img

# meta_frac: rootfs-sized metadata zone (16 works for ~65k files;
# AGENTS.md says 16–24 for rootfs-class workload)
INVFS_META_FRAC=16 bin/invf-mkfs /mnt/sde/invfs-root.img 15
```

`INVFS_DEV1` (the shadow device) is passed as an env var to every
tool that opens the volume (`invf-fuse`, `invf-import`, `invf-ls`, …).

### Step V2: Import the stage3 tree offline

Extract stage3 to an ordinary directory first, then import:

```bash
mkdir stage3-root
tar xJpf stage3-amd64-openrc-*.tar.xz -C stage3-root

INVFS_DEV1=/mnt/sde/invfs-shadow.img \
  bin/invf-import /mnt/sde/invfs-root.img ./stage3-root
# imported: 3076 dirs, 54026 files, 8986 symlinks ... in ~35 s
```

Verify after import (offline, no FUSE needed):

```bash
INVFS_DEV1=/mnt/sde/invfs-shadow.img bin/invf-ls /mnt/sde/invfs-root.img | wc -l
# expect 66088 on a clean stage3 (54026 + 3076 + 8986)
```

> Do NOT extract through the FUSE mount. Stage3 ships FHS symlinks at
> `/bin /sbin /lib /lib64` pointing into `usr/*`; import preserves
> them, and the guest kernel resolves them natively.

### Step V3: Volumes over which the guest sees four disks

Disk 1 (10 GB GPT): p1 = 256 MiB FAT32 ESP, p2 = 2 GiB swap,
p3 = shadow half of the InvFS pair. Disk 2 (15 GB): raw InvFS volume
(`invfs-root.img` above). The init script picks `sda3` as `INVFS_DEV1`
and `sdb` as `INVFS_RAW`.

Build /mnt/sde/invfs-uki/: the `vmlinuz` (built with `CONFIG_FUSE_FS=y`,
`CONFIG_VIRTIO=y`, `CONFIG_VIRTIO_NET=m`, `CONFIG_AHCI=y`) and
`initramfs/` described in the next step, plus a cpio wrapper:

```bash
(cd /mnt/sde/invfs-uki/initramfs && find . | sort \
  | cpio -o -H newc --owner=0:0 2>/dev/null) | gzip \
  > /mnt/sde/invfs-uki/initramfs.img
```

### Step V4: The initramfs init

The init script is versioned at **`tools/initramfs-init.sh`** (the same
file `tools/mkinitramfs.sh` installs as `/init`). Its key property:

**Volumes are identified by content, not by kernel device name.** The
script probes every block device with `invf-fuse --probe-uuid <dev>`,
which reads the InvariantFS superblock magic (`InvariFS` at 0x00) and
prints the 16-byte volume uuid (0x08). Kernel names (`sda`/`sdb`/`vda`)
are not stable, and — as hit in practice — an unconditional
`mknod /dev/sda3` fallback makes a *nonexistent* partition look present,
so the old name-based `[ -b ... ]` branch picked the wrong layout and the
mount failed on a single-device volume.

Selection rules:
- first probed InvFS device → `INVFS_RAW`;
- a second distinct one → `INVFS_DEV1` (shadow/mirror);
- optional kernel cmdline overrides `invfs.raw_uuid=<hex>` /
  `invfs.dev1_uuid=<hex>` pin exact volumes.

After the mount it waits for `InvariantFS mounted` in `/tmp/fuse.log`,
`rbind`s `/proc /sys /dev` into the new root, `insmod`s
`failover` → `net_failover` → `virtio_net` in that order (the guest
kernel ships virtio-net as a module), and `exec chroot /mnt/invfs
/sbin/init` (`switch_root` refuses a FUSE root).

The initramfs directory also carries `bin/busybox` (+ sh/chroot/mount
applet symlinks), `sbin/invf-fuse`, and
`lib/modules/{failover,net_failover,virtio_net}.ko` — those are the
modules for the exact `bzImage` in use (build them from the same tree;
out-of-tree module builds will produce `Unknown symbol` at insmod).
Note the initramfs busybox has **no `dd`/`od`** applets, which is why
device probing lives in `invf-fuse` rather than in the shell.

### Step V5: In-guest settings

Edit through the host FUSE mount each time (volume unmounted when
the guest is not running). Script the edits — plain `cat > file`
overwrites are reliable; `sed -i` created a half-alive shadow record
once, so read files back after any write.

1. Serial console getty with autologin — `etc/inittab`:

   ```
   s0:12345:respawn:/sbin/agetty --autologin root -L -w 115200 ttyS0 vt100
   ```

2. Root password — rewrite `etc/shadow` from scratch (stage3 ships it
   locked as `root:*:`) and verify the read-back:

   ```bash
   HASH=$(openssl passwd -6 root)   # pick your own policy in production
   awk -v h="$HASH" 'BEGIN{FS=OFS=":"} $1=="root"{$2=h} {print}' \
       shadow-source > etc/shadow
   chmod 600 etc/shadow
   head -1 etc/shadow   # MUST show the hash; an empty read is a broken record
   ```

3. Network + SSH end-up (done inside the guest once, they persist):

   ```bash
   rc-update add dhcpcd default && rc-service dhcpcd start
   rc-update add sshd default  && rc-service sshd start
   printf 'ssh-ed25519 <host pubkey> opencode@host\n' > /root/.ssh/authorized_keys
   ```

### Step V6: Boot

```bash
qemu-system-x86_64 -machine q35,accel=kvm -m 4G -cpu host -smp 2 \
  -kernel /mnt/sde/invfs-uki/vmlinuz \
  -initrd /mnt/sde/invfs-uki/initramfs.img \
  -append console=ttyS0,115200 \
  -drive id=disk1,file=/mnt/sde/invfs-disk1.img,format=raw,if=ide \
  -drive id=raw_vol,file=/mnt/sde/invfs-root.img,format=raw,if=ide \
  -drive id=shadow,file=/mnt/sde/invfs-shadow.img,format=raw,if=ide \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
  -display none -serial stdio -monitor none -no-reboot
```

Expected console markers:

```
INVFS_RAW=/dev/sdb DEV1=/dev/sda3
InvariantFS mounted: 66088 files
Chrooting to InvFS root...
INIT: version 3.15 booting
OpenRC 0.63.3 is starting up Gentoo Linux (x86_64)
INIT: Entering runlevel: 3
This is localhost (Linux x86_64 7.3.0-rc2)
localhost login: root (automatic login)
```

Then from the host: `ssh -p 2222 root@localhost` (dhcpcd assigns
10.0.2.15; `mount | grep ' / '` shows `invfs[sdb] on / type fuse`).

### Known pitfalls found during this bootstrap

| Symptom | Cause | Fix |
|---|---|---|
| `INIT: cannot execute "/sbin/agetty"` | respawn target lost or broken in `inittab` (or stale FUSE record) | rewrite the full file (`cat >`), never `sed -i` alone |
| `unix_chkpwd: password check failed for user (root)` | `etc/shadow` record truncated by the FUSE edit | rewrite shadow from scratch, verify read-back |
| DHCP: `no valid interfaces found` | `virtio_net` is `=m` and nothing insmods it | initramfs `insmod`s failover/net_failover/virtio_net in that order before chroot |
| ENOSPC mid-extract through FUSE | tar on a live InvFS mount does not handle the dcache ghost face | import offline with `invf-import` |
| getty prints nothing on ttyS0 | `--noclear` and legacy 38400 defaults lose the serial line | use the `s0` line above with `-L -w 115200` |
| ring_"respawning too fast" | exec failed 6+ times in a row | look for the first `cannot execute` message above it |
| `/init: line 23: mount: not found` / `sleep: not found` | the initramfs ships only `bin/busybox` + `bin/sh`, no applet symlinks | `initramfs-init.sh` runs `/bin/busybox --install -s /bin` before any command |
| `chroot: can't execute '/sbin/init': No such file or directory` | guest usr-merge (`/sbin -> usr/bin`, `/lib64 -> usr/lib64`); if those top-level symlinks are absent the ELF loader is lost and execve returns ENOENT | verify the top-level symlinks imported; do **not** run `invf-fsck -f` (below) |
| `invf-fsck -f` reports `orphans: N -> freed` and afterwards the root lacks `/bin /sbin /lib /lib64 /boot /dev` | on a fresh stage3 import `invf-fsck -f` mis-classifies live top-level entries as orphans and removes them | use plain read-only `invf-fsck` on a fresh import; defer `-f` until the engine bug is fixed |

## QEMU/VM Path

### (Legacy) Step 1: Create and format the volume

> SUPERSEDED for VMs by the Verified Bootstrap Path above. Kept for
historical reference: some steps (chroot+kernel build recipes,
configure-guest.sh) still help the bare-metal install.


```bash
# Create a 10 GiB sparse InvariantFS image
bin/invf-mkfs vol-root.img 10

# Mount it via FUSE
mkdir /mnt/root
bin/invf-fuse vol-root.img /mnt/root
```

### Step 2: Extract Gentoo stage3

```bash
# Download stage3 from https://www.gentoo.org/downloads/
tar xJpf stage3-amd64-openrc-*.tar.xz -C /mnt/root
```

### Step 3: Configure the guest

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

### Step 4: Build kernel and initramfs

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

### Step 5: Build the InvariantFS initramfs

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

### Step 6: Assemble the boot disk

```bash
tools/mkdisk.sh vol-root.img disk.img
```

This creates a GPT disk with:
- **Partition 1**: 256 MiB ESP (FAT32) with GRUB EFI (removable),
  kernel, and initramfs
- **Partition 2**: InvariantFS volume (byte-copied from `vol-root.img`)

### Step 7: Boot with QEMU

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

## Step 8: Bare-Metal Installation

Real hardware install on a physical PC via SSH from a Gentoo live CD.
Tested on Intel i3-10100 / H510 / Realtek NIC.

### Disk layout

| Partition | Size | Filesystem | Purpose |
|-----------|------|-----------|---------|
| sda1 | 512 MiB | FAT32 | EFI System Partition (GRUB) |
| sda2 | 1 GiB | ext4 | /boot (kernel + initramfs) |
| sda3 | ~115 GiB | InvariantFS | / (root) |
| sda4 | 2 GiB | swap | swap |

> **Use ext4 for /boot, not XFS.** GRUB's XFS module is unreliable on
> many setups and will fail to find the kernel at boot. ext4 is
> bulletproof with GRUB.

### Multi-device volumes

InvariantFS supports shadow volumes across multiple devices:

```bash
# SSD for raw writes, HDD for shadow/compressed storage
INVFS_META_FRAC=256 invf-mkfs /dev/sda3 115 /dev/sdb1 400
```

The `INVFS_META_FRAC` controls metadata zone sizing (256 = 1/256th of
volume for metadata). Import completes in seconds with WP29 deferred
flush watermarks.

### Stage3 import

```bash
# Mount InvFS via FUSE
invf-fuse /dev/sda3 /mnt/gentoo -o allow_other

# Extract stage3 to tmpfs first (fast)
mkdir -p /tmpfs-staging
tar xJpf stage3-amd64-openrc-*.tar.xz -C /tmpfs-staging

# Import into InvFS
invf-import /dev/sda3 /tmpfs-staging

# Copy to real mount
cp -a /tmpfs-staging/* /mnt/gentoo/
```

### Chroot setup

```bash
mount -t proc proc /mnt/gentoo/proc
mount -t sysfs sysfs /mnt/gentoo/sys
mount --rslave /dev /mnt/gentoo/dev
cp /etc/resolv.conf /mnt/gentoo/etc/
```

> **FUSE limitation:** The Linux kernel VFS layer strips execute bits
> from files on FUSE mounts. This means `emerge` fails inside chroot —
> bash cannot `source` `.ebuild` files through FUSE. Workaround:
> compile kernel and dracut on a **separate machine**, then upload the
> binaries to the target.

### Kernel: compile on a separate machine

Since emerge doesn't work through FUSE, compile the kernel elsewhere:

1. Get the remote's kernel config: `ssh root@target "zcat /proc/config.gz" > .config`
2. Download kernel source matching the remote's version
3. `make olddefconfig && make -j$(nproc) bzImage modules`
4. `make modules_install` then strip to essential `.ko` files only
5. Upload `bzImage` and modules to the remote

Minimal modules for a typical desktop:
```
fuse.ko          # FUSE driver (required for root mount)
r8169.ko         # Realtek NIC (if applicable)
```

All storage (AHCI/SCSI/SD), GPU (i915), and USB (xHCI) are typically
built-in (`=y`) — check with `grep "=y" /boot/config-*`.

### Initramfs

Build a minimal initramfs on the build machine:

```bash
mkdir initramfs && cd initramfs
mkdir -p bin sbin usr/bin usr/sbin proc sys dev lib64 usr/lib64

# Busybox (dynamic linking)
cp /path/to/busybox bin/busybox
ln -s busybox bin/sh
ln -s busybox sbin/switch_root

# InvFS tools + fuse.ko
cp /path/to/invf-fuse usr/bin/
cp /path/to/fuse.ko .

# Shared libraries (copy from ldd output)
cp /lib64/ld-linux-x86-64.so.2 lib64/
cp /usr/lib64/libfuse3.so.4 usr/lib64/
# ... (see ldd output for full list)
```

The init script (`init` at the root of the cpio):

```bash
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# Load FUSE
insmod /fuse.ko
[ -e /dev/fuse ] || mknod /dev/fuse c 10 229

# Parse root= from cmdline
ROOT=""
for x in $(cat /proc/cmdline); do
    case $x in
        root=INVFS:*) ROOT="${x#root=INVFS:}" ;;
        root=invfs:*) ROOT="${x#root=invfs:}" ;;
        rootfstype=invfs) ;;  # just the flag
        root=/dev/*) ROOT="${x#root=}" ;;
    esac
done

[ -z "$ROOT" ] && { echo "no root= on cmdline"; exec /bin/sh; }

mkdir -p /mnt/newroot
invf-fuse "$ROOT" /mnt/newroot -o allow_other 2>&1
[ $? -ne 0 ] && { echo "invf-fuse failed"; exec /bin/sh; }

# Verify mount succeeded (don't use 'mount | grep' — busybox may not show FUSE)
[ ! -d "/mnt/newroot/bin" ] && [ ! -d "/mnt/newroot/usr" ] && {
    echo "mount check failed"
    ls -la /mnt/newroot/
    exec /bin/sh
}

# Find init (Gentoo usr-merge: /sbin -> /usr/bin)
INIT=""
for p in /sbin/init /usr/sbin/init /usr/lib/systemd/systemd /bin/sh; do
    [ -e "/mnt/newroot$p" ] || [ -L "/mnt/newroot$p" ] && { INIT="$p"; break; }
done
[ -z "$INIT" ] && { echo "no init found"; exec /bin/sh; }

# Move mounts to new root (mount --move, don't unmount!)
mkdir -p /mnt/newroot/dev /mnt/newroot/proc /mnt/newroot/sys /mnt/newroot/run
mount --move /dev /mnt/newroot/dev 2>/dev/null
mount --move /proc /mnt/newroot/proc 2>/dev/null
mount --move /sys /mnt/newroot/sys 2>/dev/null

exec switch_root /mnt/newroot "$INIT"
```

Package into cpio.gz:

```bash
find . -print0 | cpio --null -o -H newc | gzip -9 > initramfs.img
```

### Important: /sbin/init symlink

Gentoo stage3 uses usr-merge (`/sbin` → `/usr/bin`). After importing
the stage3, verify `/sbin/init` exists:

```bash
ls -la /mnt/gentoo/sbin/init  # must exist (real file or symlink)
```

If it's missing, create the symlink:

```bash
ln -sf openrc-init /mnt/gentoo/usr/bin/init  # /sbin -> usr/bin
```

Without this, the initramfs will drop to a shell at `switch_root`.

### GRUB installation

`grub-install` fails on FUSE mounts ("failed to get canonical path
of invfs"). Use `grub-mkimage` manually:

```bash
grub-mkimage -O x86_64-efi \
    -o /mnt/efi/EFI/Gentoo/grubx64.efi \
    -p /EFI/Gentoo \
    fat part_gpt part_msdos normal linux configfile \
    search search_label search_fs_uuid reboot echo test \
    all_video loadenv ext2

# Fallback EFI entry
cp /mnt/efi/EFI/Gentoo/grubx64.efi /mnt/efi/EFI/BOOT/BOOTX64.EFI
```

Register with UEFI:

```bash
efibootmgr --create --disk /dev/sda --part 1 \
    --label "Gentoo Linux" \
    --loader '\EFI\Gentoo\grubx64.efi'
```

### grub.cfg (on EFI partition, FAT32)

```grub
set default=0
set timeout=3

menuentry 'Gentoo Linux' {
    insmod ext2
    insmod part_gpt
    set root=(hd0,gpt2)
    linux /vmlinuz-7.3-rc2 rootfstype=invfs root=/dev/sda3
    initrd /initramfs-7.3-rc2.img
}
```

> **Put grub.cfg on the EFI partition** (FAT32), not the ext4 boot
> partition. GRUB looks for its config relative to the EFI binary's
> location. Also copy the kernel and initramfs to the EFI partition as
> a fallback — FAT32 is always readable by GRUB.

### fstab (on the InvFS root)

```
/dev/sda2  /boot    ext4  defaults  0 2
/dev/sda4  none     swap  sw        0 0
proc       /proc    proc  defaults  0 0
sysfs      /sys     sysfs defaults 0 0
tmpfs      /tmp     tmpfs defaults,noatime,nosuid,nodev,mode=1777,size=4G  0 0
tmpfs      /var/tmp tmpfs defaults,noatime,nosuid,nodev,mode=1777,size=2G  0 0
```

`/tmp` and `/var/tmp` **must** be tmpfs — portage uses `rename()` which
fails on InvFS, and build scripts expect fast temporary storage.

### Volume recovery after unclean shutdown

If the volume mounts read-only after a crash:

```bash
invf-fsck -f /dev/sda3
```

The volume may show "corrupt inode record" warnings for records from
the last incomplete write — this is expected and non-fatal.

## Boot Options (kernel cmdline)

| Parameter | Effect |
|-----------|--------|
| `root=/dev/vda2` | Root device (default: `/dev/vda`) |
| `rootfstype=invfs` | Tells initramfs to use InvFS |
| `invfs.sweepboot` | Maintenance boot: sweep engine-side without FUSE mount |
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
- `/dev/sda3` exists (`ls /dev/sda*`)
- `fuse.ko` loaded (`lsmod | grep fuse` or `dmesg | grep fuse`)
- `invf-fuse` runs manually (`/usr/bin/invf-fuse /dev/sda3 /mnt/newroot`)
- `/sbin/init` exists on the root (`ls -la /mnt/newroot/sbin/init`)

### switch_root drops to shell ("can't access tty")

Usually means `/sbin/init` was not found. See the `/sbin/init` symlink
section above. The initramfs will list the root contents before dropping
to help diagnose.

### Kernel panics at boot

Ensure the kernel includes:
- `CONFIG_FUSE_FS=y` (built-in, NOT module — initramfs loads before modules)
- `CONFIG_AHCI=y` or `CONFIG_SATA_AHCI=m` (storage)
- `CONFIG_R8169=m` or `=y` (Realtek NIC, if applicable)
- `CONFIG_DRM_I915=m` or `=y` (Intel GPU, if applicable)

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
invf-fsck -f /dev/sda3
```

### emege fails through FUSE

The Linux kernel VFS layer (`fs/fuse/file.c`) strips execute bits from
files on FUSE mounts. bash cannot `source` `.ebuild` files, so
`emerge` always fails inside an InvFS chroot.

**Workaround:** compile packages on a separate machine and upload the
binaries. For the kernel and dracut, this is the recommended approach.
