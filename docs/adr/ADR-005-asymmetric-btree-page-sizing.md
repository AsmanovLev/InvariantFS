# ADR-005: Asymmetric B+ Tree Page Sizing (16K Internal / 4K Leaves)

## Status
Proposed

## Context
In Meta-v3, selecting a uniform page size presents a fundamental trade-off:
- **4 KiB pages**: Low fanout (~50–60 keys per internal node), requiring deeper tree levels (4–5 levels for large directory structures), increasing traversal latency. However, leaf COW rewrite cost on small metadata updates (e.g. `touch`, `chmod`, `rename`) is minimal (4 KiB per modified leaf).
- **16 KiB pages**: High fanout (~200–250 keys per internal node), yielding a shallow tree (2–3 levels for hundreds of millions of keys). However, COW write amplification increases 4x: modifying a single 200-byte inode row rewrites an entire 16 KiB leaf page.

## Decision
Adopt **asymmetric B+ tree page sizing**:
1. Internal nodes use 16 KiB pages:
   - Maximizes branching factor ($B \approx 250$).
   - Keeps total tree height $\le 3$ even for hundred-million-node volumes.
   - Internal nodes are modified infrequently compared to leaf entries.
2. Leaf nodes use 4 KiB pages:
   - Minimizes COW rewrite amplification on frequent file/directory modifications.
   - Matches host OS page cache granularity and NVMe native physical block sizing.
3. Fold Optimization:
   - The fold worker sorts delta keys prior to application and updates leaf pages in batched multi-key passes.
   - Leaves are packed to a 60–70% target fill factor to prevent immediate cascading leaf splits on subsequent writes.

## Consequences
### Positive
- Combines the shallow depth and low lookup amplification of large pages with the low write amplification of small leaf pages.
- Reduces total I/O during metadata churn.

### Negative / Trade-offs
- Node decoders and page buffers must support variable page lengths depending on node level flags.
