# WP64 — sweep --no-realize + graceful shutdown

**Branch:** `wp/64-sweep-no-realize-ctrlc`
**Worktree:** `/tmp/invfs-wp64`
**Severity:** MEDIUM
**Source:** user request; long-term usability improvement
**Estimated effort:** 4-6 h (both features)

---

## Status snapshot

Two independent sweep-UX improvements, neither blocking:

- **Bug A:** `invf-sweep --no-realize` — suppress the auto-realize of the
  previous checkpoint during `vol_ckp_begin`, keeping both checkpoints live.
- **Bug B:** graceful Ctrl+C shutdown in `invf-sweep` — catch SIGINT, finish
  the current batch, flush, close cleanly.

Neither exists today. `--realize` in the sweep CLI is a dead flag (see
§Background).

---

## Background: the checkpoint lifecycle

```
Sweep #1 starts:
  vol_ckp_begin() → arms CKP0
  sweep runs, moves data RAW→Shadow
  sweep finishes, CKP0 stays LIVE
  retained blocks: held for rollback

Sweep #2 starts:
  vol_ckp_begin() → sees CKP0
  → auto-realizes CKP0 (deletes registry, frees retained blocks)
  → arms CKP1
  sweep runs...
```

Auto-realize in `vol_ckp_begin` is the ONLY realization path. There is no
standalone `invf-sweep --realize` CLI (the flag is silently ignored;
`tools/test-compact.sh` passes it but the test passes by accident because
auto-realize fires regardless).

---

## Bug A — `invf-sweep --no-realize`

### Purpose

Suppress the auto-realize step in `vol_ckp_begin()`. When passed:
- The previous checkpoint stays LIVE
- Its retained blocks stay held (df shows "used" space)
- The new sweep still arms its own checkpoint
- Result: TWO live checkpoints, both holding blocks

### Use case: codec blue-green deployment

```
Before sweep:
  Shadow: A.ppm 100KB (PPMd codec), B.zip 50KB (verbatim)

invf-sweep --no-realize --reencode-with zstd:
  New Shadow: A.ppm 80KB (ZSTD+BCJ), B.zip 50KB (verbatim, unchanged)
  CKP0: LIVE → retains 100KB of old PPMd blocks
  CKP1: LIVE → retains new blocks

Verify:
  $ invf-cat A.ppm | md5sum   # should match original
  $ invf-ls                    # check sizes

If OK:
  $ invf-sweep                 # auto-realizes CKP0 → frees old blocks

If NOT OK:
  $ invf-rollback              # restores CKP0 state (old PPMd)
```

### Implementation

1. `vol_ckp_begin()` (`src/core/vol_rollback.c:176`): add parameter
   `int no_realize`. When `no_realize && had_ck`, skip:
   - `ret_registry_delete()` (lines 334-343)
   - `vol_write_ckp0(v, NULL)` (line 340 — the old CKP0 clear)
   - old staging run free (lines 354-367)
   Print: `checkpoint: --no-realize: previous checkpoint kept live`

2. `src/cli/sweep.c`: parse `--no-realize` in the argv loop (line 456-459).
   Pass to `vol_ckp_begin()`.

3. `volume.h:583`: update `vol_ckp_begin` signature.

### Validation

- `invf-sweep --no-realize img`: two live checkpoints, df shows retained
- `invf-fsck img`: both checkpoints reported
- `invf-rollback img`: restores to pre-sweep-of-sweep#2 (CKP0 state)

---

## Bug B — graceful Ctrl+C in invf-sweep

### Purpose

Currently Ctrl+C sends SIGKILL to sweep, leaving:
- Uncommitted batch writes
- Live checkpoint with inconsistent state
- Potential fsck issues on next open

With graceful shutdown:
- Catch SIGINT, set a flag
- After current batch/record: print "stopping sweep gracefully..."
- vol_flush(), close volume cleanly
- Checkpoint stays live, but state is consistent
- Next sweep can safely continue

### Implementation

1. `src/cli/sweep.c:main()`: add `volatile sig_atomic_t stopping = 0`
   and `signal(SIGINT, sweep_sigint_handler)` / `signal(SIGTERM, ...)`

2. Handler: sets `stopping = 1`, prints to stderr

3. Main sweep loop (`sweep_survey` + batch flush): check `stopping` after
   each file and before each batch commit. If stopping:
   - Break out of the loop
   - Flush current batch
   - Close volume
   - Print summary: "sweep stopped gracefully after N files"

4. FUSE path (`src/cli/fuse_fs.c`): already handles SIGUSR1 for sweep
   trigger; add SIGINT handler for clean unmount (FUSE handles this via
   `fuse_session_loop` but sweep background thread needs cleanup)

### Validation

- Start sweep on a large volume, Ctrl+C mid-run
- Check: no "INVFS_ROLLBACK_ABORT_AT" needed, fsck clean on next open
- Check: next sweep continues from where it left off

---

## Out of scope

- `invf-sweep --realize` as standalone CLI (auto-realize covers 99% of
  cases; see WP58b discussion)
- FUSE background sweep graceful shutdown (FUSE already handles SIGINT via
  fuse_session_loop; the background sweep thread is a separate concern)

---

## Coordination notes

- Subagent ID: `wp64-sweep-no-realize-ctrlc`
- E2E gates: `tools/test-compact.sh` (should still pass with --no-realize
  on one leg), `tools/test-rollback.sh` (no change)
- Dependencies: WP58b (Bug A/B both landed)

---

## Key file:line index

- `vol_ckp_begin()`: `src/core/vol_rollback.c:176`
- auto-realize logic: `src/core/vol_rollback.c:321-367`
- sweep CLI main loop: `src/cli/sweep.c:425+`
- sweep batch flush: `src/cli/sweep.c:530+` (sweep_survey) and `:666+`
- signal handling in FUSE: `src/cli/fuse_fs.c:2753+`
