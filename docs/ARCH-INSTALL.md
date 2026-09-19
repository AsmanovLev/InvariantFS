# Arch Linux on InvariantFS — Install Guide

Status: **verified 2026-09-19**. A full Arch Linux userspace boots from a
single-device and a two-device InvariantFS root under QEMU (3 GB RAM,
`q35`/KVM), with serial autologin and SSH. This is the Arch analogue of
`docs/GENTOO-INSTALL.md`; the verified-bootstrap shape is the same
(direct kernel + initramfs, offline `invf-import`, no `switch_root`,
chroot into the FUSE root), with one important difference: **systemd
as PID 1 requires H1 fixes** (see "systemd on a FUSE root") — the
verified path uses a busybox-init fallback if those fixes are not in
place.

Regression for the packaging half (no boot, no FUSE):
`tools/test-arch-install.sh`.

---

## 1. Prerequisites

- Linux host with FUSE 3, `parted`, `qemu-system-x86_64`, KVM.
- InvariantFS binaries (`bin/invf-*`) built from this tree (`make`).
- ~5 GB of disk for images (they are sparse) plus a staging tree (~600 MB).
- For the fallback boot: the kernel you boot must have `fuse` as a module
  that matches the `fuse.ko` the initramfs carries. Here:
  `/boot/vmlinuz-7.2.5-200.fc44.x86_64` +
  `/lib/modules/7.2.5-200.fc44.x86_64/kernel/fs/fuse/fuse.ko`.
  The tree ships `bin/busybox-static`; no distro busybox is needed.

## 2. Build the staging tree (on an ordinary filesystem, NOT FUSE)

The Arch bootstrap tarball is a chrootable minimal system. Extract **as
root** (it contains root-owned, mode-0500/0555 directories):

```bash
cd /path/to/work
curl -LO https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst
sudo tar --zstd -xf archlinux-bootstrap-x86_64.tar.zst   # -> root.x86_64/
```

Install `base` + SSH + DHCP into it:

```bash
B=$PWD/root.x86_64
printf 'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n' \
    | sudo tee $B/etc/pacman.d/mirrorlist
sudo rm -f $B/etc/resolv.conf && sudo cp /etc/resolv.conf $B/etc/resolv.conf
# pacman space-check + download-user are unreliable in this chroot:
sudo sed -i 's/^CheckSpace/#CheckSpace/; s/^DownloadUser/#DownloadUser/' $B/etc/pacman.conf
sudo mount -t proc proc $B/proc
sudo mount --rbind /sys $B/sys
sudo mount --rbind /dev $B/dev
sudo chroot $B /bin/bash -c '
  pacman-key --init && pacman-key --populate archlinux
  pacman -Sy --noconfirm
  pacman -S --noconfirm --needed base openssh dhcpcd iproute2 iputils'
sudo umount -R $B/proc $B/sys $B/dev
```

> `pacman` failing with `could not determine cachedir mount point … not
> enough free disk space` is the `CheckSpace` bug above, not a real full
> disk. `DownloadUser=alpm` likewise fails when the bootstrap has no
> `alpm` passwd entry.

`tools/test-arch-install.sh` automates all of this (and skips the build if
`ARCH_STAGE` points at an existing tree).

## 3. Provision the guest

```bash
# password + sshd
echo root:root | sudo chroot $B chpasswd
sudo chroot $B ssh-keygen -A
# PermitRootLogin yes / PasswordAuthentication yes in $B/etc/ssh/sshd_config
```

Serial getty (systemd vocabulary, kept for a systemd-capable root):

```
# /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear -s %I 115200,38400,9600 vt102
```

`/etc/fstab` — root is the FUSE mount owned by the initramfs; only the
pseudo-filesystems and tmpfs belong here:

```
proc      /proc proc     defaults                                     0 0
sysfs     /sys  sysfs    defaults                                     0 0
devtmpfs  /dev  devtmpfs mode=0755,nosuid                             0 0
tmpfs     /tmp  tmpfs    defaults,noatime,nosuid,nodev,mode=1777,size=1G 0 0
tmpfs     /var/tmp tmpfs defaults,noatime,nosuid,nodev,mode=1777,size=512M 0 0
```

