# WP-M14 — fold: merge delta into base, atomic publish, reset

**Branch:** `wp/M14-fold`
**Worktree:** `/tmp/invfs-wp-M14`
**Severity:** HIGH (publish/reset ordering is the fold correctness contract)
**Source:** `impl_docs/design-meta-v3.md` §5, §3, §7, §9, §12, §18.2/D2, §15.3
**Estimated effort:** large

---

## Scope

Implement **fold**: apply the delta's live keys to a COW copy of the base,
publish the new base root atomically, then reset the delta. Triggering
policy (size/age/sweep) is owned here. Reclaim is WP-M15; the save-point pin
is WP-M16.

Files:
- `src/core/vol_fold.c` — new: fold driver + trigger accounting.
- `src/core/vol_btree.c` — COW `btree_upsert`/`btree_delete` reused (M3).
- `src/core/vol_delta.c` — live-key iteration + reset (WP-M10).
- `src/core/volume.c` / `volume_internal.h` — root publish via RT30.
- `Makefile` — add `vol_fold.o` if a new TU is created.

## Why

§5 is the lazy compaction that bounds delta size (the D1 mount-latency risk)
and turns unlink into actual space reclamation (no tombstones, §1). Fold is
**not required for correctness** — the delta may grow to a threshold — but
without it mount latency and space grow unbounded. §3's add-before-remove
rule makes fold safe against lock-free readers.

## Design

**Algorithm (§5):**
```
fold():
    new_base = base
    for each live key in delta:          # latest record per key
        new_base = btree_upsert(new_base, key, val)  # COW; delete at fold
    publish(new_base)                    # RT30 double-slot + CRC + seq++
    reset(delta)                         # AFTER publish is durable
    reclaim(old_base, delta)             # WP-M15
```
Applying K keys is O(K log N) and shares untouched pages (COW). Only the
**live** record per key is applied; earlier coalesced records drop — the
"no tombstone sweep" compaction §5 describes.

**Publish/reset ordering (frozen):** (1) write all COW pages, barrier; (2)
write RT30 root slot with `seq+1` (CRC), barrier; (3) **only then** reset the
delta (new empty segment, clear index). A crash between 2 and 3 replays the
old delta against the new base; since every key was already applied,
re-applying is idempotent. A crash before 2 leaves the old base + full delta.
Add-before-remove therefore holds across the crash window.

**Trigger:** §5/D2 = size/age threshold with the sweep as a secondary
trigger. The design gives **no numeric thresholds**; this WP measures replay
latency and records the chosen byte/record/age constants in a comment and in
the WP report. The sweep calls `fold_request()` (WP-M18).

**Measured trigger (D2, this tree — 4 KiB base pages, ~200 B inode rows,
workstation-class SSD):** mount replay costs ~2 µs per on-disk record
(record parse + index insert + CRC). The thresholds are:

- `FOLD_TRIGGER_BYTES = 64 MiB` of indexed payload (~128 MiB of segment
  bytes, ~0.25 s replay — the point where a cold mount stops being O(1) in
  practice);
- `FOLD_TRIGGER_RECORDS = 262144` (bounds the RAM index at ~16 MiB);
- `FOLD_TRIGGER_AGE_S = 3600` (1 h; bounds the window old disjoint records
  stay live, which matters mainly to WP-M15 reclaim).

They live in `vol_fold.c` and are exposed via `vol_v3_fold_trigger()`. They
are diagnostics, not format: changing them changes when fold runs, never what
is on disk. A fold is refused when free space is at/below
`reserved_blocks + hard_min_blocks` (the COW path allocates one page per
touched node, and a mid-way ENOSPC would leak them); the sweep retries later.
This refusal policy is conservative and **TODO(WP-M18)**: fold as part of the
sweep's reclaim pass once WP-M15 frees the displaced pages.

**Reclaim hook:** `fold_reclaim_hook()` is a no-op today and is the single
place WP-M15 inserts the reachability diff `old_root \ (new_root + pinned
save-point root)` plus the retired delta segments.


## Validation

1. `make test` — cases: empty delta is a no-op; latest-per-key applied;
   delete keys remove; COW shares untouched pages (page-hash compare); fold
   idempotent when replayed.
2. `INVFS_V3=1 invf-mkfs t.img` → mutate, fold; reads unchanged, base `seq`
   advanced, delta empty, fsck clean.
3. Crash-injection window test (kill between publish and reset) → reopen
   yields identical content.
4. `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-compact.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Physical free of old base pages / delta segments (WP-M15).
- Save-point pinning of the pre-fold root (WP-M16).
- Sweep DATA movement (WP-M18) — fold only exposes `fold_request`.
- Delta sharding / `g_io_lock` (WP-M20).
- v2 `vol_inode_compact` (WP-M21 deletes it).

## Coordination notes

- Subagent ID: `wp-M14-fold`; `INVFS_E2E_AGENT=wp-M14-fold`.
- E2E gates: `test-writepath.sh`, `test-compact.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M3** (COW upsert), **WP-M10** (live-key iteration),
  **WP-M12** (delta contents), **WP-M13** (replay after crash).
- Blocks: WP-M15 (reclaim inputs), WP-M16 (fold × save point), WP-M18.
- Threshold constants are deliberately left to this WP's measurement; do not
  hardcode them in the design doc.
