# ADR-002: Fold Watermark Invariant for Lock-Free Read Consistency

## Status
Proposed

## Context
In Meta-v3, the "add-before-remove" rule specifies that readers consult the delta log before the base B+ tree, ensuring that delta mutations shadow base values. However, a subtle race exists:
- A reader snapshots the current base root pointer $R_0$ before a fold publishes $R_1$.
- The fold worker publishes $R_1$ and subsequently truncates or resets the delta log.
- The reader proceeds to search the reset delta log (missing the folded key) and then looks up the key in $R_0$ (where the updated key is absent). The reader observes a transient stale state or phantom `ENOENT`.

## Decision
Incorporate a monotonic fold watermark $W$ into the published base root descriptor:
1. When the fold worker builds a new base root $R_1$, it sets $W = \max(\text{delta\_seq})$ of all records included in the fold.
2. The watermark $W$ is published atomically with the base root in the `RT30` descriptor.
3. Every delta record carries a monotonic sequence number `seq`.
4. Readers resolve keys using the strict comparison:
   ```c
   if (delta_entry.exists && delta_entry.seq > base.watermark) {
       return delta_entry.value;
   }
   return btree_search(base.root, key);
   ```
5. Delta entries with $\text{seq} \le W$ are guaranteed to be in the base tree and can be cleaned up in the background without requiring global reader drain synchronization.

## Consequences
### Positive
- Strict snapshot isolation for lock-free readers without requiring RCU grace-period stalls during delta truncation.
- Completely decouples delta log cleanup from read path execution.

### Negative / Trade-offs
- Requires passing the base watermark down to read resolution helpers.
