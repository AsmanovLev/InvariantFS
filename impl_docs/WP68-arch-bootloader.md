# WP68 — on-target kernel/initramfs + bootloader for an InvFS root

**Branch:** `wp/68-arch-bootloader`
**Worktree:** `/tmp/invfs-wp68`
**Severity:** HIGH (central unsolved problem for a real install)
**Source:** read-only investigation 2026-09-19; `tools/mkdisk.sh` (Fedora-
specific, stale); `docs/GENTOO-INSTALL.md` bare-metal section;
`INCIDENTS.md` OVMF/UKI 25 MB FAT32 blocker.
**Estimated effort:** 2–4 days

---

## Scope

A real Arch install must (1) install a **target kernel + modules + an
initramfs built on the target** into the InvFS root, and (2) have a
**bootloader read that kernel+initramfs before FUSE exists**. Today:

- the verified path boots the **host** kernel + a host-`uname -r`-locked
  initramfs via `-kernel`/`-initrd` (not "standard");
- `grub-install` **fails on a FUSE mount** ("failed to get canonical path
  of invfs", `docs/GENTOO-INSTALL.md`);
- FUSE-only means **no bootloader can read `/boot` from inside the image**.

This WP builds the scripted, distro-neutral disk+boot flow and a
"standard-guide" install path.

---

## Hard constraint (state it in the doc)

**The bootloader cannot read InvFS.** There is no kernel driver and no
EFI driver for the format. Therefore the kernel + initramfs must live on
a filesystem the firmware/bootloader understands (ESP/FAT32 and/or a
small ext4 `/boot`), and the InvFS volume is a separate partition/device
reached by the initramfs hook (WP67).

---

## Work

1. **Disk layout tool** `tools/mkdisk-arch.sh` (generalize the
   Fedora-hardcoded `tools/mkdisk.sh`):
   - GPT: `p1` ESP (FAT32, ~512M–1G), optional `p2` ext4 `/boot`,
     `p3` InvFS raw volume;
   - `invf-mkfs` on the InvFS partition (partition, not whole disk —
     untested in the boot docs, verify);
   - install GRUB (`--removable` for ESP) **without** `grub-install`
     against FUSE: install to the ESP with a hand-written `grub.cfg`
     (`rootfstype=invfs`, `root=invfs:/dev/sda3` or `invfs.raw_uuid=`).
2. **On-target kernel + initramfs** `tools/arch-install-root.sh`:
   - stage Arch (`pacstrap`-equivalent outside FUSE, or offline import —
     see the pacstrap question below), install `linux` + `mkinitcpio`;
   - run `mkinitcpio -p linux` **on the target** (inside a chroot over
     the staging tree, or via the FUSE mount once WP65/66 allow) using
     the WP67 `invfs` hook;
   - copy `vmlinuz-linux` + `initramfs-linux.img` to the ESP/`/boot`;
   - fallback: embedded initramfs / UKI if OVMF 25 MB FAT32 is a problem
     (`INCIDENTS.md` — document the chosen shape).
3. **`pacstrap` through FUSE** — decide explicitly:
   - (preferred, proven) **stage on an ordinary fs → `invf-import`**, then
     install kernel/initramfs into the imported tree; OR
   - `pacstrap` directly onto the FUSE mount once WP65 (rename) and the
     socket/xattr gaps are closed. Document which, and why.
4. **Boot test** extend `tools/boot-arch-qemu.sh` with a `--bootloader`
   path (OVMF + GRUB reading the ESP) in addition to `-kernel`.
5. **Docs:** a new "standard guide" section in `docs/ARCH-INSTALL.md`:
   "at step 1.10 (Format the partitions), instead of `mkfs.ext4` …",
   with the ESP/`/boot` caveat and the hook cmdline.

---

## Validation

1. `tools/test-arch-install.sh` extended: assert
   `/boot/vmlinuz-linux` + `/boot/initramfs-linux.img` exist in the
   volume and are bit-exact via `invf-cat`.
2. QEMU: OVMF + GRUB boots the target Arch kernel; initramfs `invfs`
   hook mounts the volume; PID 1 starts (systemd per WP66, or the
   busybox fallback meanwhile).
3. `invf-fsck` clean after the install; `tools/test-arch-install.sh`
   single + two-device PASS.
4. `make test` unaffected.

---

## Out of scope (do NOT touch)

- rename/checkpoint (WP65) and systemd (WP66) — cross-linked blockers.
- mkinitcpio hook internals (WP67) — this WP consumes it.
- Other distros' installers.

---

## Coordination notes

- Subagent ID: `wp68-arch-bootloader`; `INVFS_E2E_AGENT=wp68-arch-bootloader`.
- E2E gates: `tools/boot-arch-qemu.sh --bootloader`,
  `tools/test-arch-install.sh`.
- Requires OVMF, KVM, network+sudo for the stage build.
- Dependencies: WP67 (hook), WP66 (PID 1), WP65 (rename, if pacstrap-on-FUSE).

---

## Key file:line index

- `tools/mkdisk.sh` (stale, Fedora-specific), `tools/mkinitramfs.sh:8,46`
- `tools/boot-arch-qemu.sh:43-44,83-127,156-203`
- `tools/test-arch-install.sh:40-88,217-281`
- bare-metal GRUB notes: `docs/GENTOO-INSTALL.md` (§ grub-mkimage, line ~535)
- OVMF/UKI blocker: `INCIDENTS.md` (25 MB FAT32 UKI)
- hook contract: `packaging/mkinitcpio/invfs_hook`
