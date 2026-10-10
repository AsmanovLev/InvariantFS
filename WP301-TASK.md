# WP301-TASK — fuzz bitflip setup decline (deterministic)

Branch: `wp/301-fuzz-setup` · Worktree: `/srv/flakey/wt-301-fuzz-setup`
Base: `main` @ 74dec5a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

`tools/fuzz/bitflip.py` never reaches lane execution on iters 7, 15, 23,
31, 39, 47, 55 -- exactly the `--seal-every 8` iters: the setup
`invf-sweep` on a fresh image declines every file (`swept=0 skipped=6`)
and exits 1, which the harness counts as setup failure. The same corpus
swept by hand claims lanes and exits 0; a pre-3/3 sweep binary fails
identically (pre-existing, not a 3/3 regression). Ruled out: content
(byte-identical a.txt claims standalone), env (clean-env rerun fails),
image size/path/cwd (replicated exactly). Still open: why the
harness-built image declines while a hand-built twin does not. Full
entry: `INCIDENTS.md` "test-fuzz bitflip + test-usr1-savepoint OPEN".

## Scope (files you may modify — nothing else)

- `tools/fuzz/*` (harness + setup path: image build, seed derivation,
  the setup sweep invocation, `--seal-every` scheduling).
- `tools/run-sealfuzz-gate.sh` ONLY if the root cause lives in the gate
  wiring (it drives the same harness); say so in the report.
- New regression probe under `tools/fuzz/` if the fix needs one.

Explicitly OUT (will be reverted on review): lane logic (`src/core/*`
transform/dispatch), `src/codecs/*`, CI workflow files, `tools/test-flakey.sh`.

## Design (constraints)

1. The defect is in the SETUP path (harness-built image vs hand-built
   twin), not in any lane. Prove it by diffing the two images
   (superblock, seed handling, file layout, sweep argv, env) before
   changing anything.
2. Deterministic repro first: the exact failing command for iter 7,
   runnable by the orchestrator in under 2 minutes. No fix without it.
3. If the root cause is a harness bug, fix the harness. If it is an
   engine bug on the setup path, fix it minimally AND keep the harness
   fix (the harness must not depend on the bug). File engine findings
   with file:line either way.
4. `--seal-every 8` correlation is evidence, not conclusion: explain the
   mechanism (what seal state the setup sweep sees on those iters).

## Validation (exact commands, all must pass)

- Failing seeds (7, 15, 23 at minimum) reach lane execution: no setup
  decline, harness proceeds past setup.
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly (`make bin/<name>`); never `| head`
  a suite pipe (redirect to file, then read).

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH in clean shells: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/301-fuzz-setup`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-301-fuzz-setup-*`), NEVER
  touch other subdirs. Do NOT touch `/dev/sdb1` (live) / `/dev/sdb2`
  (frozen). `/tmp` is a tight tmpfs -- keep fixtures small or on your
  own subdir.
- e2e/fuzz runs that need isolation go through `tools/run-e2e.sh` with
  `INVFS_E2E_AGENT=wp301`. Full 53-suite e2e is FORBIDDEN (only the
  gates named above).

## Report back (exact format, no variation)

```
WP: wp/301-fuzz-setup
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers)
Result: PASS | FAIL (failing assertion + log path)
Root cause: <mechanism in one paragraph with file:line>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. A clean FAIL + precise TODO beats a
broad PASS that touches the world. End your reply with the report (it
is captured as your task output).
