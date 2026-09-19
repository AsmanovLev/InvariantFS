# WP69 — sweepboot init: stale tracked file vs built artifact

**Branch:** `wp/69-sweepboot-init`
**Worktree:** `/tmp/invfs-wp69`
**Severity:** LOW (latent test lie; no runtime impact)
**Source:** read-only investigation 2026-09-19.
**Estimated effort:** 0.5 day

---

## Scope

`vm/initramfs/init` is a **tracked, stale WP23b file** that still uses
`switch_root` and carries the `invfs.sweepboot` branch. But
`tools/mkinitramfs.sh` overwrites it in the built image with
`tools/initramfs-init.sh`, which has **neither** `switch_root` **nor**
`sweepboot` (the WP63 rewrite dropped the WP23 branch).

Consequence: `tools/test-sweepboot.sh:52-53` greps the **stale tracked
file** and passes, while the actually-built initramfs has lost the
sweepboot branch. The test asserts a behavior the shipped artifact does
not have.

---

## Work

1. Decide the intent:
   - **restore** `invfs.sweepboot` into `tools/initramfs-init.sh` (if the
     sweep-at-boot feature is still wanted), or
   - **retire** it and delete the stale tracked `vm/initramfs/init`.
2. Fix `tools/test-sweepboot.sh` to test the **built artifact** (or
   `tools/initramfs-init.sh` directly), never the stale tracked copy.
3. Remove `tools/mkinitramfs.sh`'s copy of `tools/sweepboot-init.sh` if
   the branch is retired (currently `mkinitramfs.sh:38-39` stages it).
4. If restoring: cross-check the WP23 sweepboot contract and add it back
   to the cmdline table in `docs/GENTOO-INSTALL.md:603-611`.

---

## Validation

1. `bash tools/test-sweepboot.sh` — PASS against the built/real init.
2. Build the initramfs, extract, assert the init matches
   `tools/initramfs-init.sh` (no stale divergence).
3. No regression in `tools/test-arch-install.sh` / `test-void-install.sh`.

---

## Out of scope (do NOT touch)

- WP65–WP68 (rename/systemd/mkinitcpio/bootloader).

---

## Coordination notes

- Subagent ID: `wp69-sweepboot-init`; `INVFS_E2E_AGENT=wp69-sweepboot-init`.
- E2E gates: `tools/test-sweepboot.sh`.
- Dependencies: none.

---

## Key file:line index

- stale tracked: `vm/initramfs/init` (switch_root ~:76,80; sweepboot)
- built init: `tools/initramfs-init.sh`
- test: `tools/test-sweepboot.sh:52-53`
- staging: `tools/mkinitramfs.sh:38-39`
- cmdline table: `docs/GENTOO-INSTALL.md:603-611`
