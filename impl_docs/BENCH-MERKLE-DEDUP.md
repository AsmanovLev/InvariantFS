# Merkle dedupe: multi-level matching versus 64-KiB-only (measurement)

Measured 2026-11-17 offline, without a filesystem: to decide whether the hash tree pays off,
we need numbers, not reasoning.

## Methodology

Corpus (all in `/dev/shm`, extracted from `small.img` via `ext4fs extract_batch`):
4 ext4 images of 256 MiB each, ~64 MB of files inside (2848 files):

| image | what it is |
|---|---|
| `A.img` | base (`mkfs.ext4 -d srcA`) |
| `B.img` | THE SAME set of files, different ext4 geometry (`-b 4096 -I 256`) = metadata **shift** |
| `C.img` | A + one file changed by 2 KiB + one new file |
| `D.img` | A + 50 files changed by 1 KiB each |

Hashing: leaves of 4 KiB, `blake2b(128 bit)`; node at level k =
`H(child0 || child1)`; levels 4/16/64/256/1024 KiB. Match pool --
nodes of image A. (A BLAKE3 tree works the same way; I used blake2b for the sake of
experiment speed -- the hash class is the same.)

Metric: the fraction of image bytes covered by nodes whose hash is already in the pool.

## Results

```
A vs A (identical)                                     4 KiB 100.0% ... 1 MiB 100.0%
A vs B (metadata shift, same files)
   4 KiB  77.9%   16 KiB 73.7%   64 KiB 72.8%   256 KiB 72.0%   1 MiB 71.5%
A vs C (1 file changed by 2 KiB + 1 new)
   4 KiB  99.6%   16 KiB 99.3%   64 KiB 98.3%   256 KiB 94.1%   1 MiB 82.8%
A vs D (50 files changed by 1 KiB each)
   4 KiB  99.6%   16 KiB 99.2%   64 KiB 98.0%   256 KiB 93.9%   1 MiB 82.8%
```

## What this means for size

Recalculation into “how much has to be appended on top of A” (256 MiB image):

| policy | A vs C | A vs D | A vs B (shift) |
|---|---|---|---|
| 1 MiB extents | 44.0 MiB | 44.0 MiB | 73.0 MiB |
| 64 KiB extents | 4.4 MiB | 5.1 MiB | 69.6 MiB |
| 4 KiB extents | **1.0 MiB** | 1.1 MiB | **56.6 MiB** |
| dynamic (by match structure) | **~1.0 MiB** | ~1.1 MiB | ~56.6 MiB |

The key point: **a tiny edit poisons the entire node of its size at all levels
above**. One file changed by 2 KiB, with 1-MiB extents, forces
rewriting 44 MiB, while with 4-KiB detection -- 1 MiB. That is a 44x difference on
a single change.

## Conclusion: dynamic extents + tree remove the tradeoff

Previously we measured two opposing things:
- space/compression/rounding like LARGE extents (64 KiB: waste 8.5%,
  1 MiB: 0.7%; compression of 4-KiB segments is 23-32% worse);
- dedupe likes SMALL ones (4 KiB: 99.6% versus 82.8%).

The tree separates these two decisions:
1. **Matching** runs on 4-KiB leaves (and/or at any level) -- dedupe
   sees small matches.
2. **Storage** -- only uncovered runs, with extent size = the natural
   run size from the ladder (a small hole -> 4 KiB, a solid new region
   -> 1 MiB). Large extents get good compression and low rounding,
   small ones -- only where otherwise the whole node would be lost.
3. Compaction merges/splits extents when the match picture changes.

## Cost

- **Index**: 32 B per 4-KiB leaf = 0.78% of logical volume; levels 16/64 KiB
  are derived from the leaves and need not be stored. This is cheaper than the 9% we
  lose to rounding now.
- **Shift** remains a weak spot (A vs B: 77.9% at 4 KiB). The tree makes it
  possible to catch (a series of matching nodes with an offset = a shift detector), but this
  is separate work; CDC is not needed on our corpora (measured earlier).
- **Incrementality**: editing a file = recomputing the changed leaves and the path to
  the root (log n) instead of reading and hashing the stored bytes, as now.
