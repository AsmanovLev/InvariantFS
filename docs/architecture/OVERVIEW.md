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
| **Bitmap** | Physical block allocation state | One bit per 4 KiB block | Rewritten on block alloc/free |
| **L2P Journal** | Logical-to-Physical block mapping | Append-only circular journal | Compacted by sweep |
| **Meta-v3 Area** | Inodes, dirents, xattrs, recipes | B+ Tree + Append-only Delta Log | Merged by background fold |
| **RAW Zone** | High-throughput write landing | Linear append-only (LZ4) | Drained into Shadow by sweep |
| **Shadow Zone** | Long-term archival storage | Type-clustered, deduplicated | Compacted during sweep |

---

## 3. The Write Path Lifecycle

1. **Ingest**: File data lands in the **RAW Zone**, compressed with fast LZ4 or stored verbatim.
2. **Metadata Mutation**: An inode row and dirent are recorded in the **Delta Log**. The in-memory overlay immediately makes it visible to readers without blocking.
3. **Fold (Metadata Consolidation)**: Background fold worker merges delta records into the COW B+ tree base, publishes the new root atomically via the RT30 double-slot, and resets the delta.
4. **Sweep (Data Consolidation)**: `invf-sweep` drains RAW blocks into the **Shadow Zone**, clusters data by type (e.g., text vs binary), deduplicates identical segments, and applies heavier codecs (PPMd8, ZSTD+BCJ) with verification guards.

---

## 4. Key Guarantees

- **Bit-Exact Verification**: Before any re-compressed or transcoded block is committed, it is decompressed in-memory and compared against the original hash. If bit-exactness fails, the original data is retained verbatim.
- **Lock-Free Reads**: Readers query the Delta Log and the immutable B+ tree base without acquiring global locks.
- **Atomic Root Publication**: The RT30 descriptor uses alternating double slots with CRC32C validation. The higher sequence number with a valid checksum is authoritative; torn writes fall back automatically.
