# BENCH — archiver lanes and OpenZL (2026-11-26)

What this answers: for the `archiver/` prototype (`miniar`, MAR3) and for the
engine's batch lanes, **is OpenZL a lever, and which codec belongs on which
lane?** Every number below is from one corpus and one machine, and the codec
comparison was run through separate processes (see the caveat at the end).

## Corpus

A real MAR3 built by `miniar` from the InvariantFS tree: **48 entries** —
43 C sources from `src/core` (the text lane), two different FLAC tracks sharing
one embedded JPEG cover, and three QCOW2 images with written guest data.
Archive payload **4 843 225 bytes**, in two lanes:

| lane | bytes | what |
|---|---:|---|
| sources | 2 191 275 | the 43 C files, one contiguous span each (43 spans) |
| remainder | 2 651 950 | covers, QCOW2 recipes and members, catalogue, literal bytes |

The lanes are not a guess: `measure.samples()` reads them out of the MAR3
structure, which is what the archiver itself records.

## Size — the source lane (2 191 275 B)

| method | bytes | vs PPMd |
|---|---:|---:|
| **PPMd** (`7z PPMD:o=8:mem=64m`) | **399 990** | — |
| zstd -19 | 480 433 | +20.1% |
| OpenZL trained, level 12 | 527 610 | +31.9% |
| OpenZL trained, level 6 | 537 653 | +34.4% |
| OpenZL generic, level 12 | 553 874 | +38.5% |
| OpenZL generic, level 6 | 568 760 | +42.2% |

Two things this settles. **`--train-inline` is not a rounding error** — it moves
OpenZL 7.2% (568 760 -> 527 610), so the earlier "OpenZL is 4.3% worse" result was
not merely an unfair configuration. And **even trained it stays 32% behind PPMd**
and 10% behind zstd -19: on source text, OpenZL is not the lever.

## Size — the whole archive (4 843 225 B)

| method | bytes | note |
|---|---:|---|
| PPMd (whole archive) | 2 080 532 | smallest, and the slowest by a wide margin |
| LZMA2 (`7z -mx=9 -mmt=1`) | 2 110 649 | |
| **zstd -19 (whole archive)** | **2 118 348** | the archiver's current choice |
| Brotli (`7z -m0=BROTLI`) | 2 156 845 | |
| OpenZL generic, serial profile | 2 209 221 | +4.3% over zstd -19 |
| zstd -3 | 2 251 080 | |
| **hybrid: PPMd on sources + zstd -19 on the rest** | **2 037 862** | see below |

The hybrid is computed, not compressed: `480 433 - 399 990 = 80 443` bytes are
saved on the source lane by using PPMd instead of zstd -19, and the remainder
still carries its own layout (`16 + q + r` in `speed.py`, where `q` and `r` are
the two frame sizes). It is the best density available from codecs that are
already in the project, and it needs **no format change** in `miniar`: the
archive already has independently decodable groups, only the codec choice is
global today.

## Decode time, whole archive (median of 7 runs, subprocess wall time)

| method | ms | bytes |
|---|---:|---:|
| zstd -19 | 36.0 | 2 118 348 |
| zstd -3 | 36.6 | 2 251 080 |
| OpenZL | 38.1 | 2 209 221 |
| Brotli | 45.0 | 2 156 845 |
| LZMA2 | 49.7 | 2 110 649 |
| PPMd | **858.3** | 2 080 532 |

**The density lever costs ~24x decode time.** That is not a defect of this
corpus: PPMd's decoder is serial by construction, and the same relationship
appears on the lane split (PPMd 237.5 ms vs zstd -19 32.1 ms on 2.19 MB; hybrid
sum 268.8 ms vs 36.0 ms whole-file zstd). So the honest framing is a *policy*
choice — a density mode for cold/archival data, a speed mode for anything read
interactively — not a codec that is "better".

## Verdict

1. **OpenZL, as tested, is dominated on this corpus.** It is slower to decode
   than zstd -19, 4.3% larger on the whole archive, and 32% larger on the lane
   where text matters. Its one win is size over zstd -3 at zstd-like speed.
2. **The untested OpenZL claim is narrower than "OpenZL is bad".** What has been
   tested is the generic/serial profile and `--train-inline`. A hand-built
   multi-stream graph — one frame, many typed streams, not one frame per column
   (which lost badly at these sizes: 989 B vs 750 B for zstd -19 on the
   catalogue, because 8-24 byte columns cannot pay a frame header) — has not
   been built or measured. The bar it must clear is explicit now: **beat 527 610
   on the source lane**, or **399 990** if the goal is density.
3. **The cheap, real win is in `miniar`, not in a new codec:** choose the codec
   per group. Splitting sources to PPMd saves 3.8% of the whole archive
   (2 037 862 vs 2 118 348) with no new format, no new reader, and no OpenZL.
4. **For the engine, this is confirmation rather than news.** `vol_textzone.c`
   already sends text batches to PPMd; these numbers say that choice is worth
   ~20% on source text versus zstd -19 and quantify its price. The engine's
   residual gains are per-lane codec decisions, which it already makes — not a
   structural change.

## Reproduce

From `D:/Users/User/invfs_deploy`:

```bash
wsl python3 archiver/openzl-bench/measure.py          # builds the MAR3 + lane samples
python archiver/openzl-bench/speed.py archiver/openzl-bench/speed.mar
python archiver/openzl-bench/split_lanes.py archiver/openzl-bench/speed.mar
cd archiver/openzl-bench
./zli.exe compress --profile serial --level 12 --train-inline lane_sources.bin -o t12.zl -f
```

## Caveat on the timings

Each decode is one subprocess (`zstd` through `wsl`, `zli.exe`, `7z`), so the
medians include process/interop start-up. That inflates every figure — the
absolute milliseconds are not the codec's throughput — but it inflates the
fast codecs *relatively* more, so the true PPMd-to-zstd ratio is **wider** than
24x, not narrower. Sizes are unaffected; they are file sizes and were each
decoded and compared byte-for-byte with the input.
