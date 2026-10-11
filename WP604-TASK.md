# WP604-TASK — vol_unlink -1 with dropped dirent on stale-sealed volume

Branch: `wp/604-unlink-stale-seal` · Worktree: `/srv/flakey/wt-604-unlink-stale-seal`
Base: `main` @ 661c2f4. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Incidental finding from WP401 (in the agent's report, unfiled):
`vol_unlink` returned -1 while still dropping the dirent when deleting
a file from a stale-sealed volume in scratch (fsck OK afterwards).
A failing unlink that mutates is the worst combination: callers that
check rc believe nothing happened, while the namespace changed under
them (retry -> ENOENT confusion at best, double-free or orphaned
content at worst). Either the -1 is wrong (should be 0) or the drop is
wrong (should roll back on failure) -- determine which with a
deterministic repro first, then fix the losing side and prove it.

## Scope (files you may modify — nothing else)

- `src/core/vol_dirs.c` (unlink path) + whatever the -1-vs-drop
  decision touches (xattr/seal-stale interplay -- read first, change
  minimally, cite file:line).
- ONE regression test (focused script or lightest fitting suite leg).

Explicitly OUT (will be reverted on review): seal math/policy,
sweep logic, `src/codecs/*`, CI workflow, redefining the seal-stale
contract (consume it as is).

## Design (constraints)

1. Deterministic repro FIRST (fixture + exact commands, <2 min for
   the orchestrator): sealed volume -> post-seal write (stale) ->
   delete a file -> observe rc=-1 + dirent gone + fsck verdict.
   No fix without it.
2. The invariant to restore: rc==0 ⟺ namespace changed, rc!=0 ⟺
   namespace unchanged. Whichever side is wrong gets fixed; if BOTH
   are defensible individually, the combination still has to pick
   one (failing+mutating is never acceptable).
3. Check the siblings: rename-over, link/unlink cycles, and delete
   on sealed-clean (non-stale) volumes must behave identically
   before/after (no behavior change outside the fixed case -- prove
   with the existing suites that cover them).
4. fsck must stay clean AND the ledger must stay truthful through
   the fixed path (no silent orphans either way).

## Validation (exact commands, all must pass)

- Repro fails before (rc=-1 + dropped dirent, quoted), passes after
  (rc-behavior consistent + dirent state matches rc).
- Sibling behaviors unchanged (name the suites run).
- `make test` — rc=0. `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/604-unlink-stale-seal`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-604-unlink-stale-seal-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp604`.

## Report back (exact format, no variation)

```
WP: wp/604-unlink-stale-seal
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers, before AND after)
Result: PASS | FAIL (failing assertion + log path)
Root cause: <which side was wrong + file:line>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. End your reply with the report (it is
captured as your task output).
