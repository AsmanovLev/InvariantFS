# BUG: `unclaimed` Blocks Are Not Returned (reclaim does not free garbage)

**Status: CLOSED (48404b6).** Found while analyzing the comparison with squashfs.

## Fix Outcome

| | before | after |
|---|---|---|
| unclaimed right after writing 2×256 MiB | 18.5 MiB | **2.6 MiB** |
| unclaimed after 3-6 sweeps (plateau) | 13.6-25.8 MiB | **1.4 MiB** |
| `used` (with the 64.1 meta-zone reserve) | 119.0 MiB | **92.3 MiB** |
| file reads | ok | bit-for-bit ok |

There were TWO causes, both in `src/core/vol_delta.c`:

1. **`dl_retire_collect`** discarded a retirement record if the row had been updated
   again (the rule required `s->seq == e->seq`, i.e. an exact match
   with the moment it was queued). A record is queued AFTER commit, so any
   subsequent row update (e.g. close/fsync) cleared it --
   even though the old segments are already dead. Measured: 2 records of 4096 pba
   were queued, drain collected 0. Now `s->seq >= e->seq`; rollback
   (`s->seq < e->seq`, fence rewound) still suppresses freeing.
2. **`dl_retire_free_list`** under `lazy_reclaim=1` (the default) skipped
   rebuilding the pba_ref map, although lazy means "the map is not maintained
   incrementally" (all `-1`s in unlink/write under it are skipped). As a result,
   the `pba_ref_count() == 0` gate refused exactly the blocks that the record
   asked to return: `freed 0 blk`. Now the map is rebuilt whenever
   it is needed for a freeing decision (the conservative direction:
   counts from the live set).

Diagnostics left in the code (env-gated): `INVFS_DEBUG_RETIRE=1`
(push/drain/freed per retirement queue) and `INVFS_DEBUG_UNCLAIMED=1`
(scans of orphan blocks with their first bytes -- revealed two classes: segments of
64 KiB with a header and single `BPG3` pages with a stride of 3).

## Symptom

`invf-stats -v` prints a line that does not change either from sweep to sweep or from
`--realize`:

```
  zones detail (physical used = content class, WP-DZ):
    RAW   : logic    7.3 MiB | used     3.9 MiB | 1.85x
    SHADOW: logic  638.2 MiB | used    23.4 MiB | 27.28x
    unclaimed: 25.8 MiB (allocated, no live reference)
```

`unclaimed` = "allocated, no live references". **6 consecutive sweeps and
`--realize` were run** — the figure stays put (25.8 MiB). There are 356 files in the volume, they read
bit-for-bit, and `invf-verify` says CLEAN.

## How to Reproduce

```
# corpus: a 256 MiB ext4 image with ~66 MB of real files, two identical copies
mkfs.ext4 -q -F -d src base.img && cp base.img c1.img && cp base.img c2.img

invf-mkfs v.img 2
INVFS_RAW_NO_COMPRESS=1 invf-fuse -f v.img m &
cp c1.img c2.img m/ && sync && fusermount -u m

for n in 1 2 3 4 5 6; do INVFS_PROFILE=balanced invf-sweep v.img >/dev/null; done
invf-sweep v.img --realize
invf-stats -v -n v.img | grep -E "SHADOW|unclaimed"
```

## Composition of `used` (to avoid repeating the reading mistake)

```
used 119.0 MiB = SHADOW 23.4 (live content) + RAW 3.9
               + unclaimed 25.8 (THIS BUG)
               + meta 64.1 (META-ZONE SIZE, set by mkfs; this is a reserve, not data)
```

**`meta` is included in `used` in full, so `used` cannot be compared directly with
the archive** — this is how the "4 times worse than squashfs" mistake was already made.

Comparison by live content (zstd-19 for both, 512 MiB logically):

| | size |
|---|---|
| squashfs | 24 MB |
| InvariantFS live content (SHADOW used) | **23.4 MiB** (parity) |
| InvariantFS with the bug (`unclaimed`) | 54.9 MiB (2.3x worse) |

So **the bug is the entire space loss**; dedupe (both copies share
one recipe and pba) and compression (27.28x) work.

## Where to Look

1. `src/core/vol_reclaim.c` — the comment in `volume.h` (WP-M15) says outright
   "Currently a stub; M15 fills it". If reclaim is still a stub, `unclaimed`
   cannot decrease.
2. `src/core/vol_spt0.c` — the second half of the same disease was caught earlier:
   with a single unconfirmed segment, capture refuses to free **the whole**
   pinning ("1 of 9927 segments … this capture does NOT reclaim"), and each sweep
   adds a new one (62,743 blocks = 245 MiB). Check whether the same flag
   is blocking freeing here.
