# WP303-TASK — fsck double-ownership detection gap

Branch: `wp/303-fsck-double-ownership` · Worktree: `/srv/flakey/wt-303-fsck-double-ownership`
Base: `main` @ 74dec5a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Orchestrator finding, not yet filed as an incident: two live recipes
naming the same data block (double ownership -- the shape the leg2 root
cause produced transiently before its fix in 52eba7a) is not detected
by `invf-fsck`: the volume reports clean while one future free would
hand a live block to a stranger. This WP has two phases. Phase 1:
RE-ESTABLISH the finding with a constructive probe (a volume where two
live inodes name one block -- via `INVFS_FAULT` seams, e.g. forcing a
supersede path, or via direct image surgery on a scratch copy; see
`src/core/vol_fault.h` for the seam list) and confirm fsck passes
silent. Phase 2: fix the detection + regression test. If Phase 1 shows
the gap is already closed (fsck names the double owner), stop there:
report PASS with the probe as the deliverable and change nothing.

## Scope (files you may modify — nothing else)

- Phase 1: scratch probes only (your own subdir; nothing committed
  except the final regression test).
- Phase 2: fsck ownership accounting (`src/core/vol_fsck*` /
  wherever the block-ownership walk lives -- find it, cite file:line)
  + ONE regression test (extend the lightest suite that already
  drives fsck on crafted images, or a focused new script if none
  fits -- justify the placement in the report).

Explicitly OUT (will be reverted on review): allocator changes,
reclaim policy, sweep/dedupe logic, `src/codecs/*`, CI workflow.

## Design (constraints)

1. The invariant at stake: every allocated data block has exactly one
   live owner; fsck must name violations (inode pair + block), never
   pass silent. Read how fsck currently walks ownership before
   designing the check.
2. The fix must be READ-side (detection + honest verdict), never a
   silent repair: a double-owned block the checker cannot attribute
   stays reported, following the `-f` philosophy (refuse-and-name
   over invent-a-state).
3. The regression test must FAIL on the unpatched engine (prove it by
   stashing the fix and running the test) and PASS with it.
4. Mind the leg2 lesson: content-blind recipe keys can make two rows
   name one blob -- the check must see sharing even when keys collide.

## Validation (exact commands, all must pass)

- Phase-1 probe: crafted double-ownership volume + `invf-fsck`
  verdict quoted verbatim in the report (silent-OK = gap confirmed).
- Post-fix: the probe volume is NAMED (both owners + block), clean
  volumes still verify OK (no false positives on the existing
  corpus -- run the fsck-related e2e gates you touch).
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK (vol-find ledger is
  line-sensitive: new/moved `vol_find` calls need ledger updates in
  the same commit).
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH in clean shells: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/303-fsck-double-ownership`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-303-fsck-double-ownership-*`),
  NEVER touch other subdirs. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp303`.
  Full 53-suite e2e is FORBIDDEN.

## Report back (exact format, no variation)

```
WP: wp/303-fsck-double-ownership
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers)
Result: PASS | FAIL (failing assertion + log path)
Finding: <phase-1 probe + fsck verdict, verbatim>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. End your reply with the report (it is
captured as your task output).
