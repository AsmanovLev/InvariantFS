# WP-DEFER: one deferred-work queue instead of five

> **CORRECTION 2 (measured on the stand — this is the root cause, read first).**
>
> A single unlink of an empty file on the stand: wall **906 ms**, daemon CPU 1080 ms,
> block-I/O writes **127 076 KiB = 124 MiB**.
>
> Volume bitmap = 500 695 040 bits / 8 = **62.6 MB**, mirrored onto two devices
> -> 124 MiB. So the entire bitmap is written on EVERY operation.
>
> Cause: dirty tracking -- ONE contiguous byte range
> (`bm_lo`/`bm_hi`, volume_internal.h:1179): `if (byte < bm_lo) bm_lo = byte;
> if (byte+1 > bm_hi) bm_hi = byte+1;`. On a 500-million-block volume, two
> freed blocks in different places give a range over the WHOLE bitmap. Coalescing
> happens only within a single byte.
>
> Hence: 906 ms per unlink, hours for `rm -rf` of 160k files, rsync with zero
> block I/O. This is not HDD latency, it is write volume (write amplification
> ~30 000x).
>
> The right structure -- a BITMAP OF DIRTY PAGES, not a range:
> 15 300 pages for the whole volume = 2 KB of bits; flush writes only dirty pages
> in LBA order. This is the `DFR_PAGE` slice of the plan below -- it has NOT been done.

> **CORRECTION (after reading the code and phase measurements — read before everything else).**
>
> The premise below ("we write a bitmap page for every free block") is **wrong**:
>
> - `vol_bm_dirty` (volume_internal.h:1179) only extends the `bm_lo`/`bm_hi` byte
>   range in RAM — there is no I/O there at all, and flush writes ONE contiguous
>   slice, i.e. coalescing already exists;
> - `vol_free_blocks` (volume.c:3428) = `spt0_block_pinned` (pin check in RAM)
>   + `vol_free_run` (bit clearing in RAM);
> - local phase measurement: **`probe+retire` = 0.00 ms**, i.e. the free path
>   during unlink costs nothing.
>
> So the "free + page" slice will not give a measurable win, and there is no need
> to do it first. The real nonzero-time phase for an empty file is **`hold+delta`**
> (`vol_pba_ref_hold` + `pba_ref_ensure`), and it is the only one
> present in unlink and absent in create (create 0.8 ms, delete 48 ms on the
> stand). This is the cost of MAINTAINING THE REFERENCE MAP PER OPERATION, not writing
> the bitmap.
>
> Revised breakdown:
>
> 0. Measure `vol_pba_ref_hold` / `pba_ref_ensure` separately on the stand
>    (the same `[tl]` marking, one extra marker).
> 1. If confirmed, the queue's subject becomes **servicing
>    the pba map**: replace per-operation hold/release with deferred application at
>    a fence (this is batching, just with a different payload).
> 2. `DFR_PAGE` / `DFR_FREE` — remove from the plan: they are already implemented in the code.
>

## Why

The engine currently has five independent "defer, group, apply at a barrier" mechanisms,
each with its own policy, application point, and guarantees:

| # | mechanism | where | what it defers |
|---|---|---|---|
| 1 | group commit | `vol_write.c` (staged set + `vmux_barrier`) | delta and session data writes |
| 2 | staged/doom | `dl_staged_keep` / `dl_doom_locked` | staging fate on barrier failure |
| 3 | reclaim | `vol_reclaim_drain`, `vol_reclaim_bump_epoch` | block freeing |
| 4 | fold | `vol_fold_request`, `g_fold_gate` | folding the delta into the base |
| 5 | bitmap | `vol_bm_dirty` on every block | map page writes |

The cost is visible in the measurements: file deletion on the stand — **48 ms/op** (1-2 random
HDD accesses at p50 16.7 ms), while **the whole volume free-space map is
≈ 61 MiB, and dev0 is 3.4 MiB**, i.e. all these pages are already in memory. We write
them on every touch instead of "once per barrier, in order".

Mechanism 5 is the most expensive and the most unnecessary in its current form.

## Model

One queue, one owner, one application policy.

```
vol_defer_post(v, kind, key, arg)   -- producer, never blocks I/O
vol_defer_apply(v, mode)            -- at a barrier, mode = SOFT | HARD
```

### Work kinds and coalescing keys

