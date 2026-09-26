# ADR-010 Amendment 2: Guest-File Windows by Composition (no new pack command)

## Status

Proposed — 2026-09-26. Amends the "Amendment: Multi-member QCOW2 decomposition"
section of ADR-010. The `WINDOW_SRC` opcode and `GUEST_ENUM` command described in
the original Decision section remain **not implemented, and this amendment
explains why they are the wrong shape**.

## Trigger: the 2026-09-26 measurement

Two cloud images (`ubuntu2204-base.qcow2`, 735 051 776 B, and
`ubuntu2404-base.qcow2`, 624 829 952 B — 1 296.9 MiB together) were
decomposed to convergence on a 12 GiB volume:

```
used             : 5519.5 MiB
logical bytes    : 19265.9 MiB        (12 065 files -> 1 205 after nesting)
SHADOW: logic 19212.0 MiB | used 3114.9 MiB | 6.17x
RAW   : logic     0.9 MiB | used 2180.2 MiB | 0.00x
TEXT  : logic    53.0 MiB | used   23.7 MiB
```

**Compression is not the problem.** ZSTD reaches 6.17x on the guest bytes,
against ~2.9x for the per-cluster zlib the images already carry. We compress
*better* than the source and still land 4.3x above it.

The reason is redundancy, measured directly from the inode population:

```
A.qcow2                                    recipe, 1 segment (Q2R3)
├── !mbr0002-rankimg            1.75 GiB   28 714 ZSTD segments
└── !mbr0001-diskimg            3.5 GiB    1 segment, algo=rawdisk (a marker:
    │                                       the member is NOT stored whole)
    └── !mbr0001-p0001          2.5 GiB    40 944 ZSTD segments
        └── !mbr0016-p0016      913 MiB
            └── 1 205 leaves              per-file ZSTD segments
```

The same guest bytes are stored three times: once as QCOW2 clusters
(`rankimg`), once as partition members (`p0001`/`p0016`), once as individual
files. Block dedupe cannot collapse them: it merges byte-identical **and
identically aligned** segments, and the three representations are framed at
three different offsets (guest LBA, partition offset, file extent).

The `RAW: 2180.2 MiB at 0.9 MiB logic` line is a separate finding, not
redundancy: `invf-fsck` reports `save point: live`, and the SPT0 savepoint
pins the pre-sweep base tree, so every recipe page the sweep superseded is
still allocated. It is reclaimable, not lost.

## Why the original Decision section is the wrong shape

ADR-010 proposed `INVFS_ALGO_WINDOW_SRC = 24` plus a new containerpack command
`INVFS_PACK_CMD_GUEST_ENUM = 6` and an ABI bump to `IVPACK_API_VERSION = 3`.
Two problems, both now visible:

1. **It adds a representation instead of removing one.** A window that points
   into `diskimg` still needs `diskimg` to exist as stored bytes. The current
   amendment deliberately does *not* store `diskimg` whole — its data lives in
   the partition members. Pointing windows at `diskimg` therefore reinstates
   the 3.5 GiB member and makes the redundancy worse.
2. **`GUEST_ENUM` asks a pack for something the FS can already derive.** The
   file→offset mapping is not new knowledge:
   - `qcow2` already writes, for every allocated cluster,
     `{file_off, mem_off, csize, repro, level, mem, strat}` in the Q2R3 recipe
     table — that *is* the guest-LBA → rankimg mapping, with the inflate
     parameters attached;
   - `ext4fs` already parses its extent tree into `rrange_t {logical, image,
     len}` and then **discards it**: `cmd_enumerate`
     (`tools/codecpacks/ext4fs.codecpack/ext4fs.c:1290`) prints only
     `idx<TAB>sname<TAB>usize`.

   A guest file's bytes are therefore the composition of two tables the FS
   already holds, plus one it is already throwing away.

## Decision (revised)

Publish each guest file as a chain of per-cluster window recipe entries whose
source is the **`rankimg` member**, composing the existing tables. No new
containerpack command, no ABI bump, no new pack.

### 1. Opcode: `INVFS_ALGO_WINDOW_SRC`

One new recipe opcode, as originally specified:

```c
#define INVFS_ALGO_WINDOW_SRC 24  /* recipe entry: window into another inode */
```

A `WINDOW_SRC` entry means: "these `length` bytes are
`src_len` bytes of inode `src_inode_id` at `src_off`, inverse-transformed by
`transform_kind`." The sibling field group is the one ADR-010 already drafted
(`src_inode_id`, `src_off`, `src_len`, `transform_kind`, `repro_*`).

The source is always a `!mbrNNNN-rankimg` sibling, so `transform_kind = 1`
means "inflate this cluster" using the Q2R3 recipe's recorded zlib parameters
— the same `invfs_deflate_repro_encode` inverse the MRMP `kind 2 REPRO` path
already uses, so the machinery is in place and tested.

### 2. ext4fs `enumerate` grows two columns (backward compatible)

```
idx <TAB> sname <TAB> usize <TAB> image_off <TAB> len
```

