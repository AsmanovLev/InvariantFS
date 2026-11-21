# InvariantFS Benchmark — Silesia corpus

> **SUPERSEDED — read this before quoting any number below.**
> This run is dated 2025-08-25 and predates Meta-v3 entirely (the `v0.5.0`
> default format), WP10/WP14 batching, and the v3 COW base-page accounting.
> It was taken without `INVFS_META_FRAC` set, and **no reclaim-gate
> condition was recorded**, so its `2.95x` is not a current figure and
> must not be compared against a gated run. Per AGENTS.md §1.7 a claim
> that cannot be pointed at in the tree is not restated, so this WP did
> **not** substitute a replacement ratio: the current Silesia numbers do
> not exist in the tree, and the live benchmark document is
> `docs/benchmarks/`. Two notes specific to the text below: the comment
> “sweep releases the RAW originals” describes v2 behaviour — on v3 the
> builtin container lanes do not release the superseded segments at lane
> time (see `AGENTS.md` §2.5) — and `bench-invfs.sh` was never written.
> The raw table is kept below as a historical record, otherwise unmodified.

Date: 2025-08-25. Corpus: Silesia, 12 files, **211 938 580 B** (202.16 MiB).
Host: i5-10500H (12 threads), Fedora 44, volume/images in tmpfs. All numbers are
useful data bytes only (filesystem metadata excluded; for INVFS — occupied blocks
of data zones per bitmap).

## Compression (ratio = logical / on-disk)

| System | Settings | On-disk | Ratio | Write | Unpack |
|---|---|---:|---:|---|---|
| SquashFS | xz, -b 1M, dict 100% (max) | 52 916 224 B | **4.01x** | 7.9 s wall / 85.7 s CPU (MT) | n/a¹ |
| SquashFS | zstd lvl19, -b 1M | 57 077 760 B | 3.71x | 10.1 s wall / 108.9 s CPU (MT) | n/a¹ |
| **InvariantFS** | import LZ4 + sweep ZSTD-19 (per file) | 71 884 800 B | **2.95x** | 0.7 s + 61.7 s sweep (1 thread) | **0.46 s** cold² |
| EROFS | lz4hc lvl9 (from an earlier series, synthetic corpus) | — | ~1.73x³ | — | — |

¹ squashfs — read-only image, “unpack” = mounting.
² FUSE mount + tar of the entire corpus after drop_caches (sudo). Warm ≈ 0.2 s.
³ on a different corpus; no directly comparable figure.

## Archiver baselines (same corpus)

| Tool | Size | Ratio | Time |
|---|---:|---:|---|
| zpaq m5 (official silesia.zpaq) | 47 753 698 B | 4.44x | — |
| xz -9 (tar stream) | ~52 MB | ~4.0x | minutes, MT |
| zstd -19 (tar stream) | ~57 MB | ~3.7x | ~10 s MT |

## Comments

- **The gap with xz (4.01x vs 2.95x)** is explained by granularity: INVFS compresses
  each file WHOLE with one ZSTD-19 stream (the window is bounded by the file),
  whereas xz on the tar stream sees the whole corpus as a single 212MB stream and the
  100% dictionary of the 1M block works over a shared context. The main headroom is
  cross-file context (shared dictionary/sweep session) and a stronger level
  on large files.
- **Write time**: for INVFS this is two phases — import 0.7s (LZ4, instant
  access) and a background sweep 62s (ZSTD-19, single-threaded). Sweep is easily
  parallelized across files (currently 1 thread) → realistically ~8-10s MT,
  comparable to mksquashfs.
- **Read**: 0.46s cold across the whole corpus via FUSE (~440 MB/s) — the bottleneck
  is not decompression but round trips; recipe-cache (WP4d) should raise this higher.
- The INVFS metric is honest: used blocks from the bitmap of both data zones after sweep;
  sweep releases the RAW originals.

Reproduce: `/var/tmp/bench/harness/fs-bench.sh` (btrfs/erofs), the INVFS leg —
commands in the session history. The `bench-invfs.sh` script was **never written**: neither
`tools/` nor `tests/` exist in the tree, so this run is not reproducible from
the repository.

> This run (2025-08-25, 2.95x) predates WP10/WP14. Current numbers on the
> same corpus and reads vs btrfs lived in a separate B30+ series document
> (Silesia end-to-end, batching, B34–B36) under `src/doc/`. It was deleted along
> with all of `src/doc/` in d559a8b: 18 documents from the v2 era (L2P mapper,
> CKP0/`\x01reten`), 15 of which never mentioned Meta-v3 at all — see
> AGENTS.md §1.7. Read from there: `git show d559a8b^:src/doc/16-benchmarks.md`
> (1137 lines). The live benchmark doc is `docs/benchmarks/`.
