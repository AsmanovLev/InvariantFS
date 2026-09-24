# ADR-009: Meta-v3 Durability Contract (fsync, close, per-append barrier)

## Status
Accepted

## Context

Meta-v3 became the default (and only) format in v0.5.0, but its durability
contract was implicit and partly wrong:

- `vol_sync()` returned 0 immediately for `VOLF_V3` behind a stale
  "v3 skeleton is empty/read-only" comment, so FUSE `.fsync` neither flushed
  the dirty bitmap nor issued a barrier on v3.
- `vol_close()` wrote the CLEAN superblock before any barrier; the only
  barrier was an opt-in `INVFS_FSYNC` that ran *after* the CLEAN write.
- `vol_delta_append()` barriers after **every** record, which makes each
  metadata mutation durable by construction and masks the no-op `fsync`.
- The v3 io-error latch (`io_latched`) was ignored by `vol_write_enabled()`
  on v3, so a latched v3 volume kept accepting mutations.

The contract needed to be made explicit, correct, and testable.

## Decision

1. **`fsync` is a real durability point on v3.** `vol_sync` performs
   `vol_flush` (which persists the dirty bitmap via `vol_v3_bitmap_flush`)
   followed by one `vmux_barrier`, and latches the volume on failure, exactly
   as the v2 path does.
2. **The CLEAN superblock never precedes durability.** `vol_close` barriers
   before writing CLEAN. The barrier is the default for every backing store;
   `INVFS_CLOSE_NOBARRIER=1` is the explicit, documented opt-out. A failed
   barrier leaves the volume DIRTY and latched.
3. **The per-append delta barrier stays.** It is a durability ordering
   (design §9), not the lock-free read-consistency mechanism — readers get
   consistency from the in-RAM index being published after the record bytes
   and sequence number are assigned (ADR-002's watermark covers the fold
   race). Relaxing it is deferred until measured, and would require a
   `delta_durable` anchor and a redefined acknowledged-write contract.
4. **The io latch applies to v3.** `vol_write_enabled` refuses mutations on a
   v3 volume once `io_latched` is set, and `vol_io_error_latch` logs the
   latch using `io_latched` (v3 sets `needs_recovery` for the whole session,
   so it cannot gate the message).
5. **Failure injection works on v3.** `INVFS_SYNC_FAIL_AT=N` now reaches the
   v3 path; on v3 the v2 inode-area zeroing is a no-op, so the latch, the
   refused mutations, and the no-CLEAN close are what is exercised.

The full contract, including the commit-point table and the
structure-before-reference rule, is in `docs/architecture/META-V3.md` §4.

## Consequences

### Positive
- `fsync` has a well-defined meaning: it pins the derived allocation bitmap
  and issues a real storage barrier.
- A CLEAN volume is always a durable volume; a failed close barrier recovers
  instead of silently adopting a torn tail.
- The per-append barrier gives each namespace mutation immediate durability,
  which is stronger than POSIX requires and is now documented rather than
  incidental.

### Negative / Trade-offs
- Two barriers on a v3 close/fsync (`vol_flush` already barriers the bitmap,
  then `vol_sync`/`vol_close` barriers again). This is deliberate redundancy
  for ordering clarity; it can be folded later if measurement shows it
  matters.
- The per-append barrier costs one device flush per metadata record. It is
  kept for durability, not correctness, and remains a known throughput
  cost.

## Revisit triggers

- Measurement shows the per-append barrier dominates metadata throughput,
  and a `delta_durable` anchor plus fsync-deferred commit contract is
  designed and proven against the fold/read paths.
- Device-cache semantics change such that the double barrier in
  `fsync`/close is demonstrably wasteful.
