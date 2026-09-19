# WP45 — big-volume e2e fixture + suites

Branch: `wp/45-e2e-bigvol` · Worktree: `/tmp/invfs-wp45`

## Scope (ONLY these)
- `tools/test-fixture-bigvol.sh` — shared fixture: mkfs on a fresh
  sparse pair (dev0 1 GiB, dev1 2 GiB via `INVFS_DEV1`), build a source
  tree in `/tmp/opencode/bigvol-src` (~30k small objects: dirs,
  files with deterministic pseudo-random content from `awk` seeds,
  symlinks, a nested tree), then `invf-import` and record the canonical
  counts into a JSON that every suite sources.
- `tools/test-stats-mapper.sh` — import → `invf-ls`/`invf-stats`
  population + logical bytes + unclaimed expectations.
- `tools/test-sweep-mapper.sh` — import → offline `invf-sweep` →
  assert `swept`/`hashed`/`freed` counters NONZERO, logical==physical
  post-conditions, then `invf-cat` 20 seeded names vs original bytes
  (bit-exact) and `invf-fsck` clean.
- `tools/test-heat-mapper.sh` — import → mount+touch reads/heats →
  unmount → heat persist pass → reopen and assert the table non-empty
  for touched names.

## Why
Everything currently passes because fixtures fit one extent; the
stage3-sized volume (66k records over 214 extents) is where the
legacy walkers silently no-op. A fixture isolating ~30k objects across
MANY extents is the minimum e2e pressure to catch mapper-unaware walks
permanently.

## Style
Match existing `tools/test-*.sh`: `#!/bin/sh`, `set -u`, print
`PASS`/`FAIL`, exit codes, no `set -e` bash-isms, cleanup with
`fusermount -uz` and `rm -f` of its own volume paths (STRICTLY under
`/tmp/invfs-e2e-<suite>/`), and NO direct e2e runs without
`INVFS_E2E_AGENT=wp45 bash tools/run-e2e.sh <suite>`.

## Validation
This WP's suites start as EXPECTED-RED (they encode the bug) — they
must be marked `.skip`-tolerant for the first landing so the fixture
merges without breaking `make e2e` before WP41/42/43/44 land:
- add them behind `tools/test-meta-extent-walk.sh`-style gating: skip
  (SKIP in \"todo\" style) when the component is not fixed, i.e. the
  script itself checks `invf-stats` population and only asserts when
  nonzero — so they turn green automatically as the component WPs merge.

## Out of scope
- Any src/ change. Tools/fixture only.

## Coordination notes
- Fixture file names live under `/tmp/invfs-e2e-${SUITE}`; nothing
  shared with the QEMU boot volumes.
- `make e2e` gates run through `run-e2e.sh` with the standard lock.
