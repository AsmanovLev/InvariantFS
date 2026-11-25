# Handoff: why small files are expensive (and why it is NOT delta placement)

State at the end of the session in which the fsck-blind-to-delta bug (`7cc8f5b`)
was found and fixed, and the hypothesis “move the metadata to SSD” was tested and
**disproven as the root cause** before any code was written.

## Measurements (stand 192.168.0.2, after disabling sweep)

| operation | on the stand | locally, same binary |
|---|---|---|
| create empty file | 0.8 ms/op | ~0.1 ms/op |
| delete | **48 ms/op** | 1.07 ms/op |
| delete 100 / 1000 / 4000 files | 48.2 / 47.9 / 48.3 ms/op | — |

Delete is **flat** in the number of files: it is not O(N²) and not database degradation.

## Key conclusion

`create` and `unlink` go through THE SAME metadata path:

```
vol_inode_delta_put + vol_dirent_delta_put -> delta_put -> vol_delta_append
                                          -> pwrite record into segment -> vmux_barrier
```

The difference of 0.8 ms vs 48 ms — 47 ms — cannot be the delta, the fence, nor
delta placement: they are the same for both operations. It lies entirely in what
ONLY unlink does:

- `vol_retire_recipe` — deferred release of recipe blocks;
- `vol_pba_ref_release` — editing the pba reference map;
- `vol_free_blocks` — freeing blocks;
- `vol_bm_dirty` on each block — tracking of “dirty” bitmaps;
- `vol_reclaim_drain` — draining the reclaim queue.

Hence: **moving the delta to the RAW zone (dev0/SSD) will not speed up delete.** This
needed to be checked before implementation; verified by reasoning from already-measured
numbers, not a new experiment — the zone-migration experiment was started and rolled back.

## Hardware latency (measured on the stand, O_DIRECT, queue 1)

| device | random 4K read |
|---|---|
| `/dev/sdb1` (HDD, shadow zone) | p50 **16.7 ms**, p90 24.5, p99 35.4, max 39.0 |
| `/dev/sda2` (SSD) | p50 **0.13 ms**, p90 0.6, p99 1.1, max 1.7 |

Hence delete = 48 ms/op is **1-2 random accesses to the HDD per operation**,
not algorithmic complexity. Cross-check: emerge-webrsync, ~160k files in 3 hours
= 15 files/s = 66 ms/file ~ 4 random I/Os per file.

Create (0.8 ms/op) does NOT make random HDD accesses: its writes are sequential
(append to a segment), and the fence is amortized over a batch. Delete does, because
freeing blocks edits bitmap pages scattered around: read-modify-write of a random
page per block.

## What to do next (in order)

0. **Batch block frees** — the main and cheapest fix. Accumulate freed
   blocks in RAM and apply bitmap pages once per fence (a page covers 32768
   blocks), plus sort by pba for locality. This is the same group commit that
   already works in `vol_write.c`, and it touches neither the layout nor the
   Shadow contract. Expected: 48 ms -> single-digit ms.
1. **Timings inside `vol_unlink`** (`src/core/vol_dirs.c:738`). Wrap
   `clock_gettime(CLOCK_MONOTONIC)` around four phases (retire / pba_ref_release /
   free_blocks+bm_dirty / reclaim_drain) and take 200 unlink measurements on the stand.
   This will answer the question in one run. Favorite hypothesis: `vol_bm_dirty` +
   `vol_reclaim_drain` pull in rewriting of metadata pages per block, not per
   operation.
2. **If bitmap/reclaim is confirmed** — batching: accumulate frees and
   mark dirty bits in RAM, write the bitmap page once per fence.
   This is the same technique already used in `vol_write.c` (group commit).
3. **Only after that** — the question of metadata placement (delta on dev0). If it is
   done at all, follow the offset rule: RT30 is exactly 48 bytes with no free
   fields, SPT0 immediately after it, so there is nowhere to store a second pointer;
   the segment's shadow copy must be computed from the address, not addressed by a
   field. The contract “RAW (dev0) died -> Shadow (dev1) is readable” must still hold:
   either the delta is mirrored (the copy lags, the loss window is bounded by fold
   frequency and must be documented explicitly), or the tail is lost — and that is no longer “readable”.
4. **Inline content for small files** — a separate axis (fewer records, not
   faster writes). Trade-offs: the inode row becomes variable-length,
   `chmod` rewrites the content, dedupe/retention do not apply to inline
   data. “One record per file” runs into hardlinks (nlink > 1 -> N names).

## What is NOT a solution

- **AG / namespace sharding.** A different axis: partitioning vs placement. Ruled
  out by measurement: 3452 -> 3353 files/s at 10x growth, rebuilds = 1, and there
  is not a single mutex in `vol_write.c`. AG does not speed up a single unlink.
- **Re-tuning sweep.** Sweep is disabled by default (`interval = iv ? atoi(iv) : -1`),
  and that is correct; `INVFS_SWEEP_INTERVAL=1` in the stand's initramfs was a stand bug.

## The same pathology observed from the other side

`emerge-webrsync` sat for 3 hours with **zero block I/O** (`/proc/PID/io`), while the
machine was held up by its own `rm -rf /var/tmp/emerge-webrsync.*`: deleting ~160k files
at ~48 ms = hours. It is neither the network nor Portage — it is the same `unlink` path.
