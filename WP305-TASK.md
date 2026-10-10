# WP305-TASK — runner-only reds: sweep-thread pid-scan + ext4fs orphan enumerate

Branch: `wp/305-runner-reds` · Worktree: `/srv/flakey/wt-305-runner-reds`
Base: `main` @ 74dec5a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Two e2e items pass on the dev box and fail deterministically on GH
24.04 runners; neither is understood. Full entry: `INCIDENTS.md`
"test-fuse-sweep-thread + test-ext4fs-orphan OPEN (runner-only reds)".
(a) sweep-thread: `setup()` mounts fine, then `daemon_pid($IMG)`
finds no /proc cmdline containing the image path ("no daemon pid"),
and the leftover mount breaks cleanup. Same-uid rules out hidepid
alone; daemon-exit-vs-scan-miss not established. (b) ext4fs orphan
leg: `$E4 enumerate` declines the default 8 MB `mkfs.ext4 -q -F`
orphan_file image that must yield exactly 1 member; ht0/ht1/fsA/fsB
pass on the same run. e2fsprogs-version shape suspected, not shown.

## Scope (files you may modify — nothing else)

- `tools/test-fuse-sweep-thread.sh` (pid-scan robustness, setup/cleanup).
- `tools/test-ext4fs.sh` (orphan leg: enumerate tolerance or version
  detection -- read the enumerate decline path first).
- The decline paths they exercise ONLY if root-caused there (with
  proof); say so in the report.
- A temporary CI workflow for runner-side debugging ONLY if local
  reproduction fails (name it `debug-wp305-*.yml`, delete it in the
  same branch before the final report -- it must never merge).

Explicitly OUT (will be reverted on review): sweep transform/dedupe,
seal, `src/codecs/*`, `tools/test-flakey.sh`, permanent CI changes.

## Design (constraints)

1. Reproduce FIRST, and reproduce the RUNNER, not the box: match the
   runner env locally (ubuntu:24.04 container if docker is available,
   else the runner's e2fsprogs/kernel versions from the workflow logs)
   -- a fix for something you cannot reproduce is a guess.
2. sweep-thread: establish daemon-exit vs scan-miss with evidence
   (/proc snapshot timing, daemon log/state at scan time). If the
   scan misses a live daemon, fix the scan (argv shape? timing?);
   if the daemon exits, find WHERE (worker crash?) and file it.
3. ext4fs orphan: capture the exact bytes the runner's mkfs.ext4
   emits that local does not (superblock/orphan-file feature diff),
   then decide: tolerate in enumerate (with a guard) or pin the
   version expectation loudly.
4. No speculative changes: every edit tied to reproduced evidence,
   quoted in the report.

## Validation (exact commands, all must pass)

- Both suites green in the reproduced runner-like env AND on the dev
  box: `INVFS_E2E_AGENT=wp305 bash tools/run-e2e.sh
  tools/test-fuse-sweep-thread.sh` and `... tools/test-ext4fs.sh`.
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.
- Temporary debug workflow (if any) DELETED before the report.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change per commit, on THIS branch only (two halves =
  two commits max, each with why + validation). Imperative messages.
- `/sbin` off PATH in clean shells: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/305-runner-reds`.
  Rerun-failed-jobs 403 while in_progress -- wait, don't hammer.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-305-runner-reds-*`),
  NEVER touch other subdirs. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh`. Full 53-suite e2e is FORBIDDEN.

## Report back (exact format, no variation)

```
WP: wp/305-runner-reds
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers, box AND runner-like env)
Result: PASS | FAIL (failing assertion + log path)
Root causes: <sweep-thread: ... ; ext4fs-orphan: ... , each with evidence>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. A precise "reproduced, root cause X,
fix is Y but out of scope because Z" is an acceptable PASS for an
investigation half. End your reply with the report (it is captured
as your task output).
