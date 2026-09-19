# WP-M16 — save point + rollback

**Branch:** `wp/M16-savepoint-rollback`
**Worktree:** `/tmp/invfs-wp-M16`
**Severity:** HIGH (rollback to the wrong base or delta_end is data loss)
**Source:** `impl_docs/design-meta-v3.md` §6, §7, §12, §18.5/D5, §15.4, §16
**Estimated effort:** large

---

## Scope

Implement the **single save point** `{base_root, delta_end, flags}` and
`invf-rollback`: capture, pin the pre-fold base across folds, restore it and
truncate the delta to `delta_end`, release the pin. Replaces v2's `CKP0`
(`vol_rollback.c:7-44`).

Files:
- `src/core/invarifs.h` — save-point block-0 descriptor (new magic, e.g.
  `SPT0`), replacing `invfs_ckp0` for v3.
- `src/core/vol_rollback.c` — capture/restore/drop (rework CKP0 logic; no
  `\x01reten` registry).
- `src/core/volume.c` — load the save point at open; expose the pin.
- `src/core/vol_delta.c` — truncate to `delta_end`.
- `tools/invf-rollback.c` — v3 path; keep the CLI contract.
- `src/core/vol_reclaim.c` — consume `pinned_root` (WP-M15).

## Why

§6 fixes the semantics: one save point at a time; fold may run while it is
live but the pre-fold base is **pinned**; rollback restores it and discards
the delta tail; dropping releases the pin. §13 lists rollback as a daily
improvement over v2's coarse CKP0; D5 fixes the shape `{base_root, delta_end}`.

## Design

**Record:**
```
char     magic[4]   "SPT0"
uint32_t version, flags
uint64_t base_root  # pinned RT30 root at capture
uint64_t delta_end  # delta offset at capture
uint32_t crc32c     # over the descriptor, field zeroed
```
§12 fixes `{base_root, delta_end, flags}`; the magic/version/CRC framing is
this WP's addition, following the WP-M1 convention.

**Capture:** record the **current** base root and `delta_end` at creation.
D5's note is load-bearing: if folds occur while the point is live, the pin
must remain the root captured **at creation**, not the latest pre-fold root.

**Rollback:**
```
rollback():
    publish(savepoint.base_root)        # RT30 double-slot + seq++
    truncate_delta(savepoint.delta_end) # drop post-capture records
    rebuild delta index by replay       # WP-M13
    clear/keep savepoint per CLI contract
```

**Fold interaction:** §6 permits fold while a save point is live; WP-M14
publishes a new base but must **not** reclaim pages reachable from the pinned
root (WP-M15 marks it). Dropping the pin lets the next reclaim free that base.

## Validation

1. `make test` — cases: capture/restore identity; rollback after a fold
   restores the pinned base; `delta_end` truncation drops the post-capture
   records; drop releases the pin; a second save point is refused (K=1).
2. `INVFS_V3=1 invf-mkfs t.img` → mutate, save, mutate, fold, rollback;
   volume matches the captured state; `invf-fsck` clean.
3. `bash tools/run-e2e.sh tools/test-rollback.sh` — adapted to v3 (WP-M22
   finalises).
4. `bash tools/run-e2e.sh tools/test-meta-v3.sh`;
   `bash tools/run-e2e.sh tools/test-writepath.sh`.
## Out of scope (do NOT touch)

- Browsable snapshots / snapshot DAG / many save points (rejected, §11).
- Reclaim mechanics (WP-M15); fold triggering (WP-M14).
- v2 `CKP0` / `\x01reten` deletion (WP-M21) — v3 ignores the v2 descriptor.
- Delta log framing (WP-M10).
## Coordination notes

- Subagent ID: `wp-M16-savepoint-rollback`;
  `INVFS_E2E_AGENT=wp-M16-savepoint-rollback`.
- E2E gates: `test-rollback.sh`, `test-writepath.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M14**, **WP-M15** (pin), **WP-M13** (replay), **WP-M1**.
- Blocks: WP-M15 pin correctness, WP-M22 crash-soak gates.
- D5's "pin at creation, not latest pre-fold" note must be asserted by a unit
  test; it is the subtle bug in this WP.
