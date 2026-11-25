# Open Issues and Measured Problems

> **Former headline bug, now CLOSED (`48404b6`): `impl_docs/BUG-RECLAIM-UNCLAIMED.md`.**
> `unclaimed` blocks were not returned: 18.5 → 2.6 MiB right after a write and
> 25.8 → 1.4 MiB at the sweep plateau, with reads still bit-exact. Reclaim was
> NOT the remaining gap against squashfs — that one was block rounding of
> sub-block segments and is fixed too (`impl_docs/BENCH-VS-SQUASHFS.md`, §FIX):
> 22.88 → 20.53 MiB of data on the same corpus, i.e. +2.4% at an equal pooling
> unit (21.5 vs 21 MiB) instead of +13%.

**This is not a release plan.** The contents of v0.7 are not known in advance —
many changes are expected, and there is no point in writing down "what will be
in it": half of it will change by the time it is released. `CHANGELOG.md` is
written after the fact, on the way out.

The document serves a different purpose: **not to lose measurements**. It
contains what was found and verified on real data (stand, 322k files, 3M delta
records, distribution images), with numbers and the exact cause. Each item is
a work candidate, not a promise.

The format policy does not change: break things quietly, do not write
migration code (see `CONCEPTS.md` §9).

---

## 1. What has already been done (included in v0.7 from the 2026-11-15/16 work)

| # | commit | summary |
|---|---|---|
| 1 | `7376c3b` | fold: `qsort` instead of insertions (O(n²)→O(n log n)); honest messages by `vol_fold_request` return code; failure tracing |
| 2 | `32044ed` | mirror: barrier after writing `DEVT` (the certificate became durable); `vol_write_rdp0` writes block 0 through the mutex with CRC recalculation |
| 3 | `d5359a6` | mirror: resync limited to the occupied part (bitmap) + comparison before writing (1 MB written instead of 32 GB) |
| 4 | `ec83899` | mirror: `DEVT` is written directly to each device + `devt: MISMATCH` self-check |
| 5 | `cbe4856` | mount: traversal does not re-resolve the path (106 s → 21 s on 322k files) |
| 6 | `1acae70` | mkfs: inode-area from `dev0_blocks` (30 GiB → 1.85 GiB), `INVFS_META_FILES`, `INVFS_META_FRAC` clamp raised |
| 7 | `3fd9ff0` | sweep `--no-compress`: dedupe without re-encoding; does not kill container decomposition |
| 8 | `3c76be1` | `INVFS_RAW_NO_COMPRESS`; `invfs_profile_zstd_level(TURBO)` = 1 (was 19) |
| 9 | `d333187`,`7ede024`,`6095578` | docs: `CONCEPTS.md`, `INVARIANTS.md`, sweep decisions; fixed two lying comments |

Verified on the stand: two consecutive boots without `dev1 is stale`, `sync_seq`
equal, no `MISMATCH`, boot 96 s (45 of them the GRUB timeout), mount ~23 s.

### 1a. 2026-11-21/22: the sweep stopped being quadratic (also in v0.7)

| # | commit | summary |
|---|---|---|
| 10 | `61c628a` | `vol_delta_range()` returned `10, 30, 20` after appending `20` to a sorted view whose tail contributed keys — `presorted` stayed set, and `vol_dirent_scan()` merges that range into the base as ONE ordered stream. Fixed + regression in `tools/delta_test.c` |
| 11 | `82c9f61` | sweep resolved each candidate's name with `vol_name_of()` — a whole reverse walk of the dirent tree per file. One id→path snapshot per run, used as a HINT: it is taken only after `vol_find` proves the name still names the same inode, so a rename falls back to the live walk. 7.06 ms → 0.001 ms per candidate, 9.94 ms build per pass |
| 12 | `029b5c9` | the snapshot is filled by the collect walk itself instead of a second namespace walk (collect 7.42 ms, collect+named 7.34, collect+walk 14.89) |
| 13 | `5fcf8dc` | `PERF=1` counters: reverse name walks, snapshots, recipe publishes, `pba_ref` rebuilds and their time, worst per-file hold under `g_io_lock` |
| 14 | `fd22355` | `pba_ref_ensure` was called before every swept file's remap; each publish invalidates the map, so that was one namespace walk plus one recipe load per live inode per file — O(N²), 16 ms at 2 000 files and 1.26 s on the 66k-record stand volume, all inside `g_io_lock`. The drain now rebuilds once, for recipe-form entries too, which also closes a trailing leak (the last file's rebuild was always one publish stale) |
| 15 | `8e6ea9c` | a pba-form retire entry already carries the superseded set, so the drain replays one −1 per old occurrence instead of resetting and walking. `pba_ref_rebuild` fault seam + the `replay` phase of `pbaref_test` prove it: with the rebuild armed to fail, the retired block is free and the file byte-exact |

Green at `8e6ea9c` (re-run, not quoted): `sweep_collect` 23/23,
`walk_status_fuse` 36/36 (the armed `vol_name_of` seam is never reached),
`pbaref_test replay` 8/8, `delta_test` 183/183, `no_ckp0_surface_test` PASS,
`dedupe_test` 16/16.

---

## 2. Format changes (breaking, all in one bump)

### 2.1 `DEVT` v2

- `meta_high_water` — upper bound for resync without relying on the bitmap;
- `shadow_gen` — generation of the shadow snapshot (for checkpoint mode);
- `INVFS_DEVT_VERSION` = 2. Old volumes are read as "no valid DEVT" →
  one full resync on first open (acceptable).

### 2.2 Container member layout: `!mbrblob`

Currently: one inode per member (`!mbrNNNN`) — 21,356 members of the Debian
image produce 21,356 entries in the name index, with the limit
`CPACK_MAX_MEMBERS = 1 048 576`.

Plan: when the member count is above the threshold (`INVFS_CPACK_PACKED_MIN`,
default 512), store the members **as a single blob**:

```
<name>            recipe (BINARY, pack algo)
<name>!mbrblob    member data back-to-back
<name>!mbrt       table (idx, name, usize)
<name>!mbrmap     map (seekable packs only)
```

Offsets are computed from the table (cumulative by `idx`), so the blob can be
written with `pwrite` in any order, and batch slices need not be consecutive.

Reading — in phases:
- **1** — the filesystem materializes the members from the blob into a
  temporary directory and calls `rebuild` (correct, but N temp files per read);
- **2** — new pack command `rebuild_batch <recipe> <blob> <table> <out>`,
  reading in one call without temp files.

### 2.3 Trivial recipe and inline data (small files)

- step 1: single extent → `recipe` = (pba, len), `recipe_addr` = sentinel
  zero + flag; saving ~8.4 KB out of 12.7 KB and almost all the CPU;
- step 2: data ≤ ~1.5 KB inside the inode row (as with symlinks,
  `INVFS_META_SLACK` already reserves space); target ~0.5 KB/file.

### 2.4 Member count policy

- `CPACK_MAX_MEMBERS` = 1M nominally, but in cost terms (4–12 KB of metadata
  per small member) it is inadequate: introduce a cap (e.g. 100k) with an
  explicit refusal;
- batch slices are cut **both by bytes and by count** (256 MiB **or** 4096
  members);
- each slice gets its own subdirectory `<mbr>/b<N>/` so that the scratch
  directory never holds more than 4096 entries.

---

## 3. Correctness

| # | item | status |
|---|---|---|
| 3.1 | Invariants from `INVARIANTS.md` — turn into tests (first of all I1 `DEVT` MISMATCH, I4 block 0) | plan |
| 3.2 | Clean shutdown: `remount,ro` + flush/close so that `DEVT` cannot diverge at all | plan |
| 3.3 | Sweep recursion in a single run (collect→transform loop with depth limit) | plan |
| 3.4 | Space reclaimed in **one** sweep: SPT0 holds old blocks until the next run | plan |
| 3.5 | `lazy_reclaim=1` by default = deletions do not reclaim space; either auto-reclaim or a loud warning | decision |
| 3.6 | `no_ckp0_surface_test` (shard 4) — pre-existing failure; fix or document | debt |

---

