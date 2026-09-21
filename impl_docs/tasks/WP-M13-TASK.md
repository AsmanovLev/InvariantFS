# WP-M13 — TASK: mount replay of the delta

## Branch / Worktree
- Branch: `wp/m13-mount-replay`
- Worktree: `/tmp/invfs-wp-m13`
- Base: `main` @ `c900f35`

## Context
WP-M10 (delta log) implemented `vol_delta_mount()` which replays the delta segment chain 
at mount time, building the in-memory index. WP-M14 (fold) calls `vol_delta_mount` 
during `vol_v3_fold_request` to rebuild the empty index after a fold reset.

The question is whether `vol_delta_mount` is called at ordinary (non-fold) vol_open 
for v3 volumes. Verify this and complete the wiring if needed.

## Scope (from WP-M13 spec, impl_docs/WP-M13-mount-replay.md)

**At vol_open for v3: read base root from RT30 double-slot, replay delta into 
in-memory index, truncate torn tail. Mount cost = O(1) base + O(delta). 
Drop the O(N) v2 scan from the v3 branch.**

Files:
- `src/core/volume.c` — v3 open path; ensure delta replay at mount
- `src/core/vol_delta.c` — `vol_delta_mount` (already exists from WP-M10)
- `src/core/vol_btree.c` — RT30 root slot read
- `src/core/volume_internal.h` — delta/replay state fields if needed
- `tools/test-meta-v3.sh` — add mount-replay leg

NOT in scope: fold (WP-M14), reclaim (WP-M15), save point (WP-M16), 
repair of corrupt base tree (WP-M4: detect only).

## Implementation steps

1. **Verify delta replay at mount**: In the v3 `vol_open` path, confirm that
   `vol_delta_mount()` is called after the RT30 base root is loaded. If the v3 
   open skips this (e.g., goes straight to empty namespace), wire it in.

2. **Verify no O(N) v2 scan on v3**: The v2 mount scans the inode area building
   `nbuck`/`dbuck`/`ibuck` hash indexes. For v3 (`VOLF_V3`), this scan must NOT
   run. Confirm this is gated on `!(sb.vol_flags & VOLF_V3)`.

3. **Torn tail handling**: `vol_delta_mount` (WP-M10) already truncates torn 
   tails at the last valid record (per-record CRC). Verify this is working by
   checking the implementation in `vol_delta.c`.

4. **Double-replay idempotency**: `vol_delta_mount` must be safe to call twice 
   (replay from scratch = same result). Verify this.

5. **Add e2e leg**: In `tools/test-meta-v3.sh`, add a leg that:
   - Creates files on a v3 volume
   - Unmounts
   - Remounts
   - Verifies all files still present and bit-exact
   This validates that delta replay survives across mount cycles.

## Validation

1. `make test` — delta replay unit cases (already covered by delta_test: 
   replay equals live index, torn tail truncates, bad CRC stops, double replay 
   idempotent)

2. Build: `make -j$(nproc)` — clean, no warnings

3. E2E gates:
   ```
   INVFS_E2E_AGENT=wp-M13-mount-replay bash tools/run-e2e.sh tools/test-meta-v3.sh
   INVFS_E2E_AGENT=wp-M13-mount-replay bash tools/run-e2e.sh tools/test-writepath.sh
   ```

4. Specific scenario: `INVFS_V3=1 invf-mkfs t.img` → write files → unmount → 
   remount → all files present and correct; root `seq` unchanged by replay

## Deliverable format (AGENTS §1.6)

```
WP: wp/m13-mount-replay
Files changed: (list)
Tests run:
  $ make -j$(nproc) ... (output)
  $ make test ... (output)
  $ INVFS_E2E_AGENT=wp-M13-mount-replay bash tools/run-e2e.sh tools/test-meta-v3.sh ... (output)
Result: PASS
Remaining TODOs: (list)
```

## Key source touchpoints
- `vol_delta.c:vol_delta_mount` — already exists (WP-M10)
- `vol_fold.c` — calls `vol_delta_mount` after fold reset (WP-M14)
- `volume.c:vol_open` — v3 open path
- `volume_internal.h:331-337` — nbuck/dbuck/ibuck (v2 only)
