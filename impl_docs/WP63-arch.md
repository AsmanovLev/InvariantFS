# WP63-arch — Arch Linux rootfs install onto InvariantFS + QEMU boot

**Branch:** `wp/63-arch`
**Worktree:** `/tmp/invfs-wp63-arch`
**Severity:** N/A (integration / installer)
**Source:** user request (Arch analogue of the verified Gentoo bootstrap)
**Estimated effort:** 1–2 days

---

## Scope

Produce a reproducible Arch Linux install flow onto an InvariantFS root:
build an Arch staging tree on an ordinary filesystem, package it offline
with `invf-import` into a single-device and a two-device volume, and boot
both under QEMU (3 GB, `q35`/KVM) with serial autologin + SSH. No `src/`
engine changes; the fixes are in `tools/` and docs only.

Four initramfs/tooling defects were found and fixed along the way (Bug A–D),
plus two honest findings that are documented rather than fixed (systemd as
PID 1; the two-device FUSE-write limitation).

---

## Bug A — initramfs never loaded `fuse.ko`

**File:** `tools/initramfs-init.sh`
**Function:** init sequence

`mkinitramfs.sh` ships `fuse.ko` at the cpio root, but the init script
never `insmod`ed it. On a kernel with FUSE as a module (Fedora), the
daemon built its in-memory table and printed `InvariantFS mounted: N
files`, then `fuse_mount` failed:

```
InvariantFS mounted: 33327 files
fuse: device /dev/fuse not found. Kernel module not loaded?
[fuse_mount failed]
```

The mountpoint stayed empty, and `chroot /mnt/invfs /sbin/init` died with
`No such file or directory` followed by a kernel panic.

**Fix:** insmod before the probe/mount, with a `/dev/fuse` mknod fallback.

---

## Bug B — modules insmodded from the wrong path

**File:** `tools/initramfs-init.sh`

The script used `/lib/modules/{failover,net_failover,virtio_net}.ko`
while `mkinitramfs.sh` installs them under `/modules/`. The guest kernel
never got the NIC driver (`/sys/class/net` had no `eth0`; `dhcpcd` and
therefore SSH were dead). Fixed to `/modules/*.ko`.

---

## Bug C — busybox applets absent from the initramfs

**File:** `tools/mkinitramfs.sh`

Only `bin/sh -> busybox` was created. The init script calls `mount`,
`grep`, `cat`, `sleep`, `mkdir`, `chroot`, … as bare commands, so the
initramfs died at the first `mount -t proc`. Added applet symlinks via a
loop.

---

## Bug D — `invfs.init=` was documented but not implemented

**File:** `tools/initramfs-init.sh`

`docs/GENTOO-INSTALL.md` lists `invfs.init=<path>`, but the verified init
hardcoded `exec chroot /mnt/invfs /sbin/init`. Added parsing/selection
(default `/sbin/init`), which is what lets the Arch boot pick the
busybox-init fallback.

---

## Finding 1 — systemd cannot be PID 1 on a FUSE root

Investigated as requested. `systemd 261` starts as PID 1 after the chroot
and runs jobs, but `systemd-journald`, `systemd-udevd`,
`systemd-resolved`, `systemd-timesyncd`, `systemd-userdbd` and dbus all
fail. Without udev, `dev-ttyS0.device` never appears and
`serial-getty@ttyS0.service` dies on its dependency; journald's failure
hangs the boot on `Journal Log Access Socket`. With `systemd.log_level=debug`
PID 1 exits and the kernel panics.

**Resolution:** verified fallback — busybox `init` + `/etc/inittab` +
`ttyS0` autologin getty + `sshd`, selected with
`invfs.init=/bin/invfs-init`. Full detail in `docs/ARCH-INSTALL.md` §6.

---

## Finding 2 — first FUSE write poisons symlink lookups on two-device volumes

On a two-device volume, after the first `write()` through the mount,
execs reached through a symlinked directory (`/usr/sbin`, `/bin`,
`/sbin`) fail with `EINVAL`/`ENOTDIR`. Reproduced on the host with a raw
`invf-fuse` mount, no guest involved:

```
sudo chroot mnt /usr/bin/bash -c 'echo hi > /root/probe.txt; cat /root/probe.txt'
# /usr/bin/bash: line 1: /usr/sbin/cat: Invalid argument
sudo chroot mnt /usr/bin/agetty --version
# chroot: failed to run command '/usr/bin/agetty': Not a directory
```

Engine-level multi-device writes are fine (`tools/test-multidev.sh`
passes). **Resolution for the boot:** mount `/run /tmp /var/tmp /var` as
tmpfs and symlink `/etc/resolv.conf` into `/run`, so no runtime write
touches the FUSE root. This is an engine-side follow-up, documented in
`docs/ARCH-INSTALL.md` §7.

---

## Validation

1. **Unit tests:** `make test` — PASS (arctest 4467, blkio 86, codec 169;
   0 failures).
2. **Regression:** `tools/test-arch-install.sh` (offline package/verify,
   single + two-device) — PASS:
   - import 1384 dirs / 23700 files / 8243 symlinks, 0 skipped
   - `invf-fsck -f` then `invf-fsck` → OK, orphans 0
   - `invf-verify --deep` → 33327 files ok, 0 corrupt
   - 4 files bit-exact, DEVT on both devices, metadata mirror in sync
3. **E2E (under the lock):**
   `INVFS_E2E_AGENT=wp63-arch bash tools/run-e2e.sh tools/test-arch-install.sh`
   — PASS.
4. **QEMU boot, 3 GB, `q35`/KVM:**
   - single-device: `INVFS_RAW=/dev/sda DEV1=`, 33327 files, serial
     autologin `[root@arch-invfs ~]#`, eth0 `10.0.2.15`, sshd listening,
     SSH key/password login; `invfs[sda] on / type fuse`.
   - two-device: `INVFS_RAW=/dev/sda DEV1=/dev/sdb`, 33328 files, same
     serial/SSH success.

---

## Deliverables

- `tools/mkinitramfs.sh` — busybox applet symlinks (Bug C).
- `tools/initramfs-init.sh` — fuse.ko + /dev/fuse (A), module path (B),
  `invfs.init=` (D).
- `tools/test-arch-install.sh` — new regression (stages 2–3, no boot).
- `docs/ARCH-INSTALL.md` — new install guide + findings.

---

## Out of scope (do NOT touch)

- Any `src/` engine file (the two-device FUSE-write bug is reported, not
  fixed here).
- The Gentoo install path/docs (shared initramfs fixes are additive).
- Building an Arch kernel/modules; the host kernel + matching `fuse.ko`
  is used.
- `switch_root`/OVMF/GRUB boot flows (not exercised).

---

## Coordination notes

- Subagent ID: `wp63-arch`
- E2E gate:
  - `INVFS_E2E_AGENT=wp63-arch bash tools/run-e2e.sh tools/test-arch-install.sh`
- Requires network + sudo for the *build* branch; set `ARCH_STAGE` to an
  existing Arch root to skip it.
- Dependencies: none.
