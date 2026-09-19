# WP43 — vol_heat: mapper-aware load/decay walk

Branch: `wp/43-heat-mapper` · Worktree: `/tmp/invfs-wp43`

## Scope (ONLY these)
- `src/core/vol_heat.c` — the two linear record walks around lines
  ~440-570 (`end = v->inode_area_pos` guards at 453/544 and their
  surrounding loops).

## Why
Heat persistence mirrors the same broken legacy pattern as
`vol_compute_stats` (WP41) and the sweep collector (WP42): records in
mapper extents are invisible, so heat table load/decay sees 0 records
on a v0.3.0+ volume. Reads then never get counted and the sweep's
decay pass never burns anything.

## Design
- Start pos = 0 on mapper volumes (`vol_inode_next` handles the hop),
  `v->inode_area_start * INVFS_BLOCK_SIZE` on legacy.
- Iterate with `vol_inode_next(...)`; per record: `malloc(rl+4)` +
  `vol_read_raw()` + CRC-verify (torn ⇒ advance, do not stop), rebuild
  the header via `memcpy` from the buffer, and feed the EXISTING
  per-record body (decay, owner cross-checks, table fill).
- Preserve all legacy branches and 0x01-owner handling byte-for-byte;
  only the positioning of the walk changes.

## Validation
```
$ make test
$ INVFS_E2E_AGENT=wp43 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
```
If WP45's `tools/test-heat-mapper.sh` exists by coordination time, run
it too; otherwise leave a one-line note in the report and cover in the
WP45 merge.

## Out of scope
- vol_sweep.c, vol_dedupe.c, tools/invf-sweep.c (WP41/42/44).
- Any volume-layout change.

## Coordination notes
- e2e via the standard lock, agent tag `wp43`.
- Requires the semantics of `vol_inode_next` (mapper+legacy unified),
  which is on main since 0d5812f — no dependency on WP41/42 code.
