# WP-M15 — TASK: reclaim (reachability diff + delta segment free)

## Branch / Worktree
- Branch: `wp/m15-reclaim`
- Worktree: `/tmp/invfs-wp-m15`
- Base: `main` @ `26ef748`

## Context from compressed orchestration (b6)
WP-M14 merged (`328c22f`): fold publishes a new base, then calls `fold_reclaim_hook(v, old_root, new_root)` — currently a no-op that leaks all displaced pages. `btree_reclaim(v, old_root, keep_root)` already exists in `vol_btree.c:1187` (unit-tested in `btree_test.c:581`), using a mark+free bitmap walk. `bt_mark_rec` and `bt_free_rec` are the core primitives. The fold delta reset (`fold_delta_reset`) stops naming the old delta chain but does NOT free its segments.

WP-M16 will supply a `pinned_root` (the save-point base root). Delta segment free needs `delta_end` — a new field WP-M16 adds to the save-point record.

## Scope (from WP-M15 spec, impl_docs/WP-M15-reclaim.md)

**Free metadata that is no longer reachable: base pages reachable from neither the current base root nor a pinned save-point root, and delta segments below the save-point `delta_end` once a fold has published.**

Files:
- `src/core/vol_reclaim.c` — new: mark/scan diff, free scheduling
- `src/core/vol_btree.c` — `btree_reclaim` already exists; wire into reclaim
- `src/core/vol_fold.c` — implement `fold_reclaim_hook` body (currently TODO/WP-M15)
- `src/core/vol_delta.c` — segment free below `delta_end`
- `src/core/vol_metabuf.c` — `mbuf_free` (WP-M2)
- `Makefile` — add `vol_reclaim.o` if new TU created

**NOT in scope:** save-point creation/rollback (WP-M16); data-block/RAW/Shadow reclaim (WP-M18); fold algorithm (WP-M14); general refcount trees; v2 `vol_inode_compact`.

## Design decisions to make and document

1. **Reader drain** (design is silent): a page is freeable only after (a) fold has published newer root AND (b) no reader holds the old root. Implement a simple epoch counter: each fold bumps `g_fold_epoch`; readers snapshot it on entry and are considered "in flight" until they release. Reclaim waits for in-flight readers. Document the choice.

2. **Delta segment free**: delta segments are chained via `prev_pba` (vol_delta.c). After fold publishes, segments with offset < `savepoint.delta_end` (0 if no save point) are unreachable. Walk the chain from `delta_pba`, collect segment pbas, free them via `mbuf_free`. The delta chain head is `v->rt30.delta_pba` at fold time — save a copy before `fold_delta_reset` clears it.

3. **Pinned root**: until WP-M16, `pinned_root` is NULL and the diff is against `current_base_root` only.

## Implementation steps

1. Create `src/core/vol_reclaim.c` with:
   - `vol_reclaim_mark_and_free(v, old_root, keep_root, pinned_root)` — bitmap-allocate seen array, mark from `current_base_root`, also mark from `pinned_root` if non-NULL, then scan all allocated metadata pages, free any not marked
   - `vol_reclaim_delta_segments(v, delta_pba, delta_end)` — walk the delta chain, free segments with offset < `delta_end`
   - `g_fold_epoch` global counter + reader snapshot API

2. Implement `fold_reclaim_hook` in `vol_fold.c:264`:
   ```c
   static void fold_reclaim_hook(invfs_volume *v, invfs_blkptr old_root,
                                 invfs_blkptr new_root)
   {
       /* wait for in-flight readers (g_fold_epoch drain) */
       vol_reclaim_mark_and_free(v, old_root, new_root, v->pinned_root);
       /* delta segments freed separately after fold_delta_reset captures delta_pba */
   }
   ```

3. In `fold_v3_fold` (vol_fold.c), save `v->rt30.delta_pba` before `fold_delta_reset`, then call `vol_reclaim_delta_segments(v, saved_delta_pba, 0)` after reset.

4. Add `vol_reclaim_delta_segments` to `vol_delta.c` (or call from fold after saving the head).

5. Add `pinned_root` field to `invfs_volume` (volume_internal.h) — initially NULL, WP-M16 sets it.

6. Add `vol_reclaim.o` to Makefile CORE if new TU.

## Validation

1. `make test` — extend btree_test or add reclaim-specific cases:
   - mark from two roots (current + pinned)
   - unreachable freed, reachable kept
   - delta segment below delta_end freed
   - double-free rejected (mbuf_free idempotent)
   - reader drain: block reclaim while reader holds old root

2. Build: `make -j$(nproc)` — clean, no warnings.

3. E2E gates (run with `INVFS_E2E_AGENT=wp-M15-reclaim`):
   ```
   bash tools/run-e2e.sh tools/test-rollback.sh
   bash tools/run-e2e.sh tools/test-meta-v3.sh
   bash tools/run-e2e.sh tools/test-writepath.sh
   ```

4. Stress: `INVFS_V3=1 invf-mkfs t.img` → mutate + fold repeatedly; metadata usage bounded (no monotone growth); `invf-fsck` clean throughout.

## Deliverable format (AGENTS §1.6)

```
WP: wp/m15-reclaim
Files changed: (list of files + one-line description of change per file)
Tests run:
  $ make -j$(nproc)
  ... (output)
  $ make test
  ... (output)
  $ INVFS_E2E_AGENT=wp-M15-reclaim bash tools/run-e2e.sh tools/test-meta-v3.sh
  ... (output)
Result: PASS
Remaining TODOs: (list any deferred decisions or open issues)
```

## Key source touchpoints verified in this tree
- `vol_btree.c:1187` — `btree_reclaim(v, old_root, keep_root)` exists
- `vol_fold.c:264` — `fold_reclaim_hook` is the TODO/WP-M15 stub
- `vol_fold.c:278` — `fold_delta_reset` clears `delta_pba` (need to save before)
- `vol_delta.c` — delta chain via `prev_pba`, head is `rt30.delta_pba`
- `vol_metabuf.c` — `mbuf_free(v, pba)` exists (WP-M2)
- `btree_test.c:581` — `test_reclaim` unit harness
