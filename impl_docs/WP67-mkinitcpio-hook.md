# WP67 — make the mkinitcpio `invfs` hook real and tested

**Branch:** `wp/67-mkinitcpio-hook`
**Worktree:** `/tmp/invfs-wp67`
**Severity:** MEDIUM (enablement; no engine change)
**Source:** read-only investigation 2026-09-19; existing untested
`packaging/mkinitcpio/*`; `tools/initramfs-init.sh` direct-boot kit.
**Estimated effort:** 1–2 days

---

## Scope

An Arch/mkinitcpio integration **already exists but has never been built
or booted**:

- `packaging/mkinitcpio/invfs_install` — `add_module fuse`,
  `add_binary invf-fuse invf-sweep invf-fsck invf-verify invf-rollback`,
  stages `*.codecpack`, `add_runscript`.
- `packaging/mkinitcpio/invfs_hook` — `run_hook` sets
  `mount_handler=invfs_mount_handler` when `rootfstype=invfs` or
  `root=invfs:<dev>`; handler `modprobe fuse`, `resolve_device`,
  `invf-fuse "$dev" "$mp"`, `panic` on failure.
- `packaging/install.sh:156-163` installs them when
  `WITH_MKINITCPIO=1` (PKGBUILD sets it; install.sh default is 0).

No test exercises any of this (`grep mkinitcpio tools/` → only
`test-bootstrap.sh` asserting `--no-dracut`).

This WP: **validate and harden the hook**, add a non-boot regression,
and align it with the direct-boot kit's proven cmdline contract.

---

## Work

1. **Parity with `tools/initramfs-init.sh`.** The direct-boot kit probes
   uuid (`invf-fuse --probe-uuid`), supports `invfs.raw_uuid`,
   `invfs.dev1_uuid`, `invfs.init=`, and exports `INVFS_DEV1`. The hook
   currently assumes `root=<dev>` / `rootfstype=invfs` and a single
   device. Decide and implement:
   - two-device: a cmdline spelling (`invfs.dev1=<dev>` /
     `invfs.dev1_uuid=<hex>`) and export `INVFS_DEV1` to PID 1;
   - `invfs.init=` passthrough (WP66 systemd path vs busybox fallback);
   - rootflags passthrough (`invf-fuse -o <rootflags>`).
2. **`switch_root` interaction.** The hook relies on mkinitcpio's generic
   `init` calling `switch_root` after the mount handler returns; the
   direct-boot kit deliberately `chroot`s instead because `switch_root`
   rejects a FUSE mount. **Verify** which mkinitcpio `init` does, and if
   `switch_root` breaks, override it (or document the required `init=`).
   Cross-link WP66 (which owns the systemd/init question).
3. **`invf-fuse` path.** `add_binary invf-fuse` assumes it is on PATH
   (`packaging/PKGBUILD:33-35` installs to `/usr/bin`). Confirm, and
   ensure the dynamic libs (`libfuse3`, `libzstd`, `libz`) are pulled
   (`add_binary` resolves ldd; verify no static-link requirement).
4. **Non-boot regression** `tools/test-mkinitcpio-hook.sh`:
   - `sh -n`/shellcheck both hook files;
   - build an initramfs in a container/`mkinitcpio -c` dry run (or a
     mocked `add_binary`/`add_module` harness) and assert `invf-fuse`
     + `fuse.ko` + codecpacks are present;
   - assert the cmdline contract (`rootfstype=invfs root=...`) selects
     `invfs_mount_handler`.
5. **Wire into `make`** (optional): a `test-mkinitcpio-hook` target; no
   VM required.
6. **Fix the stale sweepboot init** (see WP69) is a sibling, not here.

---

## Validation

1. `sh -n packaging/mkinitcpio/invfs_hook packaging/mkinitcpio/invfs_install`.
2. `bash tools/test-mkinitcpio-hook.sh` — PASS (new).
3. If a build environment is available: `mkinitcpio -c` produces an image
   containing `invf-fuse`, `fuse.ko`, codecpacks; log committed to the WP
   result.
4. Boot hand-off validated by WP66/WP68 (this WP stops at "image builds
   and hook selects the handler").

---

## Out of scope (do NOT touch)

- The engine / rename / systemd questions (WP65/WP66).
- Bootloader and on-target kernel install (WP68).
- dracut/initramfs-tools hooks (separate).
- Actually publishing packages (WP62 domain).

---

## Coordination notes

- Subagent ID: `wp67-mkinitcpio-hook`; `INVFS_E2E_AGENT=wp67-mkinitcpio-hook`.
- E2E gates: new `tools/test-mkinitcpio-hook.sh`;
  `tools/test-bootstrap.sh` (regression).
- Dependencies: WP66 for the `/run`/cgroup + systemd init contract.

---

## Key file:line index

- hook: `packaging/mkinitcpio/invfs_hook`, `packaging/mkinitcpio/invfs_install`
- installer: `packaging/install.sh:146-171`, `packaging/PKGBUILD:33-35`
- direct-boot kit: `tools/mkinitramfs.sh`, `tools/initramfs-init.sh:54-67,108-163`
- dracut equivalent: `packaging/dracut/90invfs/module-setup.sh`
- stale init: `vm/initramfs/init` (WP69)
