# ADR-003: Generation-Based Savepoints and Rollback Semantics

## Status
Proposed

## Context
Design §6 originally specified savepoints as `{base_root, delta_end}`, where `delta_end` was a byte offset into the delta log. Rollback was defined as truncating the delta log to `delta_end` and restoring `base_root`.

However, if background fold runs while a savepoint is live:
1. Fold resets the delta log to start a fresh chain.
2. The byte offset `delta_end` becomes meaningless in the new delta segment chain.
3. Earlier delta records prior to the fold are merged into the new base, but the savepoint references the older base root.

## Decision
Redefine savepoint tracking to use generation tuples:
1. A savepoint is stored as `{base_gen, delta_seq}`.
2. When creating a savepoint, the current base tree root is pinned (`pinned_root = base_root`), preventing reachability diff reclamation from freeing its pages.
3. The delta sequence number at the time of savepoint creation is recorded as `delta_seq`.
4. On rollback:
   - Mount the volume using the pinned `base_gen` root.
   - Truncate any delta records with $\text{seq} > \text{delta\_seq}$.
   - If fold has occurred since the savepoint, initialize a new delta generation rooted at `delta_seq`.

## Consequences
### Positive
- Resolves undefined behavior when rolling back a volume where fold occurred during savepoint lifespan.
- Decouples savepoint validity from physical byte offsets in circular or multi-segment delta logs.
