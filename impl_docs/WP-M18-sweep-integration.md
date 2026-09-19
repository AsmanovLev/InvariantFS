# WP-M18 — sweep integration: data RAW→Shadow, live-set iteration

**Branch:** `wp/M18-sweep-integration`
**Worktree:** `/tmp/invfs-wp-M18`
**Severity:** HIGH (sweeping a stale set moves/frees the wrong blocks)
**Source:** `impl_docs/design-meta-v3.md` §1, §2, §5, §8, §12, §13, §14, §17
**Estimated effort:** large

---

## Scope

Reconnect the sweep to the metadata-v3 live set. The **DATA plane is
unchanged** (RAW drains into Shadow, clustering/reclaim as today). What
changes is enumeration and metadata work: iterate the base tree + delta
overlay (WP-M11) instead of the v2 record scan, request a fold (WP-M14), and
schedule metadata reclaim (WP-M15).

Files:
- `src/core/vol_sweep.c` — `vol_sweep_one` (`vol_sweep.c:850`),
  `vol_sweep_file` (`:187`), `vol_sweep_name_of` (`:1389`),
  `vol_compute_stats`.
- `src/core/vol_read.c` — live-set iteration helper (tree scan + overlay).
- `src/core/vol_dirs.c` — name resolution for `vol_sweep_one`.
- `src/core/vol_fold.c` — `fold_request` (WP-M14 trigger).
- `src/core/vol_reclaim.c` — background reclaim schedule (WP-M15).

## Why

§1: "a sweep that walks the live set instead of the whole log". In v2 the
sweep scans records and must reason about tombstones/dead entries; with v3 the
live set is the base tree plus the small delta, so iteration is O(live) and
the sweep never sees dead records. §5 makes the sweep a fold trigger, which
bounds delta growth (D1/D2). DATA movement itself does not change.

## Design

**Live-set iteration (frozen shape):**
```
for id, row in base_scan(INODE_RANGE) merged with delta index:
    if row.deleted or not latest: skip
    vol_sweep_one(v, id, name)        # existing driver
```
Shared inodes (nlink > 1, WP-M17) are visited **once**; the design does not
specify a dedup mechanism, so this WP keeps a visited-id set for one pass.

**Fold / reclaim hooks:** after a pass (or on its interval) call
`fold_request()` so the delta is bounded (D2 secondary trigger). Metadata
reclaim (WP-M15) runs after fold publishes; DATA reclaim stays the existing
RAW/Shadow path.

**Unchanged data semantics:** RAW→Shadow draining, codec/class clustering,
seals, heat counters, containerpack dispatch and `vol_compute_stats` keep
their behaviour; only the metadata enumeration feeding them changes. AGENTS
§2.4/§2.5 still describe the observable lifecycle.

## Validation

1. `make test` — a live-set iteration unit case (base+delta; deletes
   skipped; nlink-shared visited once).
2. `INVFS_V3=1 invf-mkfs t.img` → write/delete/rewrite, sweep, verify
   content bit-exact and space reclaimed (no dead-record growth); fsck
   clean; `df` recovers after sweep+fold+reclaim.
3. `bash tools/run-e2e.sh tools/test-textzone.sh`;
   `bash tools/run-e2e.sh tools/test-heat.sh`;
   `bash tools/run-e2e.sh tools/test-seal.sh`;
   `bash tools/run-e2e.sh tools/test-sweep-mapper.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- DATA-zone codecs/transcodes/containerpacks and parity/seal algorithms.
- Fold algorithm (WP-M14) and reclaim mechanics (WP-M15) — call them only.
- v2 mapper/`vol_meta_merge.c` sweep paths (WP-M21 deletes).
- Two-device placement (WP-M19).
- `g_io_lock`/concurrency (WP-M20).

## Coordination notes

- Subagent ID: `wp-M18-sweep-integration`;
  `INVFS_E2E_AGENT=wp-M18-sweep-integration`.
- E2E gates: `test-textzone.sh`, `test-heat.sh`, `test-seal.sh`,
  `test-sweep-mapper.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M11** (overlay live set), **WP-M14** (fold request),
  **WP-M15** (reclaim scheduling), **WP-M17** (shared inodes).
- Blocks: WP-M19 (migration), WP-M22 (full e2e).
- Must not change codec/transcode policy, and must not sweep DATA from a
  metadata set that excludes the delta.
