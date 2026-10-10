# WP117-TASK — sweep transform/batches parallelization (WP117 remainder)

Branch: `wp/117-sweep-transform-parallel` · Worktree: `/tmp/invfs-wp117`
Base: `main` @ 35b1c61. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

WP117 (`wp/117-iter-live-inodes-quadratic`, see `INCIDENTS.md` "PARTIALLY
FIXED (WP117 — heat done, transform/batches open)") fixed the sweep's
heat stage. What remains: `[3/7] transform` crawls (18.6 files/s at the 5%
mark on a 46,245-inode volume, ETA ~1h15m for that stage alone; 470
files/s on a 433-file volume, so not a per-file constant) and `[6/7]
batches` looks superlinear (1m29s for 420 entries vs 919ms transform on
the same 433 files). A later rootfs bench (56.5k files) never converged:
the sweep was killed after ~10h wall, single thread confirmed. Full
numbers in `INCIDENTS.md` ("What is still broken — [3/7] transform
onward", incl. "Not determined" open questions). This WP closes the
remainder: make transform (then batches, if profiling implicates it)
scale with threads.

## Scope (files you may modify — nothing else)

- `src/core/vol_sweep.c` — transform dispatch, collect walk, batches stage,
  pending-drain (`vol_sweep_pending`) ONLY as a caller of the above.
- `src/core/vol_heat.c`, `src/core/vol_dedupe.c`, `src/core/vol_textzone.c`
  — ONLY IF `make PERF=1` profiling proves time lives there; touches must
  stay inside the hot function, no redesigns.
- `src/core/perf_counters.c` — may ADD counters (never remove/rename).
- New test binaries under `src/cli/*_test.c` + `Makefile` wiring ONLY IF a
  unit test is needed for a parallel invariant (prefer e2e, see below).

Explicitly OUT (will be reverted on review, no discussion):
- heat stage internals (done by WP117), FUSE hot paths (`vol_v3_path_of`
  has its own WP per INCIDENTS), on-disk format (no format change!),
  `src/core/vol_seal.c`, `src/codecs/rs.c`, `tools/invf-sweep.c`
  (wp/201 owns the seal surface; coordinate via orchestrator on conflict),
  `tools/test-flakey.sh` legs, CI workflow, docs (except a one-paragraph
  note in the e2e script you validate with, if its contract changes).

## Design (constraints, not a blank cheque)

1. **Profile first.** `make PERF=1`, reproduce on a multi-thousand-file
   volume (see `tools/bench-rootfs.sh` + corpus notes; do NOT re-run the
   full 18h bench — build a 5–10k-file fixture that shows the same shape).
   Name the superlinear structure (per-file O(n) scan? shared list? lock?)
   in your report with file:line.
2. **Bounded worker pool, mirroring `sweep_thread_worker`.** The
   multi-segment path already parallelizes that way
   (`src/core/vol_sweep.c`: pool, bounded batches, join-before-return).
   Same pattern for transform dispatch. No unbounded threads, no thread
   per file, no new thread-lifetime machinery (no detached workers that
   outlive the pass).
3. **Deterministic output.** Same recipes, same keys, same pbas
   (modulo allocator order — document any intentional nondeterminism)
   regardless of worker count. Content-addressing means scheduling must
   not affect bytes. Prove it: sweep the fixture at 1 vs N workers,
   `cmp` the resulting recipes/volume hashes.
4. **Guards stay decisive.** Every lane's bit-exactness guard runs exactly
   as today (same accept/refuse per file, memcmp where it exists). A
   parallel sweep that transcodes something the serial one declines is a
   data-loss bug, not a speedup. No guard may be skipped, weakened, or
   moved off the values it checks.
5. **Lock discipline.** All engine mutation stays under `g_io_lock` (see
   the `fuse_loop_mt` comment in `src/cli/fuse_fs.c:4125` for why new
   locking is guilty until proven innocent). Workers do compute + lane
   decisions; any shared-structure write goes through the existing locked
   paths. No new mutexes without a comment justifying why the existing
   one cannot cover it.
6. **No behaviour change when idle.** Single worker must equal today's
   code path-for-path (keep a serial equivalent or a worker count of 1
   that exercises the same code).

## Validation (exact commands, all must pass)

- `make test` — rc=0, full output, no skips you introduced.
- `bash tools/check-repo-hygiene.sh` — OK (it checks vol-find ledger
  line numbers: if you add/move a `vol_find` call, update
  `tools/check-vol-find.sh` in the same commit or it fails).
- E2E gates (each via the runner, with attribution!):
  `INVFS_E2E_AGENT=wp117 bash tools/run-e2e.sh tools/test-writepath.sh`
  then `tools/test-textzone.sh` (batching), `tools/test-dedupe.sh`
  (dedupe stage), `tools/test-sweep-window-reclaim.sh`. All PASS.
- Perf proof: fixture sweep completes; record wall time + files/s at
  1 vs N workers; `invf-verify` (or `invf-fsck` + deep readback) clean
  after; numbers go in your report.
- Rebuild test bins explicitly after touching engine code
  (`make bin/<name>` — `make -j` does NOT relink them; stale bins lie).

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Message style:
  imperative summary + why + validation (`make test rc=0`, gates).
- NEVER `| head` a suite pipe; `$?` after a pipe is the pipe's, not the
  suite's — redirect to a file, then read the file.
- `/sbin` is off PATH in clean shells: export `PATH=$PATH:/sbin:/usr/sbin`.
- Pushes (own branch ONLY) with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/117-sweep-transform-parallel`.
  Rerunning failed CI jobs while they are `in_progress` 403s — wait.
- `/srv/flakey`: subdir-only (`/srv/flakey/wp117-*`), NEVER touch other
  subdirs (a past `rm -rf` destroyed ~10G of others' data). `/tmp` is a
  3.8G tmpfs — no multi-GB fixtures there. Do NOT touch
  `/dev/sdb1` (live) / `/dev/sdb2` (frozen).
- Do NOT run the full 53-suite e2e or the 18h bench; do NOT run flakey
  legs beyond what this doc names without asking the orchestrator.

## Report back (exact format, no variation)

```
WP: wp/117-sweep-transform-parallel
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ make test
  ... (pass/fail + numbers)
  $ INVFS_E2E_AGENT=wp117 bash tools/run-e2e.sh <suite>
  ... (per suite)
Result: PASS | FAIL (with failing assertion + log path)
Perf: <fixture size, serial wall, N-worker wall, files/s both, verify result>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope to unblock yourself (no drive-by refactors, no
"while I'm here" fixes). Record the blocker verbatim in Remaining TODOs
with file:line and hand back. A clean FAIL + precise TODO beats a broad
PASS that touches the world.
