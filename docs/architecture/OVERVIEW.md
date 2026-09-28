# InvariantFS Architecture Overview

InvariantFS is an experimental, content-addressed filesystem with a bit-exactness contract: **what you write comes back byte-for-byte identical**.

---

## 1. System Architecture

InvariantFS decouples immediate write ingestion from offline storage optimization:

```
[User / POSIX Apps]
         │
    FUSE / CLI
         │
┌────────▼────────────────────────────────────────┐
│                   METADATA                      │
│  - RT30 Superblock Descriptor (Double-slot)    │
│  - B+ Tree (Base Metadata, COW Pages)          │
│  - Delta Log (Append-only Recent Mutations)    │
│  - In-Memory Overlay & Range Scanner           │
└────────┬────────────────────────────────────────┘
         │
┌────────▼────────────────────────────────────────┐
│                   DATA PLANE                    │
│  - RAW Zone (Append-only LZ4 write landing)     │
│  - Shadow Zone (Offline consolidated storage)  │
│  - Sweep Worker (Type clustering, PPMd8/ZSTD)   │
│  - ARC Cache (Adaptive Replacement Cache)      │
└─────────────────────────────────────────────────┘
```

---

## 2. Core Storage Zones

| Zone | Role | Structure | Lifecycle |
|---|---|---|---|
| **Bitmap** | Physical block allocation state | One bit per 4 KiB block | Dirty range flushed on `vol_flush` |
| **Meta-v3 Area** | Inodes, dirents, xattrs, recipes | COW B+ Tree + append-only Delta Log | Merged by background fold |
| **RAW Zone** | Content-class tag for freshly written segments (advisory, not a fixed region) | Write-once LZ4/verbatim (ZSTD under fill pressure) | Drained into Shadow by sweep |
| **Shadow Zone** | Long-term archival storage | Type-clustered, deduplicated | Compacted during sweep |

> The v2 `L2P Journal` and append-only inode-record stream are gone in Meta-v3:
> recipes live in the B+ tree base / Delta Log, and the four zone fields in the
> superblock are **advisory policy** over one shared free-block pool (raw-class
> allocation may overflow into shadow-space blocks with the class tag
> unchanged).

---

## 3. The Write Path Lifecycle

1. **Ingest**: File data lands in the **RAW Zone**, compressed with fast LZ4 or stored verbatim.
2. **Metadata Mutation**: An inode row and dirent are recorded in the **Delta Log**. The in-memory overlay immediately makes it visible to readers without blocking.
3. **Fold (Metadata Consolidation)**: Background fold worker merges delta records into the COW B+ tree base, publishes the new root atomically via the RT30 double-slot, and resets the delta.
4. **Sweep (Data Consolidation)**: `invf-sweep` drains RAW blocks into the **Shadow Zone**, clusters data by type (e.g., text vs binary), deduplicates identical segments, and applies heavier codecs (PPMd8, ZSTD+BCJ) with verification guards.

---

## 4. Key Guarantees

- **Bit-Exact Verification**: Before a re-compressed or transcoded file is committed, it is decoded back in memory and compared **byte-for-byte (`memcmp`)** against the source by that lane's own guard — the guard is per lane, not one global loop. PPMD and ZSTD(+BCJ) batches verify at seal; codecpack transcodes verify before commit; containerpacks verify by read-back; the builtin container builders carry their own rebuild guards. The blanket compress→decompress→BLAKE3 loop **does not exist** (`impl_docs/AUDIT.md` H10), and not every lane is guarded at runtime (FLAC relies on pure-C recipe construction). When a guard refuses, the original data is retained and the file is class-stamped, so the next sweep does not retry until the codec generation rolls over.
- **Lock-Free Reads**: Readers query the Delta Log and the immutable B+ tree base without acquiring global locks.
- **Atomic Root Publication**: The RT30 descriptor carries a `root_slot[2]` double slot and a monotone `seq`, with CRC32C validation over the whole 48-byte descriptor. The higher `seq` with a valid checksum is authoritative; a torn write loses to the sibling slot. (The "double slot" is the array inside the descriptor at `0x9D0` of block 0 — not a pair of blocks. See `docs/architecture/META-V3.md` §2.)
