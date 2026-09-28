# Meta-v3 Specification: B+ Tree & Delta Log

Meta-v3 replaces the legacy v2 linear inode records, tombstone kills, and flat mapper tables with a B+ tree base and an append-only delta log.

---

## 1. Motivation

In legacy Meta-v2:
- Metadata was an append-only stream of inode records and `DELT` tombstones.
- Mount required an $O(N)$ sequential scan of every metadata block to rebuild name and inode indexes.
- Deletions accumulated as dead records until expensive compaction rewrites.

In Meta-v3:
- **Base tier**: Immutable Copy-on-Write (COW) B+ tree indexed by 64-bit Big-Endian keys.
- **Recent tier**: Append-only Delta Log capturing real-time mutations with minimal latency.
- **Overlay**: Readers look up the Delta Log first; if absent, they read the immutable B+ tree base.
- **Fold worker**: Periodic background task merges accumulated delta entries into the B+ tree, freeing obsolete pages.
- **Mount time**: $O(1)$ base root load + short delta replay (`vol_open` →
  `vol_delta_mount`, `src/core/volume.c:1565`; the replay loop and the
  torn-tail truncation are in `src/core/vol_delta.c:928-992`). The fold
  *trigger* thresholds in §3 are what bound how long that replay can get.

---

## 2. Key Structures

### RT30 Superblock Descriptor
`src/core/invarifs.h:663-671` is the authority. It is a **48-byte packed
descriptor at byte offset `0x9D0` of block 0** — not a block, and not a pair
of blocks. There is no geometry-dependent slot number: the "double slot" is
the `root_slot[2]` *array inside the descriptor*, holding the pba of base
root slot A and slot B (0 = empty), with `seq` as the atomicity anchor — on
recovery the highest CRC-valid `seq` wins.

```c
typedef struct {
    char     magic[4];       /* 0x9D0 "RT30" */
    uint32_t version;        /* 0x9D4 INVFS_RT30_VERSION (1) */
    uint32_t page_size;      /* 0x9D8 metadata base-page size (default 4096) */
    uint64_t root_slot[2];   /* 0x9DC base root slot A / B pba (0 = empty) */
    uint64_t delta_pba;      /* 0x9EC active delta segment pba (0 = none) */
    uint64_t seq;            /* 0x9F4 root generation (monotone; higher = newer) */
    uint32_t crc32c;         /* 0x9FC over the descriptor, this field read 0 */
} invfs_rt30;               /* 0x9D0 + 48 -> ends 0xA00 */
```

There is no `root_pba` field, no `root_gen` field, and no `flags` field in
this struct. Publication is `mbuf_root_publish(v, root_pba, root_gen)`
(`src/core/vol_metabuf.c:356`, `:396`), which writes the *parameter* into the
chosen `root_slot[]` entry — the generation is carried by the page header it
validates (`h->gen`) and by `seq`, not by a descriptor field.

### Key Encodings
All keys are lexicographically ordered big-endian byte sequences. The
namespaces are kept disjoint by key *length* and by a prefix byte
(`src/core/invarifs.h:1262-1282`):

- **Inodes**: 8-byte big-endian inode ID, no prefix
  (`v3_ino_key`, `src/core/vol_btree.c:2441-2448`). Being exactly 8 bytes
  is what keeps this namespace disjoint from the prefixed and lengthened
  ones below.
- **Dirents**: `parent_inode_id:u64 BE || name_len:u16 BE || name bytes`,
  no prefix — always ≥ 10 bytes, which is what keeps it disjoint from the
  8-byte inode keys (`v3_dirent_key`, `src/core/vol_btree.c:3988-3999`; the
  layout is frozen at `:3969`). The value is the child inode id, 8 bytes BE.
  A directory's own anchor entry has `name_len == 0`.
- **Xattrs**: `0x03 || inode_id:u64 BE || name_len:u16 BE || name`
  (`v3_xattr_key`, `src/core/vol_btree.c:2780-2792`; prefix
  `INVFS_V3_XATTR_KEY_PREFIX` = `0x03`, `src/core/invarifs.h:1272`). The
  value is the raw xattr value bytes. Values too large for one base page use
  **continuation keys**: the same key with `0x00 || chunk_index:u16 BE`
  appended (`v3_xattr_chunk_key`, `src/core/vol_btree.c:2795-2803`); an xattr
  name cannot contain NUL, so the two key shapes are unambiguous.
