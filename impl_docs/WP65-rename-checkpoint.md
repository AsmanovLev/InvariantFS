# WP65 — FUSE rename under a live checkpoint + RENAME_NOREPLACE

**Branch:** `wp/65-rename-checkpoint`
**Worktree:** `/tmp/invfs-wp65`
**Severity:** HIGH (blocks package managers / OS install onto an InvFS root)
**Source:** `INCIDENTS.md` (`sed -i` EBUSY), `docs/ARCH-INSTALL.md` §9/§10,
WP63 (`rename` workarounds), read-only investigation 2026-09-19.
**Estimated effort:** 1–2 days (Bug A), +0.5 day (Bug B)

---

## Scope

`rename(2)` **is** implemented (`invf_rename`, `src/cli/fuse_fs.c:1895`;
`AUDIT.md:45` PB8 "wired to the sibling-safe `vol_rename`"). The stale docs
("FUSE has no rename") are wrong. Two real defects make rename unusable
for `pacman`/`portage`/`dpkg` and for an OS install:

- **Bug A:** `vol_rename` is the **only** mutating op that hard-refuses
  while a sweep checkpoint is live (`-4` → `-EBUSY`). Every adjacent op
  (create, write, unlink, hardlink, setattr, xattr) proceeds. A package
  install works until its final atomic `rename` into place, which then
  always fails — the worst failure shape (half-working).
- **Bug B:** any nonzero `renameat2` flags are rejected with `-EINVAL`
  (`RENAME_NOREPLACE`/`EXCHANGE`/`WHITEOUT`). `mv -n`, GNU `install`, and
  modern atomic-rename idioms use `RENAME_NOREPLACE`.

Secondary (documented, decide in-WP): directory-over-directory returns
`-EEXIST` (no atomic empty-dir replace), dir-over-file returns `-ENOENT`
instead of `-ENOTDIR`.

---

## Background — why the checkpoint gate exists

`vol_rename` (`src/core/vol_dirs.c:593`):

```c
/* WP21/WP22c: refuse to rename while a sweep checkpoint is live. The
 * rollback decapitates the inode area at the checkpoint's append
 * pointer, discarding the whole rename pair -- the copy dies AND the
 * tombstone dies, so the source name resurrects ... */
if (v->ck_present) return -4;               /* vol_dirs.c:608 */
```

A rename is at least two appends: copy-at-new-name + tombstone-at-old
(fast path: `vol_hardlink` + `vol_unlink_name`, `vol_dirs.c:664-668`).
Both land after `ck.inode_area_pos`. On rollback,
`vol_rollback.c:1013-1052` zeroes `[iapos, old_append)` and restores
`inode_area_pos = iapos`. Both rename records die → destination vanishes,
source resurrects (chaos-soak "ghost").

`vol_rename` is the *only* `ck_present` gate on a mutator. Every other
op appends through `vol_append_slot`/`meta_get_append_pos` with no gate
(`src/core/volume.c:4147`, `src/core/vol_meta_merge.c:641`).

---

## Bug A — allow rename under a live checkpoint

### Design options (pick one; (a) is smallest and matches existing UX)

**(a) Rename forces the pending checkpoint to resolve (realize) first.**
Before appending the pair, if `v->ck_present`, call the existing
`vol_ckp_realize(v, &freed)` path (same machinery as
`invf-sweep --realize`), then proceed. The rename becomes the point of no
return for the sweep: honest, durable, and the user sees the realize
message. Cost: the sweep's rollback window closes on the first rename —
acceptable, because the alternative is a hard failure.

```c
/* in vol_rename, replacing the -4 gate */
if (v->ck_present) {
    uint64_t freed = 0;
    if (vol_ckp_realize(v, &freed) < 0) return -4;  /* still EBUSY */
    /* registry gone, CKP0 cleared: append the pair safely */
}
```

**(b) Journal the rename pair** so rollback replays/undoes it. Larger;
touches the WP22d journal format. Defer unless (a) proves insufficient.

**(c) Make rollback preserve post-checkpoint rename pairs.** Requires the
checkpoint to snapshot more state. Largest.

