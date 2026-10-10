# WP203-TASK — pack signatures v1 (ed25519, refuse-bad, prompt-on-missing)

Branch: `wp/203-pack-signatures` · Worktree: `/srv/flakey/wt-203-sig`
(DISK-backed by design: /tmp is a tight tmpfs; same isolation rules apply.)
Base: `main` @ 52dd16c. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Volume packs (codec/container helpers executed by the daemon) are
installed from a registry with no authenticity check: a tampered helper
is arbitrary code execution as the mounting user. Owner decision: v1 =
sign manifests, verify on install, refuse-bad always, missing prompts.
No signatures exist anywhere in tree (verify by grep before assuming).

## Scope (files you may modify — nothing else)

- NEW vendored ed25519: `src/codecs/ed25519.{c,h}` (or equivalent names)
  — minimal audited implementation (~2k lines class: TweetNaCl /
  ed25519-donna family, public-domain/MIT-style). Document provenance
  (source URL + version + hash) in the file header. Evaluate libsodium
  vs minimal-vendor IN the work, default minimal-vendor (no new system
  dependency for a durability-adjacent feature); if evaluation flips the
  choice, document why and stop for orchestrator review before wiring it.
- Pack install/verify path: find it via the existing `-y/-n` install code
  (`invfs-pack install`, manifest format + parser). Manifest gains a
  signature envelope (detached `.sig` sidecar vs embedded field — your
  call, document it). Keyring location proposal (default
  `~/.config/invfs/keys`, overridable): document, do not also invent
  three alternatives.
- Keygen: minimal `keygen` subcommand (needed for round-trip tests).
- Tests: new unit round-trip (keygen→sign→verify→tamper→refuse) as a test
  bin + e2e leg (extend the pack test script that covers install, or add
  `tools/test-pack-sign.sh` following house style) + `Makefile` wiring.
- Docs: usage text where install is documented + one INCIDENTS-adjacent
  note? NO — docs/ only (`docs/` guides that describe install), plus
  `--help` text. No AGENTS.md/INCIDENTS edits (orchestrator-owned).

Explicitly OUT: pack EXECUTION paths (only install/verify gates!);
registry server changes; key ceremonies/revocation/PKI (file as
follow-up, do not build); auto-heal; RaptorQ/pyramid/mirror/scrub;
`tools/test-flakey.sh` legs; CI workflow; engine hot paths.

## Design (locked decisions + bounded choices)

LOCKED (owner decisions, do not relitigate):
1. Bad signature = REFUSE, always, no knob (attack evidence, not absence).
2. Missing signature = interactive `are you sure? [y/N]` + `--skip-signature-verification`
   flag for scripts. Must compose with existing `-y/-n` (`-y` answers yes,
   `-n` aborts, flag skips asking entirely).
3. Sign the MANIFEST (names + helper hashes), not each helper (redundant).
4. Minimal-vendor default (see above).

YOUR CALL within bounds (document + why): envelope format, keyring
default path, exact prompt wording/exit codes, where verify hooks in the
load path if at all (install-gate is mandatory; load-time re-verify is
optional — justify either way).

## Validation (exact commands, all must pass)

- Unit: keygen→sign→verify round trip green; tampered manifest AND
  tampered helper both refuse; missing-sig prompt path (yes/no/flag/-y/-n
  matrix — script it, do not hand-wave).
- New parser/web-of-trust surface reads untrusted bytes (manifests from
  the network!): strict validation, refuse-and-report; fuzz the parser
  via the fuzz harness if reachable in <1 afternoon, else write WHY NOT
  in Remaining TODOs (no silent skipping).
- `make test` rc=0 + `bash tools/check-repo-hygiene.sh` OK (citations +
  vol-find ledger are line-sensitive; new vol_find calls need ledger
  entries in the same commit).
- E2E gate for the pack install path via `tools/run-e2e.sh` with
  `INVFS_E2E_AGENT=wp203` (name the suite(s) in your report).
- Rebuild test bins explicitly (`make bin/<name>`).

## Standing rules (binding)

- One logical change, one commit, THIS branch only. Imperative message.
- `/sbin` on PATH. sudo passwordless; FUSE needs `user_allow_other`.
- `/srv/flakey`: subdir-only under `/srv/flakey/wt-203-sig-*` (NEVER touch
  other subdirs, `/dev/sdb1`, `/dev/sdb2`). No multi-GB fixtures on /tmp.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/203-pack-signatures`.
- NEVER `| head` suite pipes (redirect to files; `$?` after a pipe is the
  pipe's, not the suite's).

## Report back (exact format, no variation)

```
WP: wp/203-pack-signatures
Files changed:
  <path>   (what + why, one line each)
Tests run:
  $ make test / unit matrix / e2e gates (pass/fail + numbers)
Result: PASS | FAIL (failing assertion + log path)
Design decisions taken: <envelope format, keyring path, verify hook points, vendor choice + why — one line each>
Remaining TODOs: <none | list>
```

## If you get stuck

Do NOT widen scope (no PKI, no registry work, no engine). Record the
blocker verbatim with file:line and hand back FAIL + precise TODO.
