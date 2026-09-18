# WP57 — heat persistence across FUSE sessions

Branch: `wp/57-heat-persist` · Worktree: `/home/user/invfs-wp57`

## Scope (prefer these files only)
- `src/cli/fuse_fs.c` (unmount/close path, sweep thread)
- `src/core/vol_heat.c` (persist/decay helpers)
- `src/core/volume.c` ONLY if strictly required (WP56 also touches volume.c;
  if you must, note it — it may force a rebase of this branch).

## Why
Read/write heat accrues in RAM per session and is persisted only by
`vol_heat_sweep_begin` (a sweep run) or an explicit `vol_heat_persist`.
`vol_close` deliberately does not persist (to avoid per-close churn), and the
FUSE background sweep is **off by default** (`INVFS_SWEEP_INTERVAL` opt-in).
Consequence: a guest that boots, reads many files, and powers off leaves
`heat=absent` on every record — verified on the swept stage3 volume
(`meta_probe --img --heat usr/bin/wcurl` → `heat=absent` after several boots).
The heat feature (promotion/decay) is therefore inert in the normal flow.

## Design (choose and justify)
- Persist accrued read heat on a **FUSE unmount** (before `vol_close`) when
  `v->heat_tab_n > 0`: one `vol_heat_persist()` fold. This is bounded (one fold
  per mount session) and matches user intent ("reads I did should count").
- Plus an optional bounded **periodic flush** (e.g. every N seconds of accrual
  and/or when `heat_tab_n` crosses a threshold) guarded so it cannot cause a
  per-op flush storm — reuse the watermark idea from `vol_should_flush`.
- Keep it OFF/churn-free for read-only/unmounted-tool opens; never persist heat
  from a time-travel handle.
- Consider an env/opt knob (e.g. `INVFS_HEAT_FLUSH_SEC=<sec>`, 0 = off) and
  document the default.

## Validation
```
$ make -j$(nproc) && make test                    # 4722 checks, 0 failures
# Functional proof on a scratch mapper volume:
#   mkfs+import, FUSE-mount, `cat`/read 100 files, unmount,
#   then: bin/meta_probe <img> --heat <one-read-file>  -> heat=... rheat>0
#   and  bin/meta_probe <img> --heat <untouched-file>  -> heat=absent
$ INVFS_E2E_AGENT=wp57 bash tools/run-e2e.sh tools/test-heat-mapper.sh
$ INVFS_E2E_AGENT=wp57 bash tools/run-e2e.sh tools/test-heat.sh
$ INVFS_E2E_AGENT=wp57 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
```
`tools/test-heat-mapper.sh` currently pumps via `invf-l2ptest`; additionally
assert the no-pump case (plain mount+reads+unmount) now shows persisted heat.
Watch record churn: heat stamps are record appends — verify the sweep/
compaction story still holds (report record growth for 100 reads).

## Rules
- Commit on `wp/57-heat-persist`; standard report; do not merge.
