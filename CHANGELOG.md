# Changelog

All notable changes to InvariantFS will be documented in this file.

## [Unreleased]

### Added
- Native v3 seal parity (WP201, par2-inspired, no byte-compat): per-chunk
  Reed-Solomon groups + footer written last; `--seal [5|10|20|25]`, `--unseal`.
- Seal scrub / heal / policy (WP401-404): `--verify-seal` read-only drift report,
  explicit `--heal` rebuild from parity, `--stale-mode` gating auto-reseal,
  incremental re-seal of dirty groups only.
- Sweep `migrate` stage (WP601): defrag relocation of fragmented live segments
  into consolidated extents, plus explicit recompress.
- Delta group-commit barrier batching (WP602); staged rewrite ahead of
  post-failure fences (WP605); tombstone pads for doomed spans, rename abort
  compensation, fence-before-ack (WP606); retire-on-fence for superseded frees,
  op-abort doom, heal-before-every-barrier (WP607).
- Unlink rolls back the namespace when reclaim fails (WP604); TAR lane batch
  durability fencing across members (WP603).
- Pack signatures: vendored ed25519, signature gate on install (WP203).

### Changed
- Soak/flakey legs accept proven-exhausted verdicts where chaos wins (WP502/503
  + gates); e2e-full re-enabled in CI with fuzz, usr1-savepoint,
  fuse-sweep-thread and ext4fs unskipped (fixed by WP301/302/305).
- `invf-verify` tolerates a larger backing file, refuses only truncation.

### Fixed
- fsck detects row/recipe incoherence (WP303); sweep remap put-failure guard
  regression test (WP304); fuzz bitflip `--seal-every` runs exactly once (WP301).

## [v0.6.0] - 2026-10-08

Crash-consistency hardening release.

### Added
- Durability contract: fsync on v3 + close barrier (WP80; see
  `docs/adr/ADR-009-v3-durability-contract.md`) — acknowledged writes are
  durable, failures leave the pre-call state, never a torn prefix.
- SPT0 savepoint capture wired into both sweep paths (WP77); every sweep
  (offline, watermark, `kill -USR1`, xattr) arms a rollback window.
- Sweep parity on v3 (WP78): text PPMd + binary ZSTD/BCJ batching, restored
  transcode/codecpack lanes; PNGR lane transcodes (WP91).
- xattr security namespaces + permission matrix (WP82).
- Every C containerpack as a versioned `.ivpack`/`.so` plugin (WP71); qcow2
  pack emits the full guest disk image (WP100); zlib-qcow2 e2e coverage (WP107).
- Two-device durability: tier/RAW-mirror indexes durable on Meta-v3 (WP98),
  commit protocol + DEVT `sync_seq` advance with stale-mirror reporting (WP99).
- Codecpack retry: a zero-scoring pack is retried after the text lane and
  exe carve (WP103).

### Fixed
- SPT0 P0: rollback could publish a recipe onto reused blocks (WP96).
- ext4fs/ntfs TEXT{PPMd} expectations (WP90); DEFER_ENOSPC gate +
  INVFS_PROFILE contract, four masked e2e failures unbroken (WP106).
- Test isolation per-command in a mount namespace (WP104); no hardcoded
  host-owned values in tests (WP105).

## [v0.5.0] - 2026-09-22

### Summary
Major release establishing **Meta-v3** as the primary, default filesystem format with B+ tree base metadata, append-only Delta Log, concurrent lock-free metadata reads, and complete retirement of legacy v2 metadata mechanisms.

### Added
- **Meta-v3 Core Architecture:**
  - Double-slot descriptor (`RT30`) at offset `0x9D0` pointing to dual atomic root slots.
  - Asymmetric B+ tree base tier with 4096-byte nodes (nodes 1..64 pinned, dynamic growth).
  - High-performance, append-only circular Delta Log (`DL30`) for low-latency writes and metadata mutations.
  - Background asynchronous fold mechanism (`vol_v3_fold`) merging delta mutations into the base B+ tree.
  - Savepoint generation engine (`SPT0`) enabling consistent transaction rollback (`invf-rollback`).
- **v3 POSIX & Linux Rootfs Support (WP-M24):**
  - Full symlink support: symlink targets stored in content-addressed BLAKE3 recipe store with transparent resolution via `vol_get_meta` and `readlink`.
  - Special device support: `mknod` support for FIFO, SOCK, CHR, and BLK devices with preserved `rdev` and mode attributes.
  - FUSE integration: accurate `st_rdev` propagation in `getattr`.
- **Id-Keyed Sweep Publication (WP-M23):**
  - Inode-id-keyed sweep updates (`vol_v3_publish_blob_inode`) eliminating root collision risks for nested directories.
  - Sweep active session protection (`vol_write_active_id`) preventing write/sweep race conditions.
  - Manual and daemon sweep support on v3 (`vol_sweep_file`).
- **Thread-Safety & Concurrent I/O:**
  - Thread-safe, position-explicit I/O (`vmux_pread`, `vmux_pwrite`, `io_pread`, `io_pwrite`) across active metadata and data read/write paths.
  - Concurrency validation suite (`concurrency_test`) running 6.4M+ concurrent lock-free operations alongside background folds.
- **CI & Validation:**
  - Automated GitHub Actions workflow (`.github/workflows/engine-ci.yml`) running builds, unit tests, and e2e test suites.
  - Comprehensive unit test suites for v3 symlinks (`symlink_v3_test`), v3 sweep (`sweep_v3_test`), delta log (`delta_test`), and btree (`btree_test`).

### Changed
- `invf-mkfs` creates Meta-v3 volumes by default.
- Deprecated legacy `INVFS_V2=1` format flag with explicit rejection.
- Path resolution and directory iteration in `invf-verify --deep` and `invf-sweep` modernized to single-pass hierarchical `vol_v3_walk` (O(n) runtime).
- `vol_sweep_one_v3` memory usage bounded by `INVFS_SWEEP_MAX_FILE` (256 MiB default cap).

### Removed
- Legacy v2 INOD record scan, INO2 metadata extents, and mapper machinery.

---

## [v0.4.0] - 2026-09-20

### Added
- Experimental Meta-v3 engine prototype and base B+ tree implementation.
- Multi-device volume support (dev0 metadata/RAW + dev1 Shadow).
- Initial delta recording and segment layout.
