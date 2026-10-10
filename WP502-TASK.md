# WP502-TASK — leg4 proven-exhausted verdict (chaos can be unrecoverable)

Branch: `wp/502-leg4-exhausted` · Worktree: `/srv/flakey/wt-502-leg4-exhausted`
Base: `main` @ bef0372 (includes the WP-J sibling-probing gate fix).
**Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Owner decision: flakey leg 4 (crash-mid-seal, arm 4b) fails whenever
seeded drop windows tear two consecutive RT30 root publishes (or
corrupt under a live seal) -- measured 2/9 passes locally on identical
seeds, same failure on CI. That outcome is UNRECOVERABLE BY DESIGN
(both root slots torn + savepoint damaged with named cause; CANNOT
REPAIR is the honest answer), yet the leg demands ladder convergence
and fails. The S-wave is exonerated (pre-merge tree fails identically;
fold/publish untouched). The engine behaved honestly throughout
(declined-for-cause rollback, named CANNOT REPAIR, no silent reads).
This WP gives the leg a second passing verdict, proven-exhausted: the
volume is lost AND the FS proves it did everything. Preserved dead-end
artifacts to replay against (on main):
`tools/flakey/artifacts/leg4-crash-mid-seal-20261011-082747`,
`-082808`, `-082830`, `-083222`, plus `-083243`, `-083304`.

## Scope (files you may modify — nothing else)

- `tools/test-flakey.sh`: leg4 `seal_resolve` + `recover` verdict
  ONLY (new proven-exhausted branch; the converged path stays
  byte-identical).
- `INCIDENTS.md`: one entry recording the verdict change + why.

Explicitly OUT (will be reverted on review): ANY engine change
(`src/*` read-only -- the engine is the honest party here), other
legs, the verify_consistent gate itself (just fixed on main -- reuse
its (a)-logic, do not redefine it), CI workflow.

## Design (constraints)

1. Today leg4 passes only on convergence. Add proven-exhausted, which
   passes iff ALL of these hold (each asserted on log artifacts the
   leg already writes -- rec-*.log, spt0 diagnostic, fsck/verify
   logs -- never on exit codes alone):
   a. Seal absent via the footer rule (exists -- keep), not via a
      torn footer misread as committed.
   b. SPT0 rollback ATTEMPTED and declined with a NAMED cause
      (parse the decline: "save point is DAMAGED (...)" + the
      diagnostic naming the unreadable pinned recipe/page).
   c. `fsck -f` ATTEMPTED with CANNOT REPAIR naming the cause
      (torn slots named with pbas, base-unreachable stated).
   d. Every loss NAMED: reuse the (a)-half of verify_consistent --
      every unreadable-or-wrong probe file (orig AND ledger-only
      siblings, per the bef0372 fix) is in the fsck/verify ledger.
   e. NO silent reads: every file readable anywhere is bit-exact vs
      orig (vol_files_exact stays mandatory -- an exact-but-unnamed
      file is fine, a wrong-but-unflagged file fails).
   f. The verdict line states EXHAUSTED (not converged) with the
      cause chain, so logs never read as success.
2. Anything failing (a)-(f) is still FAIL -- proven-exhausted must be
   HARD to earn, or it becomes a blanket excuse. In particular: an
   UNNAMED loss, a silent wrong read, or an unattempted ladder step
   fails exactly as today.
3. Determinism: the verdict must be stable across reruns on one
   artifact (replay the preserved dead-ends: all must yield
   proven-exhausted PASS, never converged, never FAIL).
4. Do NOT weaken the converged path or touch leg3/others.

## Validation (exact commands, all must pass)

- Replay: the preserved dead-end artifacts (082747/082808/082830/
  083222/083243/083304) all yield proven-exhausted PASS under the new
  verdict (drive the verdict logic against the artifact images +
  their logs; document the replay recipe so the orchestrator can
  rerun it in under 5 minutes).
- Live: `INVFS_E2E_AGENT=wp502 FLAKEY_ONLY=4 ... tools/test-flakey.sh`
  runs green in BOTH outcomes -- converged when chaos spares the
  publishes, proven-exhausted when it does not. FAIL only on
  silent/unnamed damage (prove by checking that a tampered ledger
  still fails -- e.g. run with a deliberately emptied ledger copy).
- `make test` — rc=0. `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/502-leg4-exhausted`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-502-leg4-exhausted-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`. `/tmp`
  tmpfs is tight -- fixtures on your subdir.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp502`.

## Report back (exact format, no variation)

```
WP: wp/502-leg4-exhausted
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers, artifact replays + live runs both outcomes)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope (no engine changes). Record the blocker verbatim
in Remaining TODOs with file:line and hand back. End your reply with
the report (it is captured as your task output).
