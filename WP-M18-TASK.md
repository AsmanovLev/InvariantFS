# WP-M18 — TASK: sweep integration (data RAW→Shadow, live-set iteration)

## Branch / Worktree
- Branch: `wp/m18-sweep-integration`
- Worktree: `/tmp/invfs-wp-m18`
- Base: `main` @ `26ef748`

## Context from compressed orchestration (b6)
WP-M14 merged: fold works, `fold_reclaim_hook` is a no-op stub (M15 fills it). WP-M11: overlay reads delta-first over base. WP-M17: hardlinks via nlink (shared inodes visited once). The DATA plane is UNCHANGED — RAW appends, Shadow sweep, codecs, seals, heat counters all work exactly as before. What changes: the sweep's metadata enumeration switches from v2 record scan (O(live + dead + tombstones)) to v3 base+delta overlay iteration (O(live only)).

## Scope (from WP-M18 spec, impl_docs/WP-M18-sweep-integration.md)

**Reconnect the sweep to the metadata-v3 live set. DATA plane unchanged. Changes: iterate base tree + delta overlay (WP-M11) instead of v2 record scan, request a fold (WP-M14), schedule metadata reclaim (WP-M15).**

Files:
- `src/core/vol_sweep.c` — `vol_sweep_one`, `vol_sweep_file`, `vol_sweep_name_of`, `vol_compute_stats`
- `src/core/vol_read.c` — live-set iteration helper (tree scan + overlay)
- `src/core/vol_dirs.c` — name resolution for `vol_sweep_one`
- `src/core/vol_fold.c` — `fold_request` (WP-M14 trigger, already exists)
- `src/core/vol_reclaim.c` — background reclaim schedule (WP-M15)

**NOT in scope:** DATA-zone codecs/transcodes; fold/reclaim algorithms (call only); v2 mapper sweep paths; two-device placement; g_io_lock concurrency.

## Implementation steps

1. **Live-set iteration helper** in `vol_read.c` (or new `vol_sweep_iter.c`):
   ```c
   // Walk all live inodes via base tree scan + delta overlay
   // Skips deleted entries (delta DELETE flag)
   // For nlink>1 inodes: track visited set to avoid double-sweep
   int vol_v3_iter_live_inodes(invfs_volume *v,
       int (*cb)(invfs_volume *v, uint64_t inode_id, const char *name, void *ctx),
       void *ctx);
   ```
   - Base tree scan: iterate inode keys in base B-tree (full range)
   - Delta overlay: consult delta index for each key; DELETE flag = skip
   - nlink tracking: use a simple `uint64_t *visited` array with linear probe (small for test volumes); clear on each sweep pass
   - Return error from callback to stop iteration

2. **Wire into `vol_sweep_name_of` (vol_sweep.c:1389)** for v3:
   - On v3, use the new iterator to find name from inode id (reverse dirent lookup)
   - On v2, keep existing path unchanged

3. **Sweep pass driver** — modify the sweep daemon loop (`tools/invf-sweep.c` or the embedded daemon in `vol_sweep.c`):
   - For each live inode: call `vol_sweep_one(v, id, name)`
   - After a full pass (or on interval): call `vol_v3_fold_request()` so delta is bounded
   - After fold: call `vol_reclaim_schedule()` (M15 hook)

4. **`fold_request`** already exists in `vol_fold.c` — verify it's wired for external callers.

5. **`vol_compute_stats`** (vol_sweep.c) for v3: use base tree size + delta index size instead of v2 inode area scan.

6. **Keep all existing DATA semantics unchanged**: RAW→Shadow draining, codec/class clustering, seals, heat counters, containerpack dispatch all work exactly as before — only the metadata enumeration changes.

7. **Shared inodes (nlink > 1)**: visited set ensures `vol_sweep_one` is called once per inode, not once per hardlink.

## Validation

1. `make test` — unit case: live-set iteration (base+delta; deletes skipped; nlink-shared visited once).

2. Build: `make -j$(nproc)` — clean, no warnings.

3. E2E gates (run with `INVFS_E2E_AGENT=wp-M18-sweep-integration`):
   ```
   bash tools/run-e2e.sh tools/test-textzone.sh
   bash tools/run-e2e.sh tools/test-heat.sh
   bash tools/run-e2e.sh tools/test-seal.sh
   bash tools/run-e2e.sh tools/test-sweep-mapper.sh
   bash tools/run-e2e.sh tools/test-meta-v3.sh
   ```

4. Scenario: `INVFS_V3=1 invf-mkfs t.img` → write/delete/rewrite, sweep, verify content bit-exact and space reclaimed (no dead-record growth); `invf-fsck` clean; `df` recovers after sweep+fold+reclaim.

## Deliverable format (AGENTS §1.6)

```
WP: wp/m18-sweep-integration
Files changed: (list of files + one-line description per file)
Tests run:
  $ make -j$(nproc)
  ... (output)
  $ make test
  ... (output)
  $ INVFS_E2E_AGENT=wp-M18-sweep-integration bash tools/run-e2e.sh tools/test-textzone.sh
  ... (output)
Result: PASS
Remaining TODOs: (list any deferred decisions or open issues)
```

## Key source touchpoints verified in this tree
- `vol_sweep.c:850` — `vol_sweep_one(v, id, name)`
- `vol_sweep.c:1389` — `vol_sweep_name_of` (name lookup for sweep driver)
- `vol_sweep.c:187` — `vol_sweep_file`
- `vol_fold.c` — `vol_v3_fold_request` exists (trigger)
- `vol_delta.c` — delta index structure; `vol_delta_iter` for overlay scan
- `vol_btree.c` — base tree scan (existing btree scan API)
- `vol_dirs.c` — dirent reverse lookup
- `WP-M17` — nlink tracking for shared inodes
