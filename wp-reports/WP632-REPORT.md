# WP632 — daemon pass corruption: an unverifiable extent released its tail

STATUS: **DONE** — implemented, gated (unit red→green), e2e green, ready for review.
Branch: `wp/632-daemon-pass-corruption` (4 commits, not pushed).

## Why

`spn_reclaim` frees `old_map \ new_map`, where `new_map` is the mark set the
generation walk built. The walk marks a segment as an **extent**
`[pba, pba + plen)`, with `plen` derived from the segment's own frame header
(`seg_extent`, `src/core/volume.c:2378`). When that derivation fails — a torn
frame header (the dm-flakey `drop_writes` shape), or a block of the derived run
no longer allocated — the walk can mark exactly **one** block: the head. It
does not know how far the extent reaches, because the header it would have
believed is the thing that failed.

That was counted in `spn_walk_ino` as `c->broken` and reported in the save
point's pin line ("N already damaged"), but `broken` **never reached the
reclaim decision** (`src/core/vol_spt0.c:922`). `spt0_pin_take` computed
completeness from `c.indeterminate` only — the *inode*-level hole (a recipe
that would not load or parse). The *segment*-level hole had a diagnosis and no
consequence.

So on a volume that **already** carried damage when the pass started: the
previous generation's pin had marked those extents whole, the new walk marked
their heads, and `old_map \ new_map` freed the rest of every such extent —
blocks a live recipe still names — back into the shared free pool, where the
next `mbuf_alloc` / sequential allocator hands them out and overwrites them.
The pass reports `swept=0`; the pin line says "N already damaged"; the files
are destroyed regardless.

## Design

`spt0_pin_take` passes `c.indeterminate == 0 && c.broken == 0` as
`new_map_complete` (`src/core/vol_spt0.c:1145`), and prints why when `broken`
is what stopped it (`:1115-1130`).

This is the rule the module already applies twice — to an unreadable recipe
(`spn_inode_block_set` → `SPN_SET_UNKNOWN`) and to the base tree being
published (`spn_meta_mark`): **a set that is known to be missing an owner is
not a basis for releasing anything.** A broken extent is the same hole at a
different scale, and it is bounded in the worst direction: the walk knows the
first block and not the last.

Cost: the previous pin's hold is carried one generation longer (the same trade
the unreadable-recipe and unreadable-registry cases already make). The
containing alternative — release *except* the unverifiable ones — is not
available, because "the extent I could not verify" is an unbounded set.

Damage stays **contained**: one unreadable segment, which the restore-side
check (`spt0_data_ok`) already refuses to roll back through, instead of a free
that the next allocation overwrites.

## Scope

```
src/core/vol_spt0.c              broken -> new_map_complete + the pin diagnostic   (+60/-4)
src/cli/spn_broken_extent_test.c new gate, 15 checks (new file, 227 lines)
Makefile                         CLI_MAINS + test-shard-1 wiring
tools/check-vol-find.sh          ledger entry for the gate's fixture vol_find
```

## Validation — unit gate, red then green

The gate tears a **real** 8-byte frame header on a 96 KiB incompressible file
(so the last segment is a multi-block extent), captures once healthy, drops
that window, tears, then captures again — and measures the *reclaim*, not the
log.

```
$ bin/invf-spn_broken_extent_test            # PRE-FIX (log: /tmp/wp632-redcheck-run.log)
  FAIL  capture #2 reclaimed NOTHING: a walk that could not verify an extent is not a basis for releasing blocks
  FAIL  every block of the torn extent is still allocated (the pass contained the damage instead of amplifying it)
  [spt0] reclaim: 16 blocks the previous save point held are no longer referenced by any live recipe
spn_broken_extent_test: 15 checks, 2 failures

$ bin/invf-spn_broken_extent_test            # POST-FIX (in-tree, /tmp/wp632-maketest3.log:389-406)
        (capture #2 reclaimed 0 block(s))
  ok   capture #2 reclaimed NOTHING: a walk that could not verify an extent is not a basis for releasing blocks
  ok   every block of the torn extent is still allocated (the pass contained the damage instead of amplifying it)
spn_broken_extent_test: 15 checks, 0 failures
```

16 blocks of a live segment's extent, freed under a live recipe, is the defect
stated in blocks rather than in prose.

## Tests run

```
$ timeout 5400 make -j2 test > /tmp/wp632-maketest3.log 2>&1; echo $?
0                                                        # MAKE_EXIT=0, 4920-line log
```
All four shards pass; `spn_broken_extent_test` 15/15; codecpack 91/91; hygiene,
heredoc, citation and `check-vol-find` (29 ACTS / 82 SKIP / 110 sites) all
green. (A `grep -i fail` hit at log line 442 is the *text* of an assertion
description, not a failure.)

E2E, each foreground through the runner with agent attribution
(`INVFS_E2E_AGENT=wp632-daemon-pass-corruption bash tools/run-e2e.sh <suite>`):

| suite | result | why it is the right gate |
|---|---|---|
| `tools/test-sweep-window-reclaim.sh` | **PASS** | arm 1: a sweep must not damage a clean volume. **arm 2 is the control my fix could break**: the second capture must still *discharge* the first pin ("reclaim still runs, volume intact") — so "stop reclaiming" does not pass. |
| `tools/test-usr1-savepoint.sh` | **PASS** | capture is still armed by USR1/xattr, window costs one generation, rollback undoes it |
| `tools/test-rollback.sh` | **PASS** | SPT0 restore path still publishes base_root and truncates the delta |
| `tools/test-sweep-publish-rollback.sh` | **PASS** | abandoned pass leaves the file byte-for-byte (free 257 → 315 blocks) |
| `tools/test-orphan-reclaim.sh` | **PASS** | 8/8 legs; the collector still collects 14 orphaned v3 pages — the change must not freeze the pool |
| `tools/test-unlink-rollback.sh` | **PASS** | legs A/B/C, WP604 unarmed paths unchanged |

Not run, with reason: `tools/test-flakey.sh` leg 3 (`FLAKEY_ONLY=3`) is the
soak that actually produces the `drop_writes` torn header. It refused in
preflight — `FLAKEY3_EXIT=2`, `missing tool: blockdev` — before any leg ran.
That is a tooling refusal, not a result, and the leg is standalone tier (not in
`make e2e`); the same damage is reproduced deterministically by the unit gate
above, which is what the WP asserts on.

## Out of scope

- Repairing or reconstructing a torn segment (it stays unreadable by design).
- `seg_extent`'s own failure modes; this WP only stops the reclaim acting on
  an extent it could not measure.
- The `rawdisk`/codecpack classification issue referenced by the e2e runner.
- The untracked writeback-guest script in this worktree: untracked on
  `main` too, not mine, deliberately left alone and **not** committed.

## Remaining TODOs

None. The WP doc itself is deleted in the shipping commit per AGENTS.md §1.2;
this report is the orchestration artifact and is not part of the diff.