## 4. Performance (measured → target)

| metric | now | v0.7 target |
|---|---|---|
| mount, `build_file_table` (322k files) | 21 s | ≤ 8 s (parallel traversal) |
| `vol_open` (delta 0) | 1.5 s | 1.5 s |
| fold of 3M records (bulk) | minutes | minutes (no regression) |
| resync on divergence | 1 MB written, 21 GB read | high-water in `DEVT` → ~2 GB read |
| small file | 12.7 KB, ~16 ms | 4.3 KB (step 1), 0.5 KB (step 2) |
| `fold_collect_cb` | pread per record | two passes: collect references, then sequential scan |
| container lane | 21,356 members = 62 min; RSS 5.4 GB on a 3 GB partition | `extract_batch` (≈2–4 min) + streaming read instead of whole-file |
| boot to ssh | 96 s (45 — GRUB timeout) | ~55 s (5 s timeout) |

---

## 5. Operations and stand

1. GRUB timeout 3 → 5 s (had been raised to 45 while fighting boot order).
2. `INVFS_TOOL_SCRATCH` — only on a filesystem with Landlock support (not
   `/mnt/*`, not tmpfs on a small partition): otherwise `extract` gets EACCES
   and fails **silently** (helper stderr goes to `/dev/null` by default).
3. `INVFS_LAZY_RECLAIM=0` on the stand (the cost is `pba_ref_ensure` 1.26 s on
   open).
4. Clean reinstall onto a new volume with the fixed `mkfs` (metadata 1.85 GiB
   instead of 30, `INVFS_META_FILES` for the real file count).
5. `invf-verify --deep` as a mandatory acceptance step.

---

## 6. Work candidates (no commitments or deadlines)

Format: `DEVT` v2 (`meta_high_water`, `shadow_gen`) — one bump for all breaking
changes; `!mbrblob` member layout above the threshold; trivial recipe for
single-extent files; inline data ≤1.5 KB; member count and batch slicing policy.

Correctness: invariants as tests; clean shutdown; sweep recursion in one run;
space reclaimed in one sweep; `lazy_reclaim` policy.

Performance: parallel name traversal; two-pass `fold_collect_cb`;
`extract_batch` in packs; streaming reads of containers instead of whole-file.

Operations: GRUB timeout 5 s; `INVFS_TOOL_SCRATCH` only on a Landlock filesystem;
`INVFS_LAZY_RECLAIM=0` on the stand; reinstall onto a new volume.

## Found on 2026-11-16 (late evening)

### Tests and container packs cannot be run from `/mnt/d` (drvfs) — Landlock

Any pack whose executable lies on drvfs fails to start: the helper runs under
Landlock, and rules cannot be set on drvfs, so opening the pack script gets
`EACCES`:

```
python3: can't open file '.../splt_test.codecpack/splt.py': [Errno 13] Permission denied
[cpack] enumerate failed: ... (tool absent, or the container is not in a form it reads)
```

This is diagnosable only with `INVFS_HELPER_VERBOSE=1` — otherwise the helper's
stderr goes to `/dev/null` and it looks like "the pack cannot handle this
container". Same root cause as scratch on drvfs (writing), except here it is
reading.

**Rule:** runs with container packs — from a worktree on a native filesystem
(`/var/tmp/wt_*`), not from `S:\`/`D:\`.

### `tools/test-containerpack.sh`: pre-existing failure

`FAIL: corrupt map (guard): rollback left the volume dirty` — reproduces on
commit `3c76be1` (before the packed layout work) and on `bf54544` alike. Not
related to leaves/blob; investigate separately.

### Container recursion in the filesystem: measured result

`debian12.qcow2` (1.1 GB) in full, `--no-compress`, `INVFS_SWEEP_MAX_FILE=4G`:

```
sweep #1  debian12.qcow2: qcow2 (codecpack)            -> 4 files
sweep #2  ...!mbr0001-diskimg: rawdisk (codecpack)     -> 9 files
          3 085 959 168 B  ...!mbr0001-p0001   (guest ext4)
            130 023 424 B  ...!mbr0015-p0015   (EFI, fat32)
sweep #3  ...!mbr0001-p0001: ext4fs (codecpack)        -> 1 h 22 min
         ...!mbr0015-p0015: fatfs  (codecpack)
         in volume: guest files (gshadow, group, grub.cfg, index.db, ...)
         dedupe: 1819 segments, 57.0 MiB
         used 2233 MiB, invf-ls: 4096 file(s) INCOMPLETE
```

The cost: **21,356 inodes for one image** (4–12 KiB of metadata each) and
~175 ms per member (the pack re-parses the container on every `extract`;
`batch` is not declared by `ext4fs`).

### Space after decomposition does not add up: one corrupted segment

> **STATUS 2026-11-22: the space-growth item still cannot be judged, because the
> decomposition never runs at all with the pack in this tree -- and an earlier
> version of this block claimed the opposite. RETRACTED AND REPLACED.**
>
> What is actually measured (verbose pack debug on, `tools/codecpacks/ext4fs`,
> volume from `invf-mkfs`, one `invf-sweep`):
>
> ```
> 64 MiB ext4 + 2 real files:
>   size guard refused, decomposition declined (the decomposition is not a gain):
>   66751572 fixed + 303104 content + 3840 member-cost (3 members)
>   = 67058516 B vs 67108864 B original
> 2 GiB: fixed 2 146 759 698   vs 2 147 483 648 original  -> declined
> 8 GiB: fixed 8 587 637 779   vs 8 589 934 592 original  -> declined
> member rows in the namespace afterwards: 0
> ```
>
> `fixed` is, by the comment in `src/core/vol_cpack.c:3010`, "the recipe + the
> member table + the map, which ride verbatim" -- and it is essentially the WHOLE
> partition at every size. That is the item exactly as written below: the ext4fs
> plan enumerates all unallocated blocks verbatim, because a rebuild must
> reproduce the image bit-for-bit and free blocks may hold garbage. A plan like
> that can never be "a gain", so the lane declines before it builds anything.
>
> Why the earlier version of this block said "decomposed: yes": the check counted
> `grep -E 'ext4fs|rawdisk|-> [0-9]+ file'`, which matched the PACK'S OWN FILES
> (`.invariantfs/codecpacks/ext4fs*`) as they were swept, and the `159.40x` it
> quoted was the generic zstd lane recompressing the whole image as one stream
> (a nearly-empty ext4 is mostly zeros). Both were plausible numbers measuring
> the wrong thing. The zero member rows are what settles it.
>
> Consequences, so nobody re-derives them:
> 1. nothing below about space-after-decomposition can be reproduced until the
>    pack emits a SPARSE recipe (ranges + what to fill with, item 2 of the fix
>    list) -- that is `registry`, not this repo;
> 2. the recipe-extension machinery (chunked blobs above 3 800 B, the CHLD tree,
>    the 64 MiB cap) is irrelevant to this limit: the size guard runs BEFORE the
>    recipe is built, on the projected cost of the plan;
> 3. `invf-cat` of a file past ~4 GiB is OOM-killed on this box (no streaming
>    extract path) -- measured while trying to read the container back.
>
> The text below is kept as the record of the original 2.9 GiB observation.
### disables reclaim

Measured on a 2.9-GiB Debian partition (`ext4fs`, leaf layout, **with compression**):

```
before decomposition    : used  714 MiB   (image compressed as one stream: 5.09x on zeros)
after sweep #1         : used 1209 MiB
after sweep #2         : used 1447 MiB
after sweep #3, #4     : used 1451 MiB   (12 files in volume, swept=0)
```

The growth is explained not by the recipe or the leaves, but by the savepoint journal:

```
[spt0] save point: 1 of 9927 segments have an extent this walk could not VERIFY
       (a torn frame header, or a block of the derived run no longer allocated).
       Only their head block is pinned ... this capture does NOT reclaim --
       The previous save point's hold is carried one generation longer instead.
