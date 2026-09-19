# WP52 — flush-safe owner/aux record appends + re-enable batch deferral

Branch: `wp/52-owner-append` · Worktree: `/home/user/invfs-wp52`
Base: `main@23635ef`

## Scope
- `src/core/vol_tier.c` (`wp25_owner_write`)
- `src/core/vol_textzone.c` (`tz_owner_write`, `tz_owner_load`, `vol_tz_gc`)
- `src/core/vol_sweep.c` (remove the WP42 mapper gate on batch deferral)
- `src/core/vol_meta_merge.c` (`meta_get_owner_append_pos`)
- `src/core/volume.c` (`vol_append_owner_slot`, `alloc_state_reset`,
  metadata-accounting initialization)
- `src/core/volume_internal.h` (`tz_owner.ext_idx`, owner-extent fields,
  helper declarations)
- `src/core/vol_crash.c` (`vol_should_flush` — see below; required for the
  flush storm that the owner rewrite exposed)
- `tools/test-sweep-mapper.sh` (acceptance expectations — see below)

## Why
On mapper volumes the owner append wrote through the legacy cursor
(`v->inode_area_pos`), so a large owner record (`\x01rawm`, ~516 KiB) was
placed inside whatever extent the cursor pointed at and ran past its end —
CRC valid, record unreachable. WP49's fsck reported `bad records: 2` on the
swept bigvol fixture. The same path is why WP42 disabled cross-file batch
deferral (PPMd text / ZSTD+BCJ binary) on mapper volumes.

## Design (as landed)
1. **Dedicated, extent-sized owner append.** `meta_get_owner_append_pos`
   picks the size class from the whole record (`while (65536<<sc) < total`),
   reuses the owner's own extent while the record fits, and grows it in
   place by size class otherwise; a fresh extent is allocated only when no
   adjacent run exists. The record can never run past its extent, and the
   allocation is an ordinary `alloc_meta_extent`/persist path — no re-entrant
   `vol_flush`. `vol_append_owner_slot` exposes it and leaves the shared
   file-record cursor (`inode_area_pos`, `met0.active_offset`) untouched.
2. **In-place rewrite = no self-tombstone.** When `new_pos == old_pos` the
   owner record replaced itself; emitting the position-kill tombstone would
   name `new_pos` and kill the record just written. Both writes emit only
   record+CRC in that case.
3. **Mapper-aware flush watermark.** `vol_should_flush` measured the legacy
   `[inode_area_start, inode_area_end)` region, which is meaningless on a
   mapper volume (block count vs byte offsets); it read "full" from the
   first record, so every append flushed and every flush allocated a fresh
   owner extent. It now measures the active extent's fill.
4. **Re-enable deferral.** Removed `sweep_batch_defer_ok()` and all its
   call-site guards. Legacy `format_version=0` is unchanged.
5. **GC + stats fixes exposed by deferral.** `vol_tz_gc` frees dead batches
   from the self-describing owner entry pba (`seg_extent`), not just the
   owner-WAL lookup; `vol_compute_stats` no longer counts `\x01` owners as
   regular files.

## Validation
```
$ make CC=gcc -j$(nproc) && make test          # 4722 checks, 0 failures
$ INVFS_E2E_AGENT=wp52 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
      # 6 passed, 0 failed (fsck bad records 0)
$ INVFS_E2E_AGENT=wp52 bash tools/run-e2e.sh tools/test-binbatch.sh   # PASS
$ INVFS_E2E_AGENT=wp52 bash tools/run-e2e.sh tools/test-textzone.sh   # PASS
$ INVFS_E2E_AGENT=wp52 bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
      # 6 passed, 0 failed
```
`invf-fsck` on the swept bigvol fixture (`INVFS_DEV1=…/dev1.img`):
`orphans: 0`, `missing: 0`, `bad records: 0`.

### Test change note
`tools/test-sweep-mapper.sh` asserted `swept > 0` and `dedupe hashed > 0`.
Both are zero on an all-text fixture **once WP52 re-enables deferral**: the
files are batched (`text -> PPMd`) instead of moved to Shadow one at a time,
and dedupe legitimately hashes 0 because every live entry is a shared TEXT
batch slice (dedupe skips those). The checks now accept either the generic
floor (nonzero swept/hashed) or the deferred path (nonzero deferred), which
preserves the suite's actual genome — "the sweep walk saw the records" —
while allowing the feature WP52 mandates.

## Out of scope
- WP53 (checkpoint registry warning), WP54 (provisioning), WP55 (crash
  legs), WP56 (heat persistence), WP57 (audit/assert/freeze).

## Result
PASS. Committed on `wp/52-owner-append`; not merged to main.
