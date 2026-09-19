# Void Linux on InvariantFS — Install Guide

WP64. Verified on Sep 19 2026: a Void x86_64 glibc ROOTFS imported onto a
single-device InvariantFS volume **and** a two-device (`INVFS_DEV1`) volume,
booted under QEMU with `-m 3072`, reaching runit stage 2, root autologin on
`ttyS0`, `dhcpcd` on `eth0` (10.0.2.15), and `sshd` reachable on a host
forwarded port. This guide is the Void counterpart of
[`GENTOO-INSTALL.md`](GENTOO-INSTALL.md); the Gentoo verified-bootstrap
recipe (direct `-kernel` + initramfs, offline `invf-import`, `chroot` not
`switch_root`) is the template.

Void uses **runit**, not OpenRC. The service model is a directory of
supervised services — a service is enabled by symlinking `/etc/sv/<name>`
into `/etc/runit/runsvdir/default/`. `runit-init` is `/sbin/init`
(`/sbin` is a usr-merge symlink into `usr/bin`).

---

## 1. Get a Void rootfs tarball

Void publishes `void-x86_64-ROOTFS-<date>.tar.xz` (glibc) and a `-musl`
variant from `https://repo-default.voidlinux.org/live/current/` (or a
mirror). This guide uses the glibc image:

```bash
curl -fLO https://repo-default.voidlinux.org/live/current/void-x86_64-ROOTFS-20250202.tar.xz
```

The ROOTFS tarball is the full `base-system` (runit, coreutils, util-linux,
`dhcpcd`, `sshd`, `ip`, …); no chroot package install is required.

## 2. Stage on an ordinary filesystem

**Never extract into a FUSE mount.** Extract to a normal directory first;
`invf-import` writes the same tree into the volume in one offline pass.

```bash
mkdir -p /var/tmp/invfs-wp64/stage
tar xJpf void-x86_64-ROOTFS-20250202.tar.xz -C /var/tmp/invfs-wp64/stage
```

The tree ships FHS usr-merge symlinks (`/bin`, `/sbin`, `/lib`, `/lib64` ->
`usr/*`). `invf-import` preserves them; the guest kernel resolves them
natively.

## 3. Provision

`tools/configure-void.sh` runs against either the staging directory (the
recommended path, used before import) or a host FUSE mount, and is
FUSE-portable: it rewrites files in place (`>` / truncate, same inode) and
creates symlinks with `os.symlink`, never relying on `rename(2)`. It:

- sets the hostname to `void-invfs`;
- writes `/etc/sv/agetty-serial/conf` with `GETTY_ARGS="-L -8 -a root"` and
  enables `agetty-ttyS0` (root autologin on the serial console);
- enables `dhcpcd` and `sshd` in `/etc/runit/runsvdir/default/`;
- drops `/etc/ssh/sshd_config.d/99-invfs.conf` (`PermitRootLogin yes`,
  `PasswordAuthentication yes`, `UseDNS no`);
- pre-generates the sshd host keys (copied in, not generated via rename on
  the target);
- sets the root password (default `root`, override with
  `INVFS_VOID_ROOT_PASS=…`) by rewriting `/etc/shadow` in place;
- writes `/etc/fstab`;
- patches `/etc/runit/core-services/03-filesystems.sh` with a FUSE-root
  guard (see "Known issue: FUSE root cannot be remounted" below).

```bash
tools/configure-void.sh /var/tmp/invfs-wp64/stage
```

## 4. Format the volume

Single device (15 GiB, rootfs-sized metadata zone):

```bash
INVFS_META_FRAC=16 bin/invf-mkfs root.img 15
```

Two devices (`INVFS_DEV1` is the shadow/mirror and is passed to every tool
that opens the volume):

```bash
INVFS_META_FRAC=16 bin/invf-mkfs root.img 15 shadow.img 20
```

`INVFS_META_FRAC=16` gives ~992 MB of metadata on a 15 GiB volume, enough
for a ~65k-object rootfs (`AGENTS.md` §2.7).

## 5. Import offline

```bash
# single-device
bin/invf-import root.img /var/tmp/invfs-wp64/stage

# two-device
INVFS_DEV1=shadow.img bin/invf-import root.img /var/tmp/invfs-wp64/stage
```

Expected: `imported: 623 dirs, 6460 files, 878 symlinks, 0 specials,
0 skipped` (~2 s single, ~3 s two-device). Verify:

```bash
bin/invf-ls root.img | tail -1          # 7961 file(s)
INVFS_DEV1=shadow.img bin/invf-cat root.img usr/bin/ip /tmp/ip.out
cmp /var/tmp/invfs-wp64/stage/usr/bin/ip /tmp/ip.out   # bit-exact
```

`tools/test-void-install.sh` performs stages 1–5 plus the full verification
(object count, provisioned symlinks, 11 bit-exact files, `fsck` clean,
metadata-mirror byte identity) and cleans up after itself:

```bash
INVFS_E2E_AGENT=wp64-void bash tools/run-e2e.sh tools/test-void-install.sh
```

> **Do not run `invf-fsck -f` on a volume you are about to boot.** See
> "Known issue: `invf-fsck -f` before first boot" below. The test runs it
> only to assert the on-disk clean state; the boot path imports fresh.

## 6. Build the initramfs

`tools/mkinitramfs.sh` builds `vm/initramfs.cpio.gz` from the current
kernel (`uname -r`), bundling `busybox-static` (with relative applet
symlinks), `invf-fuse`, and `fuse.ko` + `virtio_net`/`net_failover`/
`failover` built for that exact kernel:

```bash
tools/mkinitramfs.sh
```

