# Audio album-batch benchmark (synthetic corpus)

Date: 2026-10-13. Tools: `ffmpeg`, `flac -8`, `wavpack -h` (Debian).

## Corpus

10 synthetic "tracks": 30 s stereo 44.1 kHz/16-bit each (sine tone +
brown noise mix, distinct base frequency per track). Total WAV:
**52,920,780 B**. Synthetic stationary content compresses extremely
well (4.26x) — treat absolute ratios as corpus artifacts, deltas as
honest. (`mac`/APE encoder unavailable here; FLAC and WavPack are the
same linear-predictive family, so the batching math transfers.)

## Legs

| Leg | What | Result |
|---|---|---:|
| A | `flac -8` per track, sum | 12,414,971 B (4.26x) |
| B | `wavpack -h` per track, sum | 12,435,858 B |
| C | concatenated 300 s album + `flac -8` | 12,342,587 B (**+0.6%** vs A) |
| D | same album + `wavpack -h` | 12,434,986 B (**+0.007%** vs B, noise) |
| E | decode-back, raw PCM compare | album + track bit-exact |
| F | Monkey's Audio `mac -c3000` (high) per track, sum | 12,001,192 B (**4.41x**, best of three) |
| G | same 300 s album + `mac -c3000` | 12,000,020 B (**+0.01%** vs F, noise) |
| H | APE decode-back, raw compare | track + album bit-exact (52,920,000 B) |

## Reading

- Audio album batching is ~worthless as a ratio play: linear
  predictors adapt within seconds per track, so cross-track context
  buys nothing (flac +0.6% ≈ ten headers + warmup, wavpack noise).
- Same verdict shape as photo albums (JXL/AVIF legs in
  `docs/benchmarks/JXL-BATCH.md`): per-file transcode stands,
  album-batching adds only colocation (one recipe vs ten) and
  metadata savings — i.e. the opaque-batching case, not a
  compression case.
- Real music (~2x typical, non-stationary) may shift second-order
  digits, but the mechanism (fast per-track adaptation) says the
  delta stays tiny. Re-run on real albums only if someone disputes
  with numbers, not adjectives.

## Lane note: `mac` vs hostile WAVs

Monkey's Audio `mac` (built from source) SEGFAULTS (SIGSEGV) on
ffmpeg-written WAVs carrying a `LIST` chunk, and encodes clean
(stdlib-`wave`-written) WAVs fine. Any future APE lane must
normalize/sanitize container chunks before feeding `mac` — or, more
robustly, run it sandboxed (exec-helper path) so a parser crash is a
decline, never a daemon fault. Observed 2026-10-13, all legs above
ran on clean WAVs with identical audio.