- **Recipe blobs**: `0x04 || BLAKE3-256(serialized recipe)[32]`
  (`INVFS_V3_RECIPE_KEY_PREFIX` = `0x04`, `src/core/invarifs.h:1282`). This
  is why identical recipes dedup to one key, and why a recipe lookup
  recomputes the hash and hard-fails on mismatch rather than decoding.

---

## 3. The Fold Worker Lifecycle

Fold is triggered when the Delta Log exceeds thresholds
(`src/core/vol_fold.c:84-86`):
- Bytes: 64 MiB (`FOLD_TRIGGER_BYTES`)
- Records: 262,144 records (`FOLD_TRIGGER_RECORDS`, a RAM-index bound)
- Age: 3,600 seconds (1 hour) (`FOLD_TRIGGER_AGE_S`)

These are diagnostics, not format: changing them changes *when* fold runs,
never what is on disk (`src/core/vol_fold.c:50-52`).

### Step-by-Step Fold Order:
1. **Iterate & Snapshot**: Collect winning delta records in memory into key-sorted order.
2. **COW Mutation**: Apply upserts and deletes into a cloned B+ tree root, writing new pages with `gen = old_root.gen + 1`.
3. **Barrier & Root Publish**: Write dirty bitmap range, execute barrier (`vmux_barrier`), and publish the new root to RT30.
4. **Delta Reset**: Reset delta segment pointer in RT30 and clear in-memory index under `g_delta_lock`.
5. **Reclaim**: Free superseded delta blocks and unreferenced old base tree pages via reachability diff.

---

## 4. Durability Contract (WP80)

Meta-v3 has three persistence tiers with distinct guarantees. This section
is the normative contract: what `fsync(2)`, `vol_close`, and crash recovery
actually promise. The engine-side entry points are `vol_sync` (FUSE
`.fsync`/`.fdatasync`), `vol_flush`, and `vol_close` in `src/core/volume.c`.

### 4.1 Commit points

| Mutation | Commit point | Durable when it returns? |
|---|---|---|
| namespace / inode / xattr change | `vol_delta_append` (record `io_pwrite` → `vmux_barrier("delta append")` → index publish) | **yes** |
| new delta segment | `delta_new_segment` (zeroed segment + header + barrier) | yes, before `RT30` names it |
| file content | data segments are written before the delta record that names them; that record's barrier covers them | yes |
| fold (base-tree COW) | `v3_publish`: bitmap flush + barrier, then `mbuf_root_publish` (`RT30` double slot + barrier) | yes, at publish |
| dirty block bitmap | `vol_flush` / `v3_publish` (`vol_v3_bitmap_flush` + barrier) | on flush / fsync / close |

The load-bearing rule is **structure-before-reference**:

- a data segment is durable before the recipe/inode row that references it;
- a delta segment (header + zeroed payload) is durable before `RT30` names
  it as the active segment;
- COW base pages and their allocation bits are durable before `RT30`'s root
  slot names the new root.

"Durable" in that table means *a barrier has been issued and returned 0* —
see §4.2 for exactly what that does and does not buy on each platform. It
does not mean each write is write-through.

### 4.2 What the barrier actually is

`blkio_flush` is `fsync(fd)` on POSIX and `FlushFileBuffers` on Windows
(`src/core/blkio.c:619-628`). The *open* differs by platform, and the open is
what most people assume carries the guarantee:

| platform | backing store | open flags | what a barrier buys |
|---|---|---|---|
| Windows | raw device | `FILE_FLAG_NO_BUFFERING \| FILE_FLAG_WRITE_THROUGH` | every write already reached the device as issued; ordering is the handle's own |
| Windows | image file | buffered | process-death survival |
| POSIX/Linux | raw device **and** image file | plain `O_RDWR` — no `O_DIRECT`, `O_SYNC` or `O_DSYNC` (`blkio.c:296-298`) | the bytes have been handed to the host storage stack: process-death survival, and power-loss survival only to the extent the host filesystem and device honour `fsync(2)` |

So on Linux, write ordering is established by **InvariantFS**, not by the
open: the structures that must land in a given order are written in that
order and then made durable together by one barrier, and each is
magic/CRC-framed so a partially completed flush is detected at the next
mount rather than adopted. `fsync()` does not order one `pwrite` against
another at the device, and cannot promise anything about a device that lies
about its own volatile cache. `src/core/vol_crash.c` is the in-code
authority for this; `blkio_test.c` asserts the open flags so the text cannot
drift from the code.

