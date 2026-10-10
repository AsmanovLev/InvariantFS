# WP201-TASK — native seal backend v1 (par2-inspired, no compat)

Branch: `wp/201-seal-rs-native` · Worktree: `/tmp/invfs-wp201`
Base: `main` @ 35b1c61. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

`invf-sweep --seal` refuses on v3 (the v2 seal went with the retired
format; `src/core/vol_seal.c` is a 125-line refusal stub + dirty-stripe
bookkeeping). Owner decision: rebuild seal as a NATIVE port of the PAR2
idea (slice checks + recovery sets + verify protocol) with NO byte
compat with par2cmdline ("inspired, not compatible"). Math already in
tree (`src/codecs/rs.c`: Vandermonde+Cauchy over GF(2^8), `rs_encode` /
`rs_decode`, k+m ≤ 256, covered by `src/cli/rs_stability_test.c`).
Design thread (owner + orchestrator, 2026-10-09/10): per-chunk groups
(not giant stripe — RS impl limit k≤255 forbids it, and repair cost
scales with stripe size), piggyback on sweep passes (no new daemon, no
separate passes), seal-after-defrag economics, footer+CRC written last,
stale policy for post-seal writes, verify-after-write, scrub-report into
the damage ledger. Overflow design talk (pyramid/RaptorQ/mirror) is
EXPLICITLY deferred — see Out of scope.

## Scope (files you may modify — nothing else)

- `src/core/vol_seal.c` (+ `vol_seal.h` if it exists, else create the
  header next to it) — replace the refusal with the v3 implementation.
  The dirty-stripe bookkeeping (`seal_dirty_mark`) stays (allocator
  contract, still wanted).
- `tools/invf-sweep.c` — wire the seal stage (`--seal <pct>`) + keep
  `--dry-run` truthful about it. `--defrag` / `--no-defrag` flags are
  OUT (future WP owns them; seal runs on whatever layout exists).
- `src/cli/seal_test.c` (NEW) + `Makefile` wiring — round-trip unit test
  (see Validation). Mirror the style/assert macros of
  `src/cli/rs_stability_test.c`.
- `tools/test-seal.sh` — EXTEND (it asserts refusal today; teach it the
  new contract, keep every old assertion that still applies).
- Docs: wherever `--seal` is documented as refusing (`docs/`,
  `tools/` usage text, man pages if present) — update in the same commit
  (a doc contradicting the code is a doc bug per AGENTS.md §1.7; the
  hygiene gate checks citations, keep them exact with file:line).

Explicitly OUT (will be reverted on review, no discussion):
- `src/codecs/rs.c` math (USE it; if you find a math bug, stop and file
  it — do not fix here, that is its own WP).
- `src/core/vol_sweep.c` transform/dispatch/pending-core (wp/117 owns
  it; READ it freely, do not edit).
- Mirror/two-device work, RaptorQ, pyramid/2D codes, scrub auto-heal
  (report only), incremental re-seal (v1 = full recompute), GF(2^16) or
  any par2-compat work, `tools/test-flakey.sh` legs, CI workflow.

## Design (locked decisions + bounded choices)

LOCKED (owner decisions, do not relitigate):
1. Groups, not giant stripe. RS impl limit (k+m ≤ 256) forbids whole-disk
   stripes; repair cost must scale with group, not volume.
2. No par2 byte-compat. Our hashes (BLAKE3), our footer, our sizes.
   "Inspired" means slice-checks + recovery-sets + verify protocol.
3. Piggyback only: no new daemon/thread, seal runs as a sweep stage,
   verify-after-write for parity blocks (re-read + compare before the
   volume is called sealed).
4. Crash rule: footer (generation + group table + CRC) written LAST.
   No valid footer = no seal (partial seal output ignored, not trusted).
5. Stale rule: post-seal writes mark the seal dirty (warn in fsck/stats
   output: sealed-at-gen-N + M uncovered bytes); re-seal = full
   recompute. No silent staleness, ever.