With a stock distro kernel FUSE is a module (`CONFIG_FUSE_FS=m`), so the
initramfs `insmod`s `/fuse.ko` before `invf-fuse`; the init also loads the
NIC modules from `/modules/` before the chroot. (A kernel built with
`CONFIG_FUSE_FS=y` needs no `fuse.ko`; the `insmod` then simply fails
silently.)

Kernel used in testing: the host `vmlinuz-7.2.5-200.fc44.x86_64` plus the
matching `fuse.ko`/NIC modules from `/lib/modules/7.2.5-200.fc44.x86_64`.
Build your own kernel with `CONFIG_FUSE_FS=y`, `CONFIG_VIRTIO_BLK=y`,
`CONFIG_VIRTIO_PCI=y`, `CONFIG_VIRTIO_NET=m`, and the serial console if you
prefer.

## 7. Boot under QEMU

Single device:

```bash
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 3072 -smp 2 \
  -kernel /boot/vmlinuz-7.2.5-200.fc44.x86_64 \
  -initrd vm/initramfs.cpio.gz \
  -append 'console=ttyS0,115200' \
  -drive file=root.img,format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
  -display none -serial stdio -monitor none -no-reboot
```

Two devices (order matters only for which device is probed first; the
initramfs identifies volumes by their InvariantFS superblock/uuid, not by
kernel name):

```bash
    -drive file=root.img,format=raw,if=virtio \
    -drive file=shadow.img,format=raw,if=virtio \
```

The initramfs leaves the FUSE mount at `/mnt/invfs` and runs
`exec chroot /mnt/invfs /sbin/init`; `switch_root` cannot enter a FUSE root.

Expected console markers:

```
fuse: init (API version 7.45)
INVFS probe: [/dev/vda <uuid>]
INVFS_RAW=/dev/vda DEV1=                 # single-device
INVFS_RAW=/dev/vda DEV1=/dev/vdb         # two-device
InvariantFS mounted: 7961 files
Chrooting to InvFS root...
- runit: enter stage: /etc/runit/1
=> Welcome to Void!
=> Mounting pseudo-filesystems...
=> FUSE root detected; skipping root fsck/remount and mount -a
=> Initialization complete, running stage 2...
- runit: enter stage: /etc/runit/2
void-invfs login: root (automatic login)
```

## 8. Log in

The serial console autologins root. Over the network (dhcpcd assigns
10.0.2.15 on QEMU user networking):

```bash
ssh -p 2222 root@localhost        # password: root (test VM only)
```

Inside the guest:

```
# grep ' / ' /proc/mounts
invfs[vda] / fuse rw,nosuid,nodev,relatime,...
# sv status /etc/sv/sshd /etc/sv/dhcpcd
run: /etc/sv/sshd: (pid …) …; run: log: (pid …) …
run: /etc/sv/dhcpcd: (pid …) …; run: log: (pid …) …
# ip -o -4 addr show
2: eth0    inet 10.0.2.15/24 ...
```

---

## Known issues and FUSE-portability notes

### FUSE root cannot be remounted (runit core-services)

Void's `03-filesystems.sh` unconditionally runs

```sh
mount -o remount,ro / || emergency_shell
fsck -A …
mount -a
```

On a FUSE root `mount -o remount,ro /` fails: util-linux re-enters
`/sbin/mount.fuse`, which treats the bracketed source (`invfs[vda]`) as a
helper program and exits 127, dropping the boot to an emergency shell
before any service starts. `configure-void.sh` prepends a guard:

```sh
if grep -q ' / fuse ' /proc/mounts 2>/dev/null; then
    msg "FUSE root detected; skipping root fsck/remount and mount -a"
    return 0
fi
```

This is idempotent and only affects FUSE roots; the guard is visible in the
boot log.

### `invf-fsck -f` before first boot breaks runtime lookups (engine bug, out of WP64 scope)

Empirically (A/B, reproduced on both single- and two-device volumes):
importing a fresh volume and booting works; running `invf-fsck -f` on that
volume (which frees the ~900 orphans the import leaves behind and reports
`OK`) and *then* booting causes the guest's FUSE directory lookups to
degrade — the very first commands of runit stage 1 report `not found` for
binaries that are present in the offline `invf-ls` listing, and stage 2
dies with `runit: fatal: unable to start child: /etc/runit/2: not a
directory`. The on-disk tree is intact for offline reads (`invf-cat` is
bit-exact), so this is a runtime/lookup bug, not data loss.

**Workaround:** keep the bootable artifact freshly imported. `invf-fsck -f`
is safe for an offline verification pass, but re-import (or do not run it)
before booting. `tools/test-void-install.sh` runs `fsck -f` only as the
final on-disk assertion and does not boot.

### Import, do not tar through FUSE

Bulk extraction onto a live FUSE mount is unreliable (and unnecessary):
extract on an ordinary filesystem, then `invf-import` the tree offline.
The same applies to editing before first boot — provision the staging tree,
where ordinary tools work.

### busybox applets in the initramfs

`tools/mkinitramfs.sh` creates the busybox applet symlinks as **relative**
links (`ln -sf busybox bin/<applet>`). `busybox --install -s` records the
build-time absolute path, which does not exist in the guest and leaves
`mount`/`sleep`/`grep`/… "not found" in `/init`.

### usr-merge symlinks

`/bin`, `/sbin`, `/lib`, `/lib64` are symlinks into `usr/`. `/sbin/init` is
`usr/bin/init -> runit-init`; do not "fix" it. Executing binaries and
scripts from the FUSE mount works (the verified boot runs runit, coreutils
and udev from it).

### Shutdown

`poweroff` reaches runit stage 3, which kills `invf-fuse`; the volume is
recovered on the next open. A clean `poweroff` preserved the live tree and
user data in testing (offline `invf-ls` count and bit-exact `invf-cat`
confirmed). Unclean kills leave more orphans for the next `invf-fsck`.
