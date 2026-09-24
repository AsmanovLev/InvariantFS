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
- **Mount time**: $O(1)$ base root load + short delta replay ($< 250\text{ ms}$).

---

## 2. Key Structures

### RT30 Superblock Descriptor
Lives at offset `0x9D0` of block 0 and occupies alternating double slots (blocks 161–162 or equivalent based on volume geometry).

```c
typedef struct {
    char     magic[4];       /* "RT30" */
    uint32_t version;        /* INVFS_RT30_VERSION */
    uint64_t seq;            /* Monotonic publication sequence */
    uint64_t root_pba;       /* Physical block address of B+ tree root */
    uint64_t root_gen;       /* Generation number of root page */
    uint64_t delta_pba;      /* Current delta log head segment PBA */
    uint32_t page_size;      /* Default 4096 */
    uint32_t flags;          /* State flags */
    uint32_t crc32c;         /* CRC32C of preceding fields */
} invfs_rt30;
```

### Key Encodings
All keys are lexicographically ordered big-endian byte sequences:
- **Inodes**: 8-byte big-endian inode ID (`0x0000000000000002` .. `0xFFFFFFFFFFFFFFFF`).
- **Dirents**: `[parent_inode_id (8B)][name_bytes (variable)]`.
- **Xattrs**: `[target_inode_id (8B)][0xFF][xattr_name (variable)]`.

---

## 3. The Fold Worker Lifecycle

Fold is triggered when the Delta Log exceeds thresholds:
- Bytes: 64 MiB
- Records: 262,144 records
- Age: 3,600 seconds (1 hour)

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

### 4.2 `fsync` / `fdatasync`

`vol_sync` = `vol_flush` + one `vmux_barrier`. On v3 that means:

- `vol_flush` persists the dirty block bitmap (`vol_v3_bitmap_flush`);
- the barrier flushes the backing store (device cache, or the page cache of
  a buffered image file), so everything written since the previous barrier
  is on stable storage;
- a failure latches the volume (`vol_io_error_latch`): later mutations are
  refused (`vol_write_enabled` consults `io_latched` even on v3) and
  `vol_close` will not write CLEAN.

Because the delta append already barriers each record (§4.3), the metadata
of an acknowledged mutation is durable *before* `fsync` is called. What
`fsync` adds on v3 is the **bitmap**: the allocation map is a derived cache
persisted only on flush/publish, so `fsync` is the point that pins it. Data
blocks are covered by the barrier that follows them.

### 4.3 The per-append barrier (WP80/Bug C audit)

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

### 4.4 `vol_close` and the CLEAN superblock

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

### 4.5 Failure injection

`INVFS_SYNC_FAIL_AT=N` makes the Nth `vol_sync` of the process latch the
volume and report EIO (`tools/test-flushfail.sh`). On v3 the delta tail is
already barriered, so the hook's v2 inode-area zeroing is a no-op; what it
exercises on v3 is the latch, the refused later mutations, and the
no-CLEAN close. The fsync-durability crash test is
`tools/test-v3-fsync-crash.sh`.