`cpack_parse_table` parses three fields today; the two new ones are read when
present and ignored when absent, so **every existing pack and every existing
stored table keeps working**. A file whose extents are fragmented (n > 1 runs)
emits one row per run, with the run index in `idx`'s sibling column — the
existing `idx`-is-an-ino convention is preserved for the first run.

### 3. Sweep: publish leaves as windows, skip the intermediate member

In `vol_containerpack_sweep` (`src/core/vol_cpack.c`), when a container member
is itself a container that enumerates with ranges:

- the nested member is **not** committed as a stored file;
- for each leaf, the FS resolves `leaf -> (partition member offset) -> guest
  LBA -> (rankimg mem_off, csize, repro)` and publishes a recipe of
  per-cluster `WINDOW_SRC` entries;
- the `rankimg` member is the only stored copy of the guest bytes.

Savings on the measured corpus, per image: drop the partition copy
(3.4 GiB of logic) and the leaf copy (~1.2 GiB), keep `rankimg` (1.75 GiB).
At the measured 6.17x that is ~750 MiB of physical per image, i.e. the pair
goes from 5 519 MiB to roughly **4 000 MiB** — still above the 1 296.9 MiB
source, because `rankimg` plus the small remaining overhead is irreducible
while the 1:1 invariant holds. The decisive win is the *fleet* case: N images
of the same family share one `rankimg`-equivalent payload, and every guest
file window becomes a single shared PBA automatically.

### 4. Lifetime: the sibling cascade already protects the source

This is the risk ADR-010 flagged as "critical". It is smaller than it looks,
because the source is a `!`-sibling of the same parent:

- `vol_delete_siblings` removes `name!mbrNNNN-*` **and** `name!<leaf>` in one
  cascade, so deleting the container takes the windows and their source
  together;
- a window whose source is missing reads **EIO**, never zeros, and fsck already
  quarantines a directory entry that names a missing inode;
- therefore no new `windows_to` refcount counter is required for the MVP. The
  `INVFS_V3_RECIPE_KEY_WINDOW` counter from the original ADR is deferred: it is
  only needed if windows are ever allowed to outlive their parent's subtree,
  which MVP does not permit.

### 5. Read path: two hops, with an explicit budget

`vol_decode_ast_entries` gains the `WINDOW_SRC` arm (per-cluster inflate from
`rankimg`, then slice). Chain depth is fixed at two hops (leaf window →
`rankimg`), so `INVFS_WINDOW_MAX_DEPTH = 4` from the original ADR is ample.
Cost per 64 KiB served: one inflate of a member range, which is the same work
the MRMP `kind 2 REPRO` read path already performs today and is covered by the
ARC cache. The `A.qcow2` read path is unchanged — it still splices from the
recipe + `rankimg` through MRMP.

## Explicitly rejected in this amendment

- **`GUEST_ENUM` / `IVPACK_API_VERSION 3`** — superseded by composition.
- **Windows pointing at `diskimg`** — would reinstate a stored 3.5 GiB member.
- **Metadata compression (LZO/LZ4/zstd on the metadata zone).** Measured, not
  assumed: a freshly `mkfs`'d **empty** 12 GiB volume already reports
  `meta 224.4 MiB`, identical to the populated one. The metadata zone is a
  fixed mkfs reservation that does not grow with data; the live base tree on
  the measured volume is 9 496 pages = 38.8 MiB. Compressing it would save
  roughly 15 MiB out of 5 519 (0.3%) and would add a decompression step to the
  hottest path in the filesystem. The window-recipe metadata this ADR adds is
  28 714 entries x ~48 B = **1.4 MiB per image** — three orders of magnitude
  below anything that would justify a metadata codec.

## Consequences

- Positive: the fleet case (many similar VM images) collapses to one stored
  copy of the guest bytes, which is the case ADR-010 was written for.
- Negative: the partition members stop being readable as files, so anything
  that wants a whole-partition view must re-read through the windows. The
  `rankimg` and the parent's MRMP remain, so the 1:1 invariant is untouched.
- Negative: a fragmented guest file produces many window entries; the
  per-image bound from the original ADR (`INVFS_WINDOW_MAX_FILES = 50000`)
  still applies.
- Risk: retiring the savepoint (`invf-sweep --realize`) is currently manual.
  Until that policy is automated, `used` carries the pinned history — measured
  at 2 180 MiB on the corpus above. That is a separate, smaller ADR.

## Rollout

1. Opcode + read-path arm + a unit test that round-trips a window through a
   `rankimg`-shaped member (no sweep change yet).
2. `ext4fs enumerate` columns + `cpack_parse_table` optional fields.
3. Sweep publishes leaves as windows for the qcow2 -> ext4 chain, behind a
   manifest opt-in (`windows = 1`), so the nested-member behaviour stays the
   default until the numbers are in.
4. Re-run this measurement and record the before/after in
   `docs/benchmarks/QCOW2-COMPRESSION-BENCHMARK.md`.
