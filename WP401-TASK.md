# WP401-TASK — seal scrub: loud mismatch reporting, never auto-heal

Branch: `wp/401-seal-scrub` · Worktree: `/srv/flakey/wt-401-seal-scrub`
Base: `main` @ aa42c6a. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Owner decision (S-remainder): parity drift must be found by an explicit
scrub and reported LOUDLY; healing happens only via an explicit `--heal`
(WP402), never automatically. Today drift is visible only inside
`invf-verify --deep` output among other legs. This WP adds a dedicated
scrub mode that recomputes every sealed stripe against stored parity
and reports per-group status, with mismatches impossible to miss:
loud stderr, distinct exit code, and a ledger line per bad group that
WP402's heal consumes.

## Scope (files you may modify — nothing else)

- `src/cli/verify.c` (reuse the verify --deep leg at :484+, the
  `parity: N sealed stripes, M mismatched` machinery -- new MODE, not
  changed semantics of the existing leg).
- `tools/invf-sweep.c` CLI wiring for the scrub mode ONLY
  (e.g. `--verify-seal`; no sweep walk runs, like `--unseal`).
- New regression coverage for the scrub mode (extend `tools/test-seal.sh`
  or a focused script -- justify placement).

Explicitly OUT (will be reverted on review): `vol_seal` recompute
(anything that WRITES parity), the auto-reseel site
(`invf-sweep.c:1732-1744`, WP403 owns it), heal/repair logic of any
kind (WP402 owns it), `src/codecs/*`, CI workflow.

## Design (constraints -- the wave contract, do not break it)

1. READ-ONLY: scrub never writes the volume. A scrub that mutates is
   not a scrub.
2. Output contract (WP402 consumes it, keep it EXACT):
   per-group line `seal-heal-needed <group-id> <reason>` on stdout
   (reason: `mismatched` | `missing` | `extra`), plus the existing
   `parity:` summary line. Mismatch => loud stderr banner naming the
   volume + group count + the exact heal command to run.
3. Exit code: 0 = all stripes verify; distinct nonzero (not 1 -- pick
   from the fsck-style family used in tree, document it) = drift
   found. Unsealed volume = 0 with "not sealed" (never an error).
4. Torn-read safety: the scrub reads through the same paths verify
   does; a group it cannot read is reported `missing`, never skipped
   silently and never trusted.
5. Crash-mid-scrub: nothing to recover (read-only) -- state that in
   the report, do not build recovery.

## Validation (exact commands, all must pass)

- New/updated coverage: sealed-clean volume scrubs 0; corrupted-parity
  fixture (flip a byte in one parity block) scrubs LOUD with the exact
  `seal-heal-needed` line + distinct exit code; unsealed volume is quiet 0.
- `make test` — rc=0.
- `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/401-seal-scrub`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-401-seal-scrub-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`. `/tmp` tmpfs
  is tight -- fixtures on your subdir.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp401`.

## Report back (exact format, no variation)

```
WP: wp/401-seal-scrub
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
