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

   **Superseded in part (group commit).** The precondition this decision set
   has now been met: a 500-file / 12-byte import was measured at 3509
   fsyncs, 7.018 per file, 99.4% of wall time blocked in `fsync`. The
   barrier was NOT relaxed — `vol_delta_append` still returns only when its
   record is on stable storage — but the number of *physical* flushes is no
   longer one per call. `blkio_flush` is now group-committed: `blkio_pwrite`
   counts writes into `write_gen`, and a barrier is satisfied by any
   completed flush that already covers them, with the first caller in need
   becoming the flusher and the rest waiting on that one flush. The
   guarantee is per CALL and is unchanged; the flush count is per flush
   window. See `src/core/blkio.c` and
   `docs/architecture/META.md` §4.

   Two things this explicitly does **not** authorise, which the measurement
   surfaced and which remain open:
   - The three per-file inode appends (`create_node`, `write_commit`,
     `set_meta`) each coalesce to the same delta key, but each is a separate
     mutation with its own contract. Collapsing them needs the
     `delta_durable` anchor and the redefined acknowledged-write contract
     this decision already named.
   - `v3_publish` barriers the COW base pages before `mbuf_root_publish`
     barriers the root, and `META-V3.md` §4.1 calls that ordering
     load-bearing. Merging the two barriers is safe on the evidence (RT30's
     double slot plus CRC resolve a torn publish to the old root, so "root
     durable ⇒ its pages durable" holds either way) but it changes a
     documented ordering, so it is its own decision and not this one.

   Measured effect, same method: the single-threaded `invf-import` goes
   7.018 → 6 fsyncs per file, because a serialized writer has a distinct
   write before each barrier and so has nothing redundant to collapse. The
   win is on the concurrent path — 8 threads x 64 barrier rounds fall from
   503 physical flushes to 64. The 84,279-file projection moves from
   65,377 s to ~55,900 s.
4. **The io latch applies to v3.** `vol_write_enabled` refuses mutations on a
   v3 volume once `io_latched` is set, and `vol_io_error_latch` logs the
   latch using `io_latched` (v3 sets `needs_recovery` for the whole session,
   so it cannot gate the message).
5. **Failure injection works on v3.** `INVFS_SYNC_FAIL_AT=N` now reaches the
   v3 path; on v3 the v2 inode-area zeroing is a no-op, so the latch, the
   refused mutations, and the no-CLEAN close are what is exercised.

The full contract, including the commit-point table and the
structure-before-reference rule, is in `docs/architecture/META.md` §4.

## Consequences

### Positive
- `fsync` has a well-defined meaning: it pins the derived allocation bitmap
  and issues a real storage barrier.
- A CLEAN volume is always a durable volume; a failed close barrier recovers
  instead of silently adopting a torn tail.
- The per-append barrier gives each namespace mutation immediate durability,
  which is stronger than POSIX requires and is now documented rather than
  incidental.
- Group commit makes that per-mutation durability cost O(1) physical flushes
  per flush window instead of O(1) per record, without weakening what a
  returning call promises. A barrier that finds nothing new since the last
  completed flush costs no flush at all.

### Negative / Trade-offs
- Two barriers on a v3 close/fsync (`vol_flush` already barriers the bitmap,
  then `vol_sync`/`vol_close` barriers again). This is deliberate redundancy
  for ordering clarity; it can be folded later if measurement shows it
  matters. (Group commit now makes the second one free when the first
  covered it.)
- The per-append barrier still costs one device flush per metadata record on
  a *serialized* writer, because each of those records is a distinct write
  and a distinct write is exactly what a barrier cannot skip. Measured: 6
  fsyncs per imported file, down from 7.018. Only concurrent writers
  collapse. Metadata-heavy single-threaded bulk import therefore remains
  fsync-bound, and closing that gap needs the `delta_durable` anchor below,
  not more group commit.
- Group commit's counters live on the `blkio` handle, so a barrier's skip
  decision is only sound if *every* write to that handle is counted.
  `blkio_pwrite` is the single choke point that makes this true today; a
  future direct-write path would silently break the guarantee rather than
  fail loudly, which is why the counters are bumped unconditionally.

## Revisit triggers

- **Fired.** Measurement showed the per-append barrier dominates metadata
  throughput. What was built in response is group commit, *not* the
  `delta_durable` anchor: the barrier's per-call guarantee turned out not
  to require a flush per call, so the contract was kept and the flush count
  collapsed. The anchor remains the open item, for the serialized-writer
  case group commit cannot reach (see the trade-offs above).
- Device-cache semantics change such that the double barrier in
  `fsync`/close is demonstrably wasteful.
