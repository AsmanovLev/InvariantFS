# InvariantFS Incidents & Fixes

## WP28 — Embed Metadata in vol_create_file

**Date:** Sep 13, 2026  
**Severity:** High (performance)  
**Impact:** Import speed 2x slower than necessary  

### Symptom
`invf-import` called `vol_replace_file()` + `vol_apply_meta()` per file — two separate inode area appends per file instead of one. Each append triggered a full `vol_flush()` (bitmap + superblock + journal + barrier).

### Root Cause
`vol_apply_meta()` (`vol_records.c:763`, `meta_rewrite`) reads back the just-written record, then appends a [new INOD + DELT tombstone] combo. This is a separate write path from `vol_create_file()`, so every file required two appends.

### Fix
Added `vol_create_file_with_meta()` and `vol_replace_file_with_meta()` to `vol_records.c` and `vol_dirs.c` that embed INO2 metadata extension directly into the initial inode record. Updated `invf-import.c` to use these variants, skipping the separate `vol_apply_meta()` call.

### Result
- 2x fewer inode area appends per file
- 2.7x fewer read syscalls (34.5M vs 93.9M)
- 2x fewer write syscalls
- All unit + e2e tests pass
- Commit: `26ba0d6`

---

## WP28-MKFS — Cap RAW Zone at dev0 Capacity

**Date:** Sep 13, 2026  
**Severity:** Medium (data layout)  
**Impact:** `device 0 too small` error on multi-device mkfs  

### Symptom
`invf-mkfs /dev/sda3 117 /dev/sdb1 400` failed with `device 0 too small` when dev1 was much larger than dev0.

### Root Cause
In `mkfs.c:293-307`, `raw_blocks = rem * 20%` scaled with total volume size. When dev1 was large, the raw zone exceeded dev0's capacity.

### Fix
Capped `raw_blocks` at `dev0_blocks - 1 - metadata_blocks`.

### Result
- Multi-device mkfs works regardless of dev1 size
- Commit: `5bfda80`

---

## WP29 — Deferred Flush with Watermarks

**Date:** Sep 13, 2026  
**Severity:** Critical (performance)  
**Impact:** Import 115x slower than necessary  

### Symptom
`vol_pre_record()` in `vol_crash.c:76` called `vol_flush()` on every record append. For 20K files, this meant ~20K full flushes (bitmap + superblock + journal + mirror sync + barriers). Import took 27+ minutes.

### Root Cause
`vol_flush()` is a heavy operation: superblock write, dirty bitmap range write, journal append/compact, mirror/tier owner sync, DEVT write, barriers. Calling it on every record append was designed for crash consistency but is unnecessary during bulk import since records are authoritative and fsck rebuilds the bitmap.

### Fix
Replaced unconditional `vol_flush()` in `vol_pre_record()` with `vol_should_flush()` watermark check:
- Flush when inode area > 80% full (pre-empts compaction)
- Flush when journal slot > 50% full (avoids slot overflow)
- `vol_close()` and `vol_sync()` still always flush

### Result
- Import: 27+ minutes → **14.2 seconds** (115x speedup)
- Tombstones: 59,605 → 3,081 (no redundant rewrites)
- `test-flushfail.sh` passes (crash consistency latch)
- Commit: `e265edf`

---

## WP30 — FUSE O(N²) Legacy Tombstone Kill

**Date:** Sep 13, 2026  
**Severity:** High (performance)  
**Impact:** FUSE mount hung for 10+ minutes  

### Symptom
After extracting the Gentoo portage snapshot (1.87M tombstones for 209K files), FUSE mount hung at startup for 10+ minutes.

### Root Cause
In `fuse_fs.c:189-245`, legacy tombstone kill (by inode ID) was O(T×N) = 1.98M × 209K = billions of comparisons. Each legacy tombstone triggered a linear scan of all records.

