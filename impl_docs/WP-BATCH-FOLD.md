# WP-BATCH-FOLD: fold as batched merge + bulk tree write

## Measured problem

Stand: delta **1 787 659 records**, mount 81 s of which **78.6 s — replay**.
`invf-fsck -f` (which also does fold) ran for **more than an hour** and did not finish.

## Why

Currently `vol_fold` does:

```
for each of 1.79M records: bt_ins(...)      /* COW insert */
```

Each insert copies a path of ~3-4 pages → **~7 million page allocations**,
each accounted for in the allocator and reclaim, plus (without `INVFS_MIRROR_DEFER`)
a synchronous write to HDD. Hence the hours.

This is not "aggressiveness" but the wrong approach: a tree that occupies
tens of megabytes in memory is rebuilt with millions of point inserts.

## How it should work

**Streaming merge, each page written exactly once.**

1. **Base from disk, not RAM.** Base-tree leaves are already sorted on disk;
   they can be read sequentially as a stream. No RAM is needed at all.
2. **Delta from RAM.** Its size is bounded by the fold triggers
   (`64 MiB` / `262144` records), so normally it is small. The sort already
   exists (`fold_sort`).
3. **On-the-fly merge**: two sorted streams (base, delta), last-wins
   by `seq` for matching keys. The result goes straight to the leaves.
4. **Bottom-up write**: filled leaves are written as soon as they are ready,
   parent levels are built in memory (only the current path —
   O(depth × page)), and each page is written **exactly once**.
5. Cost: `N / records-per-page` writes instead of `N × 3-4` COW copies.

## Atomicity of publish (a rule, not a detail)

```
1. write all pages of the new tree
2. publish the new base-root (RT30 slot, seq+1) + barrier
3. only now drop the delta (RT30: delta_pba = 0) + barrier
```

The order is mandatory. A crash between 2 and 3 leaves the delta in place with the base
already folded: the next mount will apply the records **again**, and this is safe —
records are absolute (last-wins by `seq`), so the repeat is idempotent.

The reverse order (drop the delta first) on a crash = **data loss**,
exactly like "blocks before the row" in `vol_unlink`.

## When it doesn't fit

The rule is simple, as in LSM:

- delta sort fits in the budget → in RAM (the usual case);
- it doesn't fit → spill sorted runs to disk and merge k-way.

The base never goes into RAM** — it is streamed from disk.

## Tests that accept this

1. Small volume: old fold and bulk-fold produce a **logically identical** base
   (compare not bytes — gen/checksum differ — but `invf-ls` + `fsck`)
   and the same `vol_count_free` result.
2. The same A/B on a volume with a delta crossing a leaf-page boundary.
3. Cut between (2) and (3): after the "crash" mount sees records in the delta AND in the base
   and fold repeats without loss (idempotency).
4. Measurement: 1.79M records, fold time and page-write count — expectation
   "seconds–minutes" instead of "hours".

## Why this matters more than the rest of the queue

Without this, any large fold is a timing lottery: fold on mount
(see `vol_close`, cap 400k) cannot be safe while its duration
is unpredictable. Plus fold alone eats 78 of the 81 seconds of mount time.

## Implemented and measured (locally)

`btree_bulk_new/add/finish` in `vol_btree.c` (the page format stays inside
the module), `fold_bulk_complete()` in `vol_fold.c`, gate `INVFS_FOLD_BULK=1`.
The publish order is the same as in the incremental path: bitmap → barrier →
verify → `mbuf_root_publish` → **then** delta drop under `g_delta_lock`.

| check | result |
|---|---|
| 60 files: listing + md5 of contents, A/B | matches |
| `fold_test scenario` (update, delete inode, delete dirent, add) | 38/38 in both paths |
| `fold_test crash-verify` (cut between publish and delta drop) | 5/5 PASS under bulk |
| 4000 files / 8006 records | bulk **0.88 s** vs incremental **3.36 s** |
| 20000 files / 40000 records | bulk **5.98 s** vs incremental **20.98 s** |
| repeated fold | "bulk path" is not printed: delta is empty |
| two bulk-folds in a row + import between them | 4000/4000 names |
| 40 files with 200+-byte names | 40/40, point lookup works |

### Test limitation found, not a builder limitation

1. `btree_repair_test`, the long-name phase, required `>= 4` leaves.
   This encoded the packing density of the incremental COW path rather than the need
   of the phase (a middle leaf is needed: a left neighbor for the >200-byte separator and
   a right one). Bulk packs to fullness and yields exactly 3 leaves on this corpus,
   which is legal. The threshold was fixed to `>= 3`.
2. The same test in the repair phase decides "the file must resolve" based on
   `in_quarantine(inode_id)`, i.e. it assumes that the inode row and
   the dirent row of a file lie on the same page. The incremental path
   (insert inode, then dirent, split in half) happens to satisfy this;
   dense packing does not, and one torn leaf takes away the dirent rows of files with
   live inode rows. The tree is still correct (structurally valid,
   "kept every readable key", "exactly the named files dropped", "every row
   accounted for" — all pass).
   The proper fix is to check the **dirent key** against quarantine
   (`parent || len || name`), not the inode id; that is a separate small task.

### What this means for the stand

1.79M records: extrapolation from 40k (bulk 5.98 s) — minutes, not hours, and
the time is predictable (linear in N, without N·depth).

### Second limitation of the same phase (task for later)

Under dense packing, `scan_rec` selects a leaf with named rows,
which turns out to be **last** in the tree. `fill_vhi` then gives it
an unbounded upper bound, quarantine covers the rest of the key space,
and repair legitimately drops the whole namespace — after which the check "repair dropped
exactly the damaged subtree" fails, even though the tree is correct.

The fix requires reworking the phase model, not a one-line change: choose a victim
strictly with a right neighbor (and fail with a clear message if no such leaf
exists), and compute expected losses from quarantine, not from ids.

Until this is done: `make test` (bulk disabled) is green as before;
`INVFS_FOLD_BULK=1 make test` fails exactly on that check.
