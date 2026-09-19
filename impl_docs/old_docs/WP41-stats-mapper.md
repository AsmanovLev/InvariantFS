# WP41 — vol_compute_stats: mapper-aware record walk

Branch: `wp/41-stats-mapper` · Worktree: `/tmp/invfs-wp41`

## Scope (ONLY these)
- `src/core/vol_sweep.c` — function `vol_compute_stats()` only (lines ~1553-1767).
- `tools/invf-stats.c` — output code that renders the stats struct unchanged; only if a field needs a new sanitizer.

## Why
On a v0.3.0+ mapper volume (records live in dynamic meta extents, listed
in MET0 + the mapper table, e.g. our 15 GiB stage3 volume with 214
extents) `vol_compute_stats()` walks the LEGACY contiguous inode area
`inode_area_start .. inode_area_pos` and reads **zero** records:
population shows 0 files / 0 dirs / 0 symlinks while `invf-fsck`
(mapper-aware since 0d1ec4f) sees 66182. Sweep/stat/heat/dedupe tests
pass only because their fixtures fit inside one extent.

## Design
The mapper-aware iterator already exists: `vol_inode_next()` in
`src/core/volume.c` (mapper: walks every meta extent, legacy format:
falls back to the contiguous region). `meta_read_record_by_id()` and
`vol_get_children()` already consume it.

Rewrite the walker head of `vol_compute_stats()`:

1. Compute a start position:
   - mapper volume (`v->met0_present && v->meta_mapper &&
     v->met0.extent_count > 0`): start = 0 (`vol_inode_next` hops to
     the first extent),
   - legacy: start = `v->inode_area_start * INVFS_BLOCK_SIZE`
     (unchanged behaviour).
2. Loop the *same* per-record body against `vol_inode_next`:
   `np = vol_inode_next(v, pos, &magic, &ino, &fsize, name, cap, &rl)`
   → record position `rp = np - (uint64_t)rl - 4`, malloc `rl+4`,
   `vol_read_raw()` the full record + trailing CRC, verify CRC
   (bad ⇒ `out->bad_records++`, advance, keep going — same semantics
   as today), rebuild `invfs_inode_rec h` from the buffer by
   `memcpy` (do not trust evicted fields), then run the existing
   per-record analysis body unchanged.
3. Keep the legacy direct-loop code path exactly *as is* for
   non-mapper volumes (guard with a runtime branch) so a regression
   here cannot leak into format_version=0 handling. Prefer the shared
   branch only if the diff stays small.
4. DO NOT touch the volume compact/apply paths, sweep engine, heat or
   dedupe — they are separate WPs (WP42, 43, 44).

## Validation
```
$ make test              # 4722 checks, 0 failures
$ INVFS_E2E_AGENT=wp41 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
```
New gate in WP45's worktree (`tools/test-stats-mapper.sh`, WP45 lands it
first; if it does not exist yet, cherry-pick the fixture script per
Coordination notes below). Asserts, on a ~30k-object fresh import:
- `invf-stats` population matches `invf-import` counts
  (files/dirs/symlinks),
- `logical bytes` ≈ source tree byte sum (± 1 block per file),
- `unclaimed` ≤ 2% of used.

## Out of scope
- Any change to sweep/dedupe/heat engines (WP42/43/44).
- `vol_open`'s scan (already mapper-aware via 0d5812f).
- Tools other than invf-stats rendering.

## Coordination notes
- e2e lock: every invocation through `INVFS_E2E_AGENT=wp41 … run-e2e.sh`;
  suites serialize via `/tmp/invfs-e2e.lock`, never bypass.
- Merge order: WP41 may merge before WP42; both edit `vol_sweep.c` —
  if WP42 lands first, rebase and resolve with the WP41 walker preserved.
- File-conflict policy: this WP must not touch
  `tools/invf-sweep.c` or the sweep engine functions in `vol_sweep.c`.
