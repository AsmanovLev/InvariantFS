# WP56 — batch-commit failure, duplicate mapper pba, --realize orphan leak

Branch: `wp/56-batch-realize` · Worktree: `/home/user/invfs-wp56`

## Scope (prefer these files only)
- `src/core/vol_textzone.c` (member commit / batch seal, `tz_commit_member`)
- `src/core/vol_sweep.c` (batch flush / deferral path)
- `src/core/vol_meta_merge.c` (`meta_get_owner_append_pos` / owner extent)
- `src/core/vol_rollback.c` (`--realize` / checkpoint retained-range release)
- `tools/invf-sweep.c` (sweep error reporting for the above)
- `src/core/volume.c` ONLY if the duplicate-pba root cause requires it (note
  it in the report; WP57 runs in parallel but touches different files).

## Why
`tools/test-mapper-crash.sh` leg 4, a full offline sweep on a small
two-device mapper fixture, fails after WP52 re-enabled batch deferral:
```
tz: commit failed for d03/f00001
tz: commit failed for d03/f00029
batch flush failed (rc=-1)
sweep done: swept=0 skipped=49 failed=1     -> sweep rc=1
```
Then the mapper table carries **1 duplicate pba** and `invf-sweep --realize`
leaves **457 orphan blocks**. `tools/test-sweep-mapper.sh` still passes 6/0 on
the 30k fixture, so this is content/fixture dependent.

## Repro
```
W=/tmp/opencode/wp56
rm -rf $W; BIGVOL_NDIRS=4 BIGVOL_PERDIR=50 sh tools/test-fixture-bigvol.sh $W
# (or just run the crash suite and keep its workdir)
INVFS_DEV1=$W/dev1.img bin/invf-sweep $W/dev0.img      # observe rc / tz errors
INVFS_DEV1=$W/dev1.img bin/invf-fsck -f $W/dev0.img     # orphans
# mapper audit: python at $W/mapper_audit.py in the crash suite workdir,
# or count duplicate pbas yourself (mapper_pba at sb+0x98).
```

## Tasks
1. Explain and fix the `tz_commit_member` / batch-flush failure (why does a
   member commit fail on this fixture but not the 30k one?). A failed member
   must at worst stay RAW (soft skip, rc=0), never fail the whole sweep.
2. Eliminate the duplicate mapper pba after a completed sweep. Check the
   WP52 owner-extent allocation and any mapper slot reuse; a pba must appear
   at most once in the mapper table.
3. `--realize` must free the checkpoint's retained ranges (no orphans); tie
   in with WP53's registry work.

## Validation
```
$ make -j$(nproc) && make test                    # 4722 checks, 0 failures
$ INVFS_E2E_AGENT=wp56 bash tools/run-e2e.sh tools/test-mapper-crash.sh
      # target: 23+ passed, 0 failed, 0 skipped
$ INVFS_E2E_AGENT=wp56 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
      # 6/0
$ INVFS_E2E_AGENT=wp56 bash tools/run-e2e.sh tools/test-binbatch.sh
$ INVFS_E2E_AGENT=wp56 bash tools/run-e2e.sh tools/test-textzone.sh
```
`invf-fsck` after sweep+realize: `orphans: 0`, `bad records: 0`.

## Rules
- Commit on `wp/56-batch-realize`; standard report; do not merge.
