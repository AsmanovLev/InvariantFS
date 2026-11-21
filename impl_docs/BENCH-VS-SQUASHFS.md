# An honest comparison with squashfs (space) — measured

Corpus: a 64 MiB directory, 2889 files (real Debian), extracted from
`/dev/shm/small.img` via `ext4fs extract_batch`. zstd-19 compression on both.
InvariantFS: 1 GiB volume, writing via FUSE, 3 sweeps `INVFS_PROFILE=balanced`.

## Key methodological error not to repeat

Comparison is invalid when pooling units differ. Our unit is `TZ_BATCH_MAX`
(4 MiB); squashfs has a 128 KiB block (1 MiB max). The first comparison "22.8 MiB
versus 23 MB = parity" was incorrect: it relied on our four-times-larger
unit.

To level the playing field, `INVFS_TZ_BATCH_KB` (4..65536) was added.

## Result at EQUAL granularity

| pooling unit | InvariantFS (data) | squashfs |
|---|---|---|
| 128 KiB | 25.9 MiB | 23 MB (`-b 128K`, default) |
| 1 MiB | 23.8 MiB | 21 MB (`-b 1M`) |
| 4 MiB | 22.8 MiB | — (squashfs limit is 1 MiB) |

At equal granularity we are **13% worse**, and the gap is the same at both
sizes -- so the cause is not the block size.

## Why: sub-block segments occupy a whole block

`INVFS_DEBUG_SEGS=1 invf-stats -v` (histogram of segment sizes and
rounding up to blocks):

```
[segs] uncompressed: 696 ent(s), logic 3.40 MiB, physical 4.93 MiB,
                     waste 1.52 MiB (30.9% of physical)
[segs]   <4K      n=524  logic 0.90 MiB  physical 2.05 MiB  waste 1.14 MiB
[segs]   4-8K     n=93   logic 0.52 MiB  physical 0.73 MiB  waste 0.20 MiB
[segs]   8-64K    n=69   logic 1.35 MiB  physical 1.50 MiB  waste 0.15 MiB
[segs]   64-256K  n=10   logic 0.62 MiB  physical 0.65 MiB  waste 0.02 MiB
[segs] compressed: 626 ent(s), logic 11.10 MiB, physical 5.35 MiB,
                    saving 5.75 MiB (2.08x)
[segs]   <4K      n=340  logic 0.57 MiB  physical 1.33 MiB  waste 0.76 MiB
[segs]   8-64K    n=172  logic 4.83 MiB  physical 2.02 MiB  saving 2.82 MiB
[segs]   64-256K  n=89   logic 5.56 MiB  physical 1.91 MiB  saving 3.66 MiB
```

A segment occupies WHOLE blocks (`seg_extent`), so 1.8 KiB of data costs
4096 bytes: in the `<4K` bucket 524 segments yield 2.05 MiB physically for 0.90 MiB
of data. Same in the compressed class: 340 `<4K` segments -> 1.33 MiB.

Total for sub-block segments: **1.14 + 0.76 = 1.90 MiB (~9% of our data)**.

## What to do about it

squashfs does not have this loss: its fragments pack the tails of small files into
shared blocks (measurement: `-no-fragments` 24 MB versus 23 MB by default).

Our analog -- batches (already present: TEXT -- PPMd, binary -- zstd, `TZ_BATCH_MAX`),
but small binary files do not get into them and are written as separate segments.
If files with a payload < ~4 KiB were wrapped into a binary batch regardless of
compression gain (block rounding is more expensive anyway), the saving would be ~1.9 MiB, i.e.
22.8 -> ~20.9 MiB versus 21 MB for `squashfs -b 1M`: parity already at equal
granularity.

## Separately: the container corpus

Two copies of a 256 MiB image (512 MiB logically, 64 MB of files), zstd-19:

| | size |
|---|---|
| squashfs | 23 MB |
| InvariantFS: compressed 23.1 + incompressible 2.8 | 25.9 MiB (+13%) |
| + metadata pages in circulation | +1.5 MiB |
| + meta-zone reserve (64.1 MiB on a 2 GiB volume) | -> `used` 91.4 MiB |

Leaf layout puts text members into a blob, where per-segment zstd operates
instead of a PPMd batch: in the directory measurement the TEXT batch produced
12.6 MiB out of 50.3 MiB of text (4x). That is the +13%.

## What is NOT the cause (verified)

- compression levels: `CPACK_ZSTD_LANE=19`, balanced profile = 19 (the number in
  `algo=zstd(1)` -- is the algorithm number, not the level);
- dedupe: both copies share one recipe and pba;
- incompressible data (4.8 MiB, 0.70x) -- this is a real gzip payload
  (657073 -> zstd 657104, 1.00x), not lane overhead.

## FIX: sub-block payloads no longer occupy a whole block

The cause was twofold:
1. `sweep_dispatch` had no batch path for files < 4096, and structural lanes
   (gzip) intercepted them and, on refusal, set `GENERIC_GUARD` and did
   `return 0` -- "swept, done", so the batch branch below for them was
   unreachable;
2. `vol_tz_flush` simply discarded a batch member with `bfam == 0`, while
   `invfs_binary_family` itself requires >= `INVFS_BIN_MIN_SIZE` (4096) -- i.e.
   all sub-block payloads were discarded by construction.

