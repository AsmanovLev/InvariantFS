# WP-M9 — write path on the base tree (create / write / truncate / commit)

**Branch:** `wp/M9-write-path-base`
**Worktree:** `/tmp/invfs-wp-M9`
**Severity:** HIGH (format-breaking; first full v3 data-path closure)
**Source:** `impl_docs/design-meta-v3.md` §4, §5, §12, §13, §15.2, §16
**Estimated effort:** large

---

## Scope

Close the **v3 write path end-to-end** against the base tree: a new file's
data lands in RAW (DATA plane unchanged), its recipe is published to the
WP-M8 store, and its inode row + dirent are inserted via WP-M5/M6. Ranged
writes and truncate route through the tree, not v2 records.

Files:
- `src/core/vol_write.c` — `vol_write_begin` (`vol_write.c:44`),
  `vol_write_range` (`:493`), `vol_write_truncate` (`:561`),
  `vol_write_commit` (`:660`).
- `src/core/vol_records.c` — new-inode/setattr call `btree_upsert`, not
  record append.
- `src/core/vol_ast.c` / `vol_read.c` — recipe publish/fetch (WP-M8).
- `src/core/vol_dirs.c` — create/unlink/rename call the tree (WP-M6).
- `src/core/volume.h` / `volume_internal.h` — wsession state points at
  inode ids / tree refs, not v2 pbas.

Still **base-only**: no delta until WP-M10–M12. The public API must not
change when the delta lands; only the write-helper implementation.

## Why

WP-M5..M8 each land one structure; none proves that an actual `write(2)`
produces a durable, readable, bit-exact file. §13 requires create/delete/
rename to be tree ops and §4 requires writes to append to the fast tier.
This is the integration point that proves the v3 DATA + metadata handoff
before the delta complicates it.

## Design

**Flow:**
```
create:   alloc data in RAW (existing) -> segments -> recipe blob (M8)
          -> inode row (M5) -> dirent (M6)
write:    RAW append (existing append-only DATA plane, AGENTS §2.4)
truncate: rewrite recipe + row; attrs-only touch only the row
commit:   flush data, then recipe, then inode/dirent, then root seq
```
Segment/RAW codecs and the ranged-write session are reused; only metadata
changes.

**Durability ordering (load-bearing):** data durable → recipe durable (M8) →
inode row → dirent → root `seq` last (WP-M3). A crash leaves at most an
orphaned blob/row, reclaimable by WP-M15; it never publishes a dirent →
inode → recipe → data chain with a missing link.

**Base-only constraint:** every mutation is base `btree_upsert`/`delete`.
WP-M12 moves these exact call sites behind the delta append with no API
change, so WP-M12 must find them here.

## Validation

1. `make test` — existing binaries plus WP-M2/M3 unit binaries.
2. `INVFS_V3=1 invf-mkfs t.img` → create/write/ranged-write/truncate/read;
   `invf-cat` bit-exact (`cmp`); `invf-stat` size/mtime correct;
   unmount/reopen identical; `invf-fsck` clean.
3. `bash tools/run-e2e.sh tools/test-writepath.sh` — write/read and
   ranged-write legs.
4. `bash tools/run-e2e.sh tools/test-rawimg.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Delta log/overlay/fold/save point/reclaim (later WPs).
- DATA-zone codecs, transcode selection, containerpacks.
- v2 record/mapper deletion (WP-M21).
- Row layouts (WP-M5/M6/M7); recipe store internals (WP-M8).

## Coordination notes

- Subagent ID: `wp-M9-write-path-base`; `INVFS_E2E_AGENT=wp-M9-write-path-base`.
- E2E gates: `test-writepath.sh`, `test-rawimg.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M5**, **WP-M6**, **WP-M7**, **WP-M8**, **WP-M3**.
- Blocks: WP-M12 (mutation call sites), WP-M18 (sweep sees live files).