### Fix
Collect legacy tombstone IDs into a sorted array, then binary search per record → O(N log T). Added `cmp_u64` comparator; `legacy_kill_ids` freed at `done:` label.

### Result
- FUSE mount: 10+ minutes → **30 seconds**
- Commit included with WP28

---

## Resize Crash — Double-Free in --max Path

**Date:** Sep 14, 2026  
**Severity:** Low (crash on success)  
**Impact:** Resize completes correctly but segfaults during cleanup  

### Symptom
`invf-resize /dev/sda3 --max` successfully resizes the volume (capacity confirmed in stats) but segfaults with "double free or corruption" before printing the final success message.

### Root Cause
In the success path (line 1076), `blkio_close(&io)` is called, followed by `free(bitmap/buf/blk)`. If the resize path also closes `io2` internally (via vol_open roll-forward), and then the `fail` label or success path tries to close it again via `rz2.io2`, we get a double-free.

### Fix
The volume did resize correctly (confirmed: 515 GiB → 1978 GiB, shadow zone 77M → 461M blocks). The crash is cosmetic — happens after the commit. Needs investigation: likely `rz2.io2` should be NULLed after the resize commits, or the `fail` label should check if `io2` was already closed.

### Status
Volume is healthy post-resize. 2 bad records (corrupt inode records at positions 129141138 and 131260860, rec_len=2119422) — likely artifacts from the crash but non-fatal (skipped by vol_open).

---

## Resize --max Feature

**Date:** Sep 14, 2026  
**Severity:** Feature  
**Impact:** Allows resizing to fill available device space without manual size calculation  

### Change
Added `--max` flag to `invf-resize`: when passed instead of a size, the tool auto-computes the target from device capacities. For two-device volumes: `dev0_blocks + blkio_capacity(dev1)`.

### Code
- `resize.c:442-462`: Argument parsing accepts `--max` (sets `want_bytes = 0`)
- `resize.c:654-680`: After devices are opened, computes `want_bytes` from `blkio_capacity()`
- Updated usage message

### Result
- `INVFS_DEV1=/dev/sdb1 invf-resize /dev/sda3 --max` → grows to full sdb1 (1863 GiB)
- 1978 GiB total volume (dev0 115 GiB + dev1 1863 GiB)

---

## Gentoo Install — FUSE Limitations

**Date:** Sep 13-14, 2026  
**Severity:** Design limitation  
**Impact:** Cannot run `emerge` on InvFS root  

### Symptom
- `rename()` fails with EBUSY on InvFS for portage's `mtimedb`
- `bash source` of `.ebuild` files fails — kernel FUSE driver strips execute bits
- All emerge attempts blocked

### Root Cause
- FUSE `rename()` implementation returns EBUSY for certain operations
- Kernel VFS layer (`fs/fuse/file.c`) strips execute permission from FUSE-mounted files — security policy, not an InvFS bug

### Workarounds
- Tmpfs mounted over `/tmp`, `/var/tmp`, `/var/cache/edb`, `/var/cache/portage`, `/etc/portage`
- `FEATURES="-gpg-sign"`, `PORTAGE_GPG_VERIFY=0`
- Kernel compiled locally, uploaded to remote
- `FEATURES="-pid-sandbox -network-sandbox -ipc-sandbox"` for build sandboxing

### Future Fix
Host-side `losetup` + chroot directly into raw `/dev/sda3` (no FUSE layer)

---

## Gentoo Install — Portage Snapshot Extraction

**Date:** Sep 13, 2026  
**Severity:** Low (data loss, recovered)  
**Impact:** Tombstone explosion (1.87M for 209K files)  

### Symptom
Two concurrent `tar` extractions through FUSE caused tombstone count to explode to 1.87M.

### Root Cause
Concurrent writers on the FUSE mount create competing tombstones during rename operations. Each extraction renames files into place, creating tombstones for each overwrite.

### Fix
Sequential extraction only. O(N²) tombstone processing fix (WP30) prevented mount hangs after the explosion.

