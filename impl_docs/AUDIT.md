# InvariantFS — Documentation ↔ Source Consistency Audit

Date: 2026-08-24 · Scope: Linux-relevant lanes over `src-extracted/VFS` · Evidence: file:line throughout
Lanes: A1 format (doc02↔invarifs.h/volume.c/mkfs.c) · A2 compression/transcode (03,04,05,06↔sweep/flacx/pngx/tarx/zip/gzrepro) · A3 IO/cache (07,15↔volume/blkio/fuse_fs) · A4 recovery/ENOSPC (08,12↔volume/fsck/enospctest) · A5 port-critical (13-linux-rootfs+build logs↔fuse_fs) · A6 dedup/security/tests (10,11,18↔dedup/perms/tests)

## Verdict up front
The docs describe a **better filesystem than the one that shipped**. Roughly half the documented machinery
(Text Zone/PPMd/Brotli/BCJ2, write-time CAS dedup, refcount journaling, checkpoints, throttling watermarks,
kernel module + initramfs kit, BLAKE3 verify-after-sweep) does not exist in code. Conversely, several things
that DO exist are undocumented (LZ4 default codec, 36-byte CRC'd journal entries, INOD record framing,
offline dedup). The de-facto spec is `invarifs.h` + `volume.c`, NOT the docs. A fresh implementation built
from doc/02 would produce volumes this code cannot mount (and vice versa).

## 1. Port-blockers (must fix for any Linux rootfs duty)

| ID | Finding | Evidence | Lane |
|----|---------|----------|------|
| PB1 | **No exec bit ever**: getattr hardcodes `S_IFREG\|0444`, dirs `0555`, uid=0; on-disk inode has NO mode/uid/gid fields → every `execve()` fails | fuse_fs.c:119,131,137; invarifs.h:141-151 | A3/A6 |
| PB2 | **No symlinks/hardlinks/devices**: no `.symlink/.readlink/.link/.mknod` ops → OpenRC `/etc/runlevels/*`, stage3 layout, `/lib→usr/lib` unrepresentable | fuse_fs.c:405-417 (11-op table); README:175-176 | A6 |
| PB3 | **Volume always DIRTY after FUSE session**: `main()` never calls `vol_close`; CLEAN written only there → next mount forces read-only until manual `invf-fsck -f`. Every clean reboot looks like a crash | fuse_fs.c:465-468 vs volume.c:801-826,785-791 | A4/A5 |
| PB4 | **Linux durability fictional**: plain `open(O_RDWR)` (Windows branch uses NO_BUFFERING\|WRITE_THROUGH); sole fsync gated by opt-in `INVFS_FSYNC`, close-time only | blkio.c:281-287 vs blkio.h:19-24; volume.c:820-826 | A3/A4 |
| PB5 | **Whole-file write path**: FUSE write buffers entire file in RAM (`wctx`), flush rewrites whole file via replace; O(filesize) memory+IO per write session | fuse_fs.c:239-258,300-376; volume.c:4028-4044 | A3 |
| PB6 | **Codec cliff**: FLACR/PNGR/JXL/APE/PMP decode via `CreateProcessW` on `D:\bin\MAC.exe`,`D:\VFS\tools\jxl`,packMP3,ffmpeg; non-Windows `return -1` → those files are permanently EIO on Linux; Linux sweep can't transcode either | volume.c:4296-4299,4344,4387,4442,4576; A2 row34 | A2/A3 |
| PB7 | **Dedup data-loss bug**: offline sweep remaps duplicate segments to one canonical pba (sweep.c:241-256), but delete frees ALL mapped blocks unconditionally — deleting one of two deduped files destroys the survivor's data. Directly violates founding invariant | volume.c:3887-3897; sweep.c:203-205 | A6 |
| PB8 | **Missing POSIX plumbing**: no `.fsync` (ENOSYS), no `.truncate/.setattr(chmod/chown/utimens)`, no `.statfs` (df/emerge fail), no xattr, no `.access`; `.rename` EXISTS in engine but unwired | fuse_fs.c:405-417; volume.c:4153-4228 wired only in dokan_fs.c:755 | A3/A5 |
| PB9 | **Format ≠ its own spec**: magic `"InvariFS\0"` vs documented `"InvariantFS\0"`; metadata zone bitmap-before-journal vs doc's journal-first; journal entries 36B padded+CRC'd vs doc's 29B sketch; inode layer = append-only INOD records vs documented B-tree index. Doc-built volumes unmountable by this code | invarifs.h:18; mkfs.c:189-190; invarifs.h:175-183; invarifs.h:141-150 vs doc02:115,12-15,147-156,163-176 | A1 |
| PB10 | Device exclusivity/whole-disk guards `_WIN32`-only; Linux accepts any `/dev/*`, double-mount corrupts silently | blkio.c:56-58,100-195 | A3 |

