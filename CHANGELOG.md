# Changelog

All notable changes to InvariantFS will be documented in this file.

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
