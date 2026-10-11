# WP603-TASK — TAR-lane fsync batching (vol_create_blob_file per member)

Branch: `wp/603-tar-lane-fsync` · Worktree: `/srv/flakey/wt-603-tar-lane-fsync`
Base: `main` @ 661c2f4. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Measured in WP117: 3x7MB/3000-member tars took 5m29s in sweep stage
[3/7] (~110s/tar, 9/10 gdb samples in fsync under `vol_create_tar_file`
-> `vol_create_blob_file`). Same fsync family as WP602 (per-record
fencing), but here the unit is a container MEMBER create, not a delta
append -- so it is a separate WP with its own admission (do not widen
WP602 to cover it). Fix shape: batch/defer the per-member durability
work so one TAR costs O(1) barriers instead of O(members), without
weakening the bit-exactness guards (decode-back memcmp stays per
member -- batching barriers is not batching verification).

## Scope (files you may modify — nothing else)

- `src/core/vol_cpack.c` (TAR create path + blob-file commit
  fencing ONLY).
- Regression/perf coverage (extend an existing containerpack suite
  leg or a focused probe -- justify placement).

Explicitly OUT (will be reverted on review): lane admission/sniffing,
generic floor, sweep dispatch, seal, `src/codecs/*`, WP602's delta
paths (read them, do not touch), CI workflow.

## Design (constraints)

1. Measure FIRST: reproduce the ~110s/tar shape (or current-tree
   equivalent) on 3x7MB/3000-member tars (or nearest available
   corpus -- document what you used); quote wall + fsync profile.
2. Guards stay per member: decode-back memcmp + size checks run for
   every member exactly as today. Only the DURABILITY fencing
   batches. A member that fails its guard fails exactly as today
   (refuse + original kept).
3. Crash probe: kill -9 mid-TAR-create -> reopen -> every COMPLETE
   member exact-or-absent (never spliced), fsck verdict sane, next
   sweep heals. Document the probe.
4. No behavior change for single-member containers (degenerate case
   must read byte-identical to today).

## Validation (exact commands, all must pass)

- Perf: TAR wall time down (quote before/after); bit-exactness
  (container + every member read-back cmp clean); `make test` rc=0.
- Crash probe as above, documented.
- `bash tools/check-repo-hygiene.sh` -- OK (vol-find ledger is
  line-sensitive).
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/603-tar-lane-fsync`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-603-tar-lane-fsync-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp603`.

## Report back (exact format, no variation)

```
WP: wp/603-tar-lane-fsync
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers: wall time, crash probe)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. End your reply with the report (it is
captured as your task output).
