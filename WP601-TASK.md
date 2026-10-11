# WP601-TASK — migration zone: defrag relocation + policy recompression

Branch: `wp/601-migration` · Worktree: `/srv/flakey/wt-601-migration`
Base: `main` @ 02211df. **Do not touch `main`. Do not touch other worktrees.**

## Why (one paragraph)

Owner decision: migration is defrag-consolidation (A) AND policy
recompression of existing files (B) together, one WP. Recon (done by
the orchestrator): tier hot-copy/demote exists (`vol_tier_migrate`,
copies only, never moves canonicals); heat tracking + codec
generations with retry policy exist; `vol_publish_blob_inode`
 republishes an inode's recipe in place (the move workhorse);
`heat_extract_v3` already demonstrates read→encode→decode-verify→
publish→stamp. What does NOT exist: (1) a canonical-segment relocate
protocol (copy→repoint→verify→free, crash-safe); (2) an explicit
recompress trigger (today only generation rollover + conditions);
(3) any consolidation pass (dedupe/batches leave holes nobody
fills). Seal tracks dirty by pba range (`vol_seal.c:91`), so moves
must mark dirty (stale), never silently ride along.

## Scope (files you may modify — nothing else)

- NEW `src/core/vol_migrate.c` (+ header): relocate protocol +
  hole-fill selection + policy-recompress dispatch. New file keeps
  the blast radius reviewable.
- `src/core/vol_sweep.c`: ONE new `migrate` stage call site between
  `batches` and `finalize` (seal runs after → seal-after-defrag
  economics: seal sees the compacted layout) + nothing else.
- `tools/invf-sweep.c`: `--migrate` / `--no-migrate` wiring +
  `--recompress` explicit trigger wiring ONLY.
- `src/core/vol_seal.c`: mark-dirty calls on moved ranges ONLY
  (the API exists -- consume it, do not redesign it).
- Regression coverage (extend the lightest fitting suites or a
  focused script -- justify placement).

Explicitly OUT (will be reverted on review): transform/dedupe/
tier-copy/batch logic (read them, do not alter), seal math/format,
`src/codecs/*`, tier-copy dirty-marking (existing behavior -- file
it if wrong, do not change here), CI workflow.

## Design (constraints)

PHASE A -- relocate + defrag (do first; Phase B builds on its protocol):
1. Move protocol per segment: alloc new -> copy bytes -> memcmp
   verify -> publish-checked repoint (the leg2 discipline:
   `vol_inode_delta_put` rc decides published, never assumed) ->
   seal-dirty-mark the old pba range -> free old (savepoint pins it
   for one generation -- the standard sweep debt, measured not
   feared). Crash-mid-move: pre-repoint death leaves garbage new
   segments (unclaimed, reclaim collects); post-repoint death leaves
   new-live + old-freed-or-pinned. NEVER a splice: the leg7
   assertion shape (complete-old or complete-new) applies per move.
2. House discipline (copy heat_promote + heat_extract_v3 + WP145):
   vol_walk receipt (refuse on incomplete walk), ENOSPC admission
   (moves coexist with sources until GC -- budget-capped per run),
   failure containment (a failed move leaves the source intact).
3. Selection v1 (measure + justify, do not gold-plate): fill holes
   left by dedupe/batches with size-class matching; cap blocks/run.
   Report the fragmentation metric before/after (free-run-length
   distribution or equivalent -- define it, then move it).
4. Stage order fixed: batches -> migrate -> finalize -> seal.
   Rationale in one line each in the report if you deviate.

PHASE B -- policy recompression (after A lands in-branch):
5. Explicit FIRST: `invf-sweep --recompress` re-runs the transform
   dispatch on demand (all files, or only below-current-generation
   with a flag -- propose, do not proliferate flags).
6. Heat-tiered SECOND (proposal -- measure both, owner reviews
   numbers at merge): read-hot files -> fastest codec that still
   shrinks; cold + large -> densest within mem limits; generations
   keep gating retries as today. If measurements say the tiering
   buys nothing, report that with numbers and ship explicit-only.
7. Every recompress goes through the lane's own bit-exactness guard
   (decode-back memcmp) -- a recompress that cannot prove itself
   keeps the original, stamps nothing, retries on generation roll.

## Validation (exact commands, all must pass)

- Fragmentation metric before/after on a holed fixture (define +
  quote); full readback cmp 0 mismatches; fsck + verify clean.
- Crash-mid-move probe (kill -9 during migrate): every file
  complete-old or complete-new, never spliced; fsck verdict sane.
- Moves mark seal stale (verify STALE lines); next sweep
  auto-reseals clean (modes respected).
- Policy: explicit recompress changes codecs measurably on a
  generation-pinned fixture; heat-tiered numbers quoted (or
  explicit-only with the negative result quoted).
- `make test` — rc=0. `bash tools/check-repo-hygiene.sh` — OK
  (vol-find ledger is line-sensitive).
- Rebuild test bins explicitly; never `| head` a suite pipe.

## Standing rules (binding, from AGENTS.md + session practice)

- One WP, two sequential phases, one branch, one commit per phase
  max (A-commit then B-commit). Imperative messages + why +
  validation lines.
- `/sbin` off PATH: `export PATH=$PATH:/sbin:/usr/sbin`.
- Push own branch ONLY with emptied proxy env:
  `env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY git push origin wp/601-migration`.
- `/srv/flakey`: subdir-only (`/srv/flakey/wt-601-migration-*`),
  NEVER others. Do NOT touch `/dev/sdb1` / `/dev/sdb2`. `/tmp`
  tmpfs is tight -- fixtures on your subdir.
- e2e ONLY via `tools/run-e2e.sh` with `INVFS_E2E_AGENT=wp601`.

## Report back (exact format, no variation)

```
WP: wp/601-migration
Files changed:
  <path>   (what + why, one line each, per phase)
Tests run:
  $ <exact command>
  ... (pass/fail + numbers: fragmentation metric, crash probe, policy numbers)
Result: PASS | FAIL (failing assertion + log path)
Design decisions taken: <stage slot, selection policy, tier defaults -- one line each + measured why>
Remaining TODOs: <none | list, each one line>
```

## If you get stuck

Do NOT widen scope. Phase A is the load-bearing half -- a clean
Phase-A PASS with Phase B handed back as TODOs beats a broad FAIL.
Record blockers verbatim with file:line. End your reply with the
report (it is captured as your task output).
