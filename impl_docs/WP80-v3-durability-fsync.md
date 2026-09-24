# WP80 — v3 durability: make fsync meaningful and define the contract

**Branch:** `wp/80-v3-durability-fsync`
**Worktree:** `/home/user/invfs-wp80`
**Severity:** HIGH (no explicit durability contract; `fsync` is a no-op on v3)
**Source:** FUSE-vs-kernel discussion / ADR-008; `vol_sync` audit
**Estimated effort:** M

---

## Scope

Make the Meta-v3 durability contract explicit and correct: `fsync` must be a
real durability point, `vol_close` must not mark CLEAN before a barrier, and
the per-append barrier strategy must be audited and documented. Engine +
docs; no format change.

---

## Bug A — `vol_sync()` is a no-op on v3

**File:** `src/core/volume.c:2510-2515`

```c
if (v->sb.vol_flags & VOLF_V3) return 0;
```

The comment ("the v3 skeleton is empty/read-only -- nothing in flight") is
stale: v3 is the full default format. Consequence: FUSE `.fsync` neither
flushes the dirty bitmap (the WP75 fix) nor issues a barrier on v3. The
contract currently holds only *incidentally*, because `vol_delta.c:556`
barriers on every delta append.

**Fix:** on v3, `vol_sync` must flush pending state (`vol_flush` — which now
persists the dirty bitmap) and issue a `vmux_barrier`, returning the error on
failure (and latching, as the v2 path does). Update the stale comment.

---

## Bug B — `vol_close()` may mark CLEAN without a barrier

**File:** `src/core/volume.c:1954` (`vol_close`), barrier gated at `:1996` on
`INVFS_FSYNC`.

For a device-backed volume a CLEAN superblock must never be written before the
data/metadata it describes is durable. Decide and implement the policy: a
barrier before the CLEAN write for device mounts (or an explicit, documented
opt-out), instead of the current opt-in `INVFS_FSYNC`.

---

## Bug C — per-append barrier strategy: audit and document

`src/core/vol_delta.c:556` barriers after **every** delta append. That makes
each metadata mutation durable but costs a barrier per op, and it is why
`fsync` can appear redundant. Audit whether this is intended for lock-free
read consistency (ADR-002) or an over-synchronisation. **Do not change it
without evidence** — first measure and document:
- is the barrier per append or per batch?
- what breaks if it is relaxed (fold/read consistency)?
- what does `fsync` then have to guarantee?

Write the resulting contract into `docs/architecture/META-V3.md` (or a new
durability section) and ADR if a decision is made.

---

## Validation

1. `make test` — must pass.
2. `INVFS_E2E_AGENT=wp80-durability bash tools/run-e2e.sh tools/test-flushfail.sh`
   — must PASS (note: the `INVFS_SYNC_FAIL_AT` hook currently sits *after* the
   v3 early return; make it reachable on v3 so the v3 fsync-failure path is
   actually exercised).
3. `tools/test-writepath.sh` (fsync leg), `test-meta-v3.sh`,
   `test-meta-v3-fold.sh`, `test-meta-v3-delta.sh` — must stay green.
4. A targeted crash test: fsync-acknowledged write, then simulate power loss
   (kill / zero the un-barriered tail), reopen, confirm the file is present and
   bit-exact. Document the exact command.
5. `bash tools/check-repo-hygiene.sh` — OK.

---

## Out of scope (do NOT touch)

- The flakey F1–F3 residuals (`tools/flakey/`) — separate WP.
- The rollback/bitmap-free interaction noted by WP77.
- Sweep parity (WP78), verify (WP76), link lists (WP74).
- Changing the on-disk format.

---

## Coordination notes

- Subagent ID: `wp80-durability`
- E2E gates: `test-flushfail.sh`, `test-writepath.sh`, `test-meta-v3.sh`,
  `test-meta-v3-fold.sh`, `test-meta-v3-delta.sh`.
- Dependencies: none. WP75 (bitmap flush) and WP77 (SPT0) are already in main;
  build on them.
