# InvariantFS Concepts

A document for whoever opens the code six months from now (including yourself).
It describes the **model**, not the API. Where the current code diverges from the
model, this is stated explicitly: marked as `NOW` and `TARGET`.

---

## 1. Devices and Zones

A single flat address space: the offset tells you which device it is.

```
dev0 = SSD   ┌ metadata zone ──── superblock, B-tree, inode-area, delta, bitmap
             ├ RAW zone ───────── hot data, all new writes go here
             └ tier arena ─────── accelerating copies of data whose canon is on dev1

dev1 = HDD   ┌ metadata mirror ── copy of metadata at THE SAME offsets (mode A)
             └ shadow zone ────── canonical data archive + delta/mbuf pools
```

`shadow_zone` is not "spare space" but the **canon** for everything moved by sweep.
After that, the HDD stops being expendable: it holds the only copies
of cold data.

---

## 2. Two Independent Replication Axes

They are constantly confused, so explicitly:

| axis | options |
|---|---|
| **metadata** | `write-through` (NOW) — identical copy of every flush; `checkpoint` (TARGET) — snapshot at seal/sweep time |
| **data** | RAW only (NOW, default); duplicated on dev1 (`INVFS_RAW_MIRROR=1`); canon in shadow after sweep |

The axes are independent: you can have write-through metadata and RAW-only data (this
is today's default), or checkpoint metadata and canon in shadow.

### 2.1 Mode A — write-through (NOW)

One namespace, two identical copies of metadata, each flush certified by
`DEVT.sync_seq`. Resync is needed on divergence. Plus: either device
opens on its own. Minus: certificate, resync, cost of repair.

### 2.2 Mode B — checkpoint (TARGET)

Shadow metadata is a snapshot at the time of the last seal/sweep. Change deltas
live in the delta log on RAW and are not moved to shadow.

Consequences that must be accepted along with the mode:

- **"RAW alone is readable" is not a contract and cannot be one:** the file base is on shadow,
  the delta is on RAW, there is nothing to reconstruct mathematically.
- **"Shadow alone is readable" is a contract, but means the last sealed
  state**, not the current one.
- **A modified swept file is transitional:** both devices are required.
- **Losing RAW = losing the tail after the last seal.** This is the desired
  loss semantics.
- `DEVT.sync_seq` and resync are not needed: there is nothing to certify, because
  equality is not required.

### 2.3 The split Boundary

For the base state, the boundary runs **by file**: sweep moves a file
whole, so a file is either wholly-RAW or wholly-shadow. A partial move
makes a file transitional (readable only with both devices) — this is
acceptable but must not be a terminal state.

---

## 3. `DEVT` — pairing passport

188 bytes in block 0 of each device, offset `0x2A0`:

```
magic "DEVT" | version | dev_count | flags
dev_blocks[2]   size of each device in blocks
sync_seq        generation counter (mode A only)
vol_uuid[16]    to avoid confusion with foreign devices
dev1_hint[128]  where to find the second device
crc32c          checksum
```

This is **neither FS data nor FS metadata**, but a service passport. On open, the volume
trusts neither device: it reads the passport from both and compares
`sync_seq`. Equal — both hold one generation. Unequal — one is behind, resync
is needed. `dev_blocks` catches "a disk of the wrong size was slipped in", `vol_uuid` —
"this is not the paired device".

In mode B, `sync_seq` is not used as a certificate: shadow has its own snapshot
generation, and comparing it with RAW is meaningless.

### 3.1 Order in Mode A (after the 2026-11-15 edits)

```
barrier → publish (RT30) → sync_seq++ → write DEVT to EACH device
       → barrier
```

All three parts are mandatory, and each has been broken at some point:

- **barrier after writing DEVT** — otherwise the passport lives in the device cache, HDD
  has no FUA, and the passports diverge forever (observed: 697302 versus 697301);
- **write directly to every device**, not through the mux — otherwise the write
  loses to the resync that writes the same page via `blkio_pwrite`
  (observed: `dev0=697511 dev1=697510` with a successful return);
- **self-check after the write** — reread both and print `devt: MISMATCH`.
  An invariant that is not checked is an invariant whose breakage nobody
  knows about.

---

## 4. Block 0 — shared, not "someone's own"

```
0x000 superblock (168)   0x100 RDP0   0x140 RSZ0   0x220 reserved
0x2A0 DEVT (188)         0x360 CVT0   0x9D0 RT30   0xA00 SPT0
```

There are several independent writers, so a read-modify-write of the whole block means
losing someone else's fields. Rule: **when you edit your own field, write through the mux and
re-serialize the neighbors you carry along** (this is how `vol_write_rdp0` now
updates `DEVT` with a CRC recalculation).

---

## 5. Data Lifecycle

```
new → RAW (SSD) → sweep: compress → shadow (canon) → rewrite the recipe
                 → publish → free the old block in RAW
                 → hot data gets an accelerating copy in tier arena
read: prefer the copy on dev0, fall back to the canon on dev1
```

The order of a move — **data → recipe → publish → free** — is not
cosmetics: freeing before publishing leaves the file reading from already
given-away blocks, and a failed publish leaves the new blocks as unreferenced
garbage. Both cases cost more than the delay.

### 5.1 Choosing Sweep Victims (DECIDED)

**Rule.** Data reaches shadow by only two paths: **spill** (RAW is
full, `alloc_raw_or_shadow` takes from shadow) and **sweep** (explicit).
There is no background migration, nor is any planned.

Hence: sweep is the only lever for freeing RAW, and it must be
**selective**. Today `vol_collect_sweepables()` collects ALL live inodes and
rewrites each one: at 322k files that is a whole-volume amount of work for
a few percent of space. This is what needs to be replaced by a selection layer.

Decision inputs (all already in the code, no new infrastructure needed):

| input | source | what it says |
|---|---|---|
| pressure `raw_free / raw_zone_blocks` | available | whether anything needs doing at all |
| heat `rheat` with decay | `vol_heat.c` (HOT=8) | how often it is read |
| age | pba in RAW (append allocation) | when it was written — for free |
| compression | previous class/codec | how much will actually be freed |

**The selection function is space gain, not temperature:**

```
freed  = blocks_in_RAW − blocks_in_shadow   (the difference = compression effect)
score  = freed × W_ssd − written × W_hdd − heat_penalty
```

where `W_ssd > W_hdd` (HDD is ~16 times larger, a block on it is cheaper), and
`heat_penalty` grows with `rheat`. The point: an incompressible cold file yields
almost no gain (freed 1 MB, occupied 1 MB), one that compresses 3x does.

**Pass order:**

1. collect live files (already exists);
2. cut off prohibited ones — see below;
3. evaluate score (cheaply: size from the inode row, `rheat` from the table, compression
   from the previous class, not a real run);
4. sort in descending order, go from the top;
5. **stop at the watermark**: `raw_free` has reached `H_high` (≈25 %) — stop;
   plus a per-pass file/byte limit;
6. in batches of N files with progress, so interruption is safe.

**The goal of a pass is to free RAW down to the watermark, not to rewrite the volume.**
Therefore the stopping condition matters more than the sorting.

**Hard prohibitions (more important than any policy):**

- the file is open or in the delta tail (just written);
- the section is after **seal** (immutable, decisions are frozen);
- the file is already entirely in shadow — nothing to free.

**Relation to the post-sweep cache.** After sweep, a block in RAW is marked free but
remains readable; the allocator takes from the **cold end** of the FIFO, so
fresh copies live longer. This applies **only to post-sweep files**: spill
files have no copy in RAW at all, and for RAW-only files RAW is their home, not a
cache.

**Metrics without which this is guesswork:** `freed_bytes` versus `moved_bytes`
(whether compression pays off), `post-sweep hit rate`, `evicted_then_read_within(T)`.

### 5.2 Lazy Eviction (TARGET)

Currently sweep frees a RAW block immediately, so rereading a cold
file goes to HDD. Goal: keep the RAW copy until space pressure arrives
(eviction on demand as it fills). This is not "warming the cache again", but
not destroying it.

The mechanism partly exists: tier arena + `\x01tier0` ("canonical-on-dev1 +
acceleration-copies-on-dev0, NEVER a move"). Essentially the goal is to merge the `rawm`-index
and the tier arena into one concept: "canon on HDD, optional copy on SSD".

### 5.3 FREEING Policy (not a cache policy)

This is not about what to keep in RAM (that is ARC's job), but about the discipline
of freeing. After sweep, a segment's canon lies on dev1, and a block in RAW can be
marked free — **but not reused immediately**: until someone overwrites it,
it holds a valid copy, and reading from SSD stays fast.

So **the free list itself is the cache**, and a policy is needed for one
decision: **which entry to take from it for a new write**.

The discipline I propose:

- **Allocate from the coldest end** of the free list (FIFO by time of
  freeing). Then recently freed — i.e. most likely hot — blocks
  live in the cache longest. This is CLOCK/second-chance, not LRU: strict LRU
  requires a pointer chain and produces random writes.
- **Batch invalidation.** When the allocator takes a block from the head of the list,
  the table entries pointing to these blocks are dropped in a batch. One by one —
  the per-eviction overhead eats the whole gain.
- **Hysteresis on watermarks:** evict down to the upper one, stop at the
  lower one. Otherwise you get "evicted — read — brought back" and a double write.
- **Two numbers in reporting, not one:** `free` (can be taken immediately) and
  `free-with-cache` (can be taken, but at the cost of losing a copy). One number lies.

Safety is where all future bugs will live:

- the cache is **never authoritative**: the canon on dev1 is the single source
  of truth, losing the table must break nothing but speed;
- a table entry is valid only while the block has not been reused, so
  invalidation is tied to the **event of the allocator issuing a block**, not to
  per-block epochs (an epoch over 29M blocks is tens of megabytes of extra
  state);
- on volume open, the table can be **discarded entirely** — it is a cache.

An alternative that requires none of this: **never free at all**
(arena + heat), as `\x01tier0` does today. We pay in space, but carry
no invalidation logic and no possibility of handing out one block twice.

Recommendation: start with the arena (safe, already there), and move to
"free-but-readable" only if space becomes a real problem — because it is in
free-space accounting that this scheme shoots itself.

---

## 6. Loss Semantics

Asymmetric, and this is not obvious.

| what died | mode A (write-through) | mode B (checkpoint) |
|---|---|---|
| **dev0 (RAW)** | metadata intact, **all file contents lost** (by default data is only on RAW). For files already swept, or with `raw_mirror=1` — only the tail is lost | the tail after the last seal is lost; swept data is read from shadow |
| **dev1 (shadow)** | nothing lost (it is a mirror) — as long as sweep has not migrated anything there | **the archive is lost**: everything moved to shadow, plus the snapshot metadata |

Key point: **after the first sweep, HDD stops being expendable**. The intuition
"shadow can be lost" holds only until archiving.

---

## 7. `lazy_reclaim` — a policy, not an optimization

`lazy_reclaim=0` frees blocks immediately, `1` defers it.

Decision: **after seal, lazy_reclaim is mandatory.** Seal makes the section
practically atomic — references are no longer rewritten, so deferred
reclaim cannot leave a dangling reference, while any immediate reclaim changes
the state of the section and breaks parity. Before seal it is the user's choice, with
no lazy by default.

---

## 8. Cost Model (measured baseline)

Stand: i3-10100, dev0 = SSD 110 GB, dev1 = HDD 1.8 TB, volume of 322,888 files.

| what | value | note |
|---|---|---|
| `vol_open` | 1.5 s | `delta=0.03s`, `pba_ref=1.26s` |
| `build_file_table` | 21 s | after removing repeated path resolution (was 106 s) |
| mount (before FUSE) | ~23 s | single-threaded name traversal |
| boot to ssh | ~96 s | **of which ~45 s is the GRUB menu timeout**, ≈15 s BIOS, the rest openrc |
| fold (bulk) | linear | 3,021,686 records in minutes, not hours |
| resync | by occupied portion | 21 GB used out of 32 GB; 1 MB written per one-block mismatch |
| small file | 12.7 KB, ~16 ms | cost is for the **first write**, not per byte |
| empty file | 0.3 KB, 1874 files/s | |

The figure "boot 96 s" without a breakdown is misleading — 45 of those seconds are GRUB.

---

## 9. Format Policy

The format is not frozen: we break it silently, with no migration code. The magics
(`BPG3`, `DSG3`, `RT30`, `MET0`, `SPT0`, `PCK0`, `ANC0`, `DEVT`) are not
renumbered: they are on-disk markers, not versions.

---

## 10. Debt: comments that lie

1. `vol_tier.c` — "`raw_mirror=1`, the default". In fact the default is **0**.
2. `volume.h` — "canonical data lives on dev1", "no sole copies on dev0, ever".
   By default this is false: data lives on dev0 until sweep or the mirror
   touches it.
3. "the DEVT bump certifies both devices" — before 2026-11-15 it did not certify
   (there was no barrier; the write lost to resync).