## 2. High severity

| ID | Finding | Evidence | Lane |
|----|---------|----------|------|
| H1 | Multithreaded FUSE races: reads/writes of `g_entries` snapshot unlocked while flush/create/unlink/sweep rebuild/free it → UAF on listing during writes | fuse_fs.c:124,206,233 vs :221-227,328-349 | A3 |
| H2 | ~4 GB hard file cap (u32 file_size, u16 blocks×64K); no grow-in-place | invarifs.h:98,128; volume.c:1490-1497 | A3 |
| H3 | Tombstone-vs-bitmap ordering: frees land in RAM first, tombstone appended later; interleaved flush can persist freed bits over live records under power loss | volume.c:3887-3933 vs :1196-1200 intent | A4 |
| H4 | FUSE flush ignores `vol_replace_file` failure, returns success → silent data loss on late ENOSPC (Dokan got the fix, FUSE didn't) | fuse_fs.c:362-364,375 | A4 |
| H5 | One-way READONLY latch at ~0.1% free; `vol_set_readonly(v,0)` has zero callers | volume.c:1272-1281,2827-2828 | A4 |
| H6 | `invf-fsck -f` refuses to repair/compact when only problem is l2p_miss → stuck read-only | volume.c:1046-1073 gate | A4 |
| H7 | readdir caps at fixed `ents[4096]`, silently truncates large dirs (~1.1MB stack array/call) | fuse_fs.c:150,157 | A3 |
| H8 | ZIP containers lack idempotence marker → re-exploded (new record+tombstone) every sweep pass; TAR/GZ/PNG/FLAC guarded | volume.c:3351-3371 vs :3387,3399,3410 | A2 |
| H9 | Security model absent both platforms: no perm storage, dokan discards ACCESS_MASK, WinFsp NULL SD → world-rw anything; sshd StrictModes etc. will reject | invarifs.h:141-151; dokan_fs.c:238; winfsp_fs.c:137-146 | A6 |
| H10 | "Invariant" enforced only where cheap: blanket compress→decompress→BLAKE3 loop DOES NOT EXIST; real proofs only for GZ (memcmp 9×3 grid) and PNG; FLAC/JXL check length only | volume.c:4987-5015,5312-5344 vs 2403-2447,4360-4375; doc06:156-170 | A2 |
| H11 | Forced `-d` debug flag + hand-rolled argv parse → `-o allow_other` impossible | fuse_fs.c:454-461 | A5 |

## 3. Documented-but-nonexistent (design fiction inventory)

- Text Zone + PPMd batches, Brotli, ZSTD-dict tags, BCJ2 for ELFs, WavPack, LZMA2, user profiles/.ini (04,05) — dead constants at most: invarifs.h:30,36,40; volume.c:3447-3454
- Entropy>7.5 heuristic, deflate-level sniffing (05) — none; ZIP kept verbatim as windows instead (superseded design, stale doc)
- Write-time BLAKE3 Content Index, refcounts via journal DELTA, Bloom filter, snapshots, semantic ELF/PE section dedup (10) — zero blake3 refs in volume.c; real dedup = offline remap in sweep
- Watermarks 80/95/98%, EAGAIN throttle, EDQUOT quotas, 5% root reserve, auto-return-to-RW, --raw-zone-size presets (12) — actual: 0.8% sweep headroom, plain ENOSPC, one-way latch
- Journal types DELTA/META, SWEEP_COMMIT, CHECKPOINT replay, Recovery-state 0x52 usage, mount-time BLAKE3 sampling, bit-rot re-sweep from RAW backup (RAW freed at sweep!) (08) — types dead-defined invarifs.h:47-51; recovery = manual offline fsck
- Kernel driver `invarifs_core.ko` + BCJ2 + request_module autoload + chattr +S NoSweep + initramfs kit + mount.invarifs (13) — NOTHING exists; doc13 explicitly rejects FUSE, ships FUSE-only reality
- Compactor >30% fragmentation trigger, batched text sort pipeline (06) — absent; `sweep_cursor` superblock field dead since mkfs (never read/advanced)
- Tag Edit append-delta flow (07:157-167) — no such function anywhere

## 4. Verified honest / matches docs (trust anchors)

- ARC cache chapter (15) matches arc.c line-for-line: bytes-budgeting, 256MB default+suffix parser, >half refusal, ghosts B1/B2, p-adaptation, invalidation-on-delete, per-process scope, full stats struct
- Segment self-check `[4B csize][4B crc32c]` verified BEFORE decompress — exact match (03:127-129 ↔ volume.c:1824-1833,2305-2341)
- Container windows: original bytes preserved whole, members = true on-demand ranges (stored/tinfl), nested `a.zip!inner.zip!x.txt` recursion — as documented
- TAR/GZR recipes incl. guards and `!part0` markers; GZ re-deflate grid with memcmp proof — strongest enforced invariant in tree
- ENOSPC atomicity core: worst-case precheck, mid-file reclaim, RAW→SHADOW spill, errno mapping, enospctest contract — implemented and tested (E6-E14)
- Superblock core 124B offsets + CRC32C coverage byte-exact vs doc; bitmap formula identical at mkfs/open
- blkio portable layer clean (OVERLAPPED fenced in _WIN32; shared 4K bounce RMW path)
- Prior port logs f6c-f6k: port COMPILED AND RAN clean (libfuse 3.18.2, rc=0 smoke traces) — "port attempt failures" were actually successes; real gaps (xattr ENOSYS) simply never implemented; tests_fuse.ps1 recreates image each run, masking DIRTY-unmount (PB3)
- README Known-limitations section is accurate and even understated (linear-scan claim now stale — O(1) hash index exists)

## 5. Gap list → revised roadmap (FUSE-root route; kernel-module route = separate XL project)

| WP | Work package | Tag | Effort |
|----|--------------|-----|--------|
| WP1 | Wire missing FUSE ops: rename (backend ready!), setattr/truncate, fsync, statfs, symlink/readlink/link, mknod, access; proper argv/-o parsing, drop forced -d | code-fix | M |
| WP2 | Metadata v2: store type/uid/gid/mode/mtime (+symlink target, dev_t) — extend inode rec (format v2 w/ version field) or sidecar keyed by inode_id; update getattr/build_file_table/fsck | format-change | M |
| WP3 | Lifecycle+durability: vol_close on .destroy (CLEAN), auto-recovery at mount (clear needs_recovery after replay) or fsck hook; fdatasync barriers after journal/bitmap/inode writes, default-on for device mounts | code-fix | M |
| WP4 | Ranged/append write path replacing whole-file wctx rewrite | code-fix | L (long pole) |
| WP5 | Codec neutrality: Linux paths or explicit gate-off for APE/JXL/PMP (+NoSweep pinning so sweep never transcodes system files; also fixes PB6 readability cliff) | code-fix | M |
| WP6 | Dedup safety: refcounts or disable offline dedup until then (PB7) | code-fix | S-M |
| WP7 | Fix g_entries locking (H1), readdir cap (H7), FUSE flush ENOSPC propagation (H4), ZIP sweep idempotence marker (H8) | code-fix | S-M |
| WP8 | Build system: invf-fuse in build_linux.sh, kill /mnt/d hardcode, static-link recipe for initramfs | recipe-tooling | S |
| WP9 | Initramfs kit rewritten for FUSE (busybox /init, devtmpfs, invf-fuse, switch_root, CONFIG_FUSE_FS kernel) + offline rootfs packer preserving modes + finalize `invf-fsck -f` → first boot CLEAN | recipe-tooling | M |

Estimated distance to OpenRC-bootable Gentoo root on InvariantFS (FUSE route): **4–8 engineer-weeks**
(long poles WP2, WP4). Doc13's literal kernel-module ambition: 4–9+ months, effectively a new project.

Boot ladder (unchanged, now evidence-grounded):
M0 host build+smoke (works today per f6 logs) → M1 WP1+WP3 quick wins, busybox shell on VFS root →
M2 WP2 metadata, real perms/symlinks, stage3 import → M3 WP3 barriers + WP6, crash/power tests →
M4 WP4 ranged writes, package-manager grade → M5 full OpenRC boot, productionization (WP5/8/9 polish).
