# WP602-TASK — group commit: batch delta barriers (fsync-per-record)

Branch: `wp/602-group-commit` · Worktree: `/srv/flakey/wt-602-group-commit`
Base: `main` @ 661c2f4. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Measured in WP117 (16/16 post-fix gdb samples): fsync-per-delta-record
is now 100% of sweep batches time (`vol_delta_append` -> `vmux_barrier`
-> `blkio_flush` -> fsync: recipe publish + row put + stamp per member,
~3 fsyncs x ~12ms on HDD). The fix is group commit / barrier batching
in `vol_delta.c` / `volume.c` / blkio paths: accumulate a bounded group
of delta appends behind one barrier instead of fencing every record.
The durability contract is load-bearing here (an acked fsync means
durable; the whole crash story stands on it) -- batching changes its
granularity, so the proof burden is a crash probe, not just wall time.

## Scope (files you may modify — nothing else)

- `src/core/vol_delta.c` (append batching), `src/core/volume.c`
  (barrier plumbing it touches), blkio flush paths it calls.
- Regression/perf coverage for the new behavior (extend an existing
  delta/fold suite or a focused probe -- justify placement).

Explicitly OUT (will be reverted on review): lane logic, sweep
dispatch, seal, `src/codecs/*`, fdatasync-vs-fsync policy debates
beyond what's needed (measure, don't philosophize), CI workflow.

## Design (constraints)

1. Measure FIRST: reproduce the 16/16 profile shape on a batch-heavy
   fixture (text/batch members) before changing anything; quote
   fsyncs-per-member and wall time.
2. Correctness before speed: every barrier that used to fence one
   record still fences its group before ack. An fsync that returned 0
   must still mean durable -- prove it with kill -9 probes (kill
   mid-batch -> reopen -> every acked write present+exact, unacked
   may be either; the leg7 assertion shape).
3. Bounded groups only (count and/or bytes cap, documented): an
   unbounded batch is a latency spike and a bigger crash window.
   Memory overhead O(group), never O(volume).
4. No behavior change when the group size is 1 (flag/env to force
   it -- proves the batching is the only delta).

## Validation (exact commands, all must pass)

- Perf: batches wall time down on the fixture (quote before/after +
  fsyncs-per-member); `make test` -- rc=0.
- Crash probe: kill -9 mid-batch + drop_caches + reopen -> acked
  exact, ledger/fsync honesty intact (document the probe).
- `bash tools/check-repo-hygiene.sh` -- OK (vol-find ledger is
  line-sensitive).
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/602-group-commit`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-602-group-commit-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp602`.

## Report back (exact format, no variation)

```
WP: wp/602-group-commit
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers: wall time, fsyncs/member, crash probe)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. A clean FAIL + precise TODO beats a
broad PASS. End your reply with the report (it is captured as your
task output).
