# WP44 — vol_dedupe: mapper-aware pass-1 hasher

Branch: `wp/44-dedupe-mapper` · Worktree: `/home/user/invfs-wp44`

## Scope (ONLY these)
- `src/core/vol_dedupe.c` — the pass-1 hasher walk (legacy
  `pos = inode_area_start*BLOCK; end = inode_area_pos; while(...)` around
  line 310) and the merge-apply area-full pre-check that shared the same
  legacy bound.

## Why
On v0.3.0+ mapper volumes the dedupe pass-1 scanner saw zero records
(records live in dynamic meta extents, not the legacy contiguous inode
area), so merging/dedup was a no-op on real volumes. Confirmed with
`src/core/vol_dedupe.c` (`dedupe: hashed 0 live segments`) before the fix
and `hashed 30000` after, on the WP45 30k-object fixture.

## Design
- Drive the walk with `vol_records_walk(v, dedupe_cb, ctx)` (WP40 helper;
  callback receives CRC-verified full records, torn records skipped by
  the walker). Keep the exact eligibility rules: TOMBSTONE skip,
  `name_len < sizeof(name)` bound, live-record semantics, per-segment
  hashing into the existing `segs[]`/`blobcap` bookkeeping, and the merge
  logic untouched.
- The area-full pre-check for the merge-apply path becomes mapper-aware
  (extent room via `meta_get_append_pos`/`inode_area_make_room` semantics)
  instead of the legacy linear bound.
- Legacy format_version=0 path keeps working through the walker's
  fallback.

## Validation
```
$ make -j$(nproc) && make test            # 4722 checks, 0 failures
$ INVFS_E2E_AGENT=wp44 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
$ INVFS_E2E_AGENT=wp44 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
```
Observed after the fix on the WP45 fixture: `merged 4 segments, freed 57
blocks` on the legacy-bound leg, `--deep 5 files/0 corrupt`, all files
bit-exact, second sweep idempotent (`merged 0`).

## Out of scope
- The pre-existing orphan-count / `invf-fsck` exit-3 bug that trips
  `tools/test-dedupe.sh`'s `free_blocks()` helper at "== mkfs + import =="
  on main as well — separate WP.
- `tools/test-dedupe.sh` leg-2 text-batching expectations (stale vs the
  current invf-sweep output) — separate cleanup.
- `vol_textzone.c` owner-record appends (needed before batch deferral can
  be re-enabled) — follow-up WP.

## Coordination notes
- e2e via `INVFS_E2E_AGENT=wp44 bash tools/run-e2e.sh`.
- Based on main@9ce8cf8 (WP40); merged after WP41/43/45.
