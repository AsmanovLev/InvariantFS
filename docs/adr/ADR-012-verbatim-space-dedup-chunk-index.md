# ADR-012: Deduplication over the Verbatim (Logical) Space — Chunk Index + Merkle Leaves

## Status

Proposed — 2026-11-17

Depends on [ADR-010](ADR-010-cross-vm-dedup-via-windowed-recipes.md) (the reference
primitive, `INVFS_ALGO_WINDOW_SRC = 24`). Extends it: ADR-010 gave us *how to say
"these bytes are those bytes"*; this ADR gives *how to know that they are*, cheaply,
and independently of how they are stored.

## Context

### What deduplication is today

`vol_sweep_dedupe` keeps one physical copy per distinct **BLAKE3 over the STORED
bytes** of a segment: `[8 B segment header][csize payload]` (`vol_dedupe.c:423`).
Pass 1 walks live recipes, reads every live segment back from disk, hashes it; pass 2
sorts by hash, picks a canonical copy per group, rewrites the losers' recipes onto it
and retires their pbas.

It works well where the transformation is uniform: 4 identical Debian copies →
73.8 % saved (squashfs 75.4 %, duperemove 74.9 %); near-duplicate → 1.88× versus
1.12× for duperemove.

But the key is a function of the **transformation**, not of the content. Measured on
the same 64 KiB of `/usr/bin/bash`:

```
plaintext hash (what a Merkle leaf would key on)   114343cc95ef9df138a5…
  zstd-3 : csize=31125  stored-byte key  1585ded83589e7651bfd…
  zstd-19: csize=28614  stored-byte key  b001768f56ba725c69a1…   ← different
```

Anything that changes the transform invalidates the key silently: the zstd level
(`INVFS_PROFILE` is read per open, default `balanced`, `volume.c:1856`), `--no-compress`
sweeps, a codec change, and — decisively — **the segment size**.

### Why the segment size is about to change

The container corpus carries a measured +13 % against squashfs at equal granularity.
It decomposes into:

| component | measured | source |
|---|---|---|
| 64 KiB segments vs 128 KiB blocks | +4 % (24 MB vs 23 MB) | `BENCH-VS-SQUASHFS.md` |
| per-segment block rounding | +9 % (8.5 % of the compressed class, 13.5 % of the uncompressed class) | `BENCH-VS-SQUASHFS.md`, `INVFS_DEBUG_SEGS` |

Raising the segment size removes the rounding cost: 64 KiB → 25.9 MiB, 256 KiB →
22.7 MiB, 1 MiB → 20.8 MiB of data for the same corpus (bit-exactness verified at
each). But a global constant cannot satisfy both sides, because dedupe wants *small*
granularity:

| pair | 4 KiB | 64 KiB | 1 MiB |
|---|---|---|---|
| A vs C (one 2 KiB edit) | **99.6 %** | 98.3 % | 82.8 % |
| A vs D (50 × 1 KiB edits) | **99.6 %** | 98.0 % | 82.8 % |
| A vs B (metadata shift) | **77.9 %** | 72.8 % | 71.5 % |

In bytes over a 256 MiB base: the same 2 KiB edit costs **1.0 MiB** to store when
matching at 4 KiB, **4.4 MiB** at 64 KiB and **44.0 MiB** at 1 MiB
(`BENCH-MERKLE-DEDUP.md`). A small edit poisons every enclosing node at every level
above its size.

So the storage decision (how big an extent is, how it is rounded and compressed) and
the identity decision (whether this range already exists) must be **separated**. Today
they are the same constant, `INVFS_SEGMENT_SIZE`.

### The missing half

ADR-010 gave the on-disk way to reference another file's bytes (`WINDOW_SRC`,
implemented and covered by `invf-window_test`, 21 checks). What does not exist is a
**content-addressed index over decoded bytes**: there is no structure that answers
"do these 4 KiB already exist somewhere, and where can I read them from" without
reading and re-hashing the stored representations.

## Decision

### 1. Two namespaces, and the recipe is the bridge

