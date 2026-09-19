# WP-M13 — mount replay of the delta

**Branch:** `wp/M13-mount-replay`
**Worktree:** `/tmp/invfs-wp-M13`
**Severity:** HIGH (a missed/duplicated replay record is silent corruption)
**Source:** `impl_docs/design-meta-v3.md` §7, §2, §12, §15.3, §16
**Estimated effort:** medium

---

## Scope

At `vol_open`, read the base root from the RT30 double-slot and **replay the
delta** into the in-memory index, truncating a torn tail at the last valid
record. Mount cost becomes O(1) base + O(delta). No fold, reclaim, or save
point.

Files:
- `src/core/volume.c` — `vol_open`/`vol_close` v3 branch (replaces the
  WP-M1 empty-namespace path); drop the O(N) v2 scan from the v3 branch.
- `src/core/vol_delta.c` — `delta_replay` / `delta_scan_valid` (WP-M10).
- `src/core/vol_btree.c` — read/validate the RT30 root slot.
- `src/core/vol_fsck.c` — fsck replays the same log for its check.
- `src/core/volume_internal.h` — delta/replay state fields.

## Why

§7 fixes recovery: (1) read the base root (higher `seq`, torn → fall back);
(2) replay the delta from the last durable base, per-record CRC, torn tail
truncated at the last valid record; (3) mount bounded by fold cadence.
Without this the delta written by WP-M12 is lost on every unmount/reopen.
§7.4 forbids claiming power-loss safety until the soak passes.

## Design

**Open sequence:**
```
vol_open:
    if !(sb.vol_flags & VOLF_V3): v2 path (unchanged, until WP-M21)
    root = rt30_choose_root()           # higher seq with valid CRC wins
    idx  = empty
    n    = delta_replay(delta_pba, idx) # per-record crc; torn tail -> n
    if truncated: mark DIRTY / note in report (WP-M4 policy)
    mount with base=root, delta=idx
```
`rt30_choose_root` reuses the `l2p_replay` idiom (`volume.c:644-669`):
higher `seq` + valid CRC wins; equal/ambiguous is reported, not guessed.

**Replay correctness:** records apply in **append order**; the WP-M10 index
keeps the latest per key, so replay and live operation agree. A bad CRC ends
replay at that point; bytes after are ignored and the tail is marked for
truncate. Delete records replay into the index exactly as appended (WP-M12),
with no base mutation. Replaying twice must be idempotent (rebuild from
scratch).

**Recovery claim:** per §7.4/D6 this WP does **not** upgrade the AGENTS §2.2
power-loss claim; that waits for WP-M22's `tools/test-flakey.sh` soak.

## Validation

1. `make test` — a `delta_replay` unit case: replay equals a live index;
   torn tail truncates; bad CRC stops; double replay idempotent.
2. `INVFS_V3=1 invf-mkfs t.img` → mutate, unmount, remount; all mutations
   visible; fsck clean; root `seq` unchanged by replay.
3. Crafted torn tail (truncate mid-record) → mount succeeds with the valid
   prefix, no crash, truncation reported.
4. `bash tools/run-e2e.sh tools/test-meta-v3.sh` (add a replay leg);
   `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-l2p.sh`.

## Out of scope (do NOT touch)

- Fold (WP-M14); reclaim (WP-M15); save point (WP-M16).
- Delta sharding / `g_io_lock` (WP-M20).
- v2 mount-scan index deletion (WP-M21).
- Repair of a corrupt base tree (WP-M4 policy: detect/report only).
- `INVFS_VERSION` / RT30 layout (WP-M1).

## Coordination notes

- Subagent ID: `wp-M13-mount-replay`; `INVFS_E2E_AGENT=wp-M13-mount-replay`.
- E2E gates: `test-meta-v3.sh`, `test-writepath.sh`, `test-l2p.sh`.
- Dependencies: **WP-M10** (delta scan/CRC), **WP-M12** (record kinds),
  **WP-M1** (RT30), **WP-M3** (root read).
- Blocks: WP-M16 (rollback reloads a pinned root + delta_end), WP-M22.
- WP-M4's fsck must call the same `delta_scan_valid` so "mount accepts" and
  "fsck accepts" cannot diverge.
