# WP-M17 — hardlinks via nlink

**Branch:** `wp/M17-hardlinks`
**Worktree:** `/tmp/invfs-wp-M17`
**Severity:** HIGH (a wrong nlink frees live data or leaks an inode)
**Source:** `impl_docs/design-meta-v3.md` §13, §4, §6, §8, §12, WP6, §15.5, §16
**Estimated effort:** medium

---

## Scope

Make hardlinks **fully correct** on the v3 row: many names may map to one
inode id; `nlink` (introduced by WP-M5) is maintained exactly across
link/unlink/rename/overwrite, and the inode row is deleted only at
`nlink == 0`.

Files:
- `src/core/vol_dirs.c` — `vol_hardlink` (`vol_dirs.c:718`), `vol_unlink` /
  `vol_unlink_name` (`:392`/`:324`), `vol_rename` (`:593`, overwrite
  `:428`), `vol_ensure_path` (`:87`).
- `src/core/vol_records.c` — inode row create/delete at nlink 0.
- `src/core/vol_btree.c` — inode `nlink` field codec (WP-M5 row).
- `src/cli/fuse_fs.c` — `link`/`unlink` handlers pass through unchanged.
- `src/core/volume.h` — `nlink` in `invfs_dirent`/stat stays the surface
  (`volume.h:170`).

No block refcounts; data reachability is WP-M15's diff.

## Why

§13 lists hardlinks as a v3 capability ("hardlinks: unsupported → `nlink`").
Today `vol_hardlink` (`vol_dirs.c:718`) has no block refcounts and its
comment (`volume.h:200-204`) warns that unlinking **either** name retires
shared blocks. WP-M5 added `nlink`; this WP makes namespace operations
maintain it so unlink of one name preserves the other.

## Design

**nlink transitions (frozen):**
```
link(new)          -> nlink++ ; insert dirent(new -> id)     # M6 ordering
unlink(name)       -> delete dirent(name); if --nlink == 0:
                        delete inode row + xattr keys (M7); submit
                        data/recipe reachable set to M15
rename(old,new)    -> insert dirent(new); delete dirent(old) # add-before-remove
rename overwrite   -> drop target's dirent and nlink as unlink above
```
No block refcounts and no per-data-block counting (§8: reclaim is
reachability/epoch). The `nlink == 0` gate is the **only** inode-free
condition; shared data stays alive through the still-referenced row.

**Delta interaction:** on v3 every transition is a delta record (WP-M12):
the increment/decrement is a new inode-row value plus dirent add/delete
records. No tombstone (§4).

**Ordering:** link inserts the dirent after the `nlink++` row is durable.
unlink at 0 deletes the dirent **before** the inode row, so a crash never
leaves a live dirent pointing at a deleted inode — WP-M6's rule restated for
shared inodes.

## Validation

1. `make test` — cases: link → nlink 2; unlink one → nlink 1, inode + data
   still readable via the other name; unlink both → row gone, data
   reclaimable; rename overwrite drops the target count; nlink never
   underflows; dirs refuse link (EPERM).
2. `INVFS_V3=1 invf-mkfs t.img` → `ln`/`stat -c %h`/`rm`; `invf-cat` via the
   surviving name is bit-exact; `invf-fsck` clean; reopen preserves nlink.
3. `bash tools/run-e2e.sh tools/test-writepath.sh` (link legs);
   `bash tools/run-e2e.sh tools/test-rename.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Block refcounts / reflink (deferred, §Non-goals; rejected design §11).
- Reclaim of freed data (WP-M15).
- Readdir/rename mechanics beyond nlink bookkeeping (WP-M6).
- v2 hardlink limitation cleanup (WP-M21).
- Directory link counts beyond current behaviour.

## Coordination notes

- Subagent ID: `wp-M17-hardlinks`; `INVFS_E2E_AGENT=wp-M17-hardlinks`.
- E2E gates: `test-writepath.sh`, `test-rename.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M5** (row nlink), **WP-M6** (dirent ops/ordering),
  **WP-M12** (delta records), **WP-M15** (data reclaim).
- Blocks: nothing directly; WP-M18's live set must see shared inodes once.
- The `volume.h:200-204` caveat comment must be rewritten (behaviour fix);
  that is a docs-in-code change, not a new behaviour.