### 4.3 `fsync` / `fdatasync`

`vol_sync` = `vol_flush` + one `vmux_barrier`. On v3 that means:

- `vol_flush` persists the dirty block bitmap (`vol_v3_bitmap_flush`);
- the barrier flushes the backing store — everything written since the
  previous barrier is handed to the host storage stack, which is stable
  storage only to the extent the host filesystem and device honour
  `fsync(2)` (§4.2);
- a failure latches the volume (`vol_io_error_latch`): later mutations are
  refused (`vol_write_enabled` consults `io_latched` even on v3) and
  `vol_close` will not write CLEAN.

Because the delta append already barriers each record (§4.4), the metadata
of an acknowledged mutation is durable *before* `fsync` is called. What
`fsync` adds on v3 is the **bitmap**: the allocation map is a derived cache
persisted only on flush/publish, so `fsync` is the point that pins it. Data
blocks are covered by the barrier that follows them.

**What `fsync` does not cover, on v3:** the bitmap is *not* made durable
before a delta record. The record is barriered at append
(`src/core/vol_delta.c:571`); the bitmap is only written at `vol_flush`
(`src/core/volume.c:2641`). A crash inside that window leaves a durable
record naming blocks whose allocation bits are still clear on disk. This is
safe by **derivation, not by ordering**: the bitmap is a derived cache
(`vol_fsck.c:25`) and mount replay re-reserves every block a replayed segment
spans (`dl_reserve_segment`, `src/core/vol_delta.c:409`) before any
allocator call can hand it out again. The window can strand free space, it
cannot alias a live block. `tools/delta_test.c`
(`bitmap-before-delta gap is safe by replay`) is the regression test for
that derivation; there is deliberately no test asserting an ordering that
does not exist.

### 4.4 The per-append barrier (WP80/Bug C audit)

`vol_delta_append` calls `vmux_barrier(v, "delta append")` after **every**
record, not per batch. Audit result:

- **It is a durability ordering, not a read-consistency mechanism.**
  Lock-free readers (ADR-002) get their consistency from the in-RAM index
  being published *after* the record bytes are written and the sequence
  number assigned; a device barrier is irrelevant to a same-process reader.
  The in-code rationale ("make the record durable before it becomes the
  indexed winner") is exactly that: if the append is acknowledged, the
  record is on disk.
- **It is intentional (design §9).** It makes each namespace mutation an
  implicit, immediate durability point, which is why `fsync` can *appear*
  redundant. It is a deliberate durability-for-throughput trade, not an
  accident.
- **What relaxing it would require** (not done here — no measurement yet):
  a `delta_durable` append anchor, a recovery story for the torn delta tail
  (`vol_delta_mount` already truncates a torn tail, so replay stays safe),
  and redefining the acknowledged-write contract as "durable at fsync, not
  at return". Read and fold consistency would still hold, because the index
  publish ordering is unchanged; the change is purely *when* bytes reach
  the device. Until that is measured and the weaker contract is adopted
  deliberately, the barrier stays.

### 4.5 `vol_close` and the CLEAN superblock

A CLEAN superblock asserts that everything it describes is durable, so
`vol_close` barriers **before** writing CLEAN:

1. `vol_flush` persists the pending state;
2. `vmux_barrier(v, "close")` — the default for every backing store;
   `INVFS_CLOSE_NOBARRIER=1` is the explicit, documented opt-out;
3. `vol_write_sb` marks the volume CLEAN.

A failed barrier keeps the volume DIRTY and latches it, so the next mount
recovers instead of adopting a possibly-torn tail. The old opt-in
`INVFS_FSYNC` barrier is gone: it ran *after* the CLEAN write, which is the
wrong side of the ordering it was meant to provide.

### 4.6 Failure injection

`INVFS_SYNC_FAIL_AT=N` makes the Nth `vol_sync` of the process latch the
volume and report EIO (`tools/test-flushfail.sh`). On v3 the delta tail is
already barriered, so the hook's v2 inode-area zeroing is a no-op; what it
exercises on v3 is the latch, the refused later mutations, and the
no-CLEAN close. The fsync-durability crash test is
`tools/test-v3-fsync-crash.sh`.
