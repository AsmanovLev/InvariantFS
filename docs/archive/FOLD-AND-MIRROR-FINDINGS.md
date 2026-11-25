# Findings: why the stand “hung” on mount

Date: night of 15.11.2026. Stand `192.168.0.2` (RAW `/dev/sda2` 110.7 GB, shadow `/dev/sdb1` 1.8 TB),
delta ~3.0M records / ~1.5 GB, 322 889 files.

Symptom: mounting did not fit within 1200 s, `init` dropped to a rescue shell;
meanwhile the `invf-fuse` process was burning 100% CPU without a single write.

## 1. `fold_sort()` was O(n²) — this was the “eternal” fold

`src/core/vol_fold.c` sorted the delta snapshot by **insertion**:

```c
/* Stable insertion sort of the snapshot into key order (the list is small…) */
for (a = 1; a < l->n; a++) { ... while (b > 0 && vol_key_cmp(...) < 0) { ... b--; } }
```

The comment “the list is small” was true for benchmarks on 4k/20k files and
catastrophically wrong for a real volume:

| records | operations (≈n²/4) | at ~6.7·10⁷ ops/s | in our measurements |
|---|---|---|---|
| 20 000 | 1.0·10⁸ | ~1.5 s | 5.98 s (measured, bulk path) |
| 300 000 | 2.2·10¹⁰ | ~6 min | — |
| 1 787 628 | 8·10¹¹ | **~3.5 hours** | hung 30+ min and made no progress |
| 3 021 686 | 2.3·10¹² | **~9.5 hours** | — |

Meanwhile the log is silent: the `fold: bulk path, N record(s)` message is printed only
AFTER sorting, so the last line in `/tmp/fuse.log` remained
`volume open complete`, while the process spun in CPU with no I/O.

**Fix** (`vol_fold.c`): `qsort()` with a comparator on the key and a tie-breaker
by record number (determinism for equal keys). 1.7M → tens of milliseconds.
`invf-fold_test scenario` → 38/38 PASS.

## 2. The “mount fold: N -> N” message lies

`vol_fold_request()` returns 0 in three different cases: (a) the delta is empty,
(b) **triggers not reached** (`< 262144` records and `< 64 MiB`), (c) a fold
is already running (trylock). And `fuse_fs.c` prints `mount fold: delta records N -> N`
in all three — so it looks like a successful fold that did nothing.

It would be correct to distinguish these cases; for now the document records that
`N -> N` means “fold was not needed or could not take the gate”, while a real
fold is visible as `N -> 0` (or via the `fold: bulk path` line).

## 3. Shadow resync copies the ENTIRE metadata zone — 32 GB

```c
v->st.meta_end_bytes = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks) * BLOCK_SIZE;
/* mirror_resync(): uint64_t off = 0, end = v->st.meta_end_bytes; */
```

On the stand this is **32 140 627 968 bytes** — a third of the volume. Any
`DEVT sync_seq` mismatch (and it occurs after every unclean shutdown) forces
the next flush to rewrite these 32 GB to the 1.8 TB drive, page by page.
Measured: the resync did not finish within 30 minutes, and all that time the
fold had not even started — it was waiting behind `vmux_barrier`.

Practical options (discussed, not implemented):
1. copy not the whole span but only the used part (high-water mark);
2. compare pages byte-by-byte and write only the differing ones;
3. keep a dirty range in DEVT and resync that;
4. as a last resort — mount without the shadow (do not set `INVFS_DEV1`) and
   treat the shadow as a cold spare, resyncing it as a separate offline operation.

## 4. Side effect: `init` in initramfs waited 1200 s for a dead process

Before:

```sh
invf-fuse ... &
FUSE_PID=$?          # always 0, not the pid!
while [ "$i" -lt 1200 ]; do
    grep -q "InvariantFS mounted" /tmp/fuse.log && break
    sleep 1; i=$((i+1))
done                 # child liveness was not checked
```

