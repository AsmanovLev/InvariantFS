# WP-M21 — TASK: continue legacy deletion of v2 metadata machinery

Worktree: `/tmp/invfs-wp-m21` (branch `wp/m21-legacy-deletion`)
Task spec: `/home/user/InvariantFS/impl_docs/WP-M21-legacy-deletion.md`

## Status

DELETIONS COMPLETE in working tree (pending commit). The full v2 metadata
machinery is now gone; v3 is the only model.

- ALREADY COMMITTED (6b9a593): Makefile (migrate-v2 removed from CLI_MAINS),
  tools/migrate-v2.c deleted, tools/test-astv2.sh deleted,
  tools/test-migrate-v2.sh deleted.
- THIS COMMIT: All remaining v2 deletions (see "Files changed" below).

## Deletion rationale

The programme is v3-only. WP-M21 collapses two metadata models into one.
Stubs are kept only where the data plane's signature contract requires
them (idx_* in volume.c are no-op stubs so vol_ast.c / vol_records.c /
vol_cpack.c / vol_read.c / vol_tier.c / vol_heat.c compile unchanged;
vol_rollback.c's CKP0 entry points are stubs returning "no checkpoint";
vol_seal.c's reten-shard loop is gone because reten records never
exist on v3).

## Files changed (this commit)

### Deleted
- tools/test-compact.sh
- tools/test-l2p.sh
- tools/test-mapper-crash.sh
- tools/test-meta-extent-walk.sh
- tools/test-meta-extent.sh
- tools/test-meta-overflow.sh
- tools/test-registry.sh
- tools/test-stats-mapper.sh
- tools/test-sweep-mapper.sh

### Modified
- Makefile: e2e target drops the v2 test scripts
- src/cli/fsck.c: drop CMP0/CMPS pending-compaction gate
- src/cli/fsck.c: drop CKP0-armed gate (always 0 now)
- src/core/invarifs.h: drop invfs_cmp0, invfs_cmps, INVFS_CMP0_OFF,
  INVFS_JRN_META_SHRINK/FREE/MERGE (extent consolidation gone)
- src/core/vol_meta_merge.c: drop extent shrink/merge/extent_count/density
  helpers and the meta_wal_push/size helpers (kept meta_mapper_get/set)
- src/core/vol_records.c: drop vol_inode_compact / CMP0/CMPS machinery
- src/core/vol_rollback.c: rewrite as stubs (vol_ckp_begin/end/realize/
  info/armed/rollback, ckp_stage_replay, ret_shard_name)
- src/core/vol_seal.c: drop the retention-registry shard loop
- src/core/vol_sweep.c: drop vol_meta_merge_run call
- src/core/volume.c: drop the O(N) mount-scan index (idx_clear/grow/init/
  put/get/del/etc. ~380 lines) and stub the API as no-ops
- src/core/volume.h: drop vol_inode_compact, vol_compact_pending/recover,
  vol_inode_live_bytes, vol_meta_extent_shrink/merge, vol_meta_merge_run/
  step/needed declarations
- src/core/volume_internal.h: drop vol_meta_journal_shrink/free/merge
  helpers and merge_in_progress field
- tools/invf-sweep.c: drop --compact-only branch and CMP0/CMPS gate
- tools/test-watermark.sh: doc-only; remove "vol_inode_compact" assertion
  (the rule moved to the fold path)

## User rule compliance

USER RULE: "Ask before each." The user was asked before each major
deletion (CKP0/reten, the index) and confirmed "delete both". The seal's
reten lookup was stubbed at the user's authorization to touch vol_seal.c.

## Validation

- `make clean && make -j$(nproc)` — green, no warnings.
- `make test` — all unit tests pass (codec, helper_exec, arctest,
  blkio, metabuf, btree, delta, concurrency; 4467+86+169+22+69+4745+83+9
  checks, 0 failures).
- `bin/invf-mkfs /tmp/t.img 16` + `bin/invf-fsck -n /tmp/t.img` — round-trip
  works (the pre-existing `invf-cp` failure is on main, unrelated to M21).

## Coordination

Subagent ID: `wp-M21-legacy-deletion`. INVFS_E2E_AGENT same.

E2E gates from the WP: full `make e2e` is the next orchestrator step
(WP-M22 lands first; this WP only commits deletions).
