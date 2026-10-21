# Backing up a big tree onto an InvariantFS volume (chunked ingest)

How to put a 1 TB / ~1M-file laptop tree onto a dual-device InvariantFS
volume, and how to know — with numbers, not hope — how long it will
take and whether it is safe to walk away from.

This procedure is the output of the WP644 pilot. The harness it names is
`tools/test-backup-pilot.sh`; everything below is a knob or an assertion
that harness actually makes. Where this page disagrees with the code, the
code wins (AGENTS.md 1.7).

## Why the run is chunked at all

RAW is a window, not a tank (AGENTS.md 2.3, 2.4). A 1 TB backup streams
*through* a RAW extent of roughly 20 GB: ingest runs at about the write
path's rate, the sweep drains at its own, and ingest is faster than the
drain on mixed content. If you import 1 TB continuously, raw-class
allocation overflows into shadow-space blocks (legal, but the volume is
being filled faster than it is being consolidated) and you have no
verified point to resume from when the box dies at hour 30.

So the run is: **import a chunk → sweep it → `invf-fsck` must report
CLEAN → record the chunk → next chunk.** The fsck result is a gate, not
advice: a chunk that does not come back CLEAN leaves the run stopped
(`phase_run`, `tools/test-backup-pilot.sh:680`).

## Step 0 — measure before you schedule (the calibration leg)

Do not extrapolate a calendar from a guess. Import a tree you have —
`/usr` is the corpus shape the pilot used — onto a scratch volume on the
scratch disk, then let the harness report the three rates:

```sh
INVFS_PILOT_SRC=/usr \
INVFS_PILOT_WORK=/mnt/scratch/wp644 \
INVFS_PILOT_VOL_GB=16 \
  bash tools/test-backup-pilot.sh
```

The report (`$WORK/PILOT-REPORT.md`, phase at
`tools/test-backup-pilot.sh:888`) gives you:

- **per-file import rate** — files/s and MiB/s, from the per-chunk wall
  clock. This is the number that makes a 1M-file backup an overnight job
  or a week: the cost is per FILE (fsync ladder), not per byte.
- **per-GB sweep rate** — MiB/s, plus the per-stage breakdown
  (`prepare/collect/transform/heat/dedupe/batches/migrate/finalize`) from
  the sweep's own log, so you can see whether `transform` or `dedupe` is
  your wall.
- **fsck wall time** at the inode count that volume actually holds.

### What the numbers looked like, measured

The pilot's calibration leg ran on 2026-10-21 against this box's own
`/usr/lib/x86_64-linux-gnu` (3332 files, 1034 MiB, fresh 8 GiB volume on
the scratch disk), so you can recognise an order of magnitude before you
have numbers of your own:

| stage | measured | extrapolated to 1 TB / 1M files |
|---|---|---|
| import | 7.37 files/s, 2.29 MiB/s | 127.2 h |
| sweep, end to end | 1.45 MiB/s | 200.9 h |
| fsck | 1548 inodes/s at 4144 inodes | 0.2 h per full walk |

13.7 days of wall clock, and the pilot's verdict was **NO-GO inside a
7-day window** with **sweep** as the binding stage, not the codec choice:
`batches` dominates the per-stage breakdown while `transform` stays small
on a mostly-`.so` corpus. None of these is a constant. The import rate is
the host's fsync latency, not InvariantFS compute: the `/usr` leg in the
same WP spent user CPU 1m45s and sys 0m45s against an 8693 s wall, i.e.
the rest is I/O wait. The sweep rate is the content mix, and a corpus
that barely compresses sits near a floor rather than a ceiling.

The 1 TB arithmetic at the bottom of that report is **refused** (printed
as `extrapolation REFUSED`) unless the sample clears
`INVFS_PILOT_MIN_EXTRAP_MIB` / `INVFS_PILOT_MIN_EXTRAP_FILES` (default
512 MiB / 500 files). A rate measured on a few hundred KiB of corpus is
not a rate for 1 TB, and the report says so instead of printing a
confident wrong number.

## Step 1 — preflight

The `plan` phase (`tools/test-backup-pilot.sh:556`) is the preflight, and
it is cheap: it walks the source once and writes a chunk plan.

```sh
INVFS_PILOT_SRC=/backup/src INVFS_PILOT_WORK=/mnt/scratch/wp644 \
  bash tools/test-backup-pilot.sh --phases plan
```

It counts files/dirs/symlinks/specials, names the largest file, and
refuses two situations up front:

- **files above the dense-tier wall** (default 128 GiB,
  `INVFS_PILOT_MAXFILE_MB`): these need the WP634-A/WP635 work and are
  not something a bigger chunk size fixes.
- **nothing to do** — an empty or unreadable source dies here rather than
  producing a manifest full of zero-chunk rows.

It also *warns* — loudly, and without stopping — about **files larger
than RAM**. `invf-import` reads each file whole
(`tools/test-backup-pilot.sh` records the wall in its header), so a file
bigger than memory cannot go through it. Those files must go through a
FUSE mount, where writes are streamed in 64 KiB pieces; that path is
proven at 16 GiB scale. Plan for it: mount the volume, `cp` the big files
through the mount, and let `invf-import` handle the rest.

Confirm the shape by hand too — this is the check that decides whether
the whole plan is valid:

```sh
find /backup/src -xdev -type f -printf '%s %p\n' | sort -rn | head -20
```

And check the volume side of the arithmetic: ~8.3 KB of metadata per
file, flat, is the figure to size with (AGENTS.md 2.7), so ~1M files is
~8–10 GB of metadata on top of the data, plus sweep headroom because
re-encoding happens before reclaim (AGENTS.md 2.5). A ~1.2–1.3 TB volume
for a 1 TB source is the arithmetic, not a preference.