**FUSE has no `rename()`**: never `sed -i` a file that lives on the
volume. Rewrite the whole file, or better, edit the staging tree before
import.

## 4. Create the InvariantFS volume and import offline

**Never extract a tarball through the FUSE mount.** Stage the tree on an
ordinary filesystem, then import engine-side (`invf-import`, no kernel
round-trip):

```bash
BIN=/path/to/InvariantFS/bin
truncate -s 15G root.img
INVFS_META_FRAC=16 $BIN/invf-mkfs root.img 15
sudo $BIN/invf-import "$PWD/root.img" "$PWD/root.x86_64"
# imported: 1384 dirs, 23700 files, 8243 symlinks, 0 skipped in ~4 s
```

`INVFS_META_FRAC=16` gives a rootfs-sized metadata zone (AGENTS.md:
16–24 for rootfs-class trees). Import defaults uid/gid to 0; pass
`INVFS_IMPORT_KEEP_OWNER=1` to preserve the staging tree's owners.

### Two-device (INVFS_DEV1 shadow) volume

`invf-mkfs` takes the pair directly; `INVFS_DEV1` must be exported for
every tool that opens the volume:

```bash
truncate -s 20G shadow.img
INVFS_META_FRAC=16 $BIN/invf-mkfs root2.img 15 shadow.img 20
sudo env INVFS_DEV1="$PWD/shadow.img" $BIN/invf-import root2.img ./root.x86_64
```

### Post-import repair + verification

`invf-import` leaves superseded metadata records (reported as *orphans*).
Free them once; after that the volume must be clean:

```bash
$BIN/invf-fsck root.img -f          # orphans: N -> freed / REPAIRED
$BIN/invf-fsck root.img             # OK, orphans 0
$BIN/invf-verify root.img --deep    # 33327 files ok, 0 corrupt
```

Two-device verification uses the same commands with
`INVFS_DEV1=shadow.img`; `invf-stats root2.img` should report
`mirror : in sync`.

## 5. Initramfs

```bash
tools/mkinitramfs.sh      # -> vm/initramfs.cpio.gz
```

WP63 fixed four defects in the initramfs tooling that blocked the Arch
bring-up (all in `tools/`, no engine changes):

1. **Load `fuse.ko`.** The init script never `insmod`ed it. With FUSE as
   a module, `invf-fuse` built its table ("InvariantFS mounted: N files")
   but `fuse_mount` failed with `device /dev/fuse not found`, leaving an
   empty mountpoint and a `chroot … /sbin/init: No such file or
   directory` panic.
2. **Module path.** The script insmodded `/lib/modules/*.ko` while
   `mkinitramfs.sh` installs them under `/modules/`; `virtio_net` never
   loaded and the guest had no NIC.
3. **busybox applets.** `busybox` only exposes `mount`/`grep`/`chroot`/…
   through symlinks; the initramfs shipped only `sh`. Added the applet
   symlinks.
4. **`invfs.init=<path>`.** The documented cmdline option was not
   implemented (the script hardcoded `/sbin/init`); it now selects the
   PID1 binary and defaults to `/sbin/init`.

## 6. systemd on a FUSE root — WP66 findings

`systemd 261` *does* start as PID 1 after `chroot /mnt/invfs /sbin/init`:
it enumerates units and runs jobs, reaches timer units, and reacts to
failures. The original boot (2026-09-19) showed a cascade of failures
because the initramfs never set up `/run` as tmpfs or `cgroup2`:

```
[FAILED] Failed to start Journal Service.
[FAILED] Failed to start Rule-based Manager for Device Events and Files.  (udevd)
[FAILED] Failed to start Network Name Resolution. / Time Synchronization.
[FAILED] Failed to start User Database Manager. / Namespace Resource Manager.
[FAILED] Failed to start D-Bus System Message Bus.
```

### WP66 H1 fix (initramfs)

The root cause is that systemd's early-mount units expect `/run` (tmpfs)
and `/sys/fs/cgroup` (cgroup2) to exist before PID 1 starts. The
initramfs (`tools/initramfs-init.sh`) only rbinded `/proc /sys /dev`
and never mounted either. WP66 adds:

