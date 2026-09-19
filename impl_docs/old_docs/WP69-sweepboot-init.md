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

---

## Results

**Decision: RESTORE.** Sweepboot is a live, documented feature with real
consumers, so retiring it would have silently deleted working
functionality:

- `docs/GENTOO-INSTALL.md:609` documents `invfs.sweepboot` in the kernel
  cmdline table; `:321` lists `sweepboot-init.sh` as an initramfs member.
- `src/doc/13-linux-rootfs.md:146-183` specifies the whole maintenance
  boot contract (engine-side sweep, self-hosting packs, `reboot -f`
  oneshot pattern, failure fall-through).
- `tools/invf-sweep.c:57,155,292,708` implements the dedicated
  `--extract-packs` helper mode for the branch.
- `packaging/man/invf-sweep.8:81` (and the ru translation) documents it.
- `tools/mkinitramfs.sh:31-39` deliberately stages
  `tools/sweepboot-init.sh` into the image.
- `tools/test-sweepboot.sh` is the persistent regression.

The stale tracked `vm/initramfs/init` was a **build artifact** (`vm/` is
gitignored; `mkinitramfs.sh` copies `tools/initramfs-init.sh` over it) —
not the source of truth. It was retired so no divergence remains, and
the sweepboot branch was ported into the actual built init.

### Files changed

- `tools/initramfs-init.sh` — added the `invfs.sweepboot` cmdline branch
  (parses `/proc/cmdline`, sources `/sweepboot-init.sh "$INVFS_RAW"`
  when present, then falls through to the normal FUSE mount). Placed
  after device discovery so the maintenance pass gets the same
  magic+uuid-probed device the normal boot uses; documented the option
  in the header comment.
- `tools/mkinitramfs.sh` — `mkdir -p "$IR"` before `cd "$IR"`, because
  the tracked `vm/initramfs/init` that used to force the directory to
  exist is gone (fresh checkout has no `vm/`); comment updated.
- `tools/test-sweepboot.sh` — Leg 0 now `bash -n`s and greps
  `tools/initramfs-init.sh` (the file `mkinitramfs.sh` installs as
  `/init`) for `invfs.sweepboot` and the `sweepboot-init.sh` source,
  instead of the stale tracked copy. `deep_ok` no longer treats
  `invf-verify`'s exit code as a file-integrity verdict: that exit code
  also folds in parity drift, which is the pre-existing core bug owned
  by `test-seal.sh`. It now greps the `0 corrupt,` line (the
  sweepboot-relevant invariant), prints the parity line, and mirrors the
  `|| true` pattern of `tools/test-compact.sh:466`.
- `vm/initramfs/init` — `git rm`'d (stale WP23b, `switch_root`-based).
- `impl_docs/WP69-sweepboot-init.md` — this section.

`tools/sweepboot-init.sh` was **kept** (the branch sources it).

### Validation

- `make -j4` — clean, rc 0.
- `bash tools/test-sweepboot.sh` — **PASS** (Legs 0-6).
- `make test` — PASS: 4467 + 86 + 169 + 22 checks, 0 failures
  (1 skip: privilege drop, not root).
- Initramfs build/extract: `bash tools/mkinitramfs.sh` succeeded; the
  extracted `/init` is **byte-identical** to `tools/initramfs-init.sh`
  (`diff` clean) and the extracted `/sweepboot-init.sh` is byte-identical
  to `tools/sweepboot-init.sh`. The built `/init` contains the
  `invfs.sweepboot` branch and sources `/sweepboot-init.sh`; its only
  `switch_root` occurrences are the comments explaining it is *not*
  used. A fresh-checkout simulation (worktree with `vm/` removed) also
  built cleanly.

### Pre-existing failures observed (not introduced, not fixed)

- Leg 6's `invf-verify --deep` reports
  `parity: N sealed stripes, 1 mismatched, 1 missing` after `--seal`.
  Reproduced identically with all WP69 changes stashed (`git stash`),
  and with a minimal `mkfs` + `cp` + `sweep` + `--seal` (no sweepboot
  code involved); `tools/test-seal.sh` fails the same way on unmodified
  `main` (`FAIL: first seal should write every stripe`). This is the
  core seal-parity bug recorded in `INCIDENTS.md:612-613` and
  `impl_docs/WP58b-mapper-owner-orphans.md:204`, outside WP69's scope.

### Remaining TODOs

- `test-seal.sh` / `invf-verify --deep` parity remain red on the
  pre-existing core seal-parity mismatch (`src/core/vol_seal.c`); needs
  its own WP. WP69 only stopped `test-sweepboot.sh` from re-asserting it.
- `docs/GENTOO-INSTALL.md:321` still names the initramfs member
  `sweepboot-init.sh`, which matches what `mkinitramfs.sh` stages; no
  doc change needed for the restore.