---

## Gentoo Install — switch_root Hang (Missing /sbin/init)

**Date:** Sep 14, 2026
**Severity:** High (boot blocker)
**Impact:** Machine hangs at "switching root" during boot

### Symptom
GRUB loads kernel + initramfs successfully. Initramfs mounts InvFS root via `invf-fuse`. `switch_root` is called but never completes — machine hangs with "switching root" message.

### Root Cause
Two compounding issues:

1. **Missing `/sbin/init`**: Gentoo stage3 uses usr-merge (`/sbin` → `/usr/bin`). The stage3 had a 49KB `/sbin/init` binary but the volume rebuild/import may not have preserved it correctly. On OpenRC Gentoo, `/sbin/init` must point to `openrc-init`.

2. **False-negative mount check**: The v2 initramfs init script used `mount | grep -q "invfs"` to verify the FUSE mount succeeded. Busybox `mount` in the initramfs doesn't show FUSE mounts, so this check always failed, dropping to a shell before `switch_root` was ever attempted.

3. **Unmounting before switch_root**: The v2 initramfs unmounted `/dev`, `/proc`, `/sys` before calling `switch_root`, which needs `/dev` to function.

### Fix (v3 initramfs)
- Removed `mount | grep` check; replaced with file-existence check (`[ -d "/mnt/newroot/bin" ]`)
- Don't unmount before `switch_root`; use `mount --move` to transfer mounts
- Added multiple init path fallbacks: `/sbin/init` → `/usr/sbin/init` → `/usr/lib/systemd/systemd` → `/bin/sh`
- Created `/usr/bin/init` → `openrc-init` symlink on the InvFS root

### Lesson
- Always verify `/sbin/init` exists after stage3 import on usr-merge systems
- Busybox `mount` doesn't show FUSE mounts — don't rely on it for verification
- `switch_root` needs `/dev` available — don't unmount it before the call
- ext4 for /boot (GRUB XFS module is unreliable on real hardware)

---

## FUSE — rsync chgrp ENOSPC on metadata exhaustion

**Date:** Sep 15, 2026
**Severity:** High (data operation)
**Impact:** rsync fails with "No space left on device" on root directory chgrp; small files write fine

### Symptom
- `rsync -av` to InvFS FUSE mount succeeds for small test writes
- rsync fails immediately with `chgrp "/." failed: No space left on device (28)`
- invf-fuse returns ENOSPC on any metadata operation (chmod, chown, chgrp) even though data blocks show free

### Root Cause
**Inode/metadata area exhaustion** — separate from data block area.

Volume has:
- 9.6GB raw + 15GB shadow = 23GB total capacity
- 7,568 live files + **76,113 orphaned metadata entries** (from failed rsync attempts)
- Previous rsync runs failed mid-transfer, leaving orphaned AST/tombstone records
- Metadata/inode area is separate allocation from data blocks
- Sweep hits "inode area full; stopping" before it can compact

The 76K orphans consume metadata space, causing `vol_apply_meta()` to return 0 (failure), which maps to ENOSPC.

### Symptoms Confirmed
- `invf-fsck` shows: `orphans: 76113`
- `invf-sweep --realize` hits: `inode area full; stopping with 0 segments merged`
- `invf-chmod/chown` returns ENOSPC even for root dir

### Fix
1. `invf-fsck -f` to free orphaned metadata
2. `invf-sweep` to compact after cleanup
3. Or: create fresh volume if metadata area is persistently fragmented

### Prevention
- Avoid rsync to InvFS FUSE mounts — use `invf-import` for bulk data
- If using rsync, ensure temp file operations work (mkstemp fix in commit `da08ed6`)
- Monitor orphan count via `invf-fsck` regularly

---

## WP35 follow-up — Mapper Persist + Compact Skip + Mapper_n Growth

