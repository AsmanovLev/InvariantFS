# WP — v3 recipe blob loss: the batch registry is a block owner

Branch: `wp/v3-recipe-blob-loss` · Worktree: `/srv/bench/worktrees/wt-v3-recipe-loss`
Agent: `wp-v3-recipe-loss`

## Scope

| file | what |
|---|---|
| `src/core/vol_spt0.c` | `spn_reclaim` consults the batch registry before freeing a pinned block |
| `src/core/vol_textzone.c` | new `tz_v3_reg_owned_blocks` — the registry's block extents as an owner set |
| `src/core/volume_internal.h` | `tz_v3_extent` + the declaration |
| `src/cli/batch_owner_test.c` | **new** — the regression driver |
| `tools/test-batch-owner.sh` | **new** — the regression suite |
| `Makefile` | build the driver, run the suite from `make test` |

## Why

`tools/fuzz/opseq.py --seed 0x5e9 --only-image 1 --ops 81` produced a volume
in which a file written by the last op could not be read:

```
vol_read_inode: v3 inode 24: recipe blob missing/corrupt
  CORRUPT: f011.bin
deep: 4 files ok, 1 corrupt, 260405 bytes verified
```

That is the bit-exactness invariant broken: bytes written seconds earlier were
gone. It is the highest-priority class of defect this project has.

The starting hypothesis (block reuse / double free, with the orphan collector
implicated) was **half** right. `INVFS_RECLAIM_ORPHANS` was not involved: the
sweep drains the collector unconditionally, but the collector is not what frees
the block. Nor was the standalone sweep: the volume is clean immediately after
op 80, and the `full_check` that follows begins with `invf-sweep --realize`.
Instrumenting the op loop with a `verify --deep` between every housekeeping
step puts the corruption squarely there:

```
STEP PRE-realize verify rc=0
STEP realize rc=0
STEP POST-realize verify rc=1     <-- the sweep just wrote the volume away
STEP fsck-f rc=3                  "root_slot[0] pba 14953 is torn"
```

## Design

A v3 text/binary batch segment has **two owners**: the recipe of every member
that points into it (its `zone == TEXT` entries), and the batch registry — the
hidden `\x01tzb` file whose row is what `tz_v3_gc` frees the block through.
The two owners do not die at the same time. A batch stops being named by any
live recipe the moment its last member is rewritten or deleted; its registry
row survives until the sweep's stage-6 GC runs.

`spn_reclaim` (`src/core/vol_spt0.c:669`, called from
`spt0_pin_take` at `:808`) only ever asked the first question, so during that window it
freed a block the registry still owned:

```
spt0_capture -> spn_reclaim  (vol_spt0.c)
    "in the previous pin's mark set AND no live recipe names it"  -> FREE
```

The free pool is shared with the metadata zone (one pool, no hard regions,
AGENTS.md §2.3), so the chain inside a single sweep is (the file:line in this paragraph are
the PRE-FIX tree, which is where the defect lives):

1. **stage 1, `prepare`** — the capture's reclaim frees the dead batch's block
   (pba 14953). The registry row is still on disk.
2. **stage 3, `transform`** — the sweep re-encodes a file;
   `vol_v3_recipe_store` → `mbuf_alloc` hands the *same block* back as a base
   B+-tree page, and `mbuf_root_publish` makes it the **live base root**:
   `mbuf_root_publish root=14953 gen=49 -> slot[0]`.
3. **stage 6, `batches`** — `tz_v3_gc` sees the registry row is not named by
   any live recipe and frees `pba=14953, phys=1` — the live root page. Traced:
   `[trace] vol_free_blocks pba=14953 n=1 retain=0 ck=0 rel=0` from
   `vol_tz_gc` (`src/core/vol_textzone.c:1551`, the free at `:1618`).
4. **stage 7, `finalize`** — the fold calls `vol_v3_base_root`, which refuses
   the freed slot[0] and falls back to slot[1] = the *pre-transform* root
   14960. The fold republishes a base that does not contain the transform's
   new recipe. The inode row (in the delta) still names it.
5. The new recipe's blob page 14944 is on disk and intact but **unreachable**:
   `btree_search` returns `found=0`, so `vol_v3_recipe_load` fails and
   `f011.bin` is unreadable.

