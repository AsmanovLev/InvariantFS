# ADR-004: Thread-Safe Position-Explicit I/O (`vmux_pread`/`pwrite`)

## Status
Accepted

## Context
The volume multiplexer (`vmux`) and `blkio` layers historically relied on implicit file offsets managed by `io_seek` (`vmux_seek`), followed by `io_read` or `io_write`.

During concurrency testing (`concurrency_test`), concurrent lock-free metadata readers (`btree_search`, `vol_delta_read_value`) run concurrently while a background thread executes `vol_v3_fold`. Because `v->mux_pos` was shared across threads on the volume handle, concurrent threads calling `io_seek` would overwrite the position immediately before another thread performed `io_write` or `io_read`. This resulted in metadata writes corrupting disk blocks and triggering CRC failures.

While FUSE operations are serialized by `g_io_lock`, background worker threads (such as fold and sweep) interact with core data structures concurrently.

## Decision
1. Implement thread-safe position-explicit primitives in the volume multiplexer:
   - `int vmux_pread(invfs_volume *v, uint64_t off, void *buf, size_t len)`
   - `int vmux_pwrite(invfs_volume *v, uint64_t off, const void *buf, size_t len)`
2. Expose `io_pread` and `io_pwrite` macros in `volume_internal.h`.
3. Transition active concurrent and core metadata paths (`mbuf_read`, `mbuf_write`, `vol_delta_read_value`, `delta_read_hdr`, `vol_delta_append`, and segment payloads) to use `io_pread`/`io_pwrite` instead of `io_seek`.
4. Keep `vmux_read`/`vmux_write` for sequential single-threaded utilities, while documenting that dropping FUSE's `g_io_lock` in the future requires completing the `pread`/`pwrite` transition across all legacy subsystems.

## Consequences
### Positive
- Eliminates position collisions between concurrent readers and background fold/sweep workers.
- Verified under `concurrency_test` with 6.4M+ concurrent read operations across 2,100+ folds without corruption.
- Uses underlying OS `pread(2)` and `pwrite(2)` system calls directly.

### Scope & Constraints
- FUSE operations continue to rely on `g_io_lock` for global transaction ordering, directory mutation safety, and legacy subsystem access.
- `g_io_lock` must NOT be removed from FUSE until all legacy paths are fully migrated or replaced.
