# WP-M7 — xattr tree (named xattrs as base keys, not INO2 TLVs)

**Branch:** `wp/M7-xattr-tree`
**Worktree:** `/tmp/invfs-wp-M7`
**Severity:** HIGH (format-breaking; replaces the inode-resident xattr area)
**Source:** `impl_docs/design-meta-v3.md` §12, §13, §15.2, §16
**Estimated effort:** medium

---

## Scope

Move **named xattrs** out of the inode row's INO2 TLV area into their own
base B+-tree key range, so `setxattr`/`removexattr` rewrites one small key
instead of the whole inode row. This is the third namespace keyspace (after
inode and dirent).

Files:
- `src/core/vol_btree.c` — xattr key codec (no algorithm changes).
- `src/core/vol_records.c` — `xattr_tlv_size` (`vol_records.c:947`),
  `meta_parse_ext` (`:960`), `vol_get_xattr` (`:1390`), `vol_set_xattr`
  (`:1432`), `vol_remove_xattr` (`:1490`), `vol_list_xattr` (`:1537`).
- `src/core/invarifs.h` — xattr key prefix; drop the row's dead
  `xattr_len`/`xattr bytes` fields via WP-M5's `row_version`.
- `src/core/volume.h` — `vol_*_xattr` signatures unchanged; `fuse_fs.c`
  call sites and the ACL transport (`fuse_fs.c:532`) unchanged.

No delta: writes go straight to the base tree, as in WP-M5/M6.

## Why

§13 lists per-file metadata updates as a daily operation that must stop
rewriting whole records. The INO2 TLV area (`invarifs.h:953`) is a v2 growth
path §10 deletes, and ACL xattrs (`fuse_fs.c:532`) are the common case. A
named key per xattr keeps the inode row fixed-size and makes `listxattr` an
ordered range scan.

## Design

**Key (frozen here):** `0x03 || inode_id:u64 BE || name_len:u16 BE || name`.
The design says "inode + dirent + xattr" are the namespace (§15.2) but does
**not** fix the encoding; WP-M5/M6 froze the other two, so this WP adds the
xattr prefix and reuses WP-M3 byte-lexicographic ordering, making
`listxattr(id)` a `btree_scan` over a contiguous range.

**Value:** the raw xattr value bytes (name is in the key). The existing TLV
framing is reused for the ACL blob layout; `system.posix_acl_*` blobs are
carried verbatim (`fuse_fs.c:564`).

**Operations:** get = lookup (missing → ENODATA); set = `btree_upsert`;
remove = `btree_delete`; list = range scan. `unlink`/`rmdir` at nlink 0 must
delete the whole xattr range for the dying inode or reclaim leaks keys —
this WP owns that cascade.

**Durability:** follows WP-M3 (COW pages + barrier, then root seq). Xattrs
reference no data blocks, so no extra ordering applies.

## Validation

1. `make test` — existing binaries plus WP-M2/M3 unit binaries.
2. `INVFS_V3=1 invf-mkfs t.img` → `setfattr`/`getfattr`/`getfattr -d`
   round-trip; remove; multi-xattr list is sorted; unlink removes keys and
   `invf-fsck` stays clean.
3. `bash tools/run-e2e.sh tools/test-acl.sh` — POSIX ACLs through v3.
4. `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Delta append/overlay/fold (later WPs) — base-only here.
- ACL policy/parsing (transport unchanged).
- Large/streamed xattr values; the design does not specify spilling.
- Inode row layout (WP-M5) beyond dropping dead xattr fields.
- `vol_*_xattr` public signatures.

## Coordination notes

- Subagent ID: `wp-M7-xattr-tree`; `INVFS_E2E_AGENT=wp-M7-xattr-tree`.
- E2E gates: `test-acl.sh`, `test-writepath.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M5** (row + row_version), **WP-M6** (key prefixes),
  **WP-M3** (scan).
- Blocks: WP-M12 (xattr mutations must append to the delta).