After (in the installed image): `FUSE_PID=$!` and the condition
`while [ "$i" -lt 1200 ] && [ -d /proc/$FUSE_PID ]`.

## What was installed on the stand

`EFI/Gentoo/initramfs.img` = 6 118 085 B, md5 `ee04b672…`, backup
`initramfs.img.bak-qsortfix`. Inside: `invf-fuse` with the qsort fix
(md5 `f2425d3c…`), a patched `init`, `INVF_FOLD_CAP=0`.

## Morning checklist (what to do when stand access is available)

1. **Reconnect the ST-Link** (it is currently physically disconnected: `OpenOCD did not start`).
   Then run `python3 recover.py` from `/mnt/d/Users/User/invfs_deploy` — it will bring up
   the daemon, check input (if the STM is “stuck”, it will do `reset halt`/`reset run`
   over SWD), read the screen, check the ESP bridge and see whether the ESP is visible
   as `/dev/ttyUSB*` on the stand.
2. **Boot from disk.** In BIOS the current setting is `#1 = ADATA USB` (flash drive).
   Either remove the flash drive or swap slots #1↔#5 (done via the popup: select ADATA
   in the slot where the Hard Disk sits — AMI swaps the slots and **only then**
   saves via F10 → Yes).
3. **The first mount with the fix** is expected to be long but finite:
   replay ~1.5 GB of delta (~80 s) → bulk fold of 3.0M records (seconds with qsort)
   → flush → 32 GB shadow resync (tens of minutes). This is NORMAL and happens
   only once: afterwards `DEVT` is aligned, the delta is empty, and mounts take seconds.
4. **Verify the result** via `/tmp/fuse.log` on the stand: it should contain
   `fold: bulk path, N record(s)` and `mount fold: delta records N -> 0`
   (not `N -> N` — that means “nothing was done”).
5. **Only after that** — filesystem work: `invf-verify --deep`,
   the `parts` counter in the inode row, the orphan collector in sweep.

## Preparations made overnight

- `esp_bridge/` — C firmware for a UART0↔TCP bridge (ESP-IDF). Build: `idf.py build`.
  It can only be flashed when the ESP is back on the laptop (`COM16`).
- `bench2.py`, `time_mount.py` — local benchmarks for measuring the fold.
- `bin/invf-fuse-old` — a build with the old insertion sort, for comparison on the same delta.
- `recover.py` — restoration of the stand's control channel.

## 5. The next fold bottleneck: one pread per record

`fold_collect_cb()` calls `vol_delta_read_value()` for every record, and that
issues **one `io_pread` per record** (no cache: `delta_read_value_locked`,
the “one pread” branch). For the stand's delta this is 3 021 686 preads across
1.5 GB segments, and in KEY order rather than segment order — i.e. random
reads.

Estimate for the stand (sda2 is an SSD, p50 0.13 ms from our measurements):

```
3 000 000 × 0.13 ms ≈ 390 s ≈ 6.5 min   (value collection only)
```

Plus delta replay ~80 s, sorting (fractions of a second after the fix), writing
the new base and the 32 GB shadow resync. So the “first mount after accruing debt”
takes tens of minutes, and that is normal; what is not normal is that the
per-record reads can be almost entirely eliminated.

**Proposal:** two-pass collection. First pass — walk the index and collect
`delta_ref`s (without values). Second — traverse the **segments sequentially**
(they are append-ordered) and place the values at the references. Then the 3M
random preads become one sequential 1.5 GB scan: seconds instead of minutes,
and no random-I/O load.

This is exactly the same technique already applied in `WP-BATCH-FOLD` for the base tree
(streaming scan instead of COW upserts) — just to the other side of the merge.

## Sorting fix verification (local)

