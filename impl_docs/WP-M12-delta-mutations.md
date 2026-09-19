# WP-M12 — metadata mutations appended to the delta

**Branch:** `wp/M12-delta-mutations`
**Worktree:** `/tmp/invfs-wp-M12`
**Severity:** HIGH (the write-amplification fix; §1's core motivation)
**Source:** `impl_docs/design-meta-v3.md` §1, §4, §13, §15.3
**Estimated effort:** large

---

## Scope

Route every **metadata mutation** through the delta append instead of a base
upsert: chmod/setattr, setxattr/removexattr, create, unlink, rename,
mkdir/rmdir, inode row updates. Delete (unlink/rmdir) appends a delete
record; no tombstone, no base rewrite.

Files:
- `src/core/vol_records.c` — setattr/xattr/special-creator paths append.
- `src/core/vol_dirs.c` — `vol_mkdir` (`vol_dirs.c:25`), `vol_rmdir`
  (`:37`), `vol_unlink` (`:392`), `vol_rename` (`:593`),
  `vol_hardlink` (`:718`), `vol_ensure_path` (`:87`).
- `src/core/vol_write.c` — new-inode/commit metadata append.
- `src/core/vol_delta.c` — append call sites (WP-M10 API).
- `src/core/volume.h` — no signature changes.

Reads already overlay (WP-M11); fold/trigger/reclaim out of scope.

## Why

§1 measures the v2 cost: every chmod/setattr/xattr/rename appends a full
record + tombstone; on 15 GiB, 75% of records were dead. §4 defines the fix:
`change(key,value): append{key,value,crc}; delta_index[key]=offset`. §13
requires create/delete/rename to be a single delta append. WP-M9 established
the base-only call sites; this WP moves them behind the delta with no API
change.

## Design

**Mutation mapping:**
```
chmod/touch/setattr -> append inode key (full new row)
setxattr/remove     -> append/delete xattr key              (WP-M7)
create/mkdir        -> append inode row, then append dirent
unlink/rmdir        -> append delete dirent; at nlink 0 delete inode row
                       + its xattr keys
rename              -> append new dirent FIRST, then delete old dirent
```
No base page is touched; base mutation happens only at fold (WP-M14).

**Ordering (load-bearing):** the WP-M9 durability chain still holds, but
visible publish is now the delta append + index update: data → recipe (M8) →
inode record → dirent record, in that order, so a crash transiently exposes
at most an unreferenced earlier record. Rename keeps add-before-remove as
WP-M6 documented; the design states it for fold (§3) and WP-M6 extended it
to rename — this WP inherits that rule.

**No-tombstone guarantee:** unlink of the last link appends a delete for the
inode key; dead base records are removed only by fold, which removes the
75%-dead accumulation (§1) at the next fold/reclaim.

## Validation

1. `make test` — WP-M2/M3/M10 binaries plus a case asserting a mutation does
   **not** allocate/modify a base page.
2. `INVFS_V3=1 invf-mkfs t.img` → chmod/touch/xattr/rename/unlink loop;
   delta grows, base root `seq` unchanged, reads correct, fsck clean.
3. `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-rename.sh`;
   `bash tools/run-e2e.sh tools/test-acl.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Fold / delta reset / base publish (WP-M14).
- Reclaim of delta segments or base pages (WP-M15).
- Save point/rollback (WP-M16).
- Delta sharding and `g_io_lock` removal (WP-M20; one append lock for now).
- Key encodings (WP-M5/M6/M7); v2 paths (WP-M21).

## Coordination notes

- Subagent ID: `wp-M12-delta-mutations`;
  `INVFS_E2E_AGENT=wp-M12-delta-mutations`.
- E2E gates: `test-writepath.sh`, `test-rename.sh`, `test-acl.sh`,
  `test-meta-v3.sh`.
- Dependencies: **WP-M10**, **WP-M11**, **WP-M9**, **WP-M7/M8**.
- Blocks: WP-M13 (replay must cover all record kinds), WP-M14, WP-M18.
- Keep the base-only path behind the same helper names so fold reuses them;
  do not fork a second write implementation.