```sh
mkdir -p /mnt/invfs/run
mount -t tmpfs tmpfs /mnt/invfs/run 2>/dev/null
mkdir -p /mnt/invfs/sys/fs/cgroup
mount -t cgroup2 cgroup2 /mnt/invfs/sys/fs/cgroup 2>/dev/null || true
```

before the `exec chroot`.

### WP66 H3 fix (FUSE ops)

journald also needs `fallocate()` and `FS_IOC_SETFLAGS` (chattr +C) on
newly created journal files. Without `.fallocate` the kernel returns
EOPNOTSUPP; without `.ioctl` for `FS_IOC_SETFLAGS` it returns ENOTTY.
WP66 adds no-op stubs for both (see `src/cli/fuse_fs.c`).

### Remaining blockers (not fixed in WP66)

Without a live QEMU boot of the H1+H3-fixed initramfs, the following
are **suspected** but unconfirmed:

- **H2 (runtime lookup corruption):** the two-device first-write bug
  (`docs/ARCH-INSTALL.md` §7) and fresh-import+`fsck -f` orphans can
  make udevd/journald/exec lookups return ENOENT/ENOTDIR spuriously.
  These are engine-level bugs, not initramfs issues.
- **H4 (mount constraints):** root is always `nosuid,nodev`; systemd's
  shutdown/remount path re-enters `/sbin/mount.fuse` and exits 127.
  Affects shutdown, not startup.

### Verified fallback: busybox init + getty

Chosen via `invfs.init=/bin/invfs-init`:

```
# /bin/invfs-init
#!/bin/sh
exec /bin/busybox init

# /etc/inittab
::sysinit:/etc/rc.invfs
ttyS0::respawn:/usr/bin/agetty --autologin root --noclear -L 115200 ttyS0 vt100
::ctrlaltdel:/usr/bin/reboot
::shutdown:/bin/umount -a -r
```

`/etc/rc.invfs` mounts all volatile directories as **tmpfs**
(`/run /tmp /var/tmp /var`), brings the NIC up (`dhcpcd`, static
`10.0.2.15` fallback for QEMU user-net), writes the resolv.conf through
its `/run` symlink, and starts `sshd -D -e`. Root's shell in
`/etc/passwd` is `/usr/bin/bash` (the `/bin → usr/bin` path returned
`ENOTDIR` from a busybox-init login child in testing; use the real path).

> The tmpfs-over-`/var` is not cosmetic: see §7.

## 7. Two-device FUSE-write limitation (workaround required)

On a **two-device** volume, the first `write()` through the FUSE mount
poisons later symlink lookups. Reproduced on the host with a raw
`invf-fuse` mount (no guest involved):

```
$ sudo chroot mnt /usr/bin/bash -c 'echo hi > /root/probe.txt; cat /root/probe.txt'
/usr/bin/bash: line 1: /usr/sbin/cat: Invalid argument
$ sudo chroot mnt /usr/bin/agetty --version
chroot: failed to run command '/usr/bin/agetty': Not a directory
```

Engine-level writes are fine — `tools/test-multidev.sh` writes with
`invf-cp`, sweeps, tiers and passes. Only the FUSE write path on a
two-device volume is affected; the single-device FUSE write path is not.
**Workaround used for the boot:** keep every runtime write off the FUSE
root by mounting `/run /tmp /var/tmp /var` as tmpfs and symlinking
`/etc/resolv.conf` into `/run`. The two-device root then boots and serves
reads exactly like the single-device one.

This is an engine-side limitation tracked for a follow-up WP; nothing in
the Arch install flow itself works around it, the boot config does.

## 8. Boot under QEMU (3 GB)

Single device:

```bash
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 3072 -smp 2 \
  -kernel /boot/vmlinuz-7.2.5-200.fc44.x86_64 \
  -initrd /path/to/InvariantFS/vm/initramfs.cpio.gz \
  -append 'console=ttyS0,115200 invfs.init=/bin/invfs-init' \
  -drive id=root,file=/work/root.img,format=raw,if=ide \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
  -display none -serial stdio -monitor none -no-reboot
```

