# WP-M22 — e2e adaptation + crash soak (final programme gate)

**Branch:** `wp/M22-e2e-crash-soak`
**Worktree:** `/tmp/invfs-wp-M22`
**Severity:** HIGH (a skipped crash soak would let §7.4's claim be made falsely)
**Source:** `impl_docs/design-meta-v3.md` §7.4, §16, §15.6, §17, §18/D6
**Estimated effort:** large

---

## Scope

Adapt the test suites to v3-only and run the crash soak that authorises the
power-loss recovery claim. Adds no new engine behaviour.

Files / suites:
- `tools/test-meta-v3.sh` — expand leg 0 into the full v3 gate
  (base/delta/overlay/fold/savepoint/reclaim/replay).
- Rewrite the §16 format tests: `test-astv2.sh`, `test-meta-extent.sh`,
  `test-meta-extent-walk.sh`, `test-meta-overflow.sh`, `test-stats-mapper.sh`,
  `test-mapper-crash.sh`, `test-sweep-mapper.sh`, `test-compact.sh`,
  `test-l2p.sh`, `test-rollback.sh`, `test-registry.sh`.
- `tools/test-flakey.sh` — crash-injection soak (dm-flakey).
- `Makefile` — prune v2-only e2e entries, add the v3 gates.
- fuzz wiring for the record/page parsers (§16).
- `AUDIT.md` / `INCIDENTS.md` status updates where a finding is closed
  (AGENTS §1.7).

## Why

§16 sets the target: ~10–12 rewritten format tests, ~46 e2e unchanged **if
CLI behaviour is preserved**, ~100–150 new tests, with specific invariants.
§7.4/D6 state explicitly that the power-loss claim is **not** made until
`tools/test-flakey.sh` passes. §17 lists the residual risks this suite must
falsify: fold correctness under concurrent reads, the ordering rule,
save-point × fold, delta bounding, and reclaim without refcounts.

## Design

**Required invariants (from §16, made executable):**
1. **delta-vs-base ordering** — overlay returns delta values, deletes shadow,
   merge is sorted (WP-M11).
2. **root double-slot** — higher `seq` wins; torn slot falls back
   (WP-M1/M3/M13).
3. **refcount-free reclaim** — the diff never frees a page reachable from the
   current or pinned root (WP-M15/M16).
4. **parser fuzz** — the record/page parsers are fuzzed; a bad CRC is
   refused, not trusted (§16).

**Suite policy:** preserve CLI behaviour so unchanged e2e stays valid — any
CLI change is a finding, not a test edit. Map every v2 format test to a v3
replacement or an explicit deletion with rationale in the WP report (no
silent skips). E2E runs through `tools/run-e2e.sh` (parallel-safe, AGENTS
§1.5); the crash soak is `make flakey`, standalone (needs sudo + dm-flakey,
not part of `make e2e`).

**Crash claim:** only after the soak passes, update the AGENTS §2.2 / design
§7.4 wording to drop the "not claimed" caveat; until then the caveat stands.
Record the soak seed and duration in the WP report.

## Validation

1. `make test` — all unit binaries pass.
2. `make e2e` — full adapted suite passes (all `tools/test-*.sh` gates,
   including `test-meta-v3.sh`).
3. `make fuzz-ci` — 10k parser iterations clean; `make fuzz` if time permits.
4. `make flakey` — crash-injection soak passes; record `FLAKEY_SEED` /
   `FLAKEY_SOAK_S`. This gates the recovery claim.
5. `bash tools/check-repo-hygiene.sh`; `make docs` (AGENTS §1.7).

## Out of scope (do NOT touch)

- New features or format changes; any failure here is a bug in an earlier WP,
  fixed there (or in a new WP), not patched around in tests.
- Performance tuning and benchmarks.
- Network/multi-host tests.
- Rewriting tests that already pass unchanged.

## Coordination notes

- Subagent ID: `wp-M22-e2e-crash-soak`; `INVFS_E2E_AGENT=wp-M22-e2e-crash-soak`.
- E2E gates: `test-meta-v3.sh`; `make e2e`; `make flakey`.
- Dependencies: **all prior WPs M1–M21**.
- Blocks: the programme's completion / recovery-claim statement.
- If `test-flakey.sh` is queued behind the global e2e lock (LOCKED suite), use
  `bash tools/run-e2e.sh --bg` and report the log path (AGENTS §1.5); do not
  invoke dm-flakey directly.
