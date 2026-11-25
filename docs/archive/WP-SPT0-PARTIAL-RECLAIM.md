# WP: partial SPT0 reclaim — what is actually needed (audit 2026-11-18)

Source: delegate audit `d8d3642c…` of `src/core/vol_spt0.c`.

## Main point: this is NOT "remove the flag"

The code directly documents why the refusal is global (`vol_spt0.c:965-971`):

> "Why refuse the whole reclaim rather than reclaim everything except the unreadable
> ones … unless the extent's reach is bounded, freeing the difference releases bytes a
> live recipe still decodes."

And there is a measured test precisely for this refusal (`src/cli/spn_broken_extent_test.c:35-41`):
the previous pin marked whole extents, the new walk marked only the heads, reclaim
freed all the remaining blocks of the extents — under recipes that still read them.

So "making it partial" = **first obtain the boundary of a broken extent**, which
does not exist today.

## Call map (for editing)

1. `spt0_capture()` `:1300` → `spt0_pin_take()` `:1057`, call `:1390` (reclaim runs
   BEFORE the new pin is armed).
2. Walk: `vol_iter_inodes_at()` `vol_btree.c:6612` → `spn_walk_ino()` `:714` →
   `spn_inode_block_set()` `:673` → `seg_extent_checked()` `volume.c:2891` →
   `seg_extent()` `:2867`; damage is recorded via `spn_dig_push()` `:605`.
3. Previous generation: `spn_old_map_load()` `:542`, call `:1158`.
4. `spn_reclaim()` `:939`, call `:1160`; owner sets: `tz_reg_owned_blocks()`
   `vol_textzone.c:452`, `spn_reg_owns()` `:829`, `spn_meta_mark()` `:882` →
   `spt0_ptr_from_pba()` `:850` + `btree_mark_reachable()` `vol_btree.c:2119`.
5. Single freeing point: `spn_free_counted()` `:919` → `vol_free_blocks()`
   `volume.c:3843` → `spt0_block_pinned()` `:1834` → `vol_free_run()` `volume.c:3730`.
6. Second debt producer (retire): `vol_retire_recipe()` `vol_delta.c:833` →
   `dl_retire_push()` `:788` → `dl_retire_collect()` `:907` → `dl_retire_free_list()`
   `:996`. Reclaim bypasses the pin-hold via `v->retain_release = 1` `:1030`.

## What the structures have and what they lack

- `map` / `old_map` — bitwise per-block bitmaps (`spn_map_bytes()` `:317`).
- Healthy extent: all blocks `[pba, pba+plen)` are marked `:798-803`; `plen` from
  `[csize][crc]` in the head.
- **Broken extent: ONLY the head is marked** `:762-767`; the tail is an "unnamed UNKNOWN
  set", counted in `c.broken` `:792`. This is the root of the problem.
- `indeterminate` inode: **not a single bit** `:737-740`.
- `spn_dig {pba, csize, crc}` `:273-281`; for a broken one `csize = 0` `:792-795`.
  Digests are sorted and written BEFORE the bitmap `:1230-1247`; restore treats
  `csize == 0` as a refusal `:1464`.
- `spn_file_hdr.broken` — only a COUNTER `:1237`; `invfs_spn0` (`invarifs.h:714-724`)
  stores neither a list of broken ones nor extent boundaries.
- Freeing predicate `:1031-1036`: `old_map` ∧ ¬`new_map` ∧ `v->bitmap` ∧ not owned
  by the TZ registry ∧ not in `meta` (reachable from `root_pba`).

## What is needed to make reclaim partial

1. **Persist the boundary of a broken extent.** At a minimum — `[head_pba, last_pba]`
   (or `[head, plen]`) for each broken segment, in the same pin file. Right now only
   the head and a counter exist, so after a crash/reopen the range is unknown.
2. The freeing set becomes
   `old_map \ (new_map ∪ tz_reg ∪ meta ∪ UNKNOWN)`; `!new_map_complete` no longer
   means "free nothing globally".
3. `indeterminate` **cannot** be made a partial range filter: its recipe
   is content-addressed and defines an unbounded set. Either the global refusal
   remains, or persist the inode's recipe identity and check it at reclaim.
4. The new pin must copy the protected UNKNOWN ranges **into its own `map`**
   (keep them for one more generation); restore will refuse to dereference them anyway (`csize==0`),
   so the degradation is a leak, not corruption.
5. Keep the fail-closed gates: failure of `tz_reg_owned_blocks()`, `meta_ok == 0`,
   unreadable `old_map`, `root_pba` allocation check `:1178`.

## Risks (all real)

- Freeing the tail of a broken extent: the reader decodes the whole extent, one
  wrong block = a destroyed live file (the measured WP632 failure).
- One cannot derive `plen` from a torn header or from the **current** on-disk header:
  it may already contain a reallocated segment. Only evidence from the capture moment /
  previous pin is valid.
- Reuse of the head pba between captures: the head is pinned, but the tail
  range may belong to another object — ownership cannot be derived from the new
  `map` by this pba.
- Neighboring extents in `old_map`: a contiguous run can merge two
  allocations — a short range under-frees, a long one leaks forever.
- Missing `meta`/the TZ registry = repeat the recorded corruption of base pages and
  batches (`:861-906`).
- Pin-hold (`spt0_block_pinned()`) must remain in force for every protected
  block: FUSE-retire and sweep free in parallel.

## Conclusion

The task is reframed: **first persist the reach of broken extents**, and only
then make the freeing predicate partial. An interim conservative source
of the range is a contiguous run in `old_map` from the head (the previous capture saw the healthy
extent in full), but it must be persisted and validated, not
reconstructed by heuristics.

---

## Side finding: `invf-rollback` refuses on a fresh save point (PRE-EXISTING)

Reproduction (without FUSE):
```
invf-mkfs v.img 1 → invf-import v.img src ×2 → invf-sweep v.img → invf-fsck v.img → invf-rollback v.img
invf-spt0: refusing to roll back: the delta log the save point captured is gone
           (a fold reset it; the pinned rows are not in the pinned base root)
```
But **fold did not run** — in the sweep log:
```
fold: not triggered (2 records, 0 MiB; triggers 262144 / 64 MiB)
[spt0] save point: pinned 52 blocks (4 segments, 0 already damaged) in 9 blocks at pba 62300
save point captured (base_root=62299 delta_end=737)
```
So the "a fold reset it" diagnostics are **false**: the restore check rejects a save point
just placed for a reason that never existed. Consequence: `invf-rollback` (undo
of the last sweep) does not work even in a clean scenario, and the test
`no_ckp0_surface_test` (gap in shard 4) fails exactly on this run — its three
other LEGs are green.

Bisection: at commit `74cab2b` (before the November 18 session edits: reclaim-drain, batch-hint,
punch-hole) the behavior is IDENTICAL. So this is not a regression, but a pre-existing defect
of the SPT0 restore path, and it must be fixed as a separate task — together with partial
reclaim, since both live in the same place (`spt0_pin_take` / restore check).
