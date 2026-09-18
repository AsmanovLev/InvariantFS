# WP42 — invf-sweep + sweep engine: mapper-aware record walks

Branch: `wp/42-sweep-mapper` · Worktree: `/home/user/invfs-wp42`

## Scope (ONLY these)
- `tools/invf-sweep.c` — the collector around line 700-800
  (`area_end = vol_inode_area_pos(vol)` bound, the `live entries: N (of M
  walked)` log at ~798) and the size accounting at ~983
  (`used = vol_inode_area_pos - vol_inode_area_start`).
- `src/core/vol_sweep.c` — the sweep-engine record walks that still use
  the legacy `[inode_area_start, inode_area_pos)` bounds:
  - hint-validation around lines 108-120 (`pos >= inode_area_start*BLOCK
    && pos + sizeof(invfs_inode_rec) <= inode_area_pos` — on mapper
    volumes this REJECTS every position in an extent),
  - the walk at ~148, ~583, ~1286, ~1487.
  Do NOT touch `vol_compute_stats` (WP41 already converted it; lines
  ~1553-1790 including the `vol_records_walk` + legacy branch).

## Why
`invf-sweep` (the daemon and the offline tool) is the component that
must drain RAW into SHADOW and re-encode. On a v0.3.0+ mapper volume it
prints `live entries: 0 (of 0 walked)` because the collector's end bound
is the legacy inode area and the records live in dynamic meta extents.
Result: the sweep never sees any file, never populates dev1, and the
whole compression/consolidation pipeline is dead on real volumes
(verified on the 15 GiB stage3 volume: dev1 physically 0 bytes).

## Design
- Reuse the WP40 walker `vol_records_walk(v, cb, ctx)` (see
  `src/core/volume.h` comment and WP41's usage at vol_sweep.c:1762 as
  the reference pattern). It already walks every mapper extent via
  `vol_inode_next` and falls back to the legacy contiguous region.
- tool collector: replace the manual `while` with the walker; the
  callback retains the existing per-record policy (live-version check
  via `vol_find`/`idx_get_id`, tombstone skip, name snapshots, etc.).
  Keep the `live entries: N (of M walked)` log contract.
- hint-validation in vol_sweep.c: accept a position if it lies inside
  ANY mapper extent (walk the mapper table via `meta_mapper_get` as
  `vol_inode_next` does) OR inside the legacy region; simplest correct
  rule: drop the hint when `met0_present && meta_mapper` and rely on
  the walker, or validate per extent. State your choice in the report.
- `used` accounting at tools/invf-sweep.c:983 must not go negative on
  mapper volumes where `inode_area_start` is the legacy base — compute
  from the walker's record positions/bytes instead (e.g. accumulate
  record bytes/sizes in the callback), or gate the legacy formula.
- No behavior change on legacy format_version=0 volumes.

## Validation
```
$ make -j$(nproc) && make test         # 4722 checks, 0 failures
$ INVFS_E2E_AGENT=wp42 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
$ INVFS_E2E_AGENT=wp42 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
```
`test-sweep-mapper.sh` (WP45) currently SKIPs while the stats population
is 0; with WP41 merged it ASSERTS. Expected after WP42: sweep counters
`swept`/`hashed` NONZERO, logical bytes + population preserved, 20 seeded
names bit-exact, fsck clean — and the test flips from SKIP to PASS.

## Out of scope
- vol_compute_stats (WP41, already fixed).
- vol_heat / vol_dedupe (WP43/WP44, already fixed).
- vol_dirs / vol_records siblings / vol_tier / vol_ast / vol_textzone /
  vol_read hint-validation (a separate follow-up WP — do not touch here).

## Coordination notes
- e2e only via `INVFS_E2E_AGENT=wp42 bash tools/run-e2e.sh [--bg] <suite>`,
  never bypass the flock.
- Based on main@827efa6 (WP40+41+43+44+45 merged) — no vol_sweep.c
  conflict remains.
- COMMIT on the branch when done (a /tmp wipe destroyed uncommitted work
  once already; worktrees now live in /home/user).
