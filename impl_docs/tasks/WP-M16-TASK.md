# WP-M16 — TASK: save point + rollback

## Branch / Worktree
- Branch: `wp/m16-savepoint-rollback`
- Worktree: `/tmp/invfs-wp-m16`
- Base: `main` @ `26ef748`

## Context from compressed orchestration (b6)
No v3 save-point exists yet. The v2 rollback (`vol_rollback.c`) uses CKP0 + retention registry, completely different mechanism. The new v3 save point is `{base_root, delta_end, flags}` with a new block-0 descriptor `SPT0`. WP-M14 fold publishes new bases and calls `fold_reclaim_hook` (M15). WP-M15 needs `pinned_root` from this WP.

## Scope (from WP-M16 spec, impl_docs/WP-M16-savepoint-rollback.md)

**Implement the single save point `{base_root, delta_end, flags}` and `invf-rollback`: capture, pin the pre-fold base across folds, restore it and truncate the delta to `delta_end`, release the pin.**

Files:
- `src/core/invarifs.h` — SPT0 descriptor at a new block-0 offset (e.g. `INVFS_SPT0_OFF`)
- `src/core/vol_rollback.c` — v3 save point capture/restore/drop (rework CKP0 logic)
- `src/core/volume.c` — load SPT0 at open; expose `pinned_root` on volume struct
- `src/core/vol_delta.c` — truncate to `delta_end` (new `vol_delta_truncate`)
- `src/core/vol_fold.c` — pass `pinned_root` to `fold_reclaim_hook`
- `src/core/vol_reclaim.c` — consume `pinned_root` (WP-M15)
- `tools/invf-rollback.c` — v3 path

**NOT in scope:** browsable snapshots/snapshot DAG; reclaim mechanics (WP-M15); fold triggering (WP-M14); v2 CKP0 deletion (WP-M21).

## Design decisions to make and document

1. **SPT0 placement**: find next free 16B-aligned offset in block 0 after PCK0 (`0x3C4`). Check `invarifs.h` for current layout and pick the next free slot.

2. **Capture semantics**: record `{base_root = current RT30 root slot pba, delta_end = delta segment byte offset at capture}` — NOT the delta_pba itself (that changes on fold). `delta_end` = the offset in bytes from the delta segment start to the end of the last live record. This is the truncation point.

3. **Pin at creation**: the pinned root is the base_root captured at save-point creation. Folds that occur while the save point is live do NOT update the pin — it stays at the original captured root. M15 uses exactly this pinned root for reachability diff.

4. **K=1**: refuse a second save point while one is live (return error).

5. **v3 rollback procedure**:
   ```
   1. publish(savepoint.base_root) via RT30 double-slot + seq++
   2. truncate_delta(savepoint.delta_end) — drop post-capture records
   3. rebuild delta index by replay (vol_delta_mount sees new delta_pba)
   4. clear savepoint (write zeroed SPT0)
   ```

## Implementation steps

1. Add `INVFS_SPT0_OFF` and `invfs_spt0` struct to `invarifs.h`:
   ```c
   #define INVFS_SPT0_OFF  0x???  // next free after PCK0
   #define INVFS_SPT_MAGIC "SPT0"
   typedef struct {
       char     magic[4];     // "SPT0"
       uint32_t version;
       uint32_t flags;
       uint64_t base_root;    // pinned RT30 root pba
       uint64_t delta_end;    // delta byte offset at capture
       uint32_t crc32c;
   } invfs_spt0;
   ```

2. Add `pinned_root` and `savepoint_live` fields to `invfs_volume` in `volume_internal.h`.

3. In `volume.c` open path: load SPT0 if present, set `v->pinned_root` and `v->savepoint_live`.

4. Create `vol_spt0.c` (or add to `vol_rollback.c`):
   - `vol_spt0_capture(v)` — record current root + delta_end, write SPT0
   - `vol_spt0_restore(v)` — rollback procedure above
   - `vol_spt0_drop(v)` — clear SPT0, release pin

5. Implement `vol_delta_truncate(v, delta_end)` in `vol_delta.c`:
   - Walk the delta segment chain from current `delta_pba`
   - Find the segment and offset corresponding to `delta_end` bytes
   - Truncate to that point (write zeros / adjust segment length)
   - Reset index (call `vol_delta_close` then `vol_delta_mount` for empty)

6. Wire `pinned_root` into `fold_reclaim_hook` (vol_fold.c):
   - Pass `v->pinned_root` when calling `vol_reclaim_mark_and_free`

7. Update `tools/invf-rollback.c` — add v3 path that calls `vol_spt0_restore`.

8. Update `tools/invf-rollback.1` man page if applicable.

## Validation

1. `make test` — unit cases:
   - capture/restore identity (rollback to itself is no-op)
   - rollback after a fold restores the pinned base
   - `delta_end` truncation drops post-capture records
   - drop releases the pin; second save point refused (K=1)

2. Build: `make -j$(nproc)` — clean, no warnings.

3. E2E gates (run with `INVFS_E2E_AGENT=wp-M16-savepoint-rollback`):
   ```
   bash tools/run-e2e.sh tools/test-rollback.sh
   bash tools/run-e2e.sh tools/test-meta-v3.sh
   bash tools/run-e2e.sh tools/test-writepath.sh
   ```

4. Scenario: `INVFS_V3=1 invf-mkfs t.img` → mutate, save, mutate, fold, rollback; volume matches captured state; `invf-fsck` clean.

## Deliverable format (AGENTS §1.6)

```
WP: wp/m16-savepoint-rollback
Files changed: (list of files + one-line description per file)
Tests run:
  $ make -j$(nproc)
  ... (output)
  $ make test
  ... (output)
  $ INVFS_E2E_AGENT=wp-M16-savepoint-rollback bash tools/run-e2e.sh tools/test-rollback.sh
  ... (output)
Result: PASS
Remaining TODOs: (list any deferred decisions or open issues)
```

## Key source touchpoints verified in this tree
- `vol_rollback.c:835` — existing v2 `vol_rollback` for structure reference
- `vol_fold.c:264` — `fold_reclaim_hook` receives `pinned_root` from volume struct
- `vol_delta.c` — delta chain structure; `vol_delta_close` resets index
- `invarifs.h` — block-0 layout: PCK0 @ 0x3C4, next free after
- `volume_internal.h` — `invfs_volume` struct; add `pinned_root` here
- `tools/invf-rollback.c` — existing CLI; add v3 path
