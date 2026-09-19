# WP-M5 — inode tree (inode core + attrs + nlink + recipe ref)

**Branch:** `wp/M5-inode-tree`
**Worktree:** `/tmp/invfs-wp-M5`
**Severity:** HIGH (format-breaking; replaces the v2 inode record)
**Source:** `impl_docs/design-meta-v3.md` §12 (format), §13 (rootfs table), §15.2, §16
**Estimated effort:** large

---

## Scope

Define the **v3 inode row** in the base B+-tree and route the inode
lifecycle through it, replacing the append-record + tombstone path. The
inode row carries core attributes, `nlink`, and a **content-addressed
recipe reference**; the recipe itself is a separate immutable blob
(§12: "keep them out of the inode row").

Files:
- `src/core/vol_btree.c` — inode key codec + row encode/decode on top of
  the WP-M3 tree API (no algorithm changes).
- `src/core/volume.h` — v3 inode public surface (get/put/delete by inode
  id), `invfs_meta_pub` v3 extension.
- `src/core/vol_read.c` — `vol_read_inode` (`vol_read.c:316`) reads the
  recipe ref and fetches the recipe blob; `read_locate_record`
  (`vol_read.c:298`) becomes a base lookup.
- `src/core/vol_write.c` / `src/core/vol_records.c` — new-inode and
  setattr paths call the tree upsert instead of appending a record.

No delta (writes go directly into the tree for now; WP-M7 overlays the
delta). No dirent semantics (WP-M6).

---

## Why

The v2 record costs a full append plus a tombstone for every
chmod/rename/xattr and has no hardlinks (§1, `volume.h:200-204`). The
rootfs table (§13) requires `nlink` and O(log N) stat. The recipe can
reach ~384 MiB (`invarifs.h:982-999`); pulling it out of the row is what
makes the row fixed-ish and cacheable.

---

## Design

### Inode key

`key = inode_id` (8 bytes big-endian for byte-lexicographic order; WP-M3
keys are byte-ordered). The design doc does not fix the encoding; this WP
freezes it as **u64 big-endian**, documented in `vol_btree.c`. Dirent keys
are a separate keyspace with a namespace prefix (WP-M6).

### Inode row (additive to `invarifs.h`; exact offsets frozen here)

```
u32 row_version
u32 type                 /* INVFS_ITYP_* (reuse invarifs.h:911+) */
u16 mode
u32 uid, gid
i64 mtime, atime
u32 nlink
u64 rdev
u64 size
invfs_blkptr recipe      /* WP-M2 blkptr -> immutable AST blob */
u32 xattr_len            /* TLVs, same encoding as INO2 (invarifs.h:953) */
[xattr bytes]
```

The design doc says only "inode core + attrs + nlink + recipe ref"
(programme description) and "recipes immutable AST blobs,
content-addressed, verified on read" (§12). The concrete row layout is
this WP's decision; it is stored **versioned** so WP-M6/M7 can extend it
without a format break. Symlink target is stored in the recipe blob, as
in v2's INO2 ext.

### nlink

`nlink` lives in the row; `vol_hardlink` (`vol_dirs.c:718`) increments it
and adds a dirent (WP-M6). This fixes the `volume.h:200-204` caveat. The
design doc does not specify block refcounts for data — §8 is explicit that
reclaim is **reachability/epoch, not refcounts** — so unlinking one name
must not free shared blocks until nlink reaches zero; this WP implements
the nlink gate and leaves data-block reachability to the reclaim WP.

### Read / write

- `vol_read_inode`: parse row → if `recipe.pba != 0`, read + CRC-verify
  the recipe blob, then reassemble segments exactly as the v2 path does
  (`vol_read.c:316` onward is reused for segment decode). Bit-exactness
  checked per segment as today.
- Write: `btree_upsert` new row at the new pba; the old row's pages are
  COW-retired. No tombstone.

### Crash / durability ordering

Row publish follows WP-M3: write COW pages + barrier
(`volume_internal.h:132`), then the root slot with incremented `seq`. A
new inode's data blocks must be durable **before** the row that references
them (so a crash never yields a live inode pointing at unwritten data).

---

## Validation

1. `make test` — existing binaries plus WP-M2/M3 unit binaries.
2. `INVFS_V3=1 invf-mkfs t.img` → create files of several types (reg,
   symlink, fifo, chr/blk) and a hardlink pair; reopen; `invf-stat` and
   `invf-cat` return correct type/size/mode/uid/gid/mtime, and
   `invf-cat` is bit-exact vs the source.
3. chmod/touch/xattr update the row without growing unrelated data;
   `invf-fsck` clean.
4. `bash tools/run-e2e.sh tools/test-writepath.sh` — bit-exact write/read
   through the v3 path.
5. `bash tools/run-e2e.sh tools/test-meta-v3.sh` — leg 0 still passes.

---

## Out of scope (do NOT touch)

- Delta log/overlay/fold/save point/reclaim (later WPs).
- Dirent tree and namespace ops (WP-M6) — `vol_dirs.c` is touched only
  where the hardlink/nlink call site must be repointed.
- v2 record/mapper deletion (WP-M21 cleanup).
- Data-zone codecs / transcode policy.
- The recipe blob's internal AST encoding (unchanged from v2).

---

## Coordination notes

- Subagent ID: `wp-M5-inode-tree`; run e2e with
  `INVFS_E2E_AGENT=wp-M5-inode-tree`.
- E2E gates: `bash tools/run-e2e.sh tools/test-writepath.sh`;
  `bash tools/run-e2e.sh tools/test-meta-v3.sh`.
- Dependencies: **WP-M3** (tree API), **WP-M2** (blkptr/page IO).
- Blocks: WP-M6 (dirents need the inode row), WP-M7 (delta/fold).
- The row_version field is the extension point M6/M7 must use; do not
  silently repurpose fields.
