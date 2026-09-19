# WP-M6 — dirent tree + namespace operations

**Branch:** `wp/M6-dirent-namespace`
**Worktree:** `/tmp/invfs-wp-M6`
**Severity:** HIGH (format-breaking; the namespace users actually see)
**Source:** `impl_docs/design-meta-v3.md` §3 (ordered readdir merge), §4 (unlink delta, no tombstone), §13 (readdir table), §15.2, §16
**Estimated effort:** large

---

## Scope

Add the **dirent tree** (parent dir id + name → inode id) and rewrite the
namespace operations to use it: lookup, create, unlink, rename, readdir.

Files:
- `src/core/vol_dirs.c` — new dirent codec + `vol_mkdir`
  (`vol_dirs.c:25`), `vol_rmdir` (`:37`), `vol_list_dir` (`:113`),
  `vol_unlink` (`:392`), `vol_rename` (`:593`), `vol_hardlink` (`:718`),
  `vol_ensure_path` (`:87`).
- `src/core/vol_read.c` — `vol_read_named` (`:1015`), `vol_stat_full`
  (`:1089`) resolve names through the dirent tree.
- `src/core/vol_btree.c` — dirent key codec + ordered scan wiring (no
  algorithm changes).
- `src/core/volume.h` — namespace API unchanged in signature; `invfs_dirent`
  (`volume.h:111`) stays the listing type.

---

## Why

§13 needs O(log N) lookup and merge-based readdir for rootfs scale; the
v2 `nbuck`/`dbuck` mount-scan indexes (`volume_internal.h:331-337`) are
deleted with the v2 path. Rename/link must be expressible as tree ops, and
unlink must be a `delete` delta record, not a tombstone (§4).

---

## Design

### Dirent key

`key = (parent_inode_id:u64 BE) || name_len:u16 BE || name bytes`.

This makes a directory's entries a **contiguous ordered range**, so
`readdir` is a `btree_scan(lo, hi)` (WP-M3). The design doc says readdir is
a merge of ordered ranges (§3) but does not fix the key encoding; this WP
freezes it here. A `dir key → dir anchor` entry (name_len 0) records the
directory inode, giving `vol_is_dir` an O(log N) answer and making
mkdir/rmdir self-contained.

### Operations

- **lookup(name):** split path → dirent key → inode id → inode row
  (WP-M5). O(log N) + O(log N).
- **create:** upsert inode row (WP-M5), then insert dirent key → id.
  Ordering: inode row durable before the dirent (a dirent must never
  point at a missing inode).
- **unlink/rmdir:** delete dirent key; decrement the inode row's `nlink`;
  at 0, delete the inode row. `rmdir` (`vol_dirs.c:37`) returns `-2`
  (ENOTEMPTY) when the child range is non-empty, as today.
- **rename:** for same-directory rename, a key move on the tree; for
  cross-directory, delete old dirent + insert new. Ordering: **insert the
  new dirent first, then delete the old** — the add-before-remove rule
  §3 states for fold, applied here so a crash leaves two names, never
  zero. Overwriting an existing target must delete the target's dirent and
  drop its nlink, as `vol_rename` does today (`vol_dirs.c:428`).
- **readdir:** `btree_scan` over the directory's key range; `invfs_dirent`
  filled with name / is_dir / size / ctime. When the delta tier lands the
  scan becomes the merge of §3; this WP implements the base-side scan
  only.

### Delta note

Writes in this WP go directly to the base tree (same simplification as
WP-M5). When WP-M7 lands, inserts/deletes become delta appends and reads
consult delta-first. The API must not change at that point; only the
implementation of the write helpers.

### Crash / durability ordering

All mutations follow the WP-M3 publish rule (COW pages + barrier, then
root seq). The ordering constraints above (inode-before-dirent;
new-dirent-before-old-dirent deletion) are the load-bearing part; the
design doc states the add-before-remove rule only for fold (§3) — applying
it to rename is this WP's extension and is documented in a comment.

---

## Validation

1. `make test` — existing binaries plus WP-M2/M3 unit binaries.
2. `INVFS_V3=1 invf-mkfs t.img` → mkdir tree; create/unlink/rename across
   directories; hardlink; `invf-ls` shows sorted entries; `invf-fsck`
   clean; reopen and re-list identically.
3. Readdir on a large directory (10^4+ entries) returns the full sorted
   set with no RSS blow-up (replaces the v2 dir-hash).
4. `bash tools/run-e2e.sh tools/test-writepath.sh` — write/read and the
   rename/delete legs.
5. `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

---

## Out of scope (do NOT touch)

- Delta/overlay/fold/save-point/reclaim (WP-M7) — base-only implementation.
- Inode row layout (WP-M5).
- v2 `nbuck`/`dbuck` deletion and mapper removal (WP-M21 cleanup).
- Xattr semantics beyond what already lives in the WP-M5 row.
- Permission enforcement beyond existing `mode` handling.

---

## Coordination notes

- Subagent ID: `wp-M6-dirent-namespace`; run e2e with
  `INVFS_E2E_AGENT=wp-M6-dirent-namespace`.
- E2E gates: `bash tools/run-e2e.sh tools/test-writepath.sh`;
  `bash tools/run-e2e.sh tools/test-meta-v3.sh`.
- Dependencies: **WP-M5** (inode row + nlink), **WP-M3** (`bt_scan`),
  **WP-M2** (page IO).
- Blocks: WP-M7 (delta overlay operates on these namespace ops).
- The dirent key encoding is frozen; WP-M7's delta keys must reuse it so
  overlay merges share one ordering.