| kind | key | coalescing | applied |
|---|---|---|---|
| `DFR_PAGE` | bitmap/meta page number | bitmap, last wins | SOFT |
| `DFR_FREE` | pba + block count | array, dedup by pba | SOFT |
| `DFR_RECLAIM` | — | itself batched (already) | SOFT |
| `DFR_FOLD` | — | flag, not a batch | HARD and idle only |

### Application order (hard-coded, not configurable)

```
1. DFR_FREE     (sort by bitmap-page index, dedup)
2. DFR_PAGE     (walk the dirty set in LBA order)
3. DFR_RECLAIM
4. DFR_FOLD     (barrier-item: trylock + "no live writers" condition)
```

Why this order: frees must land BEFORE page writes, otherwise a page goes
to disk with a free block marked occupied, and at the next barrier it is
rewritten a second time. Fold goes last because it rebuilds the base to which
the other three have already been applied.

### Barrier policy

- **SOFT** (ordinary fence): apply 1-2-3, leave fold alone.
- **HARD** (volume close, mount idleness, explicit `vol_flush`): same + fold if
  the threshold is crossed and the volume is idle (`g_open_handles == 0`) or the hard cap is hit.
- The producer **never** waits for disk: it only enqueues.
- The queue is bounded: `DFR_FREE` is applied immediately on overflow
  (sorting is preserved); `DFR_PAGE` needs no bound — it is a bitmap
  of ~2 KB for the whole volume.

### Interaction with the allocator (the main trap)

A deferred `DFR_FREE` means the block is still considered occupied. Therefore:

- the allocator **must not** hand out a block queued for freeing;
- either `vol_alloc_blocks` first calls `vol_defer_apply(SOFT)` if the queue
  is nonempty (a simple and sufficient rule), or it maintains a `pending_free` set
  and subtracts it. The former is cheaper and harder to break.

## Invariants (learned in practice, not invented)

1. `dl_staged_keep` must be called **before** `di_put`.
2. `di_put` overwrites a slot **without comparing seq** — a concurrent write to
   the same key is silently lost. The queue must not introduce parallelism where
   there was none.
3. A group fence requires accounting for `delta_inflight`: otherwise fold/doom sees
   a half-applied set.
4. **Fold is not batched and not parallelized.** Two concurrent folds already
   wrecked a volume: `fold_pre_root` is overwritten, and `fold_reclaim_hook`
   frees pages that the first fold had just published as live.
   Therefore `DFR_FOLD` is a trylock item, not a batch member.
5. Freeing is applied only after the barrier that made the row deletion
   durable (retire semantics of WP607). Moving it into a batch must not apply it earlier.
6. The order "row goes first, blocks second" is unchanged: the reverse order yields
   a live row pointing to free blocks.

## Implementation breakdown

**Slice 1 — `DFR_FREE` + `DFR_PAGE`** (the only one with a measured win).
`vol_bm_dirty` stops being "per block" and becomes "set a bit in a set";
`vol_free_blocks` enqueues, `vol_alloc_blocks` drains the queue before
allocation. Expectation: 48 ms/op → single-digit ms, metric — "page writes per
operation" (was ≈ the number of freed blocks, will become ≈ 0).

**Slice 2 — `DFR_RECLAIM`** moves to the same queue; epoch bumps remain,
but are applied at queue exit rather than in a separate pass.

**Slice 3 — `DFR_FOLD`** as a barrier-item: trylock + idleness + hard cap.
Only after slices 1-2 have been live for a while.

## Non-goals

- We do not touch record group commit (`vol_write.c`): it works and is measured.
- We do not introduce parallelism into the queue: single owner, serialized application.
- We do not make a "fold batch": fold is exclusive, not a batch.
- We do not change zone layout or the contract "RAW (dev0) is dead -> Shadow (dev1) is read".

## Metrics we judge by

1. `unlink`, ms/op on the stand (currently 48) and locally (1.07).
2. Number of bitmap page writes per operation (expectation: drops ~100x).
3. `emerge-webrsync`: 160k files in 3 h = 15 files/s (currently HDD-bound).
4. No regressions: `make test`, fsck CLEAN, mount without fold in the hot path.

## Appendix: why this is not enough for the full picture

Two independent findings that still stand and are not fixed by the queue:

- `vol_delete_siblings` eats **87%** of the time to delete a file with contents
  locally (0.26-0.31 ms out of 0.30-0.38 ms) with **zero** siblings — a pure
  CPU tax from cascading lookups; fixed by a "has siblings" flag.
- `probe+retire` during unlink costs 0.00 ms: blocks are freed lazily, so
  the 48 ms come not from that function's path but from what the application does.
