# WP403-TASK — seal stale modes (auto-reseal stops being unconditional)

Branch: `wp/403-seal-stale-modes` · Worktree: `/srv/flakey/wt-403-seal-stale-modes`
Base: `main` @ aa42c6a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Owner decision (S-remainder): post-seal writes make parity stale, and
today every plain sweep silently auto-reseals (`tools/invf-sweep.c:1732`
`auto_reseal`, full recompute). That default must become a MODE:
operators who seal once and freeze the volume want to be TOLD about
staleness, not billed a recompute they did not ask for. This WP adds
the mode switch, keeps today's behavior as the default (no silent
change for existing users), and makes every mode loud about what it
did and why.

## Scope (files you may modify — nothing else)

- `tools/invf-sweep.c`: the auto-reseal decision site (:1732-1744)
  + CLI flag wiring + execution gate (:2292-2295) + mode diagnostics.
  Nothing else in that file.
- Regression coverage for the modes (extend `tools/test-seal.sh` --
  it already covers stale/auto-reseal legs; extend, do not fork).

Explicitly OUT (will be reverted on review): seal math/recompute
(WP404 owns recompute), verify/scrub output (WP401 owns it), heal
(WP402 owns it), the stale DETECTION itself (exists -- consume it),
`src/codecs/*`, CI workflow.

## Design (constraints -- the wave contract, do not break it)

1. Modes (flag name your call, document it; proposal
   `--stale-mode=auto|notify|off`):
   - `auto` (DEFAULT, today's behavior): sweep reseals stale groups,
     prints what it resealed. No behavior change for anyone.
   - `notify`: sweep does NOT reseal; it prints the stale report
     (groups stale since which generation, bytes uncovered) and exits
     0 with the volume still sealed-but-stale. Verify keeps
     reporting it honestly.
   - `off`: no reseal, no report beyond one line (for frozen
     pipelines that check staleness elsewhere).
2. Mode + `--dry-run`: plan shows what WOULD reseal under each mode.
3. Mode must compose with explicit `--seal`/`--unseal` (explicit
   always wins over the mode) and must never affect unsealed volumes
   (no descriptor => no mode output at all).
4. Detector untouched: staleness truth stays where it is (verify
   STALE lines, sealed-at-gen + uncovered bytes); this WP only
   governs the RESPONSE.

## Validation (exact commands, all must pass)

- Default run on stale volume reseals (today's legs still green).
- `notify` run: no reseal (parity bytes identical before/after),
  stale report printed, exit 0, verify still shows STALE.
- `off` run: one line, no reseal.
- Explicit `--seal` under `notify` still reseals (explicit wins).
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/403-seal-stale-modes`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-403-seal-stale-modes-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp403`.

## Report back (exact format, no variation)

```
WP: wp/403-seal-stale-modes
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