### Why (a) is correct

The rollback fidelity contract is "undo the sweep, keep post-checkpoint
writes." A rename is a post-checkpoint write; refusing it violates the
contract for every other op. Realizing the checkpoint ends the rollback
window *deliberately and visibly*, exactly as `invf-sweep --realize` does.
After realize, the rename pair is ordinary pre-next-checkpoint state.

### Tests

- `tools/test-flushfail.sh:488` (`f2mvrefuse`) asserts `-4` under a live
  checkpoint — **must be updated**: rename now succeeds (and realizes).
  The leg at `:709-715` then only needs the realize assertion.
- New: arm checkpoint → `rename` via FUSE → assert success + bit-exact +
  `invf-fsck` clean + no checkpoint live.
- `tools/test-rollback.sh` must stay green (no rename involved).

---

## Bug B — RENAME_NOREPLACE

`.rename` is registered but not `.rename2` (`fuse_fs.c:2654`). libfuse
routes flag-carrying renames into `.rename` with `flags != 0`, currently
`-EINVAL` (`fuse_fs.c:1899`).

**Fix:** accept `RENAME_NOREPLACE` in `invf_rename`:

```c
if (flags & ~RENAME_NOREPLACE) return -EOPNOTSUPP;   /* EXCHANGE/WHITEOUT */
/* forward noreplace to vol_rename (new arg or a pre-check) */
```

Add a `noreplace` path in `vol_rename`: if the destination is live, return
`-EEXIST` (file) / `-ENOTEMPTY` (dir) **before** any mutation. Register
`.rename2 = invf_rename` (or keep `.rename`; libfuse calls `.rename` with
flags when `.rename2` is absent — verify against the pinned FUSE version).

`RENAME_EXCHANGE`/`RENAME_WHITEOUT` → `-EOPNOTSUPP` (was `-EINVAL`; the
latter wrongly implies "bad argument").

### Optional (decide in-WP)

- Directory-over-directory atomic replace (empty dest): `vol_rmdir(to)`
  then fast-path rename. Needed by dpkg upgrades.
- Dir-over-file errno `-ENOENT` → `-ENOTDIR` (`vol_dirs.c:624`).

---

## Validation

1. `make test` — must pass.
2. `tools/test-flushfail.sh` — update F2 to the new contract; all legs pass.
3. New FUSE-level rename test (arm checkpoint → rename → realize → fsck).
4. `renameat2(RENAME_NOREPLACE)` returns `-EEXIST`, not `-EINVAL`.
5. `tools/test-rollback.sh` unchanged.
6. Re-run a small `pacman -S`-style temp+rename loop through a FUSE mount.

---

## Out of scope (do NOT touch)

- systemd-on-FUSE (WP66).
- mkinitcpio / bootloader (WP67/WP68).
- `sed -i`/utimens semantics beyond rename.
- The stale "FUSE has no rename" doc text — fixed here only where it
  names rename; broader doc refresh is WP67/WP68.

---

## Coordination notes

- Subagent ID: `wp65-rename-checkpoint`; `INVFS_E2E_AGENT=wp65-rename-checkpoint`.
- E2E gates: `tools/test-flushfail.sh`, `tools/test-rollback.sh`,
  `tools/run-e2e.sh tools/test-writepath.sh`.
- Dependencies: none (WP58b landed).

---

## Key file:line index

- `invf_rename`: `src/cli/fuse_fs.c:1895` (flags `:1899`, errno map `:1950`)
- `vol_rename`: `src/core/vol_dirs.c:593` (gate `:608`, dir replace `:620`,
  dir-over-file `:624`, fast path `:664`)
- rollback decapitation: `src/core/vol_rollback.c:1013-1052`
- `vol_ckp_realize`: `src/core/vol_rollback.c:618`
- `fuse_operations` table: `src/cli/fuse_fs.c:2638`
- stale claim: `docs/ARCH-INSTALL.md:297,308-310`;
  `tools/configure-guest.sh:71,105,240`; `INCIDENTS.md:486-489`
