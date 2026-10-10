# WP404-TASK — incremental re-seal (restripe dirty groups only)

Branch: `wp/404-seal-incremental-reseal` · Worktree: `/srv/flakey/wt-404-seal-incremental-reseal`
Base: `main` @ aa42c6a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Owner decision (S-remainder): re-seal recomputes everything today;
with the format's dirty bitmap (already tracked -- `dirty_skipped`
in `invfs_seal_report`, `src/core/volume.h`) a reseal should restripe
only groups dirty since the last successful seal, falling back to full
recompute when the bitmap cannot prove cleanliness. On a large sealed
volume with small post-seal writes this is the difference between
seconds and a full pass. Full recompute stays available (explicit flag
+ automatic fallback), so no trust is moved, only work skipped.

## Scope (files you may modify — nothing else)

- `src/core/vol_seal.c`: the reseal/recompute loop ONLY (consult the
  dirty bitmap, skip clean groups, count them in `dirty_skipped`).
  Do NOT change seal format, footer, verify, unseal, or heal paths
  (WP402 adds heal beside you -- do not touch its functions).
- `src/core/vol_seal.h` + `src/core/volume.h`: only what the loop
  needs (flags/config plumbing, documented).
- `tools/invf-sweep.c`: a `--reseal-full` escape hatch ONLY (force
  full recompute); default stays incremental-when-provable.
- Regression coverage (extend `tools/test-seal.sh` -- it has
  re-seal legs; extend, do not fork).

Explicitly OUT (will be reverted on review): format changes, verify
semantics (WP401 owns reporting), heal (WP402 owns it), stale modes
(WP403 owns the trigger policy), `src/codecs/*`, CI workflow.

## Design (constraints -- the wave contract, do not break it)

1. Trust rule: a group is skipped ONLY when the bitmap proves it
   clean since the last successful seal. Fresh mount (all-dirty),
   config change (`vol_redun_config` already forces full pass -- keep
   that), footer doubt, or any ambiguity => full recompute. When in
   doubt, do the work.
2. The skipped set must be VISIBLE: report lines naming skipped group
   count + bytes saved, and `dirty_skipped` accounting exact (it
   exists -- make it true, then prove it with a byte-diff of parity
   regions incremental-vs-full on the same volume).
3. Equivalence proof (mandatory): same volume resealed incremental
   vs `--reseal-full` => parity bytes IDENTICAL (cmp), verify output
   identical. Quote it in the report.
4. Crash-mid-reseal keeps the footer-last rule: incremental or not,
   a killed reseal leaves healed-or-prior. The existing image-C-style
   probe must still pass -- extend it to the incremental path.

## Validation (exact commands, all must pass)

- Small-write reseal: wall-time and parity-write bytes drop vs full
  (measure + quote); full-vs-incremental parity cmp identical.
- Ambiguity cases (fresh mount, config change) fall back to full
  (prove by forcing each once).
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK (vol-find ledger is
  line-sensitive).
- Rebuild test bins explicitly; never `| head` a suite pipe.
- sealfuzz gate still green if the loop is in its path (check;
  don't assume).

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/404-seal-incremental-reseal`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-404-seal-incremental-reseal-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp404`.

## Report back (exact format, no variation)

```
WP: wp/404-seal-incremental-reseal
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers, incl. the equivalence proof)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. End your reply with the report (it is
captured as your task output).
