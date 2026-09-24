# Architecture Decision Records (ADRs)

This directory documents key architectural decisions made in InvariantFS.

| ADR | Title | Status | Date |
|---|---|---|---|
| [ADR-001](ADR-001-meta-v3-btree-delta-model.md) | Meta-v3: B+ Tree and Delta Log Architecture | Accepted | 2026-09-20 |
| [ADR-002](ADR-002-fold-watermark-ordering.md) | Fold Watermark Invariant for Lock-Free Read Consistency | Proposed | 2026-09-22 |
| [ADR-003](ADR-003-savepoint-generation-semantics.md) | Generation-Based Savepoints and Rollback Semantics | Proposed | 2026-09-22 |
| [ADR-004](ADR-004-threadsafe-pread-pwrite-io.md) | Thread-Safe Position-Explicit I/O (`vmux_pread`/`pwrite`) | Accepted | 2026-09-22 |
| [ADR-005](ADR-005-asymmetric-btree-page-sizing.md) | Asymmetric B+ Tree Page Sizing (16K Internal / 4K Leaves) | Proposed | 2026-09-22 |
| [ADR-006](ADR-006-delta-lossless-compression.md) | Universal Delta-Lossless Decomposition | Accepted | 2026-09-23 |
| [ADR-007](ADR-007-plugin-architecture-ivpack.md) | Modular Pack Architecture, On-Volume `.invariantfs`, and `.ivpack` | Accepted | 2026-09-23 |
| [ADR-008](ADR-008-fuse-vs-kernel.md) | FUSE Userspace Daemon vs. In-Kernel Filesystem (VFS) | Accepted | 2026-09-24 |
| [ADR-009](ADR-009-v3-durability-contract.md) | Meta-v3 Durability Contract (fsync, close, per-append barrier) | Accepted | 2026-09-24 |
| [ADR-010](ADR-010-cross-vm-dedup-via-windowed-recipes.md) | Cross-VM Deduplication via Windowed Recipes (`INVFS_ALGO_WINDOW_SRC`) | Proposed | 2026-09-25 |
