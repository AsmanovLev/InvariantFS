# WP-read-parallel-bitexact — the whole-file read must have ONE implementation

## Scope

- `src/core/vol_read.c` — the WP94 parallel decode fan-out.
- `src/core/blkio.c`, `src/core/blkio.h` — the per-volume bounce buffer.
- `src/cli/read_parallel_bitexact_test.c` (new), `Makefile` — the regression test.

## Why

`test-xfs.sh:657` failed on `main` (`075a4a0`): the 300 MB `fs-b.xfs` container
came back with 672 wrong bytes, deterministically, after a load-heavy sweep.
Ground truth: `/srv/bench/wpxfs-2481377`.

The investigation is in the commit message. What matters for the fix:

1. The bad bytes are in the **stored** segment. `fs-b.xfs` is one entry
   (`algo=19`, `zone=BINARY`) at `pba=192335`, frame `csize=311040324
   crc=0xd0a422d1`. Reading the raw device bytes at that pba and comparing them
   to `orig/fs-b.xfs` reproduces the 672 bad bytes exactly — so the decode and
   the `cpack` map are faithful, and the buffer handed to the storage layer was
   already wrong.
2. Both corrupt ranges are `kind=RECIPE` map entries, i.e. served straight from
   that one segment. The pack's own `payload_base` (42896) matches the map's
   `src_off`, so the recipe payload really is the container's bytes.
3. The xfs pack's `fd_copy()` is fully error-checked (`xfs.c:893-915`), so the
   wrong bytes were in `full` — the whole-file read at `vol_sweep.c:1654` that
   feeds `vol_containerpack_sweep()`.
4. `vol_read_inode()` reaches `vol_decode_ast_entries()`, whose WP94 "fast
   path" (`vol_read.c:497-537`, worker `:331-368`) is a **second, weaker
   implementation** of the serial loop's contract: it validated nothing and
   copied `e->length` for `ALGO_NONE` where the serial loop copies `hdr`.

So the corruption entered at the read and was laundered into a CRC-valid
stored segment — which is exactly why `cpack_map_guard()` passed (it compares
the stored form against the same already-corrupt buffer) and why the frame CRC
matched.

## Design

**Bug A — the parallel decode is a weaker second implementation.**
`vol_read.c:351` did `memcpy(a->data + dst_off, blob, (size_t)e->length)`
where `blob` is a `hdr`-byte allocation. When `hdr < e->length` that is a
**heap over-read**; it returns success, so the garbage is stored and the frame
CRC is computed over the garbage. Exactly the observed shape: 5 stray bytes
where a neighbouring allocation's tail was copied, and 4096 zero bytes where
the over-read ran into fresh heap.

The serial loop already refused this shape (`hdr != e->length` for NONE,
`hdr >= e->length || hdr == 0` for the compressed algos). Rather than keep two
implementations, both now call one helper:

```
ast_frame_ok(algo, hdr, len, algo_name) -> 0 | -1
```

`decode_thread_worker()` calls it before touching `data`, and copies `hdr`
(not `e->length`) for `ALGO_NONE`. The serial loop's inline check is replaced
by the same call, so the fast path cannot rot back into a worse
implementation. Whole-file blobs (JXL/APE/FLACR/…) are deliberately not
judged — their csize is legitimately unrelated to the logical size.

**Bug B — the bounce buffer is one buffer per volume with no lock.**
`blkio_open()` mallocs a single `BLKIO_BOUNCE` (1 MiB) `io->bounce`
(`blkio.c:341`); `dev_pread()`/`dev_pwrite()` use it with no lock
(`blkio.c:485-487`, `:531-553`), while the fan-out runs up to
`INVFS_READ_THREADS` workers over one volume. `dev_pwrite`'s
read-modify-write is the worse case: two threads interleaving between the
read-back and the patch write one thread's sector over the other's and both
report success. Only four files under `src/core` take any lock; `blkio` was
not one of them.

Fixed with one `pthread_mutex_t bounce_mu` per `blkio`, initialised at the top
of `blkio_open` (ahead of every path that can reach `blkio_close`) and
destroyed in `blkio_close`. It is held for the whole transfer, and for the
whole read-patch-write cycle in `dev_pwrite`. The decompression stays parallel;
only the device hand-off is serialised, and on a device each of those is a
synchronous round-trip anyway.

**Honest scope note.** Bug B is a real defect and is fixed, but it was **not**
the cause of this incident: `test-xfs.sh` opens `wp16xfs.img` as an image
*file*, so `blkio_open` sets `aligned = 0` (`blkio.c:330`) and `bounce` is
`NULL` — `dev_pread`/`dev_pwrite` are never entered. Nothing in the repo sets
`INVFS_FORCE_DEV`. Bug B is latent on any real block device and is fixed on its
own merits.

## Validation

Red control (fixes reverted, `EXIT=1`):

```
  OK    image path (aligned=0): 2 whole-file reads of 8388608 bytes (128 segments) bit-exact
  OK    FORCE_DEV path (aligned=1, shared bounce): 2 whole-file reads of 8388608 bytes (128 segments) bit-exact
  ..    frame leg: entry 0 relabelled NONE; frame holds 502 bytes, entry claims 65536
  FAIL  frame leg: parallel read SUCCEEDED on a NONE frame shorter than the entry length; returned 8388608 bytes
read_parallel_bitexact_test: FAILED
```

Green after:

```
  OK    frame leg: the 6-worker read refuses exactly as the 1-worker one does (rc=-1)
read_parallel_bitexact_test: PASS
```

`make test`: 0 failures (`invf-blkio_test` 95/0, `invf-large_file_v3_test`,
`invf-cpack_guard_test`, `invf-ivpack_packs_test`, `check-repo-hygiene`,
`check-codecpack-sync` all green).

`bash tools/run-e2e.sh tools/test-xfs.sh` — see the commit message.

## Out of scope

- The `arc.h:5` contract ("no internal locking: the volume layer is
  caller-serialized") is still only honoured by convention. `arc.c` takes no
  lock. Not touched here — flagged, not fixed.
- `INVFS_READ_THREADS=1` does **not** select the serial loop; it runs the fast
  path with one worker (`vol_read.c:507-510` clamps to `>= 1`). Left as is;
  the regression test names the control accordingly.

## Coordination notes

Subagent `session-76e01c48-4fe1-465a-9b5f-82c54bc4d7ce`. e2e gate:
`tools/test-xfs.sh`.