Done: a sub-block non-text payload goes into the shared ZSTD batch BEFORE
structural lanes, and the family at seal time is treated as a BCJ choice, not
as permission (for payloads < block; everything not smaller than a block follows the old path).

### Measurement (same 64 MiB / 2889 files corpus, zstd-19)

| | before | after |
|---|---|---|
| uncompressed class | 696 segs, 3.40 logical -> 4.93 physical (waste 1.52) | **173 segs, 2.50 -> 2.88 (waste 0.39)** |
| compressed class | 626 segs, 11.10 -> 5.35 | **294 segs, 10.55 -> 4.05** |
| text batch | 12.6 | 13.6 |
| **data total** | **22.88 MiB** | **20.53 MiB (-10.3%)** |
| unclaimed | 5.9 | 3.7 |
| content verification | -- | **2889/2889 files bit-for-bit** |

### Comparison at EQUAL pooling unit (after fix)

| unit | InvariantFS | squashfs | gap |
|---|---|---|---|
| 128 KiB | 23.6 MiB | 23 MB | +2.6% |
| 1 MiB | 21.5 MiB | 21 MB | +2.4% |
| 4 MiB (our default) | **20.5 MiB** | -- (max 1 MiB) | better |

Before the fix the gap was +13% at both sizes.

## DISPROVEN by measurement: text members in a PPMd batch give no gain

Hypothesis: leaf layout puts guest text into a blob, where per-segment
zstd operates rather than PPMd -- and this explains the +13% of the container corpus.

We did it and measured it (corpus: 2 copies of a 256 MiB image, 2889 files, zstd-19):
- the leaf classifier left text members as inodes (196 of 2889);
- the deferred loop was no longer skipped in blob mode;
- batch lanes were allowed to take inode members (`vol_name_is_container_member`).

Result:

```
before: compressed 23.1 + uncompressed 2.8                 = 25.9 MiB
after : compressed 20.4 + text batch 2.6 + uncompressed 2.8 = 25.8 MiB
```

The text batch WORKS (23.3 MiB -> 2.6 MiB), but overall this is **-0.1 MiB**, i.e.
nothing, at the cost of +196 inode rows. The reason: a blob is ONE stream, compressed in
64-KiB segments, so cross-file redundancy of text in it is already
captured; PPMd beats zstd only on sparse context, of which
none remains inside the image.

The code was rolled back. So the +13% of the container corpus has another cause; what remains
unverified is granularity: the blob is cut into 64-KiB segments with
fresh zstd context, while squashfs -- into 128 KiB / 1 MiB blocks. This must be checked
by measuring `INVFS_SEGMENT_*` thresholds (or by comparison with squashfs -b 64K =
24 MB on the same corpus), not by reasoning.

## CAUSE OF +13% FOUND: segment-to-block rounding

The granularity hypothesis was verified by direct measurement. The
`INVFS_DEBUG_SEGS=1` diagnostic counts, for EACH segment, stored = 8 + csize and
physical blocks (`seg_extent`).

Corpus: two identical copies of a 256 MiB image (64 MB of files), zstd-19,
delivered via `invf-import` (without FUSE), 3 sweeps.

```
compressed   : 2008 segs, stored 42.26 MiB -> physical 46.20 MiB, waste 3.95 MiB (8.5%)
uncompressed :  344 segs, stored  4.98 MiB -> physical  5.76 MiB, waste 0.78 MiB (13.5%)
```

(segments of the two copies are counted twice; unique loss ~2.37 MiB on 25.9 MiB
of data = **9%**).

Mechanism: a segment = `[8 B header]+[compressed payload]`, and this occupies
WHOLE blocks. A 64-KiB payload compresses to ~21 KiB, +8 B -> 6 blocks = 24 KiB,
i.e. ~12% into garbage on every segment.

### Curve by segment size (build with a different INVFS_SEGMENT_SIZE)

| segment | our data total | waste (compressed) | bit-exactness |
|---|---|---|---|
| 64 KiB (current) | **25.9 MiB** | 8.5% | ok |
| 256 KiB | **22.7 MiB** | 2.5% | ok |
| 1 MiB | **20.8 MiB** | 0.7% | ok |
| squashfs -b 128K | 23 MB | -- | -- |
| squashfs -b 1M | 21 MB | -- | -- |

That is, with a 1 MiB segment we BEAT the best squashfs configuration
(20.8 versus 21 MB), and our compression is no worse: 18.0 MiB compressed versus
~18.2 MiB for squashfs (21 minus 2.8 MiB of incompressible gzip payload).

### What this means for policy (decision -- up to the format owner)

Pros of large segments: -20% space on the container corpus (25.9 -> 20.8).
Cons:
1. **Dedupe**: dedupe granularity equals the segment. Measured earlier: 4 KiB gives
   cross-image dedupe 4.60% versus 7.4% at 64 KiB; 1 MiB will kill near-duplicate
   dedupe almost entirely.
2. **Read**: random 4 KiB inside a large file requires decompressing
   a segment -- 64 KiB now, 1 MiB then (16x).
3. **Format**: `INVFS_SEGMENT_SIZE` is used in 144 places, including seal
   (symbol = segment) and tests, so this is not a one-liner but a policy field in
   the superblock (v0.7, format not frozen).

The option that looks like the best compromise: segment size is a VOLUME PROPERTY
(a field in the superblock), defaulting to 64 KiB for dedupe-oriented volumes and
1 MiB for volumes holding large containers, with an explicit warning about the cost
of random reads.