## Step 2 — the chunked run

One top-level directory is one chunk (`plan_chunks`,
`tools/test-backup-pilot.sh:518`), whatever its size; loose top-level
files are staged into one extra chunk so a backup does not silently drop
them. The chunk size target (`--chunk-mb`, default 4 MiB in the hermetic
gate) is advisory: it exists so you can see when a single top-level
directory is bigger than the volume's RAW window, which the run prints.

```sh
INVFS_PILOT_SRC=/backup/src \
INVFS_PILOT_WORK=/mnt/scratch/wp644 \
INVFS_PILOT_VOL=/dev/disk/by-id/ata-...-SSD \
INVFS_PILOT_CHUNK_MB=12800 \
  bash tools/test-backup-pilot.sh
```

If the volume does not exist it is created with `invf-mkfs <vol>
$INVFS_PILOT_VOL_GB`; a dual-device target is formatted by hand instead
(`invf-mkfs /dev/ssd 110 /dev/hdd 1900` is the shape the pilot assumes)
and handed to the harness as `INVFS_PILOT_VOL=<dev0>`.

Each chunk's row in `$WORK/MANIFEST` records files, bytes, the verbatim
byte count, import/sweep/fsck seconds, and free blocks before and after —
so the per-chunk space accounting is a measurement you can read back
later, not a log line you have to trust.

## Step 3 — resume, which is the point

Re-run the same command. `DONE` chunks are skipped, `FAILED` ones are
retried (`man_load`, `tools/test-backup-pilot.sh:439`). The resume proof
is an append-only event log, not a timestamp: the hermetic legs fail the
run on purpose mid-way, resume it, and assert from `$WORK/events.log`
that the already-DONE chunk was **not** imported again
(`hermetic_legs`, `tools/test-backup-pilot.sh:1024`).

Do not delete `$WORK` to "start clean" unless you mean to start over —
that is what `--fresh` is for.

## Step 4 — verify a sample, byte for byte

```sh
INVFS_PILOT_SRC=/backup/src INVFS_PILOT_WORK=/mnt/scratch/wp644 \
  bash tools/test-backup-pilot.sh --phases verify
```

`verify` (`tools/test-backup-pilot.sh:745`) takes a deterministic stride
over the sorted (volume path, source path) list — about twelve entries,
every chunk represented — and compares each one byte for byte through
`invf-cat`, symlink targets included. It is a sample, not a full
re-read: a 1 TB full comparison doubles the whole backup. Run it at
minimum before you call the backup finished, and after anything that
looks like a crash.

## Step 5 — know what a delete looks like

```sh
INVFS_PILOT_SRC=/backup/src INVFS_PILOT_WORK=/mnt/scratch/wp644 \
  bash tools/test-backup-pilot.sh --phases reclaim
```

`reclaim` (`tools/test-backup-pilot.sh:803`) unlinks one chunk through a
FUSE mount, prints the free-block count **right after the unlink**, then
sweeps until the count stops moving and prints it again. Expect a zero or
near-zero first number and a positive second one: the block count moves a
capture late, because the retiring blocks are pinned by the live savepoint
window and a shared TEXT batch is not freed by a targeted delete
(AGENTS.md 2.4, 2.5). The pass asserts the volume is CLEAN afterwards.

If the count does *not* come back after several sweeps, do not assume a
leak — check for a live savepoint (`getfattr -n user.invfs -m- /`) and
re-read AGENTS.md 2.4 step 2 before filing anything.

## Step 6 — the calendar, and the go/no-go

After the target run, `--phases report` writes the same three rates from
the real corpus, and the 1 TB / 1M-file arithmetic, with a go/no-go
against `INVFS_PILOT_CALENDAR_DAYS` (default 7). It names the binding
stage — import, sweep, or fsck — because that is what a follow-up WP
would have to move.

Treat the verdict as: **GO** if the total is inside your window (schedule
it, chunked, unattended), **NO-GO** if it is not — and then the named
stage is the WP, not a bigger chunk.

## Two-device reality (say it out loud)

dev0 holds metadata, the RAW landing area and the tier arena; dev1 holds
the canonical shadow. Swept data is **single-copy** on dev1 in the sense
that matters operationally: losing dev1 loses the backup and you
re-backup from the laptop, which still exists. That is acceptable *for a
backup* — it is not acceptable for primary storage, and this page will
not pretend otherwise.

## What this procedure does not cover

- **The network orchestrator.** Deferred by decision; every step above
  assumes a source tree you can read locally.
- **Container/archive decomposition at 16 GiB+.** The lane work is a
  separate WP; the pilot records which path each file took and moves on.
- **Windows and network sources.** Parked (AGENTS.md 1.7).
- **Crash-safety against power loss during a chunk.** A chunk boundary is
  where the run is verified; mid-chunk power loss means resume from the
  previous chunk's row, which is why the manifest is written per chunk and
  not at the end.

## Running the harness as a test

The hermetic gate needs no source tree, no root and no network:

```sh
INVFS_E2E_AGENT=wp644 bash tools/run-e2e.sh tools/test-backup-pilot.sh
```

It generates a deterministic SHA-256-counter corpus, runs the same
phases a real run does, and asserts them as legs L0–L6 (fixture
determinism, plan shape, an injected mid-run failure, the resume, the
bit-exact sample, unlink reclaim, and the report's refusal to
extrapolate a tiny sample). `$WORK` defaults to a `/dev/shm` directory
that is fine for that corpus and wrong for a multi-GB one — put
`INVFS_PILOT_WORK` on a real disk for anything real.
