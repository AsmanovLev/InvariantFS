# ADR-004: Thread-Safe Position-Explicit I/O (`vmux_pread`/`pwrite`)

## Status
Accepted

## Context
The legacy volume multiplexer (`vmux`) and `blkio` layers used implicit file offsets managed by `io_seek` (`vmux_seek`), followed by `io_read` or `io_write`. 

In WP-M20, `g_io_lock` was removed to enable concurrent, lock-free metadata reads (`vol_v3_inode_get`, `btree_search`) across multiple worker threads while a background writer thread executed `vol_v3_fold`. Because `v->mux_pos` and `io->pos` were shared across threads without synchronization, concurrent readers calling `io_seek` would overwrite the position immediately before another thread performed `io_write` or `io_read`. This resulted in metadata writes corrupting arbitrary disk blocks and failing CRC checks.

## Decision
1. Implement thread-safe position-explicit primitives in the volume multiplexer:
   - `int vmux_pread(invfs_volume *v, uint64_t off, void *buf, size_t len)`
   - `int vmux_pwrite(invfs_volume *v, uint64_t off, const void *buf, size_t len)`
2. Expose `io_pread` and `io_pwrite` macros in `volume_internal.h`.
3. Transition `mbuf_read`, `mbuf_write`, and `vol_delta_read_value` to use `io_pread`/`io_pwrite` instead of `io_seek`.
4. Keep `vmux_read`/`vmux_write` as wrappers over `vmux_pread`/`pwrite` for legacy sequential single-threaded utilities.

## Consequences
### Positive
- Fully eliminates race conditions and file position collisions across concurrent reader and writer threads.
- Enables multi-threaded FUSE metadata operations and stress workloads (verified with 6.4M+ concurrent read operations during background folds without failures).
- Uses underlying OS `pread(2)` and `pwrite(2)` system calls directly.
