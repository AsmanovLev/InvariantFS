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