| namespace | addresses | who lives there |
|---|---|---|
| **physical** | `pba` → blocks | allocator, rounding, compression, extents, bitmap, punch-hole |
| **verbatim (logical)** | `chunk_hash` → range of DECODED bytes | deduplication, references, refcounts, range verification |

A recipe entry is a *bridge*: `(file_offset, length, algo, extent_ref)`. The identity
of the bytes is the chunk hash; the extent ref only says how to obtain them.

### 2. Key = hash of plaintext leaves, 4 KiB, fixed chunking

Each inode carries a Merkle tree over its decoded bytes: leaves are 4 KiB plaintext
chunks, node = `H(child0 || child1)`, root stored in the recipe header. This gives
every level 4 KiB … 1 MiB from a single pass, and it makes the key independent of
level, codec and segmentation.

CDC is explicitly **not** adopted: measured on our corpora, fixed 4 KiB is equivalent
to CDC at 8 KiB (inter-image 4.60 %), and our shift sensitivity is a known constant
(77.9 %), not the failure mode we are solving.

### 3. `chunk_index`: hash → reference (derived, rebuildable)

```
chunk_index:  chunk_hash(32 B) → {
                  ref:      { pba, algo, decoded_off, len }   // how to obtain the bytes
                  refcount: N                                 // how many files reference it
              }
```

Derived data, like `pba_ref`: rebuilt by a walk of live recipes, optionally persisted
sorted for a fast start. It must never be a correctness dependency — losing it costs a
rebuild, not a volume.

### 4. Deduplication = lookup before write, not a second sweep pass

For every file written or compacted: walk the tree top-down; the largest matching node
becomes a reference (zero bytes), unmatched spans are collected; each unmatched span is
cut into extents along the ladder (4 KiB, 16, 64, 256 KiB, 1 MiB — never larger than
the span itself), compressed as a whole and written. Then the new extents' leaves enter
`chunk_index` with `refcount = 1`.

References materialize as ADR-010 windows (whole-inode view) or as in-extent slice
references (`block_id` + `block_offset`, the mechanism batch members already use for
4 KiB matches inside a large decoded extent).

### 5. Extent size becomes a policy, per span

An extent is as large as its unmatched span allows: a 2 KiB hole becomes a 4 KiB
extent, a 40 MiB new region becomes 40 × 1 MiB. Big extents get good compression and
0.7 % rounding; small extents exist only where a small extent is all there is.

The consequence is accepted and managed: a 4 KiB reference into a 1 MiB extent costs a
1 MiB decode to read. Compaction therefore uses observed `refcount`/read patterns to
**split** extents whose narrow slices are hot, and keeps extents that are only
referenced whole large.

### 6. Safety

- Verify on first claim: when a chunk hash is used for the first time, compare the
  bytes once; afterwards trust the 256-bit hash (the usual CAS rule).
- The Merkle root also serves range verification: any 4 KiB-aligned range can be
  checked without reading the whole file.
- `refcount` replaces "is anyone in the bitmap pointing here": an extent dies when all
  of its leaves are unreferenced.

## Consequences

### Positive

- Deduplication becomes **invariant to compression**: re-compressing a segment,
  changing the profile, installing a codec pack, or re-cutting extents no longer
  invalidates a single dedupe link. This is the precondition for everything below.
- Dynamic extents become possible at all: 64 KiB → 1 MiB segments is worth ~20 % of the
  data footprint on the container corpus, and with per-span sizing the small extents
  are paid only on the changed fraction.
- Hashing no longer requires reading stored bytes back: the tree is computed from data
  already in hand; after an edit only the changed leaves and the path to the root are
  recomputed (log n), instead of a full re-read of the volume.
- Multi-granularity matching from one structure: 99.6 % coverage at 4 KiB versus 82.8 %
  at 1 MiB on a one-file edit.
- Range verification and repair come for free from the same tree.

### Negative

