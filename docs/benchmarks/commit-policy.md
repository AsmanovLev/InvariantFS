# Deferred-commit policy — design decision, 2026-10-05

**Status:** decided and partly implemented. The mechanism lives in
`src/core/blkio.c` as env knobs (`INVFS_COMMIT_BYTES` / `INVFS_COMMIT_MS` /
`INVFS_COMMIT_IDLE_MS`); the CLI flags (`--sync`, `--commit-*`) are still
pending, and step 2 below (crash-consistency under deferred commit) is still
open. Recorded so the design survives whoever implements the rest, and so
the rationale is not re-derived from scratch.

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

## Baseline to beat

The "before" number, on a real SSD, both synthetic and real-root:

    synthetic, flat       8,000 files / 1 dir     75.7 s   = 105.7 files/s
                         8,000 files ->  48,054 fsyncs, 24,002 vmux writes
                         fsyncs/file = 6.01, writes/file = 3.00

    Arch Linux root      34,209 files             2,416.4 s =  14.2 files/s
                         + 11,186 symlinks, 3,085 dirs, 0 skipped
                         volume 2.2 GB real of a 16 GB sparse image

**The synthetic benchmark is optimistic by ~7x.** It holds the corpus flat in one
directory and every file the same size, which is the easy case for any
directory-tree cost. A real root has 3,085 directories, symlinks and wildly
varied content.

Two consequences, both load-bearing:

  - **Never quote the synthetic number as an import speed.** 14.2 files/s on a
    real root is the honest figure; 105.7 is a lab instrument.
  - **fsyncs/file is stable at 6.01** across 500 / 2,000 / 8,000 files, so the
    deferred-commit win scales with file count: cutting fsyncs by the batching
    factor applies to the real 34,209-file case almost exactly as it does to the
    synthetic one, even though the wall-clock does not.

If a future corpus wants to predict real-world import time, it needs directory
depth and size distribution, not just more files in one directory.

## The unit is dirty BYTES, not files

A file-count interval is inverted: it batches 100 tiny files well and does
nothing for one huge file. `fsync` cost scales with dirty bytes, so bytes is the
honest unit, with time and idle bounding the two risks files cannot.

## Interface

Two named extremes, plus intervals for everything between:

    --sync                 fsync on every transaction        (today's behaviour)
    --no-sync              never sync until unmount/close   (install-from-media)
    --commit-bytes <N>     sync once N are dirty. Bare number = MiB.
                           Suffixed values accepted: 4K, 512K, 1M. Minimum: one
                           block -- anything smaller is REFUSED, not clamped.
    --commit-time  <ms>    sync at most every T ms
    --commit-idle  <ms>    flush T ms after the last write

`--sync` and `--no-sync` are shorthand, not separate code paths:

    --sync        ==  --commit-bytes 0 --commit-time 0 --commit-idle 0
    --no-sync     ==  --commit-bytes 0 --commit-time 0 --commit-idle 0
                     ... plus "do not sync on ANY threshold, only at close"

The distinction has to be explicit rather than a degenerate value, because
`bytes=0` could mean either "every write" or "never". They therefore map to
distinct internal modes rather than to numbers, so that neither can be reached
by accident from a mis-typed threshold.

Whichever threshold is reached first. **Always flush on clean shutdown**
(`vol_flush` is called at `vol_close`), so power loss is bounded while a normal
reboot loses nothing -- which is why `--no-sync` is a durability-window change
and not a data-loss change.

Mutually exclusive with each other; last one on the command line wins.

### Units, and why a bare number is MiB

Dirty tracking is per page, and the minimum block is 4 KB, so a byte threshold
below one block is meaningless: any write crosses it, which makes `--commit-bytes 1`
behave exactly like `--commit-bytes 0`. Those are three spellings of "sync every
write", which is an ambiguity this interface must not have.

So: **a bare number is MiB.** `--commit-bytes 1` is 1 MiB, `16` is 16 MiB. One
number, one meaning, no suffix to forget. A suffixed value is accepted when
finer granularity is genuinely wanted (`4K`, `512K`, `1M`).

A threshold below one block is REFUSED with an explanation, not clamped and not
accepted silently:

    --commit-bytes 1    error: below the 4K block size; use --sync to mean
                             "fsync on every transaction"

Rejected rather than clamped because the failure mode is invisible otherwise:
asking for a threshold that cannot be honoured and quietly getting --sync
behaviour is how you end up believing you configured a durability policy you did
not configure.

A binary multiplier shorthand (1=1B, 2=2B, 3=4B, 4=8B) was considered and
rejected: `--commit-bytes 3` silently meaning 4 bytes reads as a typo for 3, and
nobody debugs that. It trades a visible error for an invisible one.

## Defaults

**Default (all workloads):** `bytes=1 (MiB)  time=1000  idle=500`

Chosen deliberately over the more aggressive numbers that were considered
(`bytes=16 time=5000 idle=1000` — 16x the bytes, 5x the window). Those are
better for a bulk install and worse for a live guest root, and until the
crash-consistency suite has run under deferred commit (step 2 below) the
default has to be the one whose loss window is measured in seconds rather than
minutes. A bulk importer can pass `--commit-bytes 16 --commit-time 5000` and get
the throughput; nothing has to opt IN to a five-second durability hole.

    default              bytes=1   time=1000  idle=500     <- conservative, always on
    --sync               bytes=0   time=0    idle=0       <- today's behaviour, exactly
    --no-sync            unbounded until close              <- install-from-media

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

## Measured on the real Arch root (2026-10-05)

Strict, from the same corpus and the same machine:

    34,209 files  988.2 s   fsync 248,947   deferred 0

Deferred, `INVFS_COMMIT_BYTES=16`:

    34,209 files   37.4 s   fsync       1   deferred 286,122   errors 0

**26.4x**, and fsync=1 is `vol_close` paying the single owed flush -- the safety
property doing exactly what it was designed to do.

### But the byte threshold never fired, and that must not be glossed over

The 26.4x is NOT byte-batched committing. It is "sync only at close" -- the
`--no-sync` path reached by accident, because:

    blkio writes   0     ← import does not go through blkio_write

`vmux_pwrite` calls `blkio_pwrite` directly, and the dirty-byte accumulator was
added to `blkio_write`. So `dirty_bytes` stays 0 and `commit_bytes` can never be
satisfied. Nothing triggered until `vol_close` forced the final flush.

Consequences, stated plainly:

  - The win is real, measured, and reproducible -- but it is the close-time-only
    behaviour, not the batching this design was for.
  - `INVFS_COMMIT_BYTES` and `INVFS_COMMIT_MS` are currently **dead triggers** for
    any workload writing through `vmux_pwrite`, which is all of them.
  - Only `INVFS_COMMIT_IDLE_MS` and the `vol_close` force actually fire.

**The fix is one line of accounting**: move `dirty_bytes += len` from
`blkio_write` to `blkio_pwrite`, so it counts everything that reaches the
descriptor. Until then, do not read the 26.4x as "16 MB batching works".

After that, re-measure. The expected result is that fsync rises from 1 to
roughly 2.7 GB / 16 MB ~= 170, which is the number the design predicted, and
the elapsed time should stay near the 37 s measured here -- which is itself the
interesting result, because it says the per-fsync cost was ~all of the 988 s.