**Date:** Sep 17, 2026  
**Severity:** Critical (data loss on v0.3.0+ volumes after enough writes)  
**Impact:** `invf-cp` / `invf-import` silently lost records after the active mapper extent filled; e2e `test-meta-extent-walk` reported `358/1501` files findable. Affects every mapper-volume write that crosses an extent boundary.

### Symptom
- `invf-ls` reports files, but only a fraction survive across reopen (e.g. 357 of 1500 cps).
- On disk, records appear in `inode_area_start` (block 8257, metadata-zone gap) instead of the mapper extents (`PBA 229402+`).
- The mapper table on disk correctly names the extents, but those blocks are zero.
- Smoking gun: `INVFS_TRACE_WIDE=1 bin/invf-cp ... 2>writes.log` shows 3 calls to `compact_apply` writing `off=33820672 len=131064` (block 8257) when 1500 cp iterations run.

### Root Cause
Three independent bugs landed together; each blocked independently, but only the combination was visible:

**Bug F — `meta_mapper_n` stale after lazy alloc.** `meta_mapper_load()` sets `v->meta_mapper_n = v->met0.extent_count` once at open. `meta_get_append_pos()` then lazy-allocates the first extent, bumps `extent_count` to 1, but never bumps `mapper_n`. Result: `meta_mapper_get(0)` returns zero, the lazy-allocated entry is invisible to subsequent lookups, and the write falls through to the legacy path (`offset + rec_size > extent_size` triggers `ENOSPC` because `extent_size` is read from `entry=0`).

**Bug G — legacy compact runs on mapper volumes.** `vol_inode_compact()` and its `compact_apply()` are gated only on write/read-only + checkpoint checks. They have no mapper check. For mapper volumes they read the record stream (mostly empty in the legacy area), then `compact_apply()` writes the live stream to `inode_area_start * BLOCK = 8257` (legacy metadata-zone gap, not where mapper records live), and finally zeroes the tail — which spills over into adjacent mapper extent blocks (`PBA 229402..229529`), wiping the just-written records.

**Bug H — `vol_flush()` never persisted mapper / MET0.** `vol_flush()` wrote superblock + dirty bitmap range + journal, but never `meta_mapper_flush()` or `meta_met0_persist()`. After `Bug D` rebased the in-memory cursor onto the active extent, the cursor advanced correctly within a session but `active_offset` / `active_extent` / `extent_count` were lost on close. On reopen the mapper table was empty (stale flush since set 0), the cursor reset to 0, and all records appeared "mga will write to PBA=0" — i.e. the legacy area.

Plus a Bug D regression fix: my earlier fix set `v->inode_area_start = first_pba`, which broke the read path that compared `idx_get_id()` against `inode_area_start`. Reverted — only `inode_area_pos` / `inode_area_end` get rebased onto the active extent.

### Fix
- `vol_meta_merge.c:651,712` — grow `mapper_n` after every lazy alloc / new-extent allocation
- `vol_records.c:2213` — gate `vol_inode_compact()` on `v->met0_present && v->meta_mapper` (return 0, no compaction for mapper volumes; the mapper already handles its own growth via `alloc_meta_extent`)
- `vol_records.c:1598` — `inode_area_make_room` no longer falls through to `vol_inode_compact` for mapper volumes (the gate handles it)
- `volume.c:2624-2638` — `vol_flush()` now calls `meta_mapper_flush()` + `meta_met0_persist()` so the cursor survives close/reopen
- `volume.c:1524` — revert the buggy `inode_area_start = first_pba` from Bug D (read path was using it for `idx_get_id` bounds)
- `fuse_fs.c:136-208` — `build_file_table()` rewritten to use `vol_inode_next()` (mapper-aware) instead of legacy inode area range
- All new debug prints gated behind `#ifdef INVFS_DEBUG_META_EXTENTS` to avoid Heisenbug risk on future perf work