3. `vol_dedupe.c` (merge frees blocks) and `vol_cpack.c`
   (`cpack_release_superseded`) — check whether their frees are being lost.

## Success Criteria

After the fix: `unclaimed` → 0 after two sweeps on a clean volume, and `used` minus
`meta` ≈ SHADOW used (≈ 27 MiB on this corpus versus 24 MB for squashfs).

---

# BUG 2: Freed Blocks Are Not Zeroed or punch-hole'd

**Symptom:** `cp --sparse=always v.img sparse.img` yields **306 MB**, while
the FS counts ~55 MiB as occupied. Cause: freed blocks retain old
bytes, so the sparse copy keeps them.

**Consequence:** the volume never returns space to the host — on a sparse or
compressed filesystem (and when converting to sparse) we keep paying for garbage.
On a real block device this is invisible (blocks are simply free), but when
the volume is stored as a file, it is visible.

**DONE.** `vol_punch_holes()` (volume.c) punches holes in free runs of the data region, but only AFTER
the bitmap with the frees becomes
durable (in `vol_close`: first flush the bitmap, then CLEAN, then punch) --
not earlier: a crash would "revive" the recipe while the bytes would be zeros. Runs under
1 MiB are skipped; on block devices the call is simply unsupported
(EOPNOTSUPP); dual-device volumes are skipped for now (needs the fd of the owning
device and accounting for the shadow split). `INVFS_PUNCH_HOLE=0` -- disable.

Measurement (corpus 64 MiB / 2889 files, volume 2 GiB, after 3 sweeps):

| | before | after |
|---|---|---|
| occupied on host (`du`) | -- | 52 MB |
| `--sparse=always` copy | **306 MB** | **52 MB** |
| live data (`used` minus the reserve) | ~28 | ~24 MiB |
| check | -- | verify OK, 200/200 files bit-for-bit |

---

# Reference: What Was Checked and Is NOT the Cause

- **Compression does not break dedupe.** Compression is per-segment with a clean context
  (`ZSTD_compress(cbuf+8, cbound, plain, enc_len, level)`, vol_write.c:480),
  so identical 64-KiB plaintext yields identical stored bytes.
- **Dedupe works**: on this corpus both copies of the image share one recipe
  (blake3) and one pba. Ceiling 73.8% on 4 copies, 1.88x on a near-duplicate.
- **The TEXT zone is not used in the leaf layout** (members go into the blob,
  the blob is compressed per segment); batching remains for ordinary small files.
- **sweep does not call fold**: `vol_fold_request()` is called only from FUSE at
  mount time (src/cli/fuse_fs.c:4655) and from tests, although the comment in
  `volume.h:742` claims "This is what WP-M18's sweep calls". It does not
  affect this bug (the delta was 1068 records), but the discrepancy is worth fixing.
- **`meta 64.1 MiB` in `used`** is the size of the reserved meta-zone, not data.

---

# The `RAW`/`SHADOW` Labels in `invf-stats -v` Were Read as Zones (fixed)

**Symptom of the confusion:** after a sweep, the output says `RAW: used 12.8 MiB`,
from which it naturally follows that "the sweep did not compress enough, RAW did not empty".

**Measurement by address on the same volume** (the `INVFS_DEBUG_UNCLAIMED=1` diagnostic,
and now also the permanent `by address` line in `invf-stats`):

```
zones            : meta 80.1 MiB | raw 153185 blk | shadow 612742 blk
by address       : raw-region 0.0 MiB | shadow-region 114.7 MiB occupied
content classes  : uncompressed 12.8 MiB (0.98x) | compressed 97.8 MiB (13.29x)
```

**The RAW zone is indeed empty: 0 blocks.** These 12.8 MiB are a CONTENT CLASS:
segments stored WITHOUT compression because it does not pay off. They physically
lie in the shadow region, like everything else.

Who they are: container members left as separate inodes (`!mbr<idx>-<san>`),
marked `GENERIC_GUARD{algo=gzr(9)}` -- GZR tried and gave up, and
per-segment compression measured the gain and found none. Checked on three members:
these are gzip files (`1f 8b 08`), 516 580 -> zstd 516 605 (1.00x), 443 053 -> 385 408
(1.15x), 657 073 -> 657 104 (1.00x). So the decision to "store as is"
is correct; on the 1.15x member the loss is ~63 KB, and it is visible in the dump (some
of its segments are compressed, some are not -- the decision is per-segment).

`used = meta(zone size) + occupied data blocks`: 80.1 + 114.7 = 194.8 MiB.
For the "two copies of a 256 MiB image" corpus: 64.1 + 28.2 = 92.3 MiB.