[spt0] save point: pinned 62743 blocks (9927 segments, 1 already damaged)
```

That is, **one** segment with an unverifiable extent (torn frame header)
makes the entire capture non-reclaiming: old blocks are not returned, and
every subsequent sweep adds new pinning (~245 MiB for 62,743 blocks).

What to do (in priority order):
1. find the source of the torn frame header (who writes the header
   incompletely, and when);
2. make release PER-SEGMENT: an unconfirmed segment holds only its head block
   (the policy already says so), while the other segments of the capture are
   released; currently "carry one generation longer" applies to everything;
3. only after that measure the space cost of decomposition.

Volume composition after decomposition (for reference): 12 files —
`p1.img` (recipe, 2.9 GiB logical / 0.31 GiB non-empty),
`!mbrblob` 604 MiB (leaves), `!mbrleaf` 329 KB (21,353 rows),
`!mbrmap` 1.2 MB, `!mbrt` 528 KB, plus 7 container members (e.g.
`core.img` — a real container, left as an inode).

### DIAGNOSIS: the "corrupted segment" is the container blob, not corruption

Found on a volume with decomposed `p1.img` (2.9 GiB, ext4fs, leaf layout):

```
[spt0-dmg] pba=343451 plen=0 rc=-1 inode=2 ent=0/1 algo=17 hdr=eabe6891d801829a
```

`inode 2` -- the container record `p1.img` itself. Nearby, by raw volume bytes:

```
pba=343450: 42504733 5e020000 ...   <- "BPG3", BLOB header starts here
pba=343451: eabe6891 d801829a 45345243 50303031   <- the record points here
```

That is, `recipe_addr` of the blob record addresses the blob payload (one block
past its header), while the savepoint traversal calls `seg_extent_checked()` at
that address, which expects a **segment frame header** (4B csize + 4B crc). A
blob has no such header and cannot have one -- it lies under the `BPG3` page.
The check fails, `broken++`, and under the "do not release on corruption"
policy the capture stops returning space; every subsequent sweep adds new
pinning (62,743 blocks = 245 MiB), and `used` grows: 714 -> 1209 -> 1447 ->
1451 MiB.

The data is INTACT: it is the check that breaks, not the content. Container
reads work (verified bit-for-bit separately).

What to fix: in the savepoint traversal, distinguish the kind of recipe. If the
record is a blob (recipe in the BLOB zone, `recipe_addr` addressing the payload
under `BPG3`), pin the blob's block range (header + payload by the length from
the header) instead of running a frame check over it. This also removes the
space growth on repeated sweeps.

## CRITICAL: container recipe larger than INVFS_RECIPE_STREAM_MAX -- data loss

> **STATUS 2026-11-22: no longer a data-loss path, and verified at both ends.**
> The store refuses (`vol_recipe_store`, `vol_btree.c:4341`) and the builder
> refuses (`vol_ast.c:107`); a leg pins the store side
> (`src/cli/recipe_tree_test.c`, group 7) and `large_file_test` pins the builder.
> At the OTHER end the lane declines before it wastes the work -- but read the
> previous section before treating that as a size limit: measured on 64 MiB,
> 2 GiB and 8 GiB, the in-tree pack's plan is `fixed` (verbatim) for essentially
> the whole partition, so the size guard declines the decomposition at EVERY
> size, not only at 8 GiB:
> `size guard refused, decomposition declined (the decomposition is not a gain):
> 66751572 fixed + ... = 67058516 B vs 67108864 B original`. Sweep rc=0, the
> container is untouched and bit-exact. So what remains is a CAPABILITY gap in
> the pack (a sparse recipe, `registry`), not a correctness defect here.

Reproduced on a clean volume (6 GiB) with a single file `p1.img` (Debian
partition 2.87 GiB), leaf layout, `--no-compress` NOT used:

```
before sweep: invf-cat vol.img p1.img   -> 3 085 959 168 B, bit-for-bit ok
sweep #1    : p1.img: ext4fs (codecpack)
after       : invf-cat                  -> 3 031 711 744 B  (lost 54 247 424 B)
             segment CRC mismatch: inode 2 seg 0 pba 343451 algo 17
