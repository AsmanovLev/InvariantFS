# WP47 — remaining record-area walkers: mapper-aware via vol_records_walk

Branch: `wp/47-remaining-walkers` · Worktree: `/home/user/invfs-wp47`

## Scope (ONLY these files)
- `src/core/vol_textzone.c` — the owner-record append (`tz_owner_write`,
  around lines 190-210 and 1040-1070) and any inode-area walk it feeds.
- `src/core/vol_dirs.c` — the rename/sibling collector walks (around
  lines 590-620 and 670-760).
- `src/core/vol_records.c` — `vol_delete_siblings()` (around line 735).
- `src/core/vol_read.c` — `vol_read_inode()` (~250) and
  `vol_read_range()` (~1070) fallback scans.
- `src/core/vol_tier.c` (~211, ~576) and `src/core/vol_ast.c` (~337) —
  the `idx_get_id` hint-validation `ip <= v->inode_area_pos` that
  REJECTS mapper-extent positions on v0.3.0+ volumes.

## Why
These are the remaining walkers that still bound themselves by the
legacy contiguous inode area. On a v0.3.0+ mapper volume they see zero
records (or reject valid positions), so:
- `tz_owner_write` appends owner records the mapper cannot later find →
  WP42 had to DISABLE cross-file batch deferral on mapper volumes
  (text PPMd / binary ZSTD+BCJ batching), capping compression.
- rename/delete/siblings on mapper volumes silently miss records.
- `vol_read_*` only work when the id-index hint lands on the exact
  record; any fallback scan is blind.
- tier/ast hints are disabled, forcing the broken fallback path.

## Design
Use `vol_records_walk(v, cb, ctx)` (WP40; see the API comment in
`src/core/volume.h` and the reference call site `vol_sweep.c:1762`).

- **hint-validation** (`vol_tier.c`, `vol_ast.c`): accept a hint when it
  lies inside ANY mapper extent (walk the mapper table via
  `meta_mapper_get`, mirroring `vol_inode_next`) OR inside the legacy
  region; otherwise fall back to `vol_records_walk`.
- **vol_read_inode/vol_read_range**: keep the O(1) hint jump when valid;
  when the hint is absent/invalid, locate the record with
  `vol_records_walk` (match `h->inode_id`) instead of the legacy linear
  loop. Preserve the existing CRC/AST parsing and return codes.
- **vol_dirs rename/sibling**, **vol_records delete_siblings**: replace
  the legacy collector loop with the walker; keep the
  collect-BEFORE-append discipline, name-prefix/`!`-sibling matching,
  supersede checks and result ordering unchanged.
- **vol_textzone tz_owner_write**: after this fix the owner record is
  mapper-visible; then RE-ENABLE the cross-file batch deferral that WP42
  disabled on mapper volumes (look for the mapper-gated bail-out the WP42
  diff added and restore the legacy path), keeping the generic floor as
  the fallback only.
- No behavior change for legacy format_version=0 volumes.

## Validation
```
$ make -j$(nproc) && make test            # 4722 checks, 0 failures
$ INVFS_E2E_AGENT=wp47 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
$ INVFS_E2E_AGENT=wp47 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
$ INVFS_E2E_AGENT=wp47 bash tools/run-e2e.sh tools/test-binbatch.sh
$ INVFS_E2E_AGENT=wp47 bash tools/run-e2e.sh tools/test-textzone.sh
$ INVFS_E2E_AGENT=wp47 bash tools/run-e2e.sh tools/test-dedupe.sh
```
Key acceptance: `test-sweep-mapper` stays 6/0 AND its sweep reports the
batched classes again (not just the generic floor); `test-binbatch` /
`test-textzone` pass; rename/delete legs in `test-meta-extent-walk` or a
small added case succeed on a multi-extent volume.

## Out of scope
- `vol_compute_stats` (WP41), sweep engine (WP42), heat (WP43),
  dedupe (WP44), suites (WP45/46) — all merged.
- The pre-existing `invf-fsck` orphan-count / exit-3 quirk — separate WP.

## Result (as landed)
Walkers converted and verified:
- `vol_ast.c` / `vol_tier.c` hint-validation now accepts mapper-extent
  positions (extent-aware), falling back to `vol_records_walk`.
- `vol_read.c` `vol_read_inode` / `vol_read_range` use the walker when
  the id-index hint is absent/invalid.
- `vol_dirs.c` rename/sibling collectors and `vol_records.c`
  `vol_delete_siblings` use the walker.
- `vol_textzone.c` text-zone GC mark uses the walker; `tz_commit_member`
  appends via `vol_append_slot`.

DEFERRED to a follow-up WP (WP48): re-enabling cross-file batch deferral
on mapper volumes. The first attempt routed `tz_owner_write` and
`wp25_owner_write` through `vol_append_slot`. On a two-device volume the
owner sync runs during `vol_flush` and appends the owner record into the
dev1 shadow extents; that write path fails (owner-sync io-error latch,
inode-area tail re-anchored 3220975400 -> 234983424), so the WP45
big-volume fixture import regressed at ~8k files and the volume latched
READ-ONLY. Both owner appends were reverted to the legacy cursor; the
WP42 mapper gate (files stay on the generic sweep floor) remains until
the owner-append can be made flush-safe.

## Coordination notes
- e2e only through `INVFS_E2E_AGENT=wp47 bash tools/run-e2e.sh [--bg] <suite>`.
- Based on main@5d1e4d5 (WP40-46 merged). COMMIT on the branch when done
  (worktrees live under /home/user so nothing is lost).
