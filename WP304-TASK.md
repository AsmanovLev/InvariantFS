# WP304-TASK — regression test for the put-failure shape (leg2 root cause)

Branch: `wp/304-put-failure-regression` · Worktree: `/srv/flakey/wt-304-put-failure-regression`
Base: `main` @ 74dec5a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

The leg2 wrong-bytes root cause (fixed in 52eba7a: sweep remap set
published=1 ignoring `vol_inode_delta_put` EIO, freeing live blocks
into cross-file silent corruption) has NO regression test: nothing
fails if the guard is removed. The fault seam already exists --
`INVFS_FAULT="inode_delta_put"` forces `vol_inode_delta_put` to fail
(`src/core/vol_btree.c:3932`, seam documented at
`src/core/vol_fault.h`). This WP is TEST-ONLY: fault the put during a
sweep remap and assert the engine holds (superseded blocks NOT freed,
volume consistent, every file bit-exact). Then prove the test bites by
reverting the 52eba7a guard locally and watching it fail.

## Scope (files you may modify — nothing else)

- ONE test: either a new focused script (`tools/test-put-failure.sh`,
  preferred -- small, fast, deterministic) or a leg in the lightest
  fitting suite (justify the placement in the report).
- `Makefile` e2e wiring ONLY if you add a new script (one line, the
  `test-imagelock.sh` neighborhood; e2e-full auto-extracts).
- Docs touched ONLY if they claim the guard and need the citation.

Explicitly OUT (will be reverted on review): ANY engine change
(`src/*` is read-only for this WP -- if the test exposes a second
defect, file it with file:line, do not fix it here), CI workflow,
`tools/test-flakey.sh`.

## Design (constraints)

1. The fault to inject: `INVFS_FAULT="inode_delta_put"` during an
   offline `invf-sweep` over a multi-segment volume (the remap path
   needs >=2 segments per file to trigger; check what the pre-fix
   code required -- read 52eba7a first).
2. Assertions, in order: sweep completes honestly (rc + log show the
   put failure contained, not silent); NO live-named block freed
   (free-block accounting before/after, or targeted fsck ownership
   check); every file bit-exact after; `invf-fsck` OK; `invf-verify
   --deep` 0 corrupt.
3. Bite proof (mandatory): `git stash` the 52eba7a guard hunk only,
   rebuild, run the test -> must FAIL naming the corruption shape;
   restore, rebuild, -> PASS. Quote both outcomes in the report.
4. Fast and hermetic: images under /dev/shm or your subdir, seconds
   not minutes, no mount/daemon/dm needed (offline sweep only).

## Validation (exact commands, all must pass)

- New test PASS with the guard; FAIL without it (bite proof, both
  quoted).
- `make test` — rc=0 (includes the new suite if wired).
- `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH in clean shells: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/304-put-failure-regression`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-304-put-failure-regression-*`),
  NEVER touch other subdirs. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp304`.

## Report back (exact format, no variation)

```
WP: wp/304-put-failure-regression
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers, WITH and WITHOUT the guard)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope (no engine fixes). Record the blocker verbatim in
Remaining TODOs with file:line and hand back. End your reply with the
report (it is captured as your task output).