```

That is, **one sweep makes the decomposed container unreadable**: the recipe
(2,439,560,938 B) loses a tail of ~54 MiB.

The reason is size: `INVFS_RECIPE_STREAM_MAX = 64 MiB` (invarifs.h:1507) --
the documented recipe limit, while the ext4fs recipe here is **2.27 GiB**, 36
times larger. The lane accepted it, wrote it, and reported success; the commit
guard even checked bit-for-bitness (reads THROUGH the map do not touch it), but
savepoint pinning/segment reads do touch it, and everything falls apart there.

Additionally: in the per-inode layout this same run **kills the machine with
OOM** (12 GiB RAM): each member is created via vol_create_file() with its
payload in memory, plus `invf-cat` of the same container asks for 8.8 GiB
(anon-rss 5.8 GiB) -- the OOM killer killed both the sweep and the read.

### What to fix (in order)

> **Status at `8e6ea9c` — the DATA-LOSS half is guarded, the honest-message half
> is not.** `vol_recipe_store()` refuses `blen > INVFS_RECIPE_STREAM_MAX`
> before it touches anything (`src/core/vol_btree.c:4341`), and the builder
> refuses the same way (`vol_ast.c:107`). Since the store is the last gate
> before a recipe becomes reachable, an over-cap recipe can no longer be
> PUBLISHED: the sweep's publish fails and its rollback keeps the file. A leg
> pinning exactly that is in `src/cli/recipe_tree_test.c` (group 7). What is
> genuinely still missing is item 1 below: the LANE does not refuse early, so
> the failure arrives at publish time with the store's wording instead of
> "recipe too large for this volume format; refusing to decompose" before
> hours of decomposition, and the sweep's publish-failure rollback has no leg
> of its own (`INVFS_FAULT=recipe_store:1` is the seam; nothing arms it).

1. **The lane must refuse** if the pack recipe does not fit into
   `INVFS_RECIPE_STREAM_MAX` -- with a clear message ("recipe too large for
   this volume format; refusing to decompose"). Right now the store refuses
   late and rolls back instead of the lane refusing up front.
2. **The ext4fs recipe must be sparse**: it includes ALL unallocated blocks
   of the partition (2.27 GiB, of which 86.5% are zeros and the rest garbage),
   because
   rebuild must reproduce the image bit-for-bit. The recipe format should
   become "a set of ranges + what to fill with zeros", not "the whole
   partition minus members": this takes the size from 2.27 GiB down to
   ~0.31 GiB (real ext4 metadata).
3. Only after 1-2 return to space accounting and the reclaim policy
   (all-or-nothing in spt0 is a separate task, see above).

### Separately: why this was not caught earlier

`invf-verify` on such a volume says `state: CLEAN`: it does not check recipe
segment CRCs, and reads through the map do not touch the layout. So the only
reliable detector is `invf-cat` of the container AFTER the sweep.

### /tmp in this WSL -- tmpfs at 5.8 GiB, and it overflows silently

`/tmp` (and `/dev/shm`) are mounted as tmpfs, each at 50% of RAM. Writing beyond
the limit gives an ordinary `ENOSPC` in the middle of an operation, and tools
report it in different ways:

- `invf-cat ... > /tmp/x.img` prints `write failed: stdout` and leaves a
  TRUNCATED file -- this looked like "data loss after a sweep" and cost
  several hours of diagnosis (there was no real corruption);
- `cc` fails with `fatal error: error writing to /tmp/cc*.s: No space left on
  device` while `df /` shows 818 GiB free (the vhdx is large, tmpfs is not).

Rule: large artifacts (images, read output, measurement corpora) -- only in
`/dev/shm` with explicit `df -h /dev/shm` checks, or on a Windows disk; keep
`/tmp` empty. Before any measurement -- `df -h /tmp /dev/shm /`.

### OpenZL: measurement on our recipe -- no gain, it belongs in the pack, not the core

Built `github.com/meta/openzl` (BSD, C11/C++17, `make`, no folly/boost -- i.e.
embedding is actually possible, unlike DwarFS). CLI `zli` with profiles.

A 256 MiB sample of the same ext4fs recipe (2.27 GiB / 2.87 GiB partition):

| option | compression |
|---|---|
| zstd-19 | 26.24x |
| OpenZL i8 | 22.19x |
| OpenZL serial / lz | 21.12x |
| zstd-1 (currently in the filesystem) | 20.10x |
| OpenZL le-u32 | 18.69x |
| OpenZL le-u64 | 15.56x |

Without a format description OpenZL behaves like LZ and loses to zstd-19. Its
advantage lives in `custom_parsers/` (csv, parquet, numpy, onnx, gguf).

Recipe composition (why format-aware will not help here):
   zeros 1.97 GiB (86.5%), non-empty 0.31 GiB, our AST table 683 KB (0.03%).
That is, there is nothing to structure in the recipe: 0.31 GiB is the content
of UNALLOCATED blocks of the partition (leftovers from the image build), which
rebuild must reproduce bit-for-bit.

Conclusion: (1) keep zstd-1 in `vol_create_blob_file`; (2) OpenZL is a candidate
INSIDE the `ext4fs` PACK (structured storage of bitmaps/inode tables/journal:
2.27 GiB -> tens of MiB) and/or as a codec of the generic lane for structured
user data.

### Decision: structural recipe -- deferred; structure is not our concern

The structural recipe (the pack describes bitmaps/inode tables/the journal
semantically, free space = "zeros here") is removed from the agenda as **not our
responsibility**: VM managers and TRIM/discard are responsible for which blocks
are free. If the image comes with discard information (qcow2 discard, sparse
raw, `e2image`), "zeros here" comes FROM ABOVE, and the pack should not guess
ext4 semantics.

Deferred, but not forgotten: if a requirement appears to "shrink the image
without losing bit-for-bitness", this is done by changing the contract (exact
vs functional) and lives in the pack, not in the core. The measured estimate
was: 2.27 GiB of recipe -> units of MiB.

What follows from this practically and is already done/being done:
1. The recipe is compressed (zstd-1, 42x) -- so zeros no longer cost space.
2. `strip` does not have to materialize zeros: if the pack emits a recipe with
   "holes", the filesystem still collapses zero segments by dedupe -- no
   separate work is needed.
3. Priority -- the speed and correctness of packs (batch), not structural
   encoding.

### Granularity: measured what a 4 KiB block gives

The same corpus (Debian 12 / Fedora 41 / openSUSE 15.6, raw, zeros excluded):

| | 4 KiB | 64 KiB (now) |
|---|---|---|
| within Debian | 15.0 % | 10.8 % |
| within Fedora | 12.4 % | 9.2 % |
| within openSUSE | 4.6 % | 0.0 % |
| total | 12.7 % | 7.4 % |
| Debian <-> Fedora | 0.09 % | 0.00 % |
| Debian <-> openSUSE | 4.60 % | 0.01 % |
| Fedora <-> openSUSE | 0.19 % | 0.00 % |

Conclusions:
1. Cross-image dedupe at 4 KiB -- up to 4.6%, and only on the pair that has
   shared content (Debian/openSUSE). Debian/Fedora stays at 0.09%: there is
   nothing to share.
2. The main gain is INSIDE the image (openSUSE 0 -> 4.6%, Debian 10.8 -> 15.0).
3. Overall +5.3 points for 16x index entries. A fixed 4 KiB block gives almost
   as much as 8 KiB CDC (4.60 vs 4.68%) -- a content-defined boundary is not
   needed.
4. Before changing SEGMENT_SIZE, measure the cost: dedupe index, pba_ref, sweep
   time, memory. Change only if the cost is lower than the points gained.

### The recursion chain breaks on archives without a pack (tar.gz) -- dedupe is lost

Measured (volume in /dev/shm, 256 MiB ext4 image):

| option | used |
|---|---|
| A: image with 50 MiB of scattered files | 62.6 MiB |
| B: A + `tar.gz` of THE SAME files inside the image | 75.1 MiB |

The difference is 12.5 MiB, i.e. the archive costs its full 13 MB and is NOT
deduped against identical files next to it.

Reason: no pack in the registry sniffs `tar.gz` (ext4fs/fatfs/iso9660/ntfs/
xfs/qcow2/rawdisk/vdi/wim/p7z/jxl; p7z looks at the 7z magic). The built-in GZR/
TARR reproduce such a file bit-for-bit but do not expose members, so dedupe has
nothing to catch. In the leaf layout the archive goes into a blob as a leaf; in
the per-inode layout it stays a single file `...!mbrNNNN-dup.tar.gz` with no
parts.

Consequence: recursion works only where there is a PACK. The chain
`qcow2 -> raw -> MBR -> ext4 -> files` passes, while `... -> tar.gz -> members`
breaks.

Task: a `targz` pack (gzip -> tar -> members) or an extension of `p7z` to
gzip/tar: estimate, enumerate (files inside tar), extract, strip (tar headers +
deflate description), bit-for-bit rebuild, preferably batch and map. Gives
(a) dedupe of archive contents against extracted files, (b) another DFS level.

### The GZR -> TARR chain: TAR works, GZIP refuses on real streams

Verified on the same tar (42 MB, 3149 files):

| input | class after sweep | parts |
|---|---|---|
| `d2.tar` (no compression) | CONTAINER{algo=tarr(8)} | **2153 parts** |
| `gzip -1 / -6 / -9` (GNU gzip) | GENERIC_GUARD{algo=gzr(9)} | none |
| `python gzip` (zlib, level 6, wbits=-15) | GENERIC_GUARD{algo=gzr(9)} | none |

The cause is in vol_cpack.c: the probe iterates (level 1..9) x (memLevel 7..9) x
strategy=DEFAULT x windowBits=-15 and requires a BYTE-FOR-BYTE matching deflate
stream. GNU gzip uses its own deflate (not zlib) -- it will never match; a zlib
stream matches only if it is exactly the same zlib that is linked
in our build (system or stock). Result: `.tar.gz` in real images is not
decomposed, and the archive contents are invisible to dedupe.

Consequence for dedupe: the chain
`qcow2 -> raw -> MBR -> ext4 -> tar` works down to tar members (and dedupe
catches them there), while `... -> tar.gz` -- does not.

Options (in increasing cost):
1. **functional mode**: unpack gzip, put tar as a member (TARR), compress it
   again at rebuild -- the .tar.gz bytes will be different, the content
   identical. This is the same exact/functional contract change deferred for
   images. For archives this is usually acceptable (as zip/qcow2 do).
2. extend the deflate replica to the GNU gzip algorithm -- hard and fragile
   across versions.
3. store both the deflate stream and the unpacked structure -- double storage,
   not an option.

Until the contract is changed, the chain stops at gzip BY DESIGN (an honest
refusal is better than a broken rebuild).

## RESOLVED (design): bit-exact deflate via preflate, not a search over zlib versions

The problem that prevented writing a zip pack: deflate streams in real zip
files are made by different encoders (Info-ZIP, 7-Zip, libdeflate, zopfli, Go,
Java, zlib), and they cannot be reproduced bit-for-bit with a set of "our"
zlibs. Brute-forcing versions and parameters -- 650 recompressions per stream,
which is hours for a zip with 5000 members.

Found and verified: **microsoft/preflate-rs** (Apache-2.0, Rust, v0.8.0,
used in production at Microsoft). It parses the stream into tokens, GUESSES the
encoder parameters (hash algorithm, chain depth, nice-length, window, block
splitting strategy), and stores only DEVIATIONS from the prediction (CABAC).
Unknown encoders are also recovered exactly -- with more corrections.

Measurement on real text (4 MB from Debian):

| zlib stream | corrections | % of stream | % of plaintext |
|---|---|---|---|
| level 1 | 60 B | 0.003% | 0.0015% |
| level 6 | 3 262 B | 0.199% | 0.082% |
| level 9 | 318 B | 0.020% | 0.008% |
| huffman-only | 2 828 B | 0.109% | 0.071% |
| filtered | 59 729 B | 3.4% | 1.49% |
| rle | 37 934 B | 1.5% | 0.95% |

Plus an end-to-end `util --verify` check on real `real.zip` (5.9 MB) and
`real.tar.gz` (6.0 MB): bit-for-bit recovery confirmed, and the preflate
container turned out 5.6-7.3% smaller than zstd over the archive as-is.

Integration: **helperpack** `deflate_preflate` (external process). Reasons:
- Apache-2.0 is incompatible with linking into the GPL-2.0-only core, while a
  process is fine;
- there is a C-API (`dll`: create_compression_context / compress_buffer /
  decompress_buffer / get_compression_stats) and a scanner for deflate streams
  in files (zip/pdf/png/docx) in the `container` package;
- CPU ~4 MB/s per stream -- acceptable for a background sweep, but the core
  must not block.

What this gives: zip, tar.gz, docx/xlsx, pdf, png inside images become
shareable -- their plaintext goes into parts (dedupe + our compression lanes),
and the corrections (hundredths of a percent) into the recipe. The exact
promise is preserved. Measured effect in our scenario: an archive inside an
image costs +0.1 MiB instead of +12.5 MiB.

## KEY POINT (clarified): dedupe sees stored bytes, and what breaks it is BATCHING, not compression

`vol_dedupe.c`: "BLAKE3 over the STORED bytes of every live segment ([8B header]
+ csize payload -- a complete, deterministic function of what is on disk)".
That is, what must match is the STORED bytes, not the content.

Hence all session observations:

| case | dedupe | why |
|---|---|---|
| two identical copies of a file | 73.8% | files compressed the same way -> byte-for-byte |
| near-duplicates (same files, shifted) | 1.88x | files are aligned to their own starts |
| archive member <-> same file scattered | NO | the file is compressed separately, the member -- inside a blob: different compression contexts |
| all measurements with --no-compress | YES | plaintext segments are stored |

CLARIFICATION (verified by code and measurement): segment compression is
per-segment with a CLEAN context (`ZSTD_compress(cbuf+8, cbound, plain, enc_len, level)`,
vol_write.c:480), so identical 64-KiB plaintext always produces identical
stored bytes. So "compression breaks dedupe" is INCORRECT.

What breaks it is BATCHING: small files are glued into shared buffers
(TZ_BATCH_MAX) and compressed as a whole (`class: BATCHED_BIN{algo=zstd-bcj}`,
`block=1` in the middle of a batch -- measured). A 4 KiB archive member has no
own segments, so by construction it cannot match a standalone file.

Measured on the zip pack (deflate_preflate): a zip of one 4-MB file next to
the same file costs +1.9 MiB with a 1 MB zip (the file went into a batch, the
blob did not). For a zip of 200 small files the addition is +1.9 MiB with a
2.34 MB archive (19%).

### Architectural implication

For dedupe to work AFTER compression, one must compress not the whole file but
each unique segment separately: then identical segments produce identical bytes
and dedupe sees them. The cost is a 64 KiB compression window instead of a file
(a ~1.5-2x loss in compressibility), the gain is that dedupe stops depending on
context.

Alternative: write members RAW (dedupe on plaintext) and compress only unique
segments after dedupe. But then the size guard must be dedup-aware: parts whose
plaintext is already in the volume cost ~0, while currently it charges them
full size and refuses.

This also explains why the leaf layout (one blob for all members) breaks
dedupe: member boundaries fall inside segments, and after blob compression the
segments do not match separately compressed files at all.

Work order, if dedupe of archive contents is really needed:
1. dedup-aware projection in the guard (parts already present in the volume
   cost ~0);
2. per-segment compression (or RAW storage of members) instead of compressing
   the blob;
3. only then measure the zip/tar.gz gain -- right now it is measured against
   dedupe that structurally cannot work.

### Result on dedupe of archive contents (zip pack): does not work, reasons identified

Verified on a zip of one 4-MB file with the same file next to it:

| run | addition from zip (1.9 MB) |
|---|---|
| normal (compression on) | +1.9 MiB |
| `--no-compress` (members RAW) | +2.5 MiB |
| `--no-compress` + `INVFS_CPACK_DEDUP_FREE=1` (guard does not charge members) | +2.5 MiB |

That is, a dedup-aware guard **by itself achieves nothing**: even when it allows
decomposition and the members lie RAW, no match occurs.

Reasons (all three confirmed):
1. **Batching**: small files are glued into shared buffers (TZ_BATCH_MAX) and
   compressed as a whole -- the member has no own segments (`class
   BATCHED_BIN`, `block=1` inside the batch). Compression is irrelevant here:
   compression is per-segment with a clean context (vol_write.c:480), identical
   64-KiB plaintext gives identical bytes.
2. **Offsets inside the blob**: a member starts at an arbitrary offset, so its
   bytes fall on different 64-KiB boundaries than those of a standalone copy.
3. **Dedupe traversal rules**: TEXT batches are skipped (owned), whole-file
   blobs are declared unique by construction (vol_dedupe.c).

The measurement key `INVFS_CPACK_DEDUP_FREE=1` is left in place (the guard charges
members as 0) -- for measurements only, do not enable by default: it allows
decomposition that does not dedupe.

### What actually needs to be done if archive content dedupe is needed

Not the guard, but the layout: a member must occupy ITS OWN segments, aligned to
its own start (not land in a shared batch). Then the same member from two
archives produces identical segments and dedupe sees it after compression.

The cost at 64-KiB segments: up to 63 KiB of unused tail per member (~32 KiB
on average) -- for 1M small files that is tens of GB, i.e. unacceptable.
At 4-KiB segments the same scheme costs ~2 KiB per member, and cross-distro
dedupe improves at the same time (measured: 7.4% -> 12.7%).

Conclusion: **segment size is the same lever for archive dedupe as it is for
cross-image dedupe**, while a dedup-aware guard is a supporting measure, not a
solution.

## Comparison at EQUAL presets: content at parity, metadata spoils it

Corpus: 2 identical copies of a 256 MiB image (70 MB of files inside), 512 MiB
logically. The preset is aligned: InvariantFS `INVFS_PROFILE=balanced` = zstd-19,
squashfs `-comp zstd -Xcompression-level 19`.

| | size |
|---|---|
| squashfs zstd-19 (4 copies in one archive -> 2 copies) | 25 MB |
| squashfs zstd-3 | 29 MB |
| squashfs without compression | 73 MB |
| InvariantFS zstd-19, `used` | 110.1 MiB |
| InvariantFS zstd-3 | 113 MiB |
| InvariantFS without compression | 115 MiB |

Breakdown of 110 MiB by category (invf-stats -v):
  SHADOW 24.5 MiB -- content (compression 26.35x and dedupe: both copies share
                       ONE recipe and pba 173001, so dedupe worked)
  RAW     4.0 MiB
  metadata ~82 MiB -- unfolded delta: 77 719 records
                       (save point: base_root=130998 delta_end=208717)

`--realize` does not change this (not pinning), fold does not trigger: the
threshold is 262,144 records.

CONCLUSION: on content we are at parity with squashfs (24.5 vs 25 MB), while the
visible 4x loss is the change journal, which by policy accumulates up to 262,144
records and only then gets folded.

Tasks (by importance):
1. ~~fold policy: fold the delta by size, not only by record count; provide a
   fold on volume unmount/close.~~ **DONE, both halves.** The trigger has a
   payload component (64 MiB of delta payload / 262 144 records / oldest
   record older than an hour) and `vol_close()` folds before its final
   barrier (`src/core/volume.c:2119-2131`), so a volume nobody sweeps still
   compacts on exit. `invf-sweep` reaches the fold through `vol_close` and
   through `vol_sweep_pending_finish`. What is still missing is a fold by
   delta SIZE RELATIVE TO THE BASE (the measurement above says 77k records
   can be ~82 MiB) and an unmount-time fold for read-only/rolled-back views --
   neither is what the original task asked for.
2. After that repeat the comparison -- expect ~30 MiB vs squashfs's 25 MB.
   (Superseded by `BENCH-VS-SQUASHFS.md`: at an equal pooling unit we are at
   21.5 vs 21 MiB, +2.4%, with the sub-block rounding fixed.)

### Breakdown of `used` at equal presets: content at parity, garbage not removed

Correction to the previous conclusion ("4x worse than squashfs"): `used`
includes the RESERVED metazone, so it cannot be compared directly.

Full arithmetic (2 copies of a 256 MiB image, 66 MB of files, zstd-19):

```
used 119.0 MiB = SHADOW 23.5 (content) + RAW 3.9
               + unclaimed 27.5 (allocated, no live references)
               + meta 64.1 (METAZONE SIZE, set by mkfs)
