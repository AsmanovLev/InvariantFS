# WP619 — fatten thin packs (ape/flacr/wavpack) (TASK)

Branch: `wp/619-fatten-packs`
Worktree: `/tmp/invfs-wp619` (work ONLY here when launched)
Agent id: `INVFS_E2E_AGENT=wp619`

## Problem

`ape`, `flacr`, `wavpack` registry packs are manifest-only (~1 KB):
no bundled payload, they exec an EXTERNAL tool (`depends = flac`
etc.). In the disaster case (no OS, no system tools) and under
outside version drift they fail exactly when needed. Fatten them:
bundled static payload + pinned versions + hashes in the manifest,
externals demoted to fallback.

## Scope

- The `registry/` submodule (InvariantFS-registry): per thin pack,
  add a statically built payload (musl preferred, matching the
  registry's existing `build-helpers.sh` shape) + manifest fields +
  regenerated per-pack `sha256` + `codecpacks/index.json` via the
  registry's own `make index/check/test`. Read
  `registry/Makefile` + `registry/README.md` first.
- Main tree: verify bundled-first resolution against the fattened
  packs (unit/e2e proof that the bundled payload is preferred and the
  external is fallback-only); extend the helper link-list pattern if a
  new core object appears (WP613 precedent: every new core object must
  be added to the test helper link lists or links break).
- Out of scope: builtin demotion (separate follow-up), worker
  hardening, Python packs, pushing the registry repo (prepare only —
  orchestrator reviews and pushes both repos), merges to main.

## Design constraints (binding)

1. Static payloads only (no dynamic deps leaking host libraries in);
   versions pinned + hashed in the manifest (exact pins, ranges only
   with the install-time lock design — keep exact unless the lockfile
   shape is already implemented, in which case say so).
2. Thin behavior must keep working where the external tool exists
   (fallback, never removed in this WP).
3. No binaries into the MAIN tree (hygiene). Pack binaries live in the
   registry side only (it ships `.ivpack` releases by design).
4. Prove with tests: fat pack preferred over external (e.g. hide the
   external from PATH and show identical bytes), external fallback
   still green when payload absent, `make test` + hygiene green.

## Validation

- `make test` green (main worktree); registry `make check/test`
  green; e2e ONLY via `bash tools/run-e2e.sh` with
  `INVFS_E2E_AGENT=wp619`; `bash tools/check-repo-hygiene.sh` green.

## Report back (exact format, no variation)

```
WP: wp/619-fatten-packs
Files changed:
  ...
Tests run:
  ...
Result: PASS/FAIL
Remaining TODOs: ...
```

Do NOT merge to main (either repo). Commit in the branch early and
often. Registry-side commits stay in the submodule until the
orchestrator reviews them.
