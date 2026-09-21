# ADR-001: Meta-v3 B+ Tree and Delta Log Architecture

## Status
Accepted

## Context
In InvariantFS Meta-v2, metadata was maintained as an append-only stream of inode records and tombstone deletions (`DELT`). This model had critical structural issues:
1. **$O(N)$ Mount Scans**: Mounting required reading the entire metadata area to reconstruct the hash table and directory tree.
2. **Namespace Compaction**: Unlinking files did not immediately reclaim space; tombstones accumulated until full compaction passes.
3. **Flat Extent Mappers**: Scaling beyond initial allocations required growing a flat mapper table that became fragmented and prone to consistency bugs.

## Decision
1. Retire the v2 metadata structures entirely (no dual reader, no migration shim).
2. Adopt a two-tier metadata design:
   - **Base Tier**: Immutable Copy-on-Write (COW) B+ tree indexed by 64-bit big-endian keys.
   - **Recent Tier**: Append-only Delta Log storing recent mutations.
3. Use an in-memory overlay that queries the Delta Log first before falling back to the B+ tree base.
4. Implement a background **Fold** worker that applies accumulated delta entries into a new COW B+ tree root and publishes it atomically via an alternating double-slot superblock descriptor (`RT30`).
5. Free unreferenced pages and delta segments through a reachability diff between roots without block refcounts.

## Consequences
### Positive
- Mount time is reduced from minutes on large volumes to $< 250\text{ ms}$.
- Lock-free read path: readers can access the immutable base without writer contention.
- Deletions drop superseded keys during fold without generating persistent tombstones.

### Negative / Risks
- Background fold worker adds write amplification during high-churn workloads.
- Requires careful ordering to ensure crash consistency and race-free reads during fold publication.
