# Deferred-commit policy — design decision, 2026-10-05

**Status:** decided, not implemented. Recorded so the design survives whoever
implements it, and so the rationale is not re-derived from scratch.

## Why

Import is fsync-bound. Measured on a real SSD, 2,000 files:

    fsync calls    12014     = 6.0 per file, 0 errors
    vmux writes     6002     = 3.0 per file
    vol_flush calls     2

The same engine on tmpfs (no block device under the volume) does 5,000 files/s
against 10.3 on the SSD — **500x**, with identical write and byte counts. And
`src/core/blkio.c:703` already recorded it: *"3509 fsyncs, 7.02 per file,
387.7 s of a 390.0 s wall -- 99.4% of the time blocked in fsync."*

Group commit already exists (`blkio_flush` + `write_gen` watermark) but cannot
help: it batches *concurrent* flushes, and `invf-import` is a single sequential
loop, so every flush degenerates into its own fsync.

## The unit is dirty BYTES, not files

A file-count interval is inverted: it batches 100 tiny files well and does
nothing for one huge file. `fsync` cost scales with dirty bytes, so bytes is the
honest unit, with time and idle bounding the two risks files cannot.

## Interface

    --commit-bytes <MB>   sync once N MB are dirty        default: see below
    --commit-time  <ms>   sync at most every T ms
    --commit-idle   <ms>   flush T ms after the last write
    --commit-strict        bytes=0 time=0 idle=0 == today's behaviour, exactly

whichever threshold is reached first. **Always flush on clean shutdown**
(`vol_flush` is called at `vol_close`), so power loss is bounded while a normal
reboot loses nothing.

## Defaults

    installation / bulk import    bytes=16  time=5000  idle=1000
    conservative default          bytes=1   time=1000  idle=500

One global default cannot be right for both: 16 MB / 5 s is right when
installing and wrong for a live guest root. Hence the sequence below, not a
compromise number.

## Sequence

1. Implement with **today's behaviour as the default** (`--commit-strict` is the
   implicit default). One commit.
2. Run the e2e and crash-consistency suites (`vol_crash.c`,
   `meta_clobber_test`, `fsck_rootslot_test`, the `sync_fail_at` /
   `flush_fail_at` hooks) against the install defaults. One commit records the
   result.
3. Only if those pass, flip the default in a separate commit — so rollback is a
   revert, not an archaeology exercise.

## The actual risk, stated plainly

Not the policy — **deferred commit changes what recovery has to cope with.**
The engine's consistency story (savepoints, `vol_repair`, `invf-fsck`
rebuilding L2P and the used-bitmap, the torn-base-page cases in WP86/WP89) has
only ever been tested assuming a write is durable when `write()` returns.

That is what step 2 tests. If it fails, we have found the real boundary of what
the format tolerates — worth more than the import speedup.
