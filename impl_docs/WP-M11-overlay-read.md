# WP-M11 — overlay read: delta → base, add-before-remove ordering

**Branch:** `wp/M11-overlay-read`
**Worktree:** `/tmp/invfs-wp-M11`
**Severity:** HIGH (a wrong overlay returns stale bytes or ENOENT)
**Source:** `impl_docs/design-meta-v3.md` §3, §5, §9, §12, §15.3
**Estimated effort:** large

---

## Scope

Make every read consult the **delta index first, then the base** (delta
wins), and `readdir` a **merge** of the delta's key range with the base's
ordered range. Reads take no lock (base immutable between folds). No fold,
reclaim, or lock removal (WP-M14/M15/M20).

Files:
- `src/core/vol_delta.c` — `delta_lookup`, `delta_range` cursor (WP-M10).
- `src/core/vol_btree.c` — expose `bt_scan` (WP-M3) for the merge.
- `src/core/vol_read.c` — inode/recipe lookup goes overlay-first.
- `src/core/vol_dirs.c` — `vol_list_dir` (`vol_dirs.c:113`) merges ranges.
- `src/core/volume.h` — public signatures unchanged.

## Why

§3 defines `lookup(key): delta_index first, else base`. During a concurrent
fold, correctness rests on the **add-before-remove** rule (§3, §5): fold
writes the key into the new base *before* removing it from the delta, so
`read = delta → base` is correct under any interleaving with no read lock.
§13 lists `stat`/lookup and big-dir `readdir` as operations that must stop
rebuilding RAM hashes.

## Design

**Point read:**
```
lookup(key):
    r = delta_lookup(key)
    if r: return r.delete ? ENOENT : r.value   # recent wins
    return btree_search(base, key)             # O(log N)
```
Delete records shadow the base — a delete is a value at the overlay layer,
not a missing delta entry. The design fixes "delta wins" but is **silent on
whether delete+value coalesce**; the WP-M10 index keeps one latest record
per key, so the overlay sees a single value.

**Ordered readdir:** merge two ordered streams, delta keys and base keys in
`[dir_lo, dir_hi)` (WP-M6 key `parent||name`). Delta is small (§16), so the
merge is O(k + log N); on equal names the delta wins. The design says
"merge" but does **not** fix tie-breaking beyond delta-ahead; this WP
freezes "delta wins on equal key" and documents it.

**Consistency with fold:** no lock. If fold published a key into the new
base but has not removed it from the delta, the delta copy is returned
(same value); if it removed the delta entry, the new base already has it.
Both interleavings yield the correct value (§3); readdir is symmetric.

## Validation

1. `make test` — WP-M2/M3/M10 binaries plus cases: delta value shadows
   base; delete shadows base; delta-only; base-only; readdir merge sorted +
   deduped (delta wins); a base key deleted in delta disappears.
2. A staged **concurrent-fold** interleaving test proves add-before-remove
   never yields a transient ENOENT.
3. `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-rename.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Fold implementation (WP-M14) — only consume the ordering rule.
- Wiring mutations into the delta (WP-M12).
- Reclaim of old base pages (WP-M15).
- Dropping `g_io_lock` / sharding (WP-M20) — API is lock-free; the FUSE
  lock may remain until M20.
- The WP-M10 record format.

## Coordination notes

- Subagent ID: `wp-M11-overlay-read`; `INVFS_E2E_AGENT=wp-M11-overlay-read`.
- E2E gates: `test-writepath.sh`, `test-rename.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M10** (delta/index API), **WP-M3** (base scan),
  **WP-M5/M6/M7** (key orderings).
- Blocks: WP-M12 (mutations visible only through this overlay), WP-M18.
- The merge tie-break is frozen here; WP-M14's fold uses the same rule.
