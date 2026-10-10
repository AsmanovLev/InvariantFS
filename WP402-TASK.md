# WP402-TASK — seal heal: explicit `--heal` only, never automatic

Branch: `wp/402-seal-heal` · Worktree: `/srv/flakey/wt-402-seal-heal`
Base: `main` @ aa42c6a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Owner decision (S-remainder): parity drift is repaired ONLY by an
explicit heal command -- healing takes long (full group re-reads +
reconstruction + verify) and must never stall a sweep, a mount, or a
read. WP401's scrub finds and names bad groups; this WP rebuilds them:
reconstruct data from parity, verify the reconstruction, write back,
re-verify. No auto-heal anywhere: not in scrub, not in sweep, not in
the read path (read-path self-heal stays out -- S-remainder deferred
it explicitly).

## Scope (files you may modify — nothing else)

- `src/core/vol_seal.c`: NEW heal functions only (reconstruct +
  verify + write-back). Do NOT alter existing seal / verify /
  recompute / unseal behavior -- sibling WPs (401/403/404) depend on
  it byte-for-byte.
- `src/core/vol_seal.h` + `src/core/volume.h`: declarations for the
  new heal entry point only.
- `tools/invf-sweep.c`: CLI wiring for `--heal [group ...]` ONLY
  (no group args = heal every group WP401's scrub names; explicit
  group ids = heal exactly those). No sweep walk runs with --heal.
- Regression coverage (extend `tools/test-seal.sh` or a focused
  script -- justify placement).

Explicitly OUT (will be reverted on review): changes to existing
seal/verify/recompute/unseal functions, the auto-reseal site
(`invf-sweep.c:1732-1744`, WP403 owns it), scrub output format
(WP401 owns it -- CONSUME it), read-path healing, `src/codecs/*`,
CI workflow.

## Design (constraints -- the wave contract, do not break it)

1. Input contract (WP401 produces it): group ids as printed in
   `seal-heal-needed <group-id> <reason>` lines. `--heal` with no
   args re-runs the detection itself (same recompute the scrub uses)
   and heals exactly the bad groups -- never a blind full rewrite.
2. Reconstruct -> verify (memcmp against parity recompute) -> write
   back -> re-verify. A reconstruction that does not verify is
   DISCARDED with a loud error, never written. One bad group must
   not abort the rest (continue + report each).
3. Crash-mid-heal: footer/explicit commit LAST (same rule as seal);
   a killed heal leaves the prior state (healed-or-prior, never
   half-written). Prove with a kill -9 probe like test-seal.sh image C.
4. Beyond-capacity (more losses than parity covers): clean refusal
   per group (`cannot reconstruct: needs N, has M`), volume otherwise
   untouched, exit nonzero.
5. `--dry-run --heal` prints the plan (groups, bytes) and changes nothing.

## Validation (exact commands, all must pass)

- Fixture: sealed volume + 1 corrupted data block in a covered group
  -> scrub names it -> `--heal` -> file bit-exact, verify clean,
  scrub quiet. Beyond-capacity fixture -> clean refusal.
- Kill -9 mid-heal -> volume opens, state healed-or-prior (document
  the probe).
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK (vol-find ledger is
  line-sensitive).
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/402-seal-heal`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-402-seal-heal-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp402`.

## Report back (exact format, no variation)

```
WP: wp/402-seal-heal
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Record the blocker verbatim in Remaining TODOs
with file:line and hand back. End your reply with the report (it is
captured as your task output).
