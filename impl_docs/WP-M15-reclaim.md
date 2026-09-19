# WP-M15 — reclaim: reachability diff + delta segment free

**Branch:** `wp/M15-reclaim`
**Worktree:** `/tmp/invfs-wp-M15`
**Severity:** HIGH (freeing a live page is data loss; leaking is ENOSPC)
**Source:** `impl_docs/design-meta-v3.md` §8, §6, §12, §18.4/D4, §15.4, §16
**Estimated effort:** large

---

## Scope

Free metadata that is no longer reachable: base pages reachable from
**neither** the current base root nor a pinned save-point root, and delta
segments below the save-point `delta_end` once a fold has published. Reclaim
is incremental/background. DATA-block reclaim stays with the sweep.

Files:
- `src/core/vol_reclaim.c` — new: mark/scan diff, free scheduling.
- `src/core/vol_btree.c` — reachability walk reused from WP-M4
  `fsck_v3_walk`.
- `src/core/vol_metabuf.c` — `mbuf_free` (WP-M2).
- `src/core/vol_delta.c` — segment free below `delta_end`.
- `src/core/vol_sweep.c` — background reclaim hook (with WP-M18).
- `Makefile` — add `vol_reclaim.o` if a new TU is created.

## Why

§1's dead-record bloat is fixed by removing tombstones, but fold still
retires old base pages and old delta segments. §8 defines reclaim as a
**reachability diff against the single save point** (D4), explicitly
**without a general refcount tree** (§8, §11); §17 flags this risk and §16
requires refcount-free reclaim checks.

## Design

**Reachability diff:**
```
reclaim():
    mark(bitmap, current_base_root)
    if savepoint.pinned_root: mark(bitmap, savepoint.pinned_root)
    for each allocated metadata page p:
        if !bitmap[p]: mbuf_free(p)
    free delta segments with offset < savepoint.delta_end   # none if no SP
```
The design fixes "reachability diff … or epoch retention" (§8); D4 chose the
diff because a single save point means at most two roots. This WP implements
the diff and takes the pinned root as an **optional input**: NULL until
WP-M16 supplies the live save-point root.

**Reader drain (frozen here):** the design says readers of the old base
drain "naturally" (§9) but is **silent on when an old base's pages become
freeable**. This WP freezes: a page is freeable only after (a) a fold has
published a newer root and (b) the reclaim pass observed no reader holding
the old root. The concrete epoch/hazard mechanism is this WP's decision,
documented in a comment; if it cannot be bounded, reclaim defers rather than guessing.

**Ordering:** free **after** the new root is durable (WP-M14 step 2) and
only from the not-marked set. Delta segment free happens after fold reset.
Reclaim is crash-safe: a crash before the free leaves a leak fsck can
recompute; a crash after is fine because the page was already unreachable.

## Validation

1. `make test` — cases: mark from two roots; unreachable freed; reachable
   kept; pinned subtree kept; delta segment below `delta_end` freed, above
   kept; double free rejected; free-list reuse.
2. `INVFS_V3=1 invf-mkfs t.img` → mutate + fold repeatedly; metadata usage
   bounded (no monotone growth); `invf-fsck` clean throughout.
3. `bash tools/run-e2e.sh tools/test-rollback.sh` (with WP-M16 pinning);
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`;
   `bash tools/run-e2e.sh tools/test-writepath.sh`.
## Out of scope (do NOT touch)

- Save-point **creation**/rollback (WP-M16) — consume the pinned root only.
- DATA-block/RAW/Shadow reclaim (WP-M18, unchanged).
- Fold algorithm (WP-M14).
- General refcount trees (rejected, §11).
- v2 `vol_inode_compact` / WP58-D (WP-M21 deletes it).

## Coordination notes

- Subagent ID: `wp-M15-reclaim`; `INVFS_E2E_AGENT=wp-M15-reclaim`.
- E2E gates: `test-rollback.sh`, `test-writepath.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M14** (newer root to diff against), **WP-M4** (walk),
  **WP-M2** (`mbuf_free`).
- Integrates: WP-M16 supplies `pinned_root`; WP-M18 schedules background
  reclaim.
- The reader-drain rule is the open correctness question here; the design
  does not answer it, so this WP must and must record the answer.