- **Format break.** Recipe entry grows (`stored_len` + `sub_off` for sub-block extents),
  the header gains the Merkle root and leaf bits, and the tree needs a place to live
  (recipe stream section, or a chunked side stream for large files). Acceptable only
  because the format is not frozen (v0.7), and no migration code is written.
- **Index cost**: 32 B per 4 KiB leaf = 0.78 % of logical bytes for the unique leaf
  set; upper levels are derived, not stored.
- **Split/merge policy** for extents is new behaviour with its own correctness surface
  (a split must keep every reference valid; a merge must not orphan a slice).
- **Testing**: see below — this is the real price, and it is why the change is staged.
- Two-device and TEXT-batch interactions: batches stay excluded from dedupe (as today);
  the two-device path needs the same treatment as `punch-hole` (the punch currently
  skips two-device volumes for the same reason: which device owns the extent).

### Out of scope

- CDC / content-defined chunking (measured unnecessary; would add boundary metadata).
- Fuzzy hashing (ssdeep/TLSH/MinHash). Detecting "similar but not equal" saves nothing
  without storing a delta against a reference, which is a separate mechanism; the
  Merkle leaves make such a signature cheap to add later if a photo/archive corpus ever
  justifies it.
- Sub-block allocation of extent tails in the first iteration (it removes the last
  ~0.7 % of rounding but needs a read-modify-write path).

## Alternatives considered

| alternative | verdict |
|---|---|
| Keep stored-byte keys (status quo) | Rejected: it blocks dynamic extents by construction — the ladder changes segment sizes, which is exactly what invalidates the key. |
| One global segment size knob per volume (64 KiB dedupe volumes, 1 MiB container volumes) | Rejected: a volume usually holds both kinds of data; and it still couples identity to policy. |
| CDC chunking as the dedupe unit | Rejected on measurement: fixed 4 KiB ≈ CDC 8 KiB on our corpora; the complexity buys shift tolerance we do not need. |
| Btrfs/ZFS-style physical block dedupe (with a configurable recordsize) | Same failure mode we are fixing: identity bound to recordsize and compression. |
| Fuzzy hashing for near-duplicate detection | Deferred: needs delta storage to convert similarity into saved bytes; no measured corpus of ours demands it. |

## Testing plan (staged, zero-risk first)

The heaviest cost here is validation, so the order is chosen to make each step cheap to
reject:

1. **Shadow mode** — build the Merkle leaves and `chunk_index` during a normal sweep,
   log what it *would* dedupe (bytes matching, refcounts, would-be extent plan) and
   change nothing. Reuses existing suites; runs on real volumes; a bug costs a log
   line. Gate: numbers must agree with `BENCH-MERKLE-DEDUP.md` on the same corpora.
2. **Equality** — switch the *matcher* to chunk hashes but keep 64 KiB extents. Gate:
   bit-exact reads on the existing corpora (`invf-verify`, `invf-cat` byte compare),
   and dedupe results no worse than today's 73.8 % / 1.88×.
3. **Ladder** — allow per-span extent sizes behind an env flag. Gate: the rounding
   waste must fall as measured (8.5 % → 0.7 %) with bit-exactness unchanged.
4. **Crash/recovery** — index loss, torn recipe, interrupted split: the index must be
   rebuildable and no reference may dangle. Gate: existing crash suites plus a new
   "index deleted, volume still correct" test.
5. **Perf** — the sweep no longer re-reads stored bytes for hashing; measure.

New tests required (bounded list): index rebuild determinism; split/merge reference
integrity; dedupe across a re-compression; range verification; index-loss recovery.

## Open questions

1. Persist `chunk_index` on disk (sorted array + generation) or rebuild on demand? The
   trade is start latency versus another crash-consistent structure.
2. Leaf size 4 KiB fixed, or a per-volume field with the same trade as extents?
3. Where the tree lives for large files: recipe stream section versus a chunked side
   stream (the tree is 4 MiB for a 1 GiB file at 16 B leaves).
4. Does a chunk match prefer the reference with the cheapest read (the extent that is
   already hot/most shared), and how is that decided without a per-extent access
   counter?