```
./bin/invf-fold_test scenario ~/ft2.img
  scenario: 38 checks, 0 failure(s)   SCENARIO PASS

./bin/invf-fold_test crash-setup ~/ft3.img && ./bin/invf-fold_test crash-verify ~/ft3.img
  crash verify: 5 checks, 0 failure(s)   CRASH VERIFY PASS

INVFS_FOLD_BULK=1 INVFS_FOLD_ABORT_AT=published ./bin/invf-fold_test crash-setup ~/ft4.img
  fold: bulk path, 3 record(s)          (aborted exactly in the publish/reset window)
INVFS_FOLD_BULK=1 ./bin/invf-fold_test crash-verify ~/ft4.img
  crash verify: 5 checks, 0 failure(s)   CRASH VERIFY PASS
```

Order matters: `crash-verify` is NOT a standalone test — it checks the state
left by `crash-setup` (otherwise it gives a false FAIL on a fresh volume).

## Direct sorting measurement (isolated, without the filesystem)

`/tmp/sortbench.c` — 200 000 random 8-byte keys:

```
N=200000
  insertion sort (as before): 66.47 s
  qsort           (as now):   0.030 s
  speedup:                    2227x
```

Quadratic extrapolation to the stand's delta (3 021 686 records):

```
insertion: 66.47 s × (3.02e6 / 2e5)² ≈ 15 000 s ≈ 4.2 hours
qsort:                                   ≈ 0.5 s
```

This is the answer to “why the fold hung”: it wasn't hung, it was honestly
sorting — for about five hours.

## Other measured numbers (local, ext4, FUSE)

| what | value |
|---|---|
| creating tiny files (1 byte) | **207 files/s** (single-threaded daemon) |
| space per 1-byte file | **12.7 KB** (data page + inode row + recipe) |
| 150k files of 8 bytes | **3.7 GB** on a 4 GB volume → fold failed due to lack of space |

12.7 KB per one-byte file is the root of the “small files” problem: 500k files
from a web sync give ~6.4 GB of overhead, and the metadata zone on the stand
grew to 32 GB. An optimization candidate: **inline data in the inode row / recipe** for
files smaller than a page.

At the same time `fold_space_ok()` refuses **silently** — there is not a word about
the reason in the log (we only saw it from `du` of the image). This is worth fixing
together with the fold messages.

## Measurement on 150k files (sustained rate)

```
created 150000 files in 2525.6 s   (59 files/s under continuous operation)
MOUNT WITH FOLD: 14.37 s
invf: mount fold declined or failed; the delta stays at 300000 records
InvariantFS mounted: 150000 files
```

Three conclusions:

1. **Exactly 2 delta records per file** (300 000 for 150 000) — as expected.
2. **The fold did not work not because of the code, but because of space**: 4 GB volume, 3.7 GB used,
   `fold_space_ok()` refused. And it refused **silently** — no reason in the log.
   A proper measurement needs a volume with headroom (≥10 GB for 150k files).
3. The sustained creation rate is **59 files/s** (vs 207 files/s in a short
   burst of 1000 files): the daemon is single-threaded, and under continuous
   writes the throughput drops threefold.

## 6. Why the “fold did not work” even though it did: return codes

`vol_fold_request()` returns **three different values**:

| code | meaning |
|---|---|
| `1` | fold **performed** |
| `0` | nothing to do (below trigger or gate busy) |
| `-1` | failure |

But `fuse_fs.c` had `vol_fold_request(g_vol) == 0` → **a successful fold
landed in the “declined or failed” branch**, while a no-op printed “delta records N -> N”.
Hence a whole night of being sure that the fold was not starting: it did start and
folded the delta, while we read “declined or failed”.

Verification after the fix (15 000 files -> 30 000 delta records):

```
mount with fold: 4.17 s
invf: mount fold: delta records 30000 -> 0
InvariantFS mounted: 15000 files
```

That is, **the fold works**: the delta folded down to zero, mount 4.17 s.

Failure-stage tracing was also added (previously `fold_space_ok`,
base-root reading, delta collection and publication were silent): now on failure the log
has a line `fold: FAILED at <stage>`.

## 7. Required on the stand: enable the bulk path

