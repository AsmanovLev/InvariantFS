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