Two devices: add `-drive id=shadow,file=/work/shadow.img,format=raw,if=ide`
and boot `root2.img` as the first drive.

Expected serial markers:

```
INVFS_RAW=/dev/sda DEV1=            # single
INVFS_RAW=/dev/sda DEV1=/dev/sdb    # two-device
InvariantFS mounted: 33327 files
Chrooting to InvFS root...
invfs-arch: NIC=eth0
invfs-arch DIAG addr:    inet 10.0.2.15/24 scope global eth0
invfs-arch: rc.invfs done
Arch Linux 7.2.5-200.fc44.x86_64 (ttyS0)
(none) login: root (automatic login)
[root@arch-invfs ~]# 
Server listening on 0.0.0.0 port 22.
```

From the host (key auth shown; `sshpass -p root ssh …` also works):

```bash
ssh -i ~/.ssh/id_ed25519 -p 2222 root@localhost \
    'grep PRETTY /etc/os-release; mount | grep " / "'
# PRETTY_NAME="Arch Linux"
# invfs[sda] on / type fuse (rw,nosuid,nodev,relatime,...)
```

Both boots were verified on 2026-09-19: serial autologin, `dhcpcd` lease
`10.0.2.15`, sshd listening, password and key login as root.

## 9. Standard guide — bootable GPT disk with OVMF + GRUB (WP68)

The direct `-kernel`/`-initrd` path above is useful for testing, but a
real install needs a **bootloader that reads kernel and initramfs from a
filesystem the firmware understands**. The bootloader (GRUB/EFI) cannot
read InvariantFS — there is no kernel driver or EFI driver for the
format. Therefore the kernel and initramfs must live on an **ESP (FAT32
partition)**, and the InvFS root is a separate partition reached by the
initramfs hook (WP67).

**Hard constraint:** the bootloader cannot read InvFS. There is no
kernel driver and no EFI driver for the format. Kernel + initramfs must
live on a filesystem the firmware understands (ESP/FAT32 and/or a small
ext4 `/boot`), and the InvFS volume is a separate partition/device
reached by the initramfs hook.

### 9.1. Disk layout

| Partition | Filesystem | Size | Role |
|-----------|-----------|------|------|
| p1 | FAT32 (ESP) | 512 MiB | GRUB EFI + `/boot/vmlinuz-linux` + `/boot/initramfs-linux.img` |
| p2 | raw (InvFS) | remainder | `invf-mkfs`'d root volume |

### 9.2. Creating the disk image

At step 1.10 of the Arch install guide (**Format the partitions**),
instead of `mkfs.ext4 /dev/sda2`:

```bash
# Format the ESP
mkfs.vfat -F32 -n INVFS_ESP /dev/sda1

# Create an InvFS root volume (using the helper script)
BIN=/path/to/InvariantFS/bin
truncate -s 15G /dev/sda2
INVFS_META_FRAC=16 $BIN/invf-mkfs /dev/sda2 15

# Import the staging tree offline
sudo env INVFS_IMPORT_KEEP_OWNER=1 \
    $BIN/invf-import /dev/sda2 /path/to/staging-tree

# Repair orphans
$BIN/invf-fsck /dev/sda2 -f
$BIN/invf-fsck /dev/sda2
```

Or use `tools/mkdisk-arch.sh` to assemble a complete GPT disk image:

```bash
tools/mkdisk-arch.sh --volume root.img \
    --kernel /boot/vmlinuz-linux \
    --initrd /boot/initramfs-linux.img \
    --output vm/disk-arch.img
```

### 9.3. Installing the kernel and initramfs

```bash
# Mount the ESP
mount /dev/sda1 /mnt/efi

# Copy kernel + initramfs to ESP /boot
cp /mnt/invfs/boot/vmlinuz-linux /mnt/efi/boot/
cp /mnt/invfs/boot/initramfs-linux.img /mnt/efi/boot/

# Install GRUB to ESP (standalone EFI binary)
grub-mkimage -O x86_64-efi \
    -o /mnt/efi/EFI/BOOT/BOOTX64.EFI \
    -p /boot/grub \
    fat part_gpt part_msdos normal linux configfile \
    search search_label search_fs_uuid reboot echo test \
    all_video loadenv
```