The fix makes the registry an owner set in the reclaim, exactly as a live
recipe is:

```c
/* src/core/vol_spt0.c:700 -- the owner set, loaded once per capture */
if (tz_v3_reg_owned_blocks(v, &reg, &n_reg) != 0)
    return 0;                       /* fail closed: an unreadable registry
                                       must never read as "no owner" */
/* src/core/vol_spt0.c:720 -- the veto, inside the reclaim predicate */
if (bit_get(old_map, b) && !bit_get(new_map, b) &&
    bit_get(v->bitmap, b) && !spn_reg_owns(reg, n_reg, b)) { ... }
```

`tz_v3_reg_owned_blocks` is new, at `src/core/vol_textzone.c:1109`; the
membership test `spn_reg_owns` is at `src/core/vol_spt0.c:627`; the extent
type and its declaration are `src/core/volume_internal.h:908` and `:916`.

The debt is not deferred forever: `tz_v3_gc` still frees the dead batch in the
same sweep, after it has dropped the row, so the blocks come back one stage
later instead of being leaked. Regression-test leg 3 is what keeps a "just add a
veto" non-fix from passing.

`tz_v3_reg_owned_blocks` returns the extents sorted by head pba; distinct
batches hold distinct, non-overlapping extents, so one binary search per block
answers membership exactly.

## Validation

```
$ python3 tools/fuzz/opseq.py --seed 0x5e9 --only-image 1 --ops 81 --workdir /srv/bench/fb
[image 1] FAILURE at op 81: ...            # before
$ python3 tools/fuzz/opseq.py --seed 0x5e9 --only-image 1 --ops 81 --workdir /srv/bench/wtfix
[image 1] OK (81 ops, 2s)                  # after
```

Bit-exactness, `bin/invf-cat` + `cmp` against the harness's shadow tree:
5 files, 0 mismatches, `deep: 5 files ok, 0 corrupt, 260590 bytes verified`.

Other seeds, 120 ops, image 1 — before / after:

| seed | before | after |
|---|---|---|
| `0x5e9` | FAILURE at op 85 | OK |
| `0x1234` | OK | OK |
| `0xbeef` | OK | OK |
| `0x77` | OK | OK |
| `0x2024` | FAILURE at op 104 | OK |

New gate: `make test` runs `tools/test-batch-owner.sh`, which drives the
production `spt0_drop` + `spt0_capture` and reads the allocation bitmap (and
the blocks' bytes) across the capture. Without the fix it reports
`RECLAIMED_DEAD=7`; with it, `RECLAIMED_DEAD=0`.

## Out of scope

- **`invf-fsck` says OK on a volume whose recipes are unresolvable.** Another
  agent owns that. Not touched. For the record, fsck's blindness hides nothing
  *additional* here: it does name the torn root slot (`root_slot[0] pba 14953
  is torn`) and the orphaned recipe page shows up in its unclaimed accounting,
  but it does not treat "a live inode whose recipe blob will not load" as
  damage, which is why the volume reads as healthy.
- **Seed `0x2024` fails on the unfixed tree with a different symptom** — a
  data segment that no longer decompresses (`ZSTD decompress error: Unknown
  frame descriptor`) rather than a missing recipe. It passes with this fix,
  but I did not establish that it is this root cause and do not claim it is.
  Reported as an open item.
- `tz_v3_reg_store` is still called *after* the frees in `tz_v3_gc`, so a
  failed registry write would leave the same stale row behind. Not this bug;
  not changed.
- The orphan collector's liveness predicate, the fold's reclaim, `spt0_drop`'s
  mark-set retention: all read as correct here and were not touched.

## Coordination notes

- Subagent: `wp-v3-recipe-loss`. Branch `wp/v3-recipe-blob-loss`, **not**
  merged; the orchestrator squash-merges.
- No e2e suite is required by this change: it touches the savepoint reclaim and
  the text-zone registry, and the new gate is a unit-level suite. If the
  orchestrator wants the sweep suites re-run, they are `tools/test-textzone.sh`
  and `tools/test-binbatch.sh` via `INVFS_E2E_AGENT=wp-v3-recipe-loss bash
  tools/run-e2e.sh <suite>`.
- Cost note: one registry read (a name lookup plus a small blob) per savepoint
  capture. Captures happen once per sweep, not per write.
