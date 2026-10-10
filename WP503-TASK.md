# WP503-TASK — soak oracle drop-awareness (device-torn acked writes)

Branch: `wp/503-soak-oracle` · Worktree: `/srv/flakey/wt-503-soak-oracle`
Base: `main` @ bef0372. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Soak leg5 fails THIRD STATE on files whose last acked write ran under
dm-flakey drop windows: measured case `s27.bin` (artifact
`tools/flakey/artifacts/leg5-soak-20261011-085551/`): 166443-byte
O_TRUNC write + fsync returned OK under drop, file now reads as a
VALID EMPTY file (size 0, no segments), sha = empty-string hash,
unflagged by fsck AND verify (nothing to flag -- a valid empty file
isn't damage). Device ate the write after ACKing it (truncate applied,
content dropped); no engine check can detect ACKed drops -- the suite's
own comment (`tools/flakey/soak.py` CLI-phase note) already concedes
drop is out of contract for whole-volume ops. But file ops run under
all three modes while the oracle history-pins every acked write, so a
drop-torn acked write can never satisfy the gate. The suite is green
only when luck avoids straddling writes. This WP makes the oracle
drop-aware WITHOUT weakening up-mode strictness.

## Scope (files you may modify — nothing else)

- `tools/flakey/soak.py`: gate manifest-diff ONLY (drop-touched
  tracking + exemption + re-pinning, below) + `INCIDENTS.md` entry.
- Nothing else. No engine (`src/*` read-only), no other legs, no CI.

Explicitly OUT (will be reverted on review): engine changes of any
kind (the engine is innocent here -- proven by the artifact: valid
empty file, deterministic reads, no flags), weakening up-mode
checks, skipping files silently (exemptions must be LOUD in the log).

## Design (constraints)

1. Track per-file `drop_touched`: set on any acked write (or
   successful rename/delete-source state change?) that ran under
   drop mode; cleared on the next acked write under up mode (which
   re-pins history strictly as today).
2. At gate manifest diff: drop-touched files SKIP the history check
   but MUST still be fsck/verify-clean (run those first, as today)
   and are logged loudly (`gate: <name> drop-touched, history check
   deferred (last acked write under drop)`). Up-mode-pinned files:
   full strictness, byte-identical to today.
3. Silent-corruption tripwire stays: a drop-touched file that is
   UNREADABLE must still be damage-named (fsck ledger or verify
   CORRUPT) or the gate fails -- exemption covers torn-but-valid
   states, never silent loss. An up-mode file failing history is
   still an unconditional FAIL.
4. Reads/writes under `error` mode are unchanged (they fail loudly
   and never enter history -- existing behavior, keep).
5. Repro recipe for the orchestrator: the preserved artifact
   (backing.img + model.json + oplog.txt above) PLUS a live leg5
   run is luck-dependent; the deliverable proof is (a) a unit-level
   replay of the gate logic against the artifact state showing the
   new verdict (damage-deferred, loudly logged) instead of THIRD
   STATE FAIL, and (b) live leg5 runs green in both shapes (no
   drop-tear, and drop-tear-deferred).

## Validation (exact commands, all must pass)

- Artifact replay: new gate logic on the preserved leg5 state yields
  drop-touched-deferred (loud) for s27.bin, strict PASS for the
  rest -- document the replay recipe (<5 min for orchestrator).
- Live `INVFS_E2E_AGENT=wp503 FLAKEY_ONLY=5 .../test-flakey.sh`:
  green; if a run tears another file under drop, the log must show
  the deferral (not THIRD STATE FAIL); an up-mode tear (prove by
  faulting one in up mode, e.g. targeted block overwrite on the
  mounted image... or argue why not) must still FAIL.
- `make test` — rc=0. `bash tools/check-repo-hygiene.sh` — OK.
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One logical change, one commit, on THIS branch only. Imperative
  message + why + validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/503-soak-oracle`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-503-soak-oracle-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp503`.

## Report back (exact format, no variation)

```
WP: wp/503-soak-oracle
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers, artifact replay + live both shapes)
Result: PASS | FAIL (failing assertion + log path)
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope (no engine changes). Record the blocker verbatim
in Remaining TODOs with file:line and hand back. End your reply with
the report (it is captured as your task output).