> `grub-install` fails on FUSE mounts ("failed to get canonical path
> of invfs"). Use `grub-mkimage` manually to build the EFI binary.

### 9.4. grub.cfg (on the ESP, FAT32)

```grub
set default=0
set timeout=3
set gfxpayload=keep

menuentry "Arch Linux (InvariantFS)" {
    insmod part_gpt
    insmod fat
    linux /boot/vmlinuz-linux console=ttyS0,115200 rootfstype=invfs root=/dev/sda2 rw
    initrd /boot/initramfs-linux.img
}
```

> **Put grub.cfg on the EFI partition** (FAT32), not the InvFS root.
> GRUB looks for its config relative to the EFI binary's location. The
> `root=invfs:/dev/sda2` parameter is consumed by the initramfs hook
> (WP67) which mounts the InvFS volume via FUSE and pivots to it.

### 9.5. The initramfs hook

The WP67 `invfs` hook in `mkinitcpio` handles:

1. Loading `fuse.ko`
2. Starting `invf-fuse` on the raw InvFS partition
3. Pivoting to the FUSE root as `/`

Add the hook to `/etc/mkinitcpio.conf`:

```
HOOKS=(base udev modconf block filesystems invfs keyboard)
```

Then regenerate the initramfs:

```bash
mkinitcpio -p linux
```

### 9.6. QEMU verification (OVMF + GRUB)

```bash
tools/boot-arch-qemu.sh --bootloader vm/disk-arch.img
```

This uses OVMF firmware and boots through GRUB, validating the full
boot chain. Requires OVMF (`/usr/share/OVMF/OVMF_CODE.fd` or set
`ARCH_OVMF`).

## 10. Pitfalls found during this bring-up

| Symptom | Cause | Fix |
|---|---|---|
| `invf-fuse` logs `InvariantFS mounted: N files` then `fuse: device /dev/fuse not found` | initramfs never loaded `fuse.ko` | `busybox insmod /fuse.ko` + mknod fallback |
| `chroot: can't execute '/sbin/init': No such file or directory` | the mount was empty (above), or a login/exec through `/bin`/`/sbin`/`/usr/sbin` | real `usr/bin` path; on two-device keep writes off FUSE |
| guest has no `eth0` | `virtio_net.ko` insmodded from the wrong path | insmod `/modules/virtio_net.ko` |
| `pacman … not enough free disk space` on a 100 GB disk | `CheckSpace` in the chroot | comment `CheckSpace` / `DownloadUser` |
| `Cannot utime` / `Directory renamed before its status …` on a tar extract onto the mount | FUSE has no `rename`/utimens semantics for bulk extract | import offline with `invf-import` |
| systemd never reaches a login prompt | udevd/journald fail on FUSE, `dev-ttyS0.device` never appears | use the busybox-init fallback |
| two-device: `cat`/`agetty` give `EINVAL`/`ENOTDIR` after first write | FUSE write on a two-device volume | tmpfs volatile dirs; see §7 |
| import shows thousands of orphans | superseded records left by `invf-import` | `invf-fsck -f` once, then `invf-fsck` is `OK` |

## 11. What is not claimed

- Not a bootable-on-arbitrary-hardware install. The OVMF+GRUB path is
  scripted (`tools/mkdisk-arch.sh` + `tools/boot-arch-qemu.sh
  --bootloader`) but requires real hardware to fully validate. The
  direct `-kernel`/`-initrd` QEMU path is the verified fallback.
- systemd as PID 1 is partially supported after WP66's H1+H3 fixes
  (`/run` tmpfs + cgroup2 in initramfs, fallocate/ioctl stubs). Remaining
  blockers (H2 lookup corruption, H4 remount) need a live boot to confirm.
  The busybox-init fallback remains the verified safe path.
- No package management through FUSE. As with Gentoo, build packages on a
  normal filesystem and import; FUSE `rename`/exec-bit quirks make
  `pacman`/`emerge` inside the mount unreliable.
- Power-loss during a write is not crash-safe (AGENTS.md §2.2); an
  abrupt `kill -9` of a running guest dropped live entries on the
  two-device volume in testing. Shut the guest down cleanly.