```

Conclusions:
1. On CONTENT -- parity with squashfs: 23.5 MiB vs 25 MB (and dedupe works:
   both copies share one recipe and pba).
2. `meta 64.1 MiB` is the metazone reserve (for a 2 GiB volume), not data.
   On the stand after the mkfs fix it is ~1.7% instead of 30 GiB.
3. **`unclaimed 27.5 MiB` is real garbage: it does not go away in 6 sweeps, nor
   after `--realize`.** This is the same "reclaim does not take it" disease as
   spt0-broken (see above): without it our footprint on this corpus would be
   ~27 MiB vs squashfs's 25 MB, and with it -- ~51 MiB.

On the TEXT zone: in the leaf layout it is NOT used -- members are written to a
blob by the write session, and the blob is then compressed per-segment (zstd).
TEXT batching remains for ordinary small files outside containers.

On fold: `vol_fold_request()` is called from FOUR places, not one -- the FUSE
mount path (`src/cli/fuse_fs.c:4701`), `vol_close()` (`volume.c:2128`), the
FUSE drain's tail (`vol_sweep_pending_finish`, `vol_sweep.c:2112`) and tests.
So the `volume.h` comment ("This is what WP-M18's sweep calls") is CORRECT, and
the earlier claim that the comment lies is the stale half of this paragraph.
The observable fact behind it is narrower: the OFFLINE `invf-sweep` does not
call the fold itself, it only compacts when the process closes the volume
(`tools/invf-sweep.c:2113` says otherwise and is being corrected). The delta
in this run was small (1068 records), so none of it affected the 119 MiB.

## Found 2026-11-17 (night): free space in the image and a warning

### Where the garbage from free space actually sits

Experiment: a 64 MiB ext4 image with 4 MiB of files, then 40 MiB of random data
written and deleted (via `debugfs rm`), i.e. free space filled with garbage.
After decomposition:

```
recipe: blob 32784 B                      <- recipe COMPRESSED (zstd), zeros nearly free
content classes: compressed logic 64.0 MiB | used 42.5 MiB | 1.50x
```

That is, 42.5 MiB is really occupied, but **it is not in the recipe** -- the
recipe is 32 KB. The garbage goes into a different pack channel, not the
recipe blob.

Control experiment: the same image, but with zeroed free space (`truncate`+
`mkfs`): recipe 32.8 KB, compressed class 64.0 MiB -> 4.3 MiB (14.91x). The
difference of 4.3 vs 42.5 MiB is the price of non-zeroed free space.

### Why the warning in the core does not work

The core sees only (recipe_len, full_len). The recipe is compressed, so its share
stays small even in the "bad" case (32 KB per 64 MiB): the check
`recipe_len > 20% * full_len` never fires. The change was reverted.

**The right place is the pack: it knows the number of free blocks in the
partition.** It is `ext4fs`/`ntfs`/`xfs` that should say on decompose: "N MiB
(P%) of the image are free blocks, and M% of them are non-empty; zero out/TRIM
inside the guest before packing". For the core to do this it would have to parse
the guest filesystem, which it does not.

### TRAP: a sweep "hangs" if a FUSE mount of the same volume is alive

`invf-sweep <img>` with a live `invf-fuse` on the same image does not fail, it
WAITS on flock: this looks like a hang (two of our runs "of 20 minutes" on a
64-MiB image turned out to be instantaneous after `kill` of the mount). Check
`pgrep -a invf-fuse` and unmount before measurements; do not rely on
`fusermount -u` in a `cp && sync && fusermount` chain -- it may fail to work,
leaving the process alive.

## Test tiers: what was found while returning to green (2026-11-19)

### Fixed

1. **The build blocked the unit tier** (`24329f7`): target
   `invf-cpack_map_conc_tsan/asan/test` did not link (`invfs_codec_all` +
   `vol_write_begin/range/truncate/commit/abort`). The target compiles
   `vol_cpack.c` alone and stubs out the rest of the engine; the stubs were
   incomplete. The binary never built => `make test` failed at the build step,
   i.e. the unit tier never ran at all. Added fail-closed stubs.
2. **`fsck_clean` was nailed to a nonexistent format** (`cdeca14`,
   PRE-EXISTING): the gate checked `format:       v0`, while the metadata-tree
   format prints `format:       v1 (metadata base tree)`. Any v1 volume fell
   into the second branch (expecting `orphans:`/`missing:`, which the format
   does not print) and was declared dirty => **every** e2e suite with this gate
   died at the fsck step. Now the `format: v[01]` family is matched.
3. **A label rename broke the parser** (`cdeca14`): `test-textzone.sh` and
   `test-dedupe.sh` looked for `"TEXT  :"`, renamed to `text batch` (`ef4f992`).
4. **`bang_name_test` encoded an incorrect rule** (`af3b5bc`): it required
   `n!mbr000`/`n!mbr00000` to survive `unlink('n')` ("%04u never prints this") --
   wrong: for idx >= 10000 `%04u` prints five digits, and `bf54544` extended the
   form to 1..7 for exactly this reason. The names are moved into `collide[]` as
   an accepted remnant.

### Still red

- ~~**unit shard 4: `no_ckp0_surface_test`**~~ — **GREEN since `89d76da`**
  (`invf-fsck` folded the volume under a live save point). Diagnosis kept for
  the record: `invf-rollback` rejected a just-placed save point with "the delta
  log ... gone (a fold reset it)", although fold did NOT run
  (`fold: not triggered (2 records, 0 MiB)`) and the capture succeeded
  (`base_root=62299 delta_end=737`, 52 blocks pinned). Re-verified green at
  `8e6ea9c` — this row is stale, not a finding.
- **e2e suite 2: `test-dedupe.sh`** — `FAIL: free blocks did not stabilize in 6
  sweeps`. Measurement (from the log): 120672 → 120622 → 120618 → 120607 →
  120586 → 120586, delta 17/50/4/11/21/0. Free blocks decrease MONOTONICALLY,
  i.e. each sweep allocates metadata (a new save point ~9 blocks + delta
  records) rather than releasing it. Our reclaim-drain does the opposite --
  it releases, so it is not the cause; but this has NOT been bisected. The
  unit-side companion `dedupe_test` ("free blocks increased after
  deduplication") is GREEN at `8e6ea9c` (16/16), so whatever this suite
  demands differs from what the unit leg checks. Next step: bisection on
  `74cab2b` + decide what the suite is entitled to demand (stabilization of
  counts with generational save points is unattainable by construction, or the
  sweep is obliged to release the previous pin).

## Blind/semi-blind mapping: findings on the write path (2026-11-19)

We ran code mapping with agents on `opencode-go/hy4-flash-free` with comments
forbidden as a source. The report on the write path (`vol_write.c` +
`vol_sweep.c`, saved to `/var/tmp/blind/seeded-writepath.md`) produced 6
"suspicious" spots. Below is the analysis: what is confirmed by the code, what
is not.

### Confirmed by code (needs a test)

1. **`touched[]`: different index domains.** It is filled with a RELATIVE index
   (`rel_j = j % s->child_segs`, `vol_write.c:435,568`), but read in the
   ENOSPC precheck with the ABSOLUTE `j` (`vol_write.c:856`). As long as the
   file is within one child (by default `INVFS_CHILD_SEGS_DEFAULT = 65536`
   segments = 4 GiB) the values coincide; past the child boundary the precheck
   reads someone else's slots and miscounts `fresh`. Effect: under/over-estimate
   of headroom before writing (it may refuse early or miss an overrun), not
   corruption. In e2e legs `INVFS_CHILD_SEGS` is forced small, so this is caught
   by a test, not only on 4+ GiB files. A test is needed: writing across a child
   boundary with `-o child_segs=2`.
2. **`sweep_thread_worker` recompresses only `zone==RAW && pba!=0`**
   (`vol_sweep.c:1558`): already-BINARY/TEXT segments are not reconsidered. That
   is, the claim "the sweep recompresses all segments" is incorrect by code --
   this explains why changing the zstd level does not improve already compressed
   data.

### Not confirmed / requires separate checking

3. `wheat_carry` is written twice and is not read in these files (`vol_write.c:20,113,710`).
4. `dirty` is set, but `vol_write_commit_impl` does not check it (`:1051-1234`):
   a session with no writes still commits a recipe.
5. `s->new_id` is allocated and checked by the sweep guard, but a different id
   is published (`:1151-1168`).
6. `vol_sweep_file_generic` (`vol_sweep.c:2019`) calls the full `vol_sweep_file`
   (`:2022`) -- the name is narrower than the body; `sweep_locate_ctx`
   (`:138-143`) is unused.

Items 3-6 are candidates for dead code or misleading names, not defects; check
them one at a time, starting with 4 (a no-write session committing a recipe is
extra work on every open/close).

## Full e2e tier: 29 PASS / 58 (2026-11-19)

> **Read this before quoting the number.** That run invoked the suites as
> `bash tools/run-e2e.sh <suite>` directly, in the ambient environment. `make
> e2e` (`Makefile:1607+`) prefixes every suite with `$(TESTENV)`, i.e.
> `INVFS_LAZY_RECLAIM=0 INVFS_RAW_MIRROR=1 INVFS_CODECPACKS_SYS=0`
> (`Makefile:156`). With `lazy_reclaim=1` -- the shipped default
> (`volume.c:1960`) -- a delete returns no space until a drain rebuilds the map,
> which is exactly what `test-resize` trips on (measured: PASS with the variable
> forced to 0, ENOSPC at `[A5]` without it). So 29/58 is a figure for the
> DEFAULT config run by hand, not for the path CI uses, and some of the 29 are
> policy, not defects. Re-measure with `make e2e` before treating a row below as
> a bug.

A run of `bash tools/run-e2e.sh` against each suite separately (without stopping
at the first failure). Result: **29 passed, 29 failed**. Below is grouping by
CAUSE, because 29 failures ≠ 29 problems.

### A. Packs are stale relative to the engine (≈12 suites) — ENVIRONMENT, not code

`vol: packs from volume: 0 container, 0 codec ...; built-in codecs: 15` — no
packs are loaded. `test-ivpacks` names the reason directly:
`desc name != manifest (api_version=2 name=ext4fs version=1.0.0 pack_class=containerpack)`.

Verified separately **without** the runner's env override -- behavior is the
same, so this is not my `INVFS_CODECPACKS`. The packs in `/var/tmp/packs` were
built against an old ABI and live in a DIFFERENT repository (registry, copy at
`D:/.../registry-rescue`). They need to be rebuilt.

Affected: `test-ext4fs`, `test-ntfs`, `test-ntfs-big`, `test-containerpack`,
`test-cpack-mcost-bitexact`, `test-vdi`, `test-wim`, `test-rawdisk`, `test-ivpacks`,
`test-sandbox` (runs containerpack internally), `test-rawimg`, `test-exercarve`,
`test-jxl`, `test-binbatch` (the last four count pack actions).

### B. Missing host tools / root needed (≈6) — ENVIRONMENT

`test-fatfs` (`mkfs.exfat not installed`), `test-p7z` (`no 7z archiver`),
`test-flakey` (`/dev/mapper/...`), `test-acl`, `test-seal`, `test-put-failure`
(rc=127), `test-flushfail` (`harness build`).

### C. Pre-existing SPT0-restore defect (2)

`test-rollback` (`refusing to roll back: inode 12 segment 0 names block 10420, which
the save point did not record`), `test-watermark` (`rollback failed`). The same
defect as in unit shard 4 (`no_ckp0_surface_test`); bisection on `74cab2b` has
already confirmed it is pre-existing. Analysis: `WP-SPT0-PARTIAL-RECLAIM.md`.

### D. Pre-existing free-block accounting (1)

`test-dedupe` (`free blocks did not stabilize in 6 sweeps`, numbers 120672→120586).

### E. Need separate analysis — probably OURS or real defects (6)

| suite | symptom |
|---|---|
| **`test-resize`** | **rc=139 (SIGSEGV)** — a crash, highest priority |
| `test-writeback-stall` | "gate-on: the drain never ran after the write path went quiet (21s) — a deferral that never ends is a leak, not a yield" |
| `test-heat` | `INVFS_HEAT_INIT did not seed (rheat=0)` |
| `test-multidev` | `no demotion under pressure` |
| `test-pngflac` | `palette.png: unexpected stamp cls=8 algo=1 gen=1` |
| `test-dedupe` (see D) | possibly related to `touched[]`/accounting |

Analysis order: first `test-resize` (this is a crash, not an assert), then
`test-writeback-stall` (looks like a real deferred-work leak), then
`test-heat`/`test-multidev`/`test-pngflac`.

## "Agent in worktree" scheme: result of two attempts (2026-11-19)

**Worked.** Agent B (SPT0 rollback) found the architectural cause and fixed it:
`invf-fsck` without `-f` unconditionally called `vol_fold()` and mutated the
volume under a live save point. A 20-line diff, gates run independently by me
(no_ckp0 PASS, rollback PASS, watermark PASS), commit `89d76da`.

**Failed on infrastructure.** Agent A (`test-resize` SIGSEGV) got
`server_error: The inference service is overloaded` (rc=1), did not write
`RESULT.md`, and only managed to change the **harness** `tools/test-resize.sh`:
- `export INVFS_LAZY_RECLAIM=0` (otherwise a direct suite run inherits the
  caller's env);
- build `bin/invf-anchor_test` on demand (it is not part of `make -j8` TOOLS).

These edits are **not** a crash fix and were NOT taken into the main tree:
fixing the test instead of the cause is forbidden by the task rules. Both may
be useful separately -- check them and, if needed, apply consciously, not as a
"green test".

**`test-resize`: the SIGSEGV no longer reproduces; the suite is red for a
different reason.** Re-run at `8e6ea9c` in WSL:

- `INVFS_LAZY_RECLAIM=0 bash tools/run-e2e.sh tools/test-resize.sh` →
  **`RESIZE E2E: PASS`**, all legs, including `killed mid-apply: fsck open
  rolled forward to 1G, bit-exact`;
- with the default (`lazy_reclaim=1`, `src/core/volume.c:1960`) the suite dies
  at `[A5] shrink 640M -> 300M with data in the way`: `invarifs: big.bin: write
  session failed (rc=-2, ENOSPC)`, free blocks 102034 → 3730.

So the crash is gone and what is left is the §3.5 policy question, not a crash:
`make test` forces `INVFS_LAZY_RECLAIM=0` through `TESTENV` (`Makefile:156`),
while `tools/run-e2e.sh` and the suites export nothing, so the e2e tier runs on
a default that does not return space on delete. Part of "29/58" is that
asymmetry. The SIGSEGV itself has no owning commit in `src/cli/resize.c` — it
was fixed indirectly; if it is ever seen again, the blind-map suspect
(`vol_reclaim.c` frees metadata pages, the pin guard lives only in `volume.c`,
`v->retain_release=1` is a deliberate bypass) still stands.

## Agent C (`touched[]`): the fix is correct, but the test does NOT prove the defect -- rejected

An agent in worktree `/var/tmp/wtC` proposed a fix correct in substance:
```c
-        for (j = first; j <= last; j++)
-            if (!(j < s->touched_cap && s->touched[j])) fresh++;
+        for (j = first; j <= last; j++) {
+            uint32_t rel_j = (uint32_t)(j % s->child_segs);
+            if (!(rel_j < s->touched_cap && s->touched[rel_j])) fresh++;
+        }
```
and a new test (`write_touched_domain`, `child_segs=4`, tight budget, check for
no false ENOSPC) which was never merged and is not in the tree.

**Acceptance gate failed:** the test passes **without the fix too** (verified:
`git stash` of the `vol_write.c` edits, rebuild, run -- the same 8/8 PASS). The
test is not sensitive, and a fix without a failing test is not accepted (task
rule).

**Why it is insensitive.** The difference between the absolute and relative
index appears only when the SAME relative slot `j % child_segs` is touched twice
in DIFFERENT children: then the absolute read sees someone else's/zero slot and
considers the segment "fresh", while the relative one is already touched. The
test touches different relative slots, so both variants give the same `fresh`.

**What a sensitive test should contain** (recipe for the next attempt):
1. `child_segs = 2..4`, a volume with a budget where a ONE-segment difference
   already flips the verdict (pick `reserved_blocks`/`hard_min_blocks`, not
   `meta_reserved_pct`);
2. write a segment in child 0, then **the same relative** slot in child 1
   (`j` and `j + child_segs`) so that `touched[rel]` is set;
3. RE-touch that same relative slot in the next child and require no `-2`;
4. control leg: the same scenario without the fix must give `-2` (otherwise the
   test is vacuous again).

The patch is saved at `/var/tmp/patches/touched-domain-REJECTED.patch` (not
merged). The defect is still considered real by code inspection, but formally
unproven.

### Open question that blocks either fix of `touched[]` (re-read at `8e6ea9c`)

The recipe above still describes the shape, but the DOMAIN question is not
settled and a fix attempted without it can turn an early refusal into a missed
overrun, which is the unsafe direction:

- `touched[]` is PER CHILD: `wsession_write_seg_n` sets `s->touched[rel_j]`
  after `wsession_switch_child` (`vol_write.c:434-543`), and the switch
  `memset`s the array (`:346`, `:352`).
- the ENOSPC precheck reads it with `j`, the ABSOLUTE segment index
  (`vol_write.c:856`), inside a loop over `[first, last]` that can span
  children.
- `filled` in the same loop is `wsession_filled()` = `max(mat_upto, aliased_n)`,
  both absolute (`:543`, `:764`, `:686`).

So "already materialized" has three different domains in one expression. The
`rel_j` mapping alone makes a slot touched in child 0 count as materialized in
child 1 — the precheck then under-reserves for a write that crosses the
boundary, which is worse than today's over-reserve (`fresh` too high -> a false
`-2`). A correct fix has to say, in one place, that `touched[]` answers only for
`cur_child` and that every segment of another child counts as fresh unless it is
below `filled`. That is a WP with its own leg, not a drive-by patch.

## The mount's namespace walk costs 24 syscalls per file (2026-11-25)

**Measured, not estimated.** `INVFS_PERF_TRACE` (now on by default in every
boot path) prints the split on an installed nixos root of 40 296 files:

```
vol_open(/dev/vda2): total=0.032s (sb=0.012s, mbuf=0.001s, delta=0.000s, pba_ref=0.014s)
mount fold done in 0.000s
build_file_table: total=47.868s (walk=47.820s, qsort=0.047s, 40296 files)
```

So the fold is nothing (0.000 s -- it is not the bottleneck and moving it to
unmount would have changed nothing), the sort is 47 ms, and **the walk is 47.8 s,
1.2 ms per file**. That is what turns a 120 s initramfs budget into a repair shell
(gentoo, run 45583323109) and, more importantly, what a guest's udev is waiting
for while it looks frozen.

Reproduced locally without FUSE or a guest (40 000 files imported into a 0.35 GB
volume, `invf-verify --deep`) and profiled rather than guessed:

| | before | after threading the dir inode |
|---|---|---|
| wall | 71 s | 64 s |
| pread64 | 576 471 | 570 981 |
| futex | 324 822 | 318 132 |

gprof on that run: **CPU is 0.29 s of a 64 s wall.** The time is syscalls, and
531 633 `vmux_pread` calls for 21 651 files (24.6 per file) break down as:

| caller | preads | share |
|---|---|---|
| `dl_staged_free` | 276 028 | 52% |
| `mbuf_read_ptr` | 108 549 | 20% |
| `mbuf_root_read` (via `base_root`, 36 384 calls) | 73 461 | 14% |
| `mbuf_read` | 37 292 | 7% |
| `seg_read_once` | 36 085 | 7% |

`base_root()` re-reads and re-validates both RT30 root slots on **every** call
(2 preads) plus the root page itself -- 36 384 times for 21 651 files. That is a
designed-in per-operation cost with no cache in front of it.

### Candidates, in the order they should be tried

1. **Cache the validated root (RT30 slots + root page) in the volume, invalidated
   on publication.** ~14% of reads, one page instead of three per operation, and
   the invalidation point is the same place a fold/commit publishes a new root --
   it has to be exactly right, which is why it is not a drive-by patch.
2. **Batch `dl_staged_free`.** Half of all reads, for freeing staged blocks: it
   looks like one page read per freed block. Reading the page once per run of
   adjacent frees is the obvious shape; needs the free-list format read carefully.
3. **Walk-scoped page cache for metadata.** Only worth it after (1) and (2): the
   working set is the whole tree, so a pass-scoped cache (built at walk start,
   dropped at walk end, no invalidation problem because nothing survives the
   pass) is the cheap version. Deliberately NOT the existing ARC: ARC serves data
   blocks (`vol_read.c`, keyed by pba+TZ tag) with a 64 MB default budget, and
   putting metadata in it would make data and metadata evict each other while
   making the ARC stats unreadable.
4. **Parallel walk** by subtree. Real speedup available, but `g_io_lock` and the
   strict-walk guarantee ("every live inode or an explicit failure") make it a
   design change, not a loop swap.
5. **Lazy table** -- mount immediately, build the name table in the background,
   and let lookups that miss fall back to `vol_find`. Removes the walk from the
   boot's critical path entirely, at the cost of a second resolution path and of
   the `-ENOENT` vs `-EIO` question the DEGRADED flag answers today (WP135).

Rejected: persisting a name index on the volume. That surface was deleted on
purpose and `tools/check-repo-hygiene.sh` carries a tripwire against its names
coming back.

### Three local changes tried, measured one at a time (2026-11-25)

| change | pread64 | futex | wall |
|---|---|---|---|
| baseline (`invf-verify --deep`, 21 651 files) | 576 471 | 324 822 | 71 s |
| 1. `walk_dir` passes the directory inode it already has (`vol_ino_list_dir`) | 570 981 | 318 132 | 64 s |
| 2. + `base_root` cached on `rt.seq` (was: re-validate both RT30 slots per call, 36 384 calls) | 447 387 | 343 660 | 65 s |
| 3. + the pba_ref warmup thread deferred until after the walk | 299 143 | **0** | **33 s** |

The same thread also had a second victim, found the hard way: `sib_walk_test` arms
a per-LEVEL fault countdown on `walk_dir_stop`, and a warmup still running on a
slow runner walks the namespace itself (`pba_ref_ensure` -> `vol_walk_strict` ->
`walk_dir`), eating the countdown. The test then reports "0 discriminating
positions", which reads as "the seam is broken" while the seam is fine. It only
showed up on CI because a fast local box finishes the warmup before the search
starts. The test now sets `INVFS_PBA_WARMUP=0` before `vol_open`.

(1) is correct but is not the lever: the per-ENTRY version of that redundancy had
already been removed (the `walk=103.8s` comment in `vol_dirs.c`), and the
per-DIRECTORY half moved the profile by 1%. (2) is a real 22% of the reads and
exists because `base_root` used to answer the same question from disk on every
operation. (3) is the big one and it was not a read problem at all: the warmup
thread computes the SAME pba_ref map that the walk computes, concurrently, on the
same locks -- 355 k futex syscalls of pure contention and a 41% pessimisation of a
cold walk. The mount now starts it after the table is published, where it cannot
race the walk.

Measured on the tool that shares the walk's `pba_ref_ensure` path; the boot number
(`build_file_table: walk=`) is the confirmation to collect next.

Tooling left behind: `make PROF=1 OUT=bin-prof OBJ=build/obj-prof` builds with
`-pg` for `gprof`, because "the walk is slow" is not an answer.
