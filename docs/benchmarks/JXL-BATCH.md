# JXL photo-batch benchmark (synthetic corpus)

Date: 2026-10-13. Host: Debian 13, kernel 6.12 (see `uname -r` at
repro time). Tools: `cjxl`/`djxl` (system), `zstd -19`, PIL 11.1.0.

## Corpus

12 synthetic photo-like JPEGs, 1600x1200, quality 85, seed 7
(gradients + blobs + noise + edges; generator: PIL script, kept with
the run notes, not in tree). Total: **6,047,820 B**. Synthetic, not
real photos — treat absolute ratios as indicative, deltas as honest.

## Legs

| Leg | What | Result | Ratio vs JPEG |
|---|---|---:|---:|
| A | `cjxl` lossless per file (12x), sum | 3,492,774 B, 0.7 s | **1.73x** |
| B | `djxl` decode-back vs originals | 12/12 byte-identical | bit-exact OK |
| B1 | concatenated JPEGs + `zstd -19` | 4,509,824 B, 1.6 s | 1.34x |
| B2 | concatenated JXL outputs + `zstd -19` | 3,485,911 B, 0.4 s | +0.2% over A |

## Reading

- Per-file JPEG->JXL transcode (the existing builtin lane shape) beats
  naive batch+zstd (1.73x vs 1.34x): transcoding removes the JPEG
  framing per file, batching raw JPEGs only harvests shared headers and
  whatever cross-file redundancy survives quantization.
- Second-order batching of already-transcoded JXL streams is noise
  (+0.2%): no shared context left worth taking.
- Batching wins where per-file leaves shared context AND the codec can
  use it (raw rasters, text). For lossy-compressed photos the honest
  order is transcode-first, batch-second — or not at all.
- JXL multi-frame album (one stream, per-frame JPEG reconstruction)
  remains untested here; it is the only shape that could beat A on
  JPEG originals, and needs spec-level proof work first.

## Multiframe legs (burst series: one scene + jitter, 12 frames)

Tooling note: `cjxl` v0.11.2 collapses multi-frame APNG input to ONE
frame (PIL-written APNG verified 12-frame in, 1-frame JXL out).
Multiframe works only via animated GIF input (256-color quantized —
lossy, so this leg measures container efficiency, not a lossless
photo path). `djxl` back to APNG recovers all frames.

| Leg | What | Result |
|---|---|---:|
| C1 | 12 GIF frames, `cjxl` each, sum | 487,967 B |
| C2 | same 12 frames, one multiframe JXL | 504,775 B (**-3.5%**, worse) |

Reading: stock `cjxl` lossless does NOT exploit inter-frame
redundancy here (per-frame modular coding; frame headers cost extra).
No measured gain, slight loss. Combined with the APNG-input quirk and
the GIF-only path, JXL-multiframe is DEAD as a near-term photo lane.

Further, JPEG-asset -> multiframe with bit-exact JPEG back is
tooling/format-impossible today: `cjxl` takes ONE jpeg, and JXL
reconstruction data (`jbrd`) is image-level, not per-frame. The only
achievable shape is hybrid (multiframe pixels + per-frame
reconstruction sidecars) — research, not engineering. Photo story
stands: transcode-per-file + opaque batching for colocation.

## AVIF legs (same burst series; `libavif-bin` from Debian)

| Leg | What | Result |
|---|---|---:|
| D1 | 12 frames, `avifenc --lossless` each, sum (22.8 s) | 270,007 B (1.78x vs PNG sources) |
| D2 | same 12 frames, one animated AVIF lossless (8.9 s) | 892,836 B (**3.3x worse** than D1) |
| D3 | decode-back: single frame + album frames 0/5/11 | all pixel-exact |

Reading: the animated path is lossless and frame-complete (12/12
decode pixel-exact), but 3.3x BIGGER than the per-frame sum with stock
settings — no inter-frame gain materializes, possibly different
internal defaults on the animation path. AOM has extensive inter
tuning knobs unexplored here; without dedicated tuning research,
animated-AVIF-album is DEAD as a near-term lane, same verdict as
JXL-multiframe. Per-frame AVIF lossless itself is strong (1.78x vs PNG
on this corpus) and stays a valid per-file transcode candidate.

## AVIF max-effort legs (`-s 0 -k 999 -a min-partition-size=128`)

| Leg | What | Result |
|---|---|---:|
| D4 | same 12 frames, max effort + coarsest partitions (28 s) | 1,429,017 B (**5.3x worse** than D1) |

Reading: forcing maximum block sizes backfires — the content needs
fine partitions, and no keyint setting materializes inter-frame gain
in the lossless path (frame 5 spot-checked pixel-exact, 12 frames
present). Tuning does not save the album shape; the per-frame path
stands unchallenged. Album-batching investigation closed: photo story
= transcode-per-file + opaque batching.

## AVIF min-partition leg (`-s 0 -k 999 -a min-partition-size=4`)

| Leg | What | Result |
|---|---|---:|
| D5 | same 12 frames, finest partitions (85 s) | 634,344 B (better than D2/D4, still **2.3x worse** than D1) |

Reading: fine partitions beat coarse (content wants detail), but no
setting touches the per-frame sum (270,007 B). The animation path
carries overhead the single-image path does not, and no inter-frame
coding materializes in any tested configuration. Album shape closed
for good on current tooling.