YOUR CALL within these bounds (document the choice + why in your report):
- Group geometry: default 64 data + m parity with 64 KiB symbols
  (~4 MiB data per group). Justify or adjust with measured reasoning
  (repair-read size vs footer/index size vs small-file behaviour).
- `%` menu → (k,m): default proposal 5→(20+1), 10→(9+1), 20→(8+2),
  25→(6+2); default flag value 10. Keep the menu FIXED (no arbitrary
  matrix shapes — audit surface).
- Footer placement + index format (tail region reservation needs an
  allocator touch — keep it minimal and say exactly what you touched).
- Seal descriptor visibility: `invf-fsck` must report sealed gen +
  coverage + staleness (it says "not sealed" today — update truthfully).
- O(1)-RAM streaming (house discipline, cf. WP83): bounded buffers,
  never whole-group-set in RAM unless the group itself.
- The packet/footer parser reads untrusted bytes (torn reads!): strict
  validation, refuse-and-report; use the `INVFS_FAULT` seam style if you
  need fault injection, and run the parser through the fuzz harness if
  it is reachable with less than a full afternoon's work — else write
  WHY NOT in Remaining TODOs (no silent skipping).

## Validation (exact commands, all must pass)

- New unit test green: seal round-trip (encode → drop up to m blocks
  per group → decode → byte-compare), determinism (same input twice →
  same parity), beyond-capacity refusal (m+1 losses → clean -1, never
  garbage), footer absence/corruption → "not sealed" (never trusted).
- `make test` — rc=0 (includes existing `rs_stability_test` + your new
  bin + the updated `test-seal.sh` expectations).
- E2E: `INVFS_E2E_AGENT=wp201 bash tools/run-e2e.sh tools/test-seal.sh`
  — must cover seal → verify-sealed → write-more → stale-reported →
  unseal → verify-unsealed. Extend the script (it refuses today).
- `bash tools/check-repo-hygiene.sh` — OK (vol-find ledger is
  line-sensitive: new/moved `vol_find` calls need ledger updates in the
  same commit).
- Rebuild test bins explicitly (`make bin/<name>`); never trust a stale
  bin (`make -j` does not relink them).
- Crash-mid-seal probe (manual, documented in report): kill -9 during
  the seal stage → volume opens, seal reported absent-or-prior (never
  half-trusted), fsck verdict sane. Script it if cheap; else exact
  repro steps in Remaining TODOs.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- NEVER `| head` a suite pipe; `$?` after a pipe is the pipe's — redirect
  to file, then read.
- `/sbin` off PATH in clean shells: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/201-seal-rs-native`.
  Rerun-failed-jobs 403 while in_progress — wait, don't hammer.
- `/srv/flakey`: subdir-only (`/srv/flakey/wp201-*`), NEVER touch other
  subdirs (a past `rm -rf` destroyed ~10G of others' data). `/tmp` is a
  3.8G tmpfs — keep fixtures small or on `/srv/flakey/wp201-*`.
  Do NOT touch `/dev/sdb1` (live) / `/dev/sdb2` (frozen).
- FUSE mounts in tests need `user_allow_other` (present in
  `/etc/fuse.conf`); e2e ONLY via `tools/run-e2e.sh` (never raw suite
  invocation — shared /dev/shm state); full 53-suite e2e is FORBIDDEN
  (only the gates named above).

## Report back (exact format, no variation)

```
WP: wp/201-seal-rs-native
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ make test
  ... (pass/fail + numbers)
  $ INVFS_E2E_AGENT=wp201 bash tools/run-e2e.sh tools/test-seal.sh
  ...
Result: PASS | FAIL (failing assertion + log path)
Perf/numbers: <group geometry chosen, overhead measured on fixture, seal wall time, verify time>
Remaining TODOs: <none | list, each one line>
Design decisions taken: <geometry, footer placement, stale representation — one line each + why>
```

## If you get stuck

Do NOT widen scope (no RaptorQ, no pyramid, no mirror, no scrub-heal).
Record the blocker verbatim in Remaining TODOs with file:line and hand
back. A clean FAIL + precise TODO beats a broad PASS that touches the world.
