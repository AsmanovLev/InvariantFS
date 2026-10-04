# Where does import time actually go?

**Short answer: not I/O. Not fsync. Not tree growth. Roughly 90% is CPU in the
per-file path, and it has not been localised yet.**

Everything below is measured. What is *not* measured is which function inside
the engine — that is the open question, and it needs `perf`, not more I/O
numbers.

## The numbers

Raw device, `fio --rw=randwrite --bs=4k --direct=1 --ioengine=libaio --iodepth=32`,
and InvariantFS import via `tools/perf-filecount.sh` (counters from
`make PERF=1`):

| device | raw IOPS | import files/s | write/file | flushes |
|---|---|---|---|---|
| SSD  (`sdb4`) | 6,160 | 10.3 | 3.0 | 2 |
| HDD  (`sda1`, raw) | **282** | **9.3** | 3.0 | 2 |

**A 22x difference in device capability yields a 10% difference in import
throughput.** That is the whole finding.

## Why each earlier explanation is wrong

**"7 fsync barriers per file"** — `blkio.h:27` documents page-cache writes with
no `O_DIRECT`/`O_SYNC`, and `invf-import.c:452` calls `vol_flush` once. The
counters confirm it: `vol_flush calls = 2` for a 2,000-file import, not 2,000.
(This claim was in `docs/DEBIAN-INSTALL.md` until this measurement.)

**"The metadata tree is a bottleneck"** — `write/file` is flat at 3.0 across
50 → 500 → 2,000 files, a 40x range. Whatever the per-file cost is, it is a
*fixed* cost, not something that deepens with tree size.

**"It is I/O bound"** — see the 22x table above.

**"It is slow because of the storage stack"** — partly. The earlier Debian
figure of 8.8 files/s was measured on a loop file over btrfs; the same engine
on a raw SSD partition does 10.3. So some of "Arch is slow" was the harness.

## What is true

Each file costs **~97 ms** and issues **3 metadata writes of ~5.4 KB**, against
a device capable of 6,160 IOPS that the engine uses at roughly 0.5%. Both
devices converge on ~3 writes and ~2 flushes per import regardless of size.

Note the HDD's `MB_written/files` (300-400 KB) is much higher than the SSD's
(16 KB) at the same write count — that is almost certainly **volume geometry**,
not behaviour: these runs used 1 GB and 60 GB volumes, and the metadata page
layout differs. Do not read it as the HDD writing more per file. Throughput,
write count and flush count are the comparable columns.

## What would actually answer it

CPU profiling, not I/O measurement:

    perf record -g -- invf-import vol.img corpus/
    perf report

With `make PERF=1` already available for the counter-level view, `perf` gives
the answer the counters cannot: which function inside those 97 ms.