By default the fold uses the **incremental** path (one record at a time via
`btree_upsert`), while bulk is enabled by `INVFS_FOLD_BULK=1`. For the stand's
3M-record delta the incremental path means hours (exactly what
`WP-BATCH-FOLD` was created for).

In the stand's `init`, add next to the existing lines:

```sh
export INVFS_FOLD_BULK=1
export INVF_FOLD_CAP=0
export INVF_FOLD_CAP_MB=0
```

Without this, the first mount after accruing debt will take the slow path.

## 8. Bulk path: verified and faster

Same delta (15 000 files -> 30 000 records), two paths:

| path | mount with fold | log |
|---|---|---|
| incremental (default) | **4.17 s** | `mount fold: delta records 30000 -> 0` |
| bulk (`INVFS_FOLD_BULK=1`) | **0.76 s** | `fold: bulk path, 30000 record(s)` + `delta 30000 -> 0` |

Bulk is ~5x faster already at 30k and grows linearly, whereas the incremental path
does a COW upsert per record. For the stand's delta (3 021 686 records) this is
decisive: the linear path gives on the order of a minute, the quadratic one — hours.

The night's result on the fold: **all three defects were found and fixed, both fold
paths were verified on real files, and the delta folds down to zero.**

## 9. Where 12.7 KB per 1-byte file comes from (breakdown)

Measured: 1000 files of 1 byte each added 13.2 MB, i.e. **12.7 KB/file**.
Breakdown:

| what | size | why |
|---|---|---|
| data page in RAW | 4 KB | minimal unit of a segment |
| recipe blob (RMC1) | 4 KB | 3072 B chunk + header -> its own page; **not inlined by design** (`invarifs.h`: “recipe ... not inlined, so the row stays small and cacheable”) |
| inode + dirent rows in the tree | ~4 KB | COW pages of the metadata tree on insert |
| **total** | **~12.7 KB** | file of useful size 1 byte |

The same factors grow: the 32 GB metadata zone on the stand, the 3M-record delta,
and the 32 GB shadow resync.

**Proposal (format, do when we decide to break it):** “tiny file” —
if the data is ≤ ~1.5 KB, store it **directly in the inode row** as a separate
row variant, with no RAW page and no recipe blob. Then a 1-byte file stops
costing 12.7 KB and costs ~a fraction of a tree page. For a 500k-small-file
web sync this is the difference between ~6.4 GB of overhead and ~0.5 GB.

The threshold and flag must be chosen so old volumes are readable without migration:
the new row variant is a new value in the existing `type`/`flags`,
not a format version change.

## 10. Bulk fold scaling (measured)

| delta | mount time with fold | log |
|---|---|---|
| 30 000 records | 0.76 s | `fold: bulk path, 30000 record(s)` |
| 200 000 records | **6.43 s** | `fold: bulk path, 200000 record(s)` |

200k files took 1584 s to create (63 files/s) — that is the separate cost of the write path.

Growth: 6.7x records -> 8.5x time, i.e. **linear** plus a constant
component (delta replay). Extrapolation to the stand's delta (3 021 686):

```
6.43 s × (3.02e6 / 2e5) ≈ 97 s
```

Even allowing for the stand's slower disks this is a matter of minutes —
and certainly not hours. So **the fold is no longer the bottleneck**.

## 11. What the bottleneck is now on the stand

Not the fold, but the **shadow resync: 32 GB** (the entire metadata zone) on the
1.8 TB HDD on every `DEVT` mismatch. Order of magnitude: 32 GB in small writes
on an HDD means tens of minutes, which does not fit in the 1200 s `init` budget.

Hence the morning recommendation: clear the debt **offline from a LiveCD**, where
there is no time budget:

```sh
INVFS_DEV1=/dev/sdb1 INVFS_FOLD_BULK=1 /tmp/invf-fsck -f /dev/sda2
```

(one command does both the 32 GB resync and the fold of 3M records — and we see progress).

## 12. Breaking down 12.7 KB: we pay for writing data, not for its size (measured)

The same test (800 files) with three content variants:

| what we create | speed | increment per file |
|---|---|---|
| **empty file** (`open`+`close`) | **1874 files/s** | **0.3 KB** |
| 1-byte file | 86 files/s | 12.7 KB |
| 4096-byte file | 190 files/s | 12.7 KB |

Conclusions that follow from this:

1. **An empty file is almost free**: 0.3 KB and 0.5 ms — that is the inode row and dirent
   in the metadata tree, with no separate pages.
2. **Everything else is the price of the FIRST WRITE**, and it is fixed: 1 byte and
   4096 bytes cost the same, 12.7 KB. So we pay not for the data volume,
   but for a RAW page (4 KB) plus the RMC1 recipe blob (4 KB, “not inlined” by
   design) plus the accompanying COW tree pages.
3. **Speed drops 22x**: 1874 -> 86 files/s as soon as the file
   has content. For a 500k-small-file web sync this is
   decisive: ~6.4 GB of overhead and hours of work.

**What this means for optimization.** The naive “inline data for small files”
needs to be refined: the gain comes not from “inline instead of a page”, but from
**eliminating the separate recipe blob and the separate RAW page for files whose data
fits in one page**. This is exactly the case where the recipe is the constant
“one extent”, and storing 4 KB of AST for one byte of data is pointless.

Expected effect for 500k files: 12.7 KB -> ~0.5-1 KB, i.e. ~6.4 GB ->
~0.3-0.5 GB, and creation speed on the order of empty files.

## 13. Small-file optimization plan (concretely, in code)

Facts from the code:

- the inode row is 114 bytes (`invfs_inode_row`), containing `invfs_blkptr recipe` and
  `recipe_addr[INVFS_RECIPE_ADDR_LEN=32]`;
- the recipe is a **content-addressed blob** in the same B+-tree under the key
  `0x04 || blake3(blob)[32]` (`vol_recipe_store`, `invarifs.h` §WP-M8);
- recipe deduplication does not work for our case: the recipe contains the PBA of
  the data page, which is different for every file -> every file has its own tree
  row plus its own blob;
- there is **no** compact “recipe = one extent” form in the format.

Hence two stages, both without a format version change:

**Stage 1 (the main gain): trivial recipe without a blob.**
If the file is exactly one extent (length ≤ a page), write into the row
`recipe` = pba+len and `recipe_addr` = a sentinel zero, distinguished by a
flag (a free bit in `type`/`row_version`). Reader: if
`recipe_addr` is the sentinel, the recipe is synthesized from `recipe` without a
trip to the tree. Savings: the recipe row in the tree (~4 KB COW page) + the blob
(~4 KB) -> the file costs ~4.3 KB instead of 12.7 KB.

**Stage 2 (for very small files): data in the row.**
Add a row variant with a data field of up to ~1.5 KB: then there is neither a
recipe blob nor a RAW page -> ~0.5 KB per file (measured: empty file 0.3 KB).

Expected effect on 500k web-sync files:

| variant | per file | total | creation speed |
|---|---|---|---|
| now | 12.7 KB | ~6.4 GB | 86 files/s |
| stage 1 | ~4.3 KB | ~2.2 GB | ~250 files/s (estimate) |
| stage 2 | ~0.5 KB | ~0.25 GB | ~1800 files/s (like empty) |

Side effects: fewer rows in the tree -> smaller delta -> cheaper fold;
smaller metadata zone -> cheaper shadow resync (those same 32 GB).

---

## 21. Metadata anatomy: a 30 GiB zone of which 108 MB is needed

Found on 2026-11-15 while investigating “why the stand has 19.56 GiB of
metadata occupied with 5.9 GB of data”.

### How it was calculated

`invf-mkfs`, `src/cli/mkfs.c`:

```c
dev0_blocks = size_bytes / INVFS_BLOCK_SIZE;
dev1_blocks = twodev ? size2_bytes / INVFS_BLOCK_SIZE : 0;
total_blocks = dev0_blocks + dev1_blocks;        /* 110 GB + 1.8 TB */
bitmap_blocks = div_ceil(total_blocks / 8, INVFS_BLOCK_SIZE);
inode_blocks  = total_blocks / frac;             /* frac = 64 */
metadata_blocks = bitmap_blocks + INVFS_JOURNAL_BLOCKS + inode_blocks;
```

