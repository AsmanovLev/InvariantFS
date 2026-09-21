# WP-M20 — TASK: concurrency (drop g_io_lock, lock-free base reads + delta append)

## Branch / Worktree
- Branch: `wp/m20-concurrency`
- Worktree: `/tmp/invfs-wp-m20`
- Base: `main` @ `26ef748`

## Context from compressed orchestration (b6)
Every FUSE op currently serializes on `g_io_lock` (`fuse_fs.c:31`). The two-tier design allows lock-free base reads because the base is immutable between folds. Delta append is the only writer critical section. The staged removal plan: `g_io_lock` → `base lock-free + delta append lock` → `AG-sharded delta/RAW arenas`. Full metadata sharding is explicitly rejected.

## Scope (from WP-M20 spec, impl_docs/WP-M20-concurrency.md)

**Stage out the global `g_io_lock` from metadata paths: base reads are lock-free (immutable base), delta append is the only writer critical section, shardable per-thread like RAW.**

Files:
- `src/cli/fuse_fs.c` — remove/shrink `g_io_lock` from read and metadata paths
- `src/core/vol_delta.c` — per-shard append arenas + atomic reserve
- `src/core/vol_btree.c` — assert lookup takes no lock; no shared mutable read-path state
- `src/core/vol_fold.c` — publish is atomic; readers drain the old base
- `src/core/volume_internal.h` / `vol_write.c` — shard state; RAW arena sharding shared with delta sharding

**NOT in scope:** full per-shard metadata/namespace sharding (rejected); network/multi-host; new on-disk formats; fold/reclaim algorithms beyond atomicity contracts; v2 path.

## Design decisions to make and document

1. **Memory ordering**: use `volatile` + compiler barriers for now; document that release/acquire semantics are the target. `__sync_synchronize()` or C11 `atomic_thread_fence`.

2. **Delta append granularity**: per-record vs per-batch. Measure contention with a test. Document the choice.

3. **Residual `g_io_lock` uses**: identify which FUSE ops still need serialization (non-metadata paths: RAW data write, bitmap update, etc.) and keep `g_io_lock` only for those. Document every remaining use.

4. **Reader epoch**: base reads sample `g_fold_epoch` (from M15) to detect fold events. If epoch changed mid-read, retry from base root. This is the "drain naturally" mechanism.

## Implementation steps

1. **Audit `g_io_lock` uses** in `fuse_fs.c`:
   - Categorize each use: metadata read (→ lock-free), metadata write (→ delta append lock), data write (→ keep lock), bitmap/alloc (→ keep lock)
   - Produce a table of remaining uses

2. **Add `g_delta_append_lock`** (per-shard, initially one global):
   - `pthread_mutex_t g_delta_lock = PTHREAD_MUTEX_INITIALIZER;`
   - All delta appends (vol_delta_append) go through this lock
   - All delta reads (vol_delta_lookup, vol_delta_iter) are lock-free

3. **Implement per-shard delta append** (if measurable contention):
   - Each thread gets a shard id (0..N-1) via `pthread_setspecific`
   - Each shard has its own segment + atomic reserve
   - Shared index updated atomically per key (CAS)
   - Start with global lock; profile; if no contention, keep global

4. **Base reads are lock-free**:
   - `btree_search` (WP-M3) reads only immutable pages — no lock needed
   - Delta lookup reads the index — index entry published only after record bytes written (WP-M10 CRC covers replay path)
   - If fold occurs during read: `g_fold_epoch` check; retry if changed

5. **Remove `g_io_lock` from metadata read paths** in `fuse_fs.c`:
   - `invf_read`, `invf_readdir`, `invf_getattr` — no lock
   - Keep: `invf_write`, `invf_create`, `invf_unlink`, `invf_setattr` — still need serialization (data + delta append)

6. **Fold publish is atomic** (`mbuf_root_publish` already atomic via RT30 double-slot). Readers sample `g_fold_epoch` before and after; if changed, retry from new root.

7. **Add concurrency test** (`btree_test.c` or new):
   - N reader threads doing base lookups concurrently with writer thread doing delta appends + folds
   - No torn values, no crashes
   - Run under TSan if available (report if not)

## Validation

1. `make test` — concurrency test: N readers vs 1 writer, no torn value, no crash; fold concurrently with readers.

2. Build: `make -j$(nproc)` — clean, no warnings.

3. E2E gates (run with `INVFS_E2E_AGENT=wp-M20-concurrency`):
   ```
   bash tools/run-e2e.sh tools/test-writepath.sh
   bash tools/run-e2e.sh tools/test-meta-v3.sh
   bash tools/run-e2e.sh tools/test-flakey.sh   # queued if lock held (AGENTS §1.5)
   ```

4. Stress: `INVFS_V3=1 invf-mkfs t.img` → multi-threaded create/write/stat/readdir stress; content bit-exact; `invf-fsck` clean.

## Deliverable format (AGENTS §1.6)

```
WP: wp/m20-concurrency
Files changed: (list of files + one-line description per file)
Tests run:
  $ make -j$(nproc)
  ... (output)
  $ make test
  ... (output)
  $ INVFS_E2E_AGENT=wp-M20-concurrency bash tools/run-e2e.sh tools/test-writepath.sh
  ... (output)
Result: PASS
Remaining TODOs: (list any deferred decisions, remaining g_io_lock uses, open issues)
```

## Key source touchpoints verified in this tree
- `fuse_fs.c:31` — `g_io_lock` definition
- `fuse_fs.c:85` — first lock site (invf_read_dir — metadata, can remove)
- `fuse_fs.c:1180` — `invf_read` (data read — keep for now)
- `fuse_fs.c:1386` — `invf_write` (data+delta — keep for now)
- `vol_delta.c` — `vol_delta_append` (writer critical section)
- `vol_btree.c` — `btree_search` (lock-free read of immutable pages)
- `vol_fold.c` — `mbuf_root_publish` (atomic via RT30 double-slot)
- `volume_internal.h` — `g_fold_epoch` added by M15