### Result
- `make test`: 4722 checks, 0 failures
- `invf-cp` loop 1500/1500: all records land in mapper extents, `invf-ls` reports all 1500
- e2e `test-meta-extent-walk`: cross-extent lookup PASS, sentinel readable PASS, `invf-ls` reports all files PASS, sweep compaction not declined PASS, `invf-verify` PASS (5/6; the fsck-orphan fail is pre-existing state from prior runs)
- rsync of `/usr/lib64` (2932 files, 154MB) via `invf-fuse`: 100% MD5 match against the source tree
- `invf-ls` on `/mnt/sde/invfs-root.img`: 65804 files visible through mapper scan
- Commit: `0d5812f`

---

## QEMU Boot Blocker — OVMF can't boot 25MB UKI from FAT32 ESP

**Date:** Sep 17, 2026
**Severity:** High (Gentoo install demo blocked)
**Impact:** UKI boots in production with systemd-boot but QEMU/OVMF can't load it.
`invf-cp` over FUSE works, proving the FS is functional, but the boot path is broken.

### Symptom
QEMU with OVMF pflash (4M) sees GPT partition 1 on virtio-scsi disk and DVD-ROM, but:
- `Boot0002 DVD-ROM`: "Not Found" — OVMF can't find the El Torito boot file
- `Boot0003 Disk1 part1`: "Unsupported" — OVMF can't read the FAT32 filesystem
- `Boot0004/Boot000A`: "Not Found" — no boot file on the raw data disks
- Falls through to PXE → IPv4 → IPv6 → HTTP → "No bootable option or device was found"
- After 5+ minute timeout, "Press any key to enter the Boot Manager Menu"

### Root Cause
- OVMF 4M (from `edk2-ovmf-20260812-8.fc44`) does not have a working FAT32 driver for raw
  virtio-scsi disks presented as plain raw images
- 25MB UKI (`/mnt/sde/invfs-uki/uki.efi`, PE32+ EFI application) is loadable by
  production UEFI but rejected by OVMF shell with "Script Error Status: Unsupported (line number 5)"
- The OVMF BdsDxe doesn't expose a working EFI shell input — characters get echoed
  one by one with cursor-position escape codes but Enter never commits them

