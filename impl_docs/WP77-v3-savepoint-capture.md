# WP77 — v3: wire SPT0 savepoint capture (rollback window)

**Branch:** `wp/77-v3-savepoint-capture`
**Worktree:** `/home/user/invfs-wp77`
**Severity:** HIGH (v3 rollback is dead code; sweep runs uncheckpointed)
**Source:** `test-watermark` recon (subagent `fb464674`)
**Estimated effort:** M

---

## Scope

Meta-v3's rollback mechanism (SPT0) has a load/restore path and a CLI branch
but **no producer**: `spt0_capture()` has zero callers, so the watermark pass
and the offline sweep arm nothing and `invf-rollback` always exits 1 with
"no save point". Wire capture into both sweep entry points, honour savepoint
pinning in reclaim, and update `test-watermark.sh` to the v3 semantics.

---

## Bug A — `spt0_capture()` is never called

**Evidence:**
- `grep -rn "spt0_capture" src/ tools/` → only the definition
  (`src/core/vol_spt0.c:59`) and the header declaration.
- `vol_ckp_begin` is a stub (`src/core/vol_rollback.c:65-72`, `return 0`),
  so `armed` at `src/cli/fuse_fs.c:1715` is always 0 and the `if (armed)`
  block at `:1761` (incl. `vol_ckp_end`) never runs.
- `tools/invf-sweep.c:855` likewise arms nothing.
- WP-M16 merge `1995e8e` touched only `vol_spt0.c`, `volume.c` (load),
  `vol_delta.c`, `vol_fold.c`, `invf-rollback.c` — capture was never wired.

**Fix:**
1. `src/cli/fuse_fs.c` (`invf_sweep_worker`): on v3 call `spt0_capture(g_vol)`
   for the pass, use `spt0_drop`/realize for the K=1 window, and print a v3
   line, e.g. `[watermark] save point captured (base_root=... delta_end=...)`.
2. `tools/invf-sweep.c` (`:855` region): same substitution for the offline
   path.
3. `src/core/vol_reclaim.c:82-84`: honour `pinned_root` (currently
   `(void)pinned_root;`) so a restored base cannot reference reclaimed pages.
4. Keep the v2 `vol_ckp_*` path untouched (it is a stub anyway).

Then update `tools/test-watermark.sh`: assert `save point captured` and
`rolled back to save point`, and drop the CKP0/retention/`inode compact:
skipped` assertions (lines 178, 192-193, 204-205, 211-212).

---

## Validation

1. `make test` — must pass.
2. `INVFS_E2E_AGENT=wp77-spt0 bash tools/run-e2e.sh tools/test-watermark.sh`
   — must PASS.
3. Direct: v3 image → `invf-sweep` prints the save-point line →
   `invf-rollback` succeeds and restores bit-exact content.
4. Crash leg: kill mid-sweep with a savepoint live, then rollback → bit-exact.
5. `tools/test-rollback.sh` — note: it is v2-CKP0-based and is expected to
   need its own update; do NOT try to make it pass here, just record its
   status in the report.
6. `bash tools/run-e2e.sh tools/test-meta-v3-fold.sh` and
   `test-meta-v3-delta.sh` — must stay green (fold/delta interaction).

---

## Out of scope (do NOT touch)

- `vol_flush` bitmap persistence (WP75).
- `invf-verify` (WP76).
- Sweep transcode/batch parity (WP78).
- Test-helper link lists (WP74) — `test-watermark.sh` does not compile a helper.
- Re-architecting rollback; use the existing SPT0 design (`design-meta-v3.md` §6).

---

## Coordination notes

- Subagent ID: `wp77-spt0`
- E2E gates: `test-watermark.sh`, `test-meta-v3-fold.sh`,
  `test-meta-v3-delta.sh`.
- Dependencies: none, but it touches `tools/invf-sweep.c` (WP78 touches
  `src/core/vol_sweep.c`, different file).
