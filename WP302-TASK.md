# WP302-TASK — usr1-savepoint leg A (cap=7 walk=4, deterministic)

Branch: `wp/302-usr1-savepoint` · Worktree: `/srv/flakey/wt-302-usr1-savepoint`
Base: `main` @ 74dec5a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

`tools/test-usr1-savepoint.sh` leg A fails deterministically (`cap=7
walk=4`): the savepoint is not armed before the walk, identically local
and CI. Untouched code paths (sweep arming); counting/timing semantics
not yet read. Pre-existing (reproduces on the pre-reserve-3/3 tree).
Full entry: `INCIDENTS.md` "test-fuzz bitflip + test-usr1-savepoint OPEN".
Since WP134 every full sweep trigger (offline sweep, watermark pass,
kill -USR1, xattr) must arm an SPT0 savepoint or fail closed -- leg A
is the proof of that contract for the USR1 path.

## Scope (files you may modify — nothing else)

- `tools/test-usr1-savepoint.sh` (leg A harness: counting/timing
  semantics, arm detection).
- Sweep arming path for the USR1 trigger (`src/cli/fuse_fs.c` sweep
  worker/flag area -- READ everything, change only the arming defect
  if the defect is there, with file:line in the report).
- `src/core/vol_spt0.c` ONLY if the defect is in capture (not in
  triggering); say so in the report.

Explicitly OUT (will be reverted on review): transform/dedupe/seal
logic, `src/codecs/*`, CI workflow files, `tools/test-flakey.sh`.

## Design (constraints)

1. Read the counting/timing semantics FIRST (what cap=7 and walk=4
   count, where the arm is detected, what window the leg allows).
   The defect may be harness-side (miscounted window) or engine-side
   (arm not taken). Evidence before edits.
2. Deterministic repro: the exact leg-A command, runnable by the
   orchestrator in under 5 minutes.
3. The WP134 contract stands: arm-or-fail-closed. A fix that sweeps
   without a window on the USR1 path is not a fix.
4. If the root cause is harness counting, fix the harness. If engine,
   fix minimally. File findings with file:line either way.

## Validation (exact commands, all must pass)

- `INVFS_E2E_AGENT=wp302 bash tools/run-e2e.sh tools/test-usr1-savepoint.sh`
  — PASS (leg A green; no other leg regressed).
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH in clean shells: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/302-usr1-savepoint`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-302-usr1-savepoint-*`),
  NEVER touch other subdirs. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
  `/tmp` is a tight tmpfs -- fixtures on your own subdir.
- e2e ONLY via `tools/run-e2e.sh` (never raw suite invocation).
  Full 53-suite e2e is FORBIDDEN.

## Report back (exact format, no variation)

```
WP: wp/302-usr1-savepoint
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ INVFS_E2E_AGENT=wp302 bash tools/run-e2e.sh tools/test-usr1-savepoint.sh
  ... (pass/fail + numbers)
Result: PASS | FAIL (failing assertion + log path)
Root cause: <mechanism in one paragraph with file:line>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. End your reply with the report (it is
captured as your task output).