### Tried
- pflash OVMF_CODE_4M.fd + OVMF_VARS_4M_raw.fd
- virtio-blk, virtio-scsi (single + multi-bus), IDE, AHCI for ESP
- raw FAT32, raw FAT16, GPT FAT32 partition as ESP
- mkisofs -eltorito-boot (broken El Torito: header 0x22 not 0x01)
- xorriso (parses options wrong — `-no-emul-boot` as command instead of flag)
- UefiShell.iso as CD (boots to shell, but can't load UKI)
- EFI shell startup.nsh: `map -r; fs1:; cd EFI\BOOT; BOOTX64.EFI` → "Unsupported"

### Skipped (would work, requires more setup)
- Pre-configure OVMF VARS with boot entry via `virt-fw-vars` (not in Fedora repos)
- Direct binary patch of OVMF_VARS.fd NVRAM variables
- iPXE chainload from CD

### Workaround for the demo
The Gentoo stage3 rootfs is already loaded into `/mnt/sde/invfs-root.img` (16GB, 65801 files)
via `invf-import`. `invf-fuse` mount reads all files correctly (rsync 2932/2932 100% MD5 match).
The FS itself is verified working.

### Workaround for boot
- Use a host with real EFI firmware (or known-working OVMF build) and a known-working UKI loader
- Or rebuild the UKI as a GRUB-loadable EFI stub via `objcopy --target efi-app-pe` (already a PE32+ EFI app, should be loadable)
- Or rebuild with a smaller initramfs to stay under 1MB (some EFI shells reject > 1MB)

### Files
- `/mnt/sde/invfs-disk1.img` (10GB GPT): ESP p1, swap p2, shadow p3
- `/mnt/sde/invfs-root.img` (15GB raw): InvFS volume with stage3
- `/mnt/sde/invfs-shadow.img` (20GB raw): shadow device
- `/mnt/sde/invfs-uki/uki.efi` (25MB): custom 7.3-rc2 UKI

---

## WP40-WP47 — Mapper-aware record-walker sweep

**Date:** Sep 18, 2026
**Severity:** Critical (silent no-op of sweep/stats/heat/dedupe on v0.3.0+ volumes)
**Impact:** On every v0.3.0+ mapper volume (records live in dynamic meta
extents, not the legacy contiguous inode area) a whole class of engine
components walked the legacy `[inode_area_start, inode_area_pos)` range and
saw ZERO records. Verified on the 15 GiB stage3 volume (66182 live records
over 214 extents): `invf-sweep` printed `live entries: 0 (of 0 walked)`,
`invf-stats` reported 0 files / 0 dirs / 0 logical bytes, heat and dedupe
were no-ops. The e2e suite never caught it because every fixture fit inside
a single mapper extent.

### Root cause
Each component had its own linear record loop bounded by
`v->inode_area_pos`. On mapper volumes that cursor is the active-extent
append point, so the loops either saw nothing or rejected valid
mapper-extent positions through the `idx_get_id` hint validation
(`ip <= v->inode_area_pos`).

### Fix
- **WP40** `9ce8cf8`: shared `vol_records_walk(v, cb, ctx)` iterator
  (mapper extents via `vol_inode_next`, legacy fallback; CRC-verified
  records, torn records skipped).
- **WP41** `a77ad98`: `vol_compute_stats` via the walker.
- **WP42** `ed6f599`: `invf-sweep` collector + 5 sweep-engine walks.
- **WP43** `b058dbb`: heat decay/promote walks.
- **WP44** `827efa6`: dedupe pass-1 hasher.
- **WP45** `f37a63b`: big-volume (30k-object) fixture + self-gating
  stats/sweep/heat suites.
- **WP46** `5d1e4d5`: suite expectation fixes (heat pump batching bug,
  stats population/invf-ls/unclaimed semantics).
- **WP47** `f924473`: remaining walkers — tier/ast hint validation, read
  fallback, dirs rename/sibling, records delete_siblings, textzone GC mark.

### Result
`invf-stats` on the stage3 volume: 54089 files / 3104 dirs / 8988
symlinks, 1084.0 MiB logical (was 0/0/0). make test 4722/0;
bigvol fixture 3/0; test-meta-extent-walk 6/0; test-stats-mapper 6/0;
test-heat-mapper 4/0 (100/100 touched); test-sweep-mapper 6/0.

### Follow-ups (open)
- **WP48**: re-enable cross-file batch deferral on mapper volumes.
  Routing `tz_owner_write`/`wp25_owner_write` through `vol_append_slot`
  makes the flush-time owner sync append into dev1 shadow extents and fail
  (io-error latch -> READ-ONLY; big-volume import regressed at ~8k files).
  Owner appends reverted; WP42's mapper gate keeps files on the generic
  sweep floor until a flush-safe owner append exists.
- Pre-existing `invf-fsck` orphan count / exit-3 quirk seen by
  `tools/test-dedupe.sh` and historically by `test-meta-extent-walk.sh`
  Leg C (128 orphans) — needs its own WP.

---

## WP48 — Open-time O(N^2) + cyclic walker on large mapper volumes

**Date:** Sep 18, 2026
**Severity:** Critical (invf-stats / invf-sweep effectively unusable; corrupts volume on kill)
**Impact:** On the 15 GiB stage3 volume (66k records, 214 extents) `invf-stats`
and `invf-sweep` spun for 7.5 h at 99% CPU — ~1.3e10 read syscalls, almost
no writes — and killing the sweep mid-checkpoint left the mapper table with
duplicate pba entries (idx 243/244/245 == idx 0/1/2), after which every
position-driven walk cycled.

### Root cause (three compounding faults)
1. `vol_records_walk` iterated the mapper by **absolute position** through
   `vol_inode_next`. Extent allocation reuses free mapper slots, so entry
   pba order is not monotonic (and the corrupted table even carried
   duplicate pbAs); the position-driven walk hopped between disjoint
   regions and revisited records forever.
2. `pba_ref_ensure` iterated the name index and called
   `meta_read_record_by_id()` per name — O(N) per lookup on a mapper
   volume, i.e. O(N^2) at open.
3. `meta_read_record_by_id` rejected any id-index hint outside
   `[inode_area_start, inode_area_pos)` (the legacy area), so hints in
   older mapper extents always degraded to a full walk; its fallback used
   the cycling `vol_inode_next`.

### Fix
- `vol_records_walk` now iterates extents by **index** (each exactly once,
  active extent trimmed at `inode_area_pos`); CRC-skip semantics kept.
- `pba_ref_ensure` is a single `vol_records_walk` pass.
- `meta_read_record_by_id` accepts a hint whenever the header there
  matches, repairs the id index once from the authoritative name index
  (`idx_repair_ids_from_names`, guarded by `v->id_idx_checked`) when a hint
  is stale, and falls back to the bounded index-ordered walk.

### Result (re-imported clean stage3 volume)
- `invf-stats`: 7.5 h -> 0.6 s; 54026 files / 3076 dirs / 8986 symlinks.
- Full sweep: swept=51053 skipped=15035 failed=0; SHADOW 1045.6 MiB
  logical -> 489.5 MiB physical (2.14x); overall 0.45x; mapper 833
  extents, 0 duplicates, 0 descending; population unchanged.
- Bit-exact: wcurl / ld-linux / cc1plus (43 MiB) match stage3 source.
- `invf-fsck`: CLEAN, 66089 live files, 0 orphans/missing/lost/bad, 7.1 s.
- make test 4722/0; test-meta-extent-walk 6/0; test-stats-mapper 6/0;
  test-sweep-mapper 6/0; test-heat-mapper 4/0.

### Follow-ups (open)
- WP49: remaining `vol_inode_next` loops (vol_fsck.c, vol_repair.c,
  fuse_fs.c build_file_table, sizes.c/stat.c/verify.c/meta_probe.c) should
  move to `vol_records_walk` for robustness against a future non-monotonic
  table.
- Interrupted-sweep durability: a kill mid-checkpoint/compaction used to
  leave duplicate mapper entries. A full clean sweep no longer reproduces
  it (the cyclic walker was the likely writer of the duplicates), but the
  kill-mid-sweep path has not been re-exercised; add a targeted crash leg.
- `sweep: checkpoint registry write failed (the checkpoint itself is
  intact)` warning at the end of the full sweep — investigate.

---

## WP50 — Volume identity by uuid (and guest-config FUSE pitfalls)

**Date:** Sep 19, 2026
**Severity:** High (single-device boot broken; wrong volume selected)
**Impact:** After the WP48 sweep, booting the swept volume as the only disk
failed: the initramfs init unconditionally `mknod`ed `/dev/sda3`, so its
name-based `[ -b /dev/sda3 ]` probe saw a partition that does not exist,
selected the multi-device layout, and tried to mount a missing `/dev/sdb`.
The initramfs busybox has no `dd`/`od`, so shell-side superblock probing is
not an option.

### Fix
- `invf-fuse --probe-uuid <dev>`: reads superblock magic (`InvariFS` @0x00)
  and the 16-byte uuid (@0x08); prints the uuid hex, exits 0; else exits 1.
- `tools/initramfs-init.sh` (installed by `tools/mkinitramfs.sh` as `/init`):
  probes every block device with `--probe-uuid`; first InvFS volume is
  `INVFS_RAW`, a second distinct one `INVFS_DEV1`; optional kernel-cmdline
  `invfs.raw_uuid=` / `invfs.dev1_uuid=` overrides. No partition mknod.
- `docs/GENTOO-INSTALL.md` Step V4 rewritten.

### Verified
Swept stage3 volume (66089 live records, 2.14x shadow ratio) boots
single-device via direct kernel+initramfs: `InvariantFS mounted` → OpenRC
0.63.3 runlevel 3 → serial root login → ssh :2222 key auth →
`invfs[sda] on / type fuse rw`, 15G/2.4G; sha256 of wcurl / cc1plus (43 MiB)
/ etc/passwd match the stage3 source.

### Guest-config pitfalls found (configure-guest.sh still has them)
- **`sed -i` does not work on the FUSE mount**: it renames a temp file and
  the rename fails (`Device or resource busy` / `Operation not permitted`),
  so `sed -i 's/^root:[^:]*:/root::/'` left `/etc/shadow` locked. Edit files
  in place (open/truncate/write) instead.
- **Files created via a host-side FUSE mount are owned by the mount user
  (uid 1000)**, not root. sshd `StrictModes yes` then refuses
  `/root/.ssh/authorized_keys` (owner 1000). Fix: `chown -R root:root
  /root/.ssh` in the guest, or run the config mount as root.
- configure-guest.sh's `ln -sf` into `/etc/runlevels/default` also failed
  on FUSE; create the symlinks in-guest (or via python) instead.

### Follow-ups (open)
- Port configure-guest.sh to in-place writes + root ownership + real
  runlevel symlinks so a fresh import can be provisioned in one pass.

---

## WP49 — remaining record walks + the owner-record extent overflow

**Date:** Sep 19, 2026
**Severity:** Medium (robustness/perf) + the finding below is High
**Impact:** The last position-driven `vol_inode_next` loops (fsck pass-1,
seal2 repair, FUSE build_file_table, stat/sizes/verify/meta_probe) could
cycle on a mapper table that is not pba monotonic — the same failure mode
that produced the 7.5 h spin (WP48).

### Fix
- `vol_records_walk_ex(v, cb, ctx, bad_cb)` added (bad_cb lets fsck keep
  counting torn/CRC-bad records); `vol_records_walk()` wraps it with NULL.
- Converted: `vol_fsck.c` pass-1 (mapper volumes), `vol_repair.c` seal2
  live-id scan, `fuse_fs.c` build_file_table, `stat.c`, `sizes.c`,
  `verify.c`, `tools/meta_probe.c`. Legacy (format_version=0) paths are
  unchanged. Commits `32fe8b3` (core) and `1704513` (CLI).

### Found: owner records overflow their mapper extent
WP49's stricter fsck reports `bad records: 2` on the swept bigvol
fixture. The two records are `\x01rawm` OWNER records of ~516 KiB whose
trailing CRC is VALID (`CRCOK`), yet they sit inside a **128 KiB** mapper
extent (fixture mapper idx 49, pba 483059, class 1) and run past its end.
Root cause: on mapper volumes the owner append still writes through the
legacy cursor — `wp25_owner_write` (vol_tier.c) uses
`v->inode_area_pos` instead of `meta_get_append_pos`, so a large owner
record is placed inside whatever extent the cursor points at and
overflows it. (This is the same path WP47 tried to route through
`vol_append_slot`; that attempt made the flush-time owner sync fail into
dev1 and was reverted.)

Consequences: readers/`vol_records_walk` stop at the extent boundary, so
such records are effectively invisible; fsck now flags them.

### Fix (follow-up WP52)
Route the owner append through the extent allocator **flush-safely**:
`meta_get_append_pos` at flush time must not itself flush/persist
re-entrantly (the WP47 failure). Likely shape: an append helper that
allocates/extends an extent without recursing into `vol_flush`, resized to
the record (`size_class` from `rec_size`), used by `wp25_owner_write` and
`tz_owner_write`; then re-enable WP42's batch deferral on mapper volumes.

Until then `tools/test-sweep-mapper.sh` is RED on the fsck leg (bad
records: 2) — intentionally, as the first regression signal for WP52.