The inode area was calculated from `total_blocks`, but inode rows live **only on
dev0**; the second device holds their mirror, and its 1.8 TB has nothing to do with
the number of inodes that fit.

### Measurements (meta_anatomy.py: popcount of the allocation bitmap, read-only)

| volume | metadata zone | preallocated at mkfs |
|---|---|---|
| fresh 4 GB, 1 device | 24 608 blocks (100 MB) | 100 % |
| fresh 8 GB, 1 device | 41 024 blocks (168 MB) | 100 % |
| fresh 110 GB, 1 device | 459 632 blocks (1.80 GiB) | 100 % |
| **fresh 110 GB + 1.8 TB, 2 devices** | **7 846 833 blocks (29.9 GiB)** | — |
| **stand** | **29.9 GiB** | **19.56 GiB (65 %)** |

For 322 888 files we need 322 888 × ~336 B ≈ **108 MB** of inode rows, i.e.
0.35 % of the reserve. The rest is a reserve for ~91 million files that do not exist.

The zone was eating 27 % of the SSD: RAW was 80 GiB instead of 108. GiB.

### Fix

- `inode_blocks` is now calculated from **`dev0_blocks`** (where the inodes
  actually live), not from the sum including the mirror;
- the `INVFS_META_FRAC` cap raised from 256 to 65536 (256 was the *minimum* fraction and
  still requested 7.5 GiB on the mirrored volume);
- added `INVFS_META_FILES=<N>` — size from the expected number of files:
  `N × 1024 B` (336 B row + tombstone on overwrite), rounded up to
  blocks. For rootfs this is more honest than deriving it from the volume size.

After the fix on the “110 GB + 1.8 TB” volume:

| | was | became |
|---|---|---|
| metadata zone | 29.9 GiB | **1.85 GiB** |
| preallocated | 19.56 GiB | **60 MB** (bitmap) |
| RAW | 80 GiB | **108.2 GiB** |

`INVFS_META_FILES=500000` on 110 GB gives 523 MB, `INVFS_META_FRAC=1024` —
145 MB.

Side effect: the resync boundary (the highest allocated block) is no longer
inflated by preallocation — on the stand it was 19.56 GiB.

**This does not fix an existing volume** — the zone size is written into the superblock at
mkfs time. It can only be fixed with a new `mkfs` (the format is not frozen).

---

## 22. `lazy_reclaim=1` (default): deleting files does not return space

Measured on 2-device and 1-device volumes, 2026-11-15.

The default in `volume.c`:

```c
v->lazy_reclaim = 1;  /* default: lazy_reclaim=1 (instant mount) */
```

| experiment | created | deleted all | after 90 s idle |
|---|---|---|---|
| `lazy_reclaim=1` (default), 50 000 files | raw 50 000 blocks | 50 000 | not measured |
| `lazy_reclaim=1` (default), 5 000 files | raw 5 000 blocks | **5 000** | **5 000** |
| `lazy_reclaim=0`, 5 000 files | raw 5 000 blocks | **29** | — |

That is, with the default, **only sweep** frees blocks (the orphan purge
inside it). The data volume grows monotonically: on the stand this is
gigabytes accumulated over months of churn.

Practical conclusion: for a long-lived volume, either `INVFS_LAZY_RECLAIM=0`
(price: `pba_ref_ensure` on open, 1.26 s on the stand) or a regular
sweep. Otherwise RAW (80 GiB) gets filled with dead blocks while the volume of
live data is small.

Open question: why with `lazy_reclaim=1` the background `vol_pba_warmup` does not
free blocks within 90 s of idle time. Either warmup only builds the reference map, and
freeing itself is sweep's prerogative (then this is by design and should be documented), or
there is a bug there.
