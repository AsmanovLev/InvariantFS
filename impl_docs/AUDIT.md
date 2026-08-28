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
| PB7 | **FIXED (2026-08-28)** Dedup data-loss on delete: vol_retire_inode now skips any pba still referenced by another live MAP entry (two pba-indexed bitmaps per retire: referenced-elsewhere + already-freed, O(l2p) once), plus same-file duplicate guard. Repro was: two zero files, sweep, delete one -> fsck missing:1; now clean + survivor bit-exact. Journal-level refcounts remain unneeded | volume.c vol_retire_inode | - |
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
| WP10 | **DONE (ff84bf8)** Codec registry (sniff/probe/caps/dec_mem/generation) + Text Zone cross-file PPMd batching (4MB batches, bytypesize sort, `\x01tzb` owner inode, member L2P dups) + storage-class xattr `invfs.class` + sweep-time memory policy (arc_limit/dec_mem_limit, both-directions re-sweep) + TEXT GC mark-and-sweep. Spec: `WP10-textzone-codec-registry.md`. Addresses: dead constants INVFS_ZONE_TEXT/PPMD, PB6 (probe gate), H10 (PPMD batch decode+memcmp verify), per-sweep wasted ZSTD re-encodes (class stamp) | code-fix | L |
| WP11 | **DONE (ff84bf8)** JPEG→JXL on Linux: POSIX exec layer in volume.c (`tool_resolve`: $INVFS_TOOLS → /usr/lib/invfs/tools → PATH; fork/execvp, fixed argv, mkdtemp in /dev/shm, 120 s timeout) + JPEG sniff (FF D8 FF) + probe gate (no cjxl → RAW unstamped, retried next sweep) + SOF0/1/2 decode-working-set admission vs dec_mem_limit + djxl decode-back bit-exact guard; class CODEC{JXL}. Spec: `WP11-jxl-posix.md`. Closes the PB6 readability cliff for the JXL/PMP/APE lanes | code-fix | M |
| WP12 | WP10/WP11 follow-ups (known gaps, honestly carried): (a) vol_compute_stats logical_bytes double-counts TEXT (owner + members; Silesia image showed 309 MiB logical vs 202 MiB raw); (b) JPEG upgrade retry incomplete — MEMLIMIT/GUARD{JXL} stamps re-enter vol_sweep_one, but the JXL branch fires only on RAW-zone files; (c) PNGR/FLACR transcode paths still Windows-only (their exec deps now resolve on Linux); (d) no RLIMIT_AS in tool children (WP10 §10 hardening); (e) probe() runs per file, no caching; (f) owner-AST rewrite is O(batches) per seal + 4 GB u32 cap on total batched text per volume; (g) no binary cross-file context (the xz gap, doc/16 B30); (h) **tools/invf-sweep.c has NO dedupe pass** (dedupe→GC→flush exists only in the Windows-prototype src/sweep.c) — the Silesia bench ran dedupless; port or engine-ize the pass; (i) EXE codec: ELF/PE/Mach-O sniff → BCJ(x86 call/jump normalization) prefilter → strong backend (BCJ+LZMA is the proven shape — xz/7z BCJ2; BCJ+CM in paq; BCJ→BWT→MTF→RLE chain is the bzip2-shape variant, needs experiment, not doctrine); BCJ filters available in tools/7-Zip-zstd sources; bit-exact reverse + decode-back guard, class CODEC; (j) DICOM container: preamble+DICM header kept verbatim as recipe, pixel data → lossless JXL (cjxl accepts PGM/PPM frames); same wrapper covers raw-pixel files (silesia mr/x-ray) IF geometry is known — headerless geometry sniffing is fragile, mark experimental; (k) binary batching: extend the TEXT accumulator to binary families (ELF objects/libs, record-structured files like osdb/sao) so PPMd sees cross-file context — this is also the correct answer to "dedupe file headers": 4KB-aligned block dedupe cannot catch sub-block header similarity, batched PPMd can (alternative: zstd dictionaries, doc/04 B22) | code-fix | M-L |
| WP13 | **DONE (26b0d43)** Codecpack exec path + raw_image pack (DICOM/PNM/BMP/TIFF -> lossless JXL, RIMG blob); dynamic pack registry, estimate-based admission | code+pack | M |
| WP14a | **DONE** Binary batching: zone=TEXT/algo=ZSTD(BCJ=14) batches, family sort (ELF e_machine/PE/Mach-O), class 8; Silesia 2.82->2.84x (tar-member exclusion caps the win) | code | M |
| WP14b | **M1 DONE**: container-member batching ('!'-sibling parts batch; tarx V7 space-padding fix); Silesia 74.55->59.14MB (2.84x->3.58x, beats btrfs 3.18x). **M2 DONE**: exe-as-container carving (embedded JPEG -> JXL 'name!exrN' parts, PNG carved ZSTD; EXER blob = header+part table+glue, ZSTD-19; whole-file rebuild guard; class CONTAINER{EXER}; rc 11) | code | M/L |
| WP15 | **SHELVED 2026-08-27** Video (H.264 lossless recompress via dropbox/avrecode): built after 2015-codebase port (yasm/inline-asm/arc4random/unique_ptr fixes); model broken for B-frames (crash; universal in real content) and 8x8dct (compress "OK", decompress asserts — silent-corruption class, caught by decode+memcmp guard pattern); works only on constrained CABAC streams (bframes=0, ref<=2, no 8x8dct) at ~1.03x vs zstd-19's 1.003x on the same bytes. Positive: pack-crash -> GUARD -> generic fallback path proven safe. Revisit only if the model is fixed upstream | research | M |
| WP16 | **(a) DONE** Containerpack ABI: manifest `type = container` + four argv commands (enumerate/extract/strip/rebuild, placeholders `{in} {out} {idx} {dir} {recipe}`); sweep decomposes a container into `name!mbrNNNN[-sname]` member inodes (RAW, flow through the whole pipeline incl. batching and nested decomposition) + `name!mbrt` member table + recipe blob as the main record (algo = pack's, CONTAINER{algo,gen} stamp); FS-side rebuild+memcmp guard before commit; read = pack rebuild over member siblings (whole-file ARC divert); delete cascades through `!`-siblings; fixture pack splt_test (algo 40) + tools/test-containerpack.sh. Spec: `WP16-containerpacks.md`. **(b1) DONE** ABI v1.1: seekable containers — optional `map` command (FS-owned binary MRMP map) -> CAP_SEEK, reads splice locally from recipe + member siblings via the `!mbrmap` sibling (NO pack exec, pack-absent reads work; map stored LAST at sweep as the guard-passed marker; guard = read-back through the real read path + chunked memcmp, no rebuild exec) + DEFER_ENOSPC class (cls=9: space-priced sweep admission, waits RAW, re-evaluated every sweep) + codec profiles (INVFS_PROFILE fast/balanced/dense/archive -> generic ZSTD 6/19/22/22 + env published to pack execs; default byte-identical). **(b2) PENDING**: migrate the JXL lane to a codecpack. **(c) DONE**: six production containerpacks, all C11 single-file helpers with `map` (seekable, pack-absent reads work) — rawdisk(16, MBR/EBR/GPT), ext4fs(17), fatfs(18, FAT12/16/32+exFAT), xfs(19), ntfs(20), vdi(21, dynamic VDI). All with strict refuse lists, bit-exact guard, delete cascade, e2e per pack; nested composition proven (vdi->rawdisk->GPT). **(d) engine fixes from the pack wave**: containerpack dispatcher now tries the NEXT sniff-positive pack on decline (was break-on-decline — rawdisk/fatfs share 55AA@510); test range 40+ for fixture algos. **(e) PENDING**: jxl.codecpack migration, p7z pack, qcow2. Ops note: /dev/shm tmpfs quota filled by stale test scratch silently killed big-container sweeps (EDQUOT on recipe write) — scratch dirs need hygiene | code+pack | M |

### WP12 status update 2026-08-26 (post-bench fixes, second commit)

- **(a) DONE.** vol_compute_stats skips 0x01-internal owner records in all logical
  fields. The fix exposed two latent bugs in the same function, also fixed: a record
  buffer use-after-free (freed after the CRC check, then parsed for AST), and the AST
  base offset computed as `sizeof(rec)+vol_ast_blob_len()` so num_blocks was read past
  the recipe (TEXT logic silently zero / garbage-lucky). Plus the build-system trap that
  hid all of it: `invf-stats` was missing from Makefile TOOLS, plain `make` never
  relinked it, and the e2e scripts kept testing a stale binary.
- **(b) DONE.** `vol_jxl_retry`: MEMLIMIT/GUARD{JXL} re-arms now run the full
  cjxl→djxl→memcmp path on the decoded content (new blob inode, retire old, meta carried
  over). Retry leg in tools/test-jxl.sh: "6/6 bit-exact after upgrade".
- **(e) DONE.** probe() results memoized per process; codec_test stays at 73/0 via the
  test-only `invfs_codec_probe_reset()` hook.
- **(h) DONE.** `vol_sweep_dedupe` in volume.c: BLAKE3 over stored segment bytes, keep
  one pba per hash, L2P-remap losers, free blocks; skips zone==TEXT (WP10 §11),
  whole-file JXL/APE blobs, and inodes deferred in the running sweep's accumulator.
  Linux CLI order: walk → dedupe → vol_tz_gc → vol_tz_flush. tools/test-dedupe.sh PASS.
- Still open: (c) PNGR/FLACR POSIX transcode bodies, (d) RLIMIT_AS in tool children,
  (f) owner-AST O(batches) rewrite + 4 GB u32 cap, (g) binary cross-file context,
  (i) EXE BCJ, (j) DICOM / raw-image codecpack, (k) binary batching.
- Verification: `make test` (arctest 4467/0, blkio 86/0, codec 73/0) + test-dedupe /
  test-textzone / test-jxl e2e all PASS.

| WP18 | Resize tool: grow = relocate journal+inode area right (bounded metadata move, data untouched) or sb v2 region table; shrink-right = tail-free bitmap check + same relocation (inode area currently sits at image end, blocking truncation). Today: no tool; workaround = new volume + cp -a | code | M |

| WP19 | Heat + adaptive tiering + policy profiles. Heat lives in invfs_l2p_entry pad[3] (u16 read + u8 write, keyed (inode,lba) so it survives pba remaps; saturating; decay >>=1 per sweep; hysteresis = 2 sweeps; increment once per open-session; reset on rewrite by design; heat_init knob). Promotion: PPMd/LZMA2 member hot -> ZSTD segment (top-K by heat within a per-sweep budget); demotion: frozen ZSTD -> LZMA2; dedup-shared blocks never promoted. Profiles (per-volume, INVFS_PROFILE override at sweep): archive/dense/balanced = zstd-19, fast = zstd-6, faster = zstd-3, fastest = LZ4, turbo = NONE (byte-aligned, skip even LZ4 on write); meta-profiles faster/fastest/turbo act at admission (codec substitution), the codec never sees them. Anchored: INVFS_CLASS_ANCHORED=10 — boot-trace (mount opt anchor_boot=N seconds, reads in the window get stamped; or user.invfs.anchor xattr) pins boot/working-set files out of heavy codecs forever; anchored-after-sweep => downgrade to fast form | code | M |
| WP20 | --seal: total Shadow parity. XOR stripes (k=32..50 data blocks + 1 parity block, 2-3% overhead) over everything Shadow incl. batches and whole-file blobs; owner inode \x01parity (same owner pattern as \x01tzb); seal AFTER sweep, idempotent check-and-update (never delete-then-regenerate); read path recovers a CRC-failed block from its stripe transparently + logs; verify --deep checks parity too. Zero deps (pure C); RS(m=2) via ISA-L is the later upgrade | code | M |
| WP21 | Sweep-checkpoint + rollback. Sweep end = checkpoint (inode-area + journal end positions recorded); rollback = truncate both back to the checkpoint -> post-sweep tombstones vanish -> pre-sweep record versions live again (append-only makes this sound), bitmap+L2P rebuilt by the existing fsck machinery; RAW-zone changes since the sweep are discarded. free_on_delete=0 (mount/mkfs opt) routes deletes through a \x01reten retention registry for FULL-fidelity rollback (default free_on_delete=1 = best-effort: reused blocks are gone); sweep --realize frees retained blocks (the point of no return). K-checkpoint retention = snapshots-lite (v2) | code | M |

## 6. Addendum 2026-08-26 (WP10)

New audit findings folded into WP10 spec (`WP10-textzone-codec-registry.md`):
- PPMd8 wrapper (`src/ppmd_codec.c`) debugged and verified: `CPpmd8.Stream` is a UNION
  `{In,Out}` (Ppmd8.h:88-92); assigning both members clobbers the input stream. Fixed —
  decode assigns only `.In`, encode only `.Out`. Wire format: `[2B LE props][range-coded
  stream incl. END marker]`, params o=8/64MB/CUT_OFF (props bytes `f7 13`).
- Read-path hard ceiling found: `vol_read_range` per-segment decode uses stack
  `uint8_t tmp[SEGMENT_SIZE]` (volume.c:4331) — 64KB logical-segment ceiling; PPMD batch
  slices must bypass it (heap) since decode unit is the 4MB batch.
- ARC pba-keying hazard identified and designed around: ARC keys are inode_ids today;
  TEXT batch caching keys by pba, invalidated only in GC (single point where TEXT blocks
  are freed) — required because pba reuse by alloc_blocks would otherwise serve stale
  decoded batches to a different file.
- doc/16 B29.5 confirms: sorted (bytypesize) 4MB PPMd batches beat ZSTD-19 by +12% even
  on heterogeneous batches; Benchmark.md attributes the 2.95x vs 4.01x Silesia gap to
  missing cross-file context — WP10 closes exactly that gap.
- AUDIT sections affected as WP10 lands: "Design fiction inventory" loses Text Zone/PPMd;
  PB6 gains probe-based gating; H10 gains real verify for the PPMD lane.

## 7. Addendum 2026-08-26 (2) (WP10+WP11 landed)

WP10+WP11 landed as **ff84bf8** ("WP10+WP11: Text Zone (cross-file PPMd batching),
codec registry, JPEG->JXL on Linux"). Effects on the findings above:
- "Design fiction inventory" loses Text Zone/PPMd, the codec registry, the class
  flag and the sweep memory policy — all real now (`codec.c/h`, `tz_*` in
  volume.c, `invfs.class` xattr).
- PB6 gains probe-based gating plus POSIX exec for JXL/PMP/APE; PNGR/FLACR
  transcode paths stay Windows-only (their tools resolve on Linux now, but the
  recipe code itself is still `#ifdef _WIN32`) → WP12(c).
- H10 gains a real verify for the PPMD lane (decode+memcmp at seal).
- Fixes folded into the same commit: verify --deep same-id growing records;
  stat.c poskill accounting; ls.c 0x01-name filter; vol_compute_stats
  live-version filtering; tools/invf-sweep.c now actually dispatches
  vol_sweep_one + vol_tz_gc + vol_tz_flush + vol_flush.

Silesia re-run (2026-08-26, 23 GB RAM, /dev/shm; `CORPUS_DIR=/var/tmp/bench/silesia
bash tools/bench-fs.sh`): raw 211 940 183 → **invfs data 75 287 756 (2.82x)** vs
tar|zstd-19 52 850 394 (4.01x), tar|xz -6 49 500 564 (4.28x); image alloc
162 521 088 (1.30x, incl. 32 MB journal + inode area). TEXT logic 76 MiB
(dickens/nci/reymont/webster/MANIFEST.txt → PPMd batches); mozilla/samba/xml are
TARs → 1573 extracted member parts; mr/x-ray → UNCOMPRESSIBLE; ooffice/osdb/sao →
generic ZSTD-19. Two corpus discoveries: silesia "xml" is a ustar archive (the
content sniff correctly rejected it — NULs in the header), "reymont" is a PDF
accepted by the content sniff. The bench also exposed a stats bug:
vol_compute_stats logical bytes double-count TEXT (owner + members) — the image
showed 309 MiB logical vs 202 MiB raw → WP12(a).

Estimated distance to OpenRC-bootable Gentoo root on InvariantFS (FUSE route): **4–8 engineer-weeks**
(long poles WP2, WP4). Doc13's literal kernel-module ambition: 4–9+ months, effectively a new project.

Boot ladder (unchanged, now evidence-grounded):
M0 host build+smoke (works today per f6 logs) → M1 WP1+WP3 quick wins, busybox shell on VFS root →
M2 WP2 metadata, real perms/symlinks, stage3 import → M3 WP3 barriers + WP6, crash/power tests →
M4 WP4 ranged writes, package-manager grade → M5 full OpenRC boot, productionization (WP5/8/9 polish).

## 7. Far roadmap (ideas, NOT scheduled work)

- **Template zone (explicit chunk store)**: content-defined chunking + chunk index + per-chunk refcounts + own GC; consolidates hot samples (audio samples, code snippets, asm fragments) across unrelated files. Heavy project; precondition = refcounts (old WP6). The implicit half (sorted batching captures cross-file redundancy in-context) already ships and covers most of the win. User decision 2026-08-26: roadmap, not a numbered WP.
- FS-image containers (NTFS/ext4 .img member decomposition) — theory, see WP10 #10 tail.
- RLIMIT_AS runtime enforcement in tool/pack children (WP10 #10 hardening); concurrency sweep-vs-mount formalization; fuzz/property tests for the sweep classifier.
