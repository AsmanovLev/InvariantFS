# WP66 — systemd as PID 1 on an InvariantFS root

**Branch:** `wp/66-systemd-fuse-root`
**Worktree:** `/tmp/invfs-wp66`
**Severity:** HIGH (hard blocker for a "standard" Arch/any-systemd install)
**Source:** `docs/ARCH-INSTALL.md` §6, `impl_docs/WP63-arch.md` Finding 1,
`README.md:138`, read-only investigation 2026-09-19.
**Estimated effort:** investigation 1 day; fix 1–3 days

---

## Scope

WP63/Docs concluded "systemd cannot be PID 1 on a FUSE root" from a boot
where journald/udevd/resolved/timesyncd/userdbd/dbus failed and no getty
appeared. A read-only investigation re-derived the causes and found the
blanket claim is **not** backed by a missing-syscall story: the ops
systemd needs (write, rename, truncate, fsync, mmap, `bind` in `/run`,
`mknod` for FIFO/SOCK/CHR/BLK) are implemented.

This WP does the **instrumented diagnosis first**, then fixes the real
root causes. Do not re-attempt a blind boot; capture evidence.

---

## Hypotheses to confirm (in order)

- **H1 (initramfs gap, most likely).** `tools/initramfs-init.sh` uses
  `exec chroot` (no `switch_root`/`pivot_root`, lines 156-163) and never
  mounts `/run` tmpfs or cgroup2 (the file has no `/run`/`cgroup`; only
  `/proc /sys /dev` rbind at 139-141). systemd expects to set these up
  itself very early, but inside a chroot with the FUSE root at `/` the
  early-mount unit set may fail. **Check: does `/run` exist and is it a
  tmpfs before PID 1?**
- **H2 (runtime lookup corruption).** Two known daemon bugs make execs
  fail on a FUSE root:
  - fresh import + `invf-fsck -f` (documented `docs/VOID-INSTALL.md:237-252`)
  - two-device first `write()` poisons symlink-dir lookups
    (`docs/ARCH-INSTALL.md:220-242`). Arch's `/bin /sbin /usr/sbin` are
    usr-merge symlinks, so udevd/journald/dbus exec/config lookups can
    return `ENOENT`/`ENOTDIR` spuriously.
  **Check: strace the failing unit; is it ENOENT/ENOTDIR from FUSE?**
- **H3 (missing ops, minor).** journald uses `fallocate`,
  `FS_IOC_SETFLAGS` (chattr +C), `O_TMPFILE`. None are wired
  (`.fallocate`/`.ioctl`/`.tmpfile` absent from `fuse_fs.c:2638`). These
  normally degrade, not fail. **Check: journald logs `EOPNOTSUPP`?**
- **H4 (mount constraints).** Root is always `nosuid,nodev`; `mount -o
  remount` re-enters `/sbin/mount.fuse` and exits 127
  (`docs/VOID-INSTALL.md:212-235`). Affects systemd shutdown/remount, not
  startup. Note, do not chase first.

---

## Instrumented diagnosis (do this first)

1. Boot the WP63 single-device volume with
   `systemd.log_level=debug systemd.log_target=console` and capture the
   full serial log to a file (commit it as evidence under
   `/tmp`/WP result, not into the tree).
2. Mount `/run` tmpfs + `cgroup2` **in the initramfs before chroot**
   (small `initramfs-init.sh` change) and re-boot — does udevd/dbus come
   up? This tests H1 cheaply.
3. For units that still fail, `strace -f` (or systemd's own debug) the
   failing exec to classify ENOENT/ENOTDIR (H2) vs EOPNOTSUPP (H3).

---

## Fix (depends on diagnosis)

- **If H1:** update `tools/initramfs-init.sh` (and the packaging hook,
  WP67) to `mount -t tmpfs tmpfs /mnt/invfs/run`, set up
  `/run/systemd/*`, and mount cgroup2 (`/sys/fs/cgroup`) before
  `chroot`. Consider `pivot_root`/`switch_root` alternatives.
- **If H2:** fix the daemon lookup bugs (likely a separate engine WP;
  cross-link). The fresh-import+`fsck -f` bug and the two-device first
  write are both reproducible on the host without a guest.
- **If H3:** add `.fallocate` (no-op/hole-punch where the recipe allows),
  `.ioctl` for `FS_IOC_GETFLAGS/SETFLAGS`, and `.tmpfile` (O_TMPFILE).
  Wire into the ops table + `vol_*` engine calls.

### Deliverables

- `tools/initramfs-init.sh` — `/run` tmpfs + cgroup2 (if H1).
- Engine files for H2/H3 fixes (scoped in this WP if confirmed).
- `docs/ARCH-INSTALL.md` §6 — replace "systemd cannot" with the verified
  result (works / works-with-caveats / still-broken + why).
- `impl_docs/WP66-systemd-fuse-root.md` — result section with logs.

---

## Validation

1. `make test` — must pass.
2. QEMU boot single-device: reach `systemd` "Reached target Multi-User",
   serial getty on ttyS0, sshd listening, SSH login.
3. `systemctl --failed` — report the remaining failed units (ideally
   none for `base`).
4. `tools/test-arch-install.sh` + `tools/boot-arch-qemu.sh` — no
   regression on the busybox fallback.
5. Bit-exactness of a few files via `invf-cat`; `invf-fsck` clean.

---

## Out of scope (do NOT touch)

- rename/checkpoint (WP65) — unless H2 turns out to be the rename bug,
  in which case cross-link and defer.
- mkinitcpio hook packaging (WP67) and bootloader/on-target kernel
  (WP68).
- dracut/initramfs-tools hooks (separate, unless H1 fix is shared).

---

## Coordination notes

- Subagent ID: `wp66-systemd-fuse-root`; `INVFS_E2E_AGENT=wp66-systemd-fuse-root`.
- E2E gates: `tools/boot-arch-qemu.sh`, `tools/test-arch-install.sh`.
- Requires KVM + host kernel + matching `fuse.ko`.
- Dependencies: WP67's initramfs hook is a sibling; keep the `/run`/cgroup
  fix shared.

---

## Key file:line index

- initramfs init: `tools/initramfs-init.sh:6-8,27-29,136-141,156-163`
- mkinitramfs: `tools/mkinitramfs.sh:7,19-26,40-53,71-72`
- ops table: `src/cli/fuse_fs.c:2638-2668`
- `invf_init` caps: `src/cli/fuse_fs.c:2604-2624`
- mknod/special: `src/cli/fuse_fs.c:2262-2312`,
  `src/core/vol_records.c:1362-1387`
- known lookup bugs: `docs/VOID-INSTALL.md:237-252`,
  `docs/ARCH-INSTALL.md:220-242`
- claims: `docs/ARCH-INSTALL.md:167-193`, `impl_docs/WP63-arch.md:81-93`,
  `README.md:138-139`
