# NBLI prototype bench vs the JXL lane (burst corpus + one real photo)

Date: 2026-10-13. Host: Debian 13, kernel 6.12.111+deb13-amd64, gcc-14
(Debian 14.2.0-19). Tools: NBLI prototype v0.4 rebuilt from source
(`https://github.com/WangXuan95/NBLI`, commit `6561fc8`, open `NBLI`
prototype only — the closed `fNBLI` binary was never executed, never
staged, never trusted), `cjxl`/`djxl` v0.11.2 (system), PIL 11.1.0.
All encoders single-threaded (NBLI is single-threaded by construction;
`cjxl`/`djxl` pinned with `--num_threads=1`). Every encoded file below
was decoded back and pixel-compared; anything less than exact bytes
would be reported as MISMATCH, not averaged away.

## Corpus

- Burst series: 12 frames, 1600x1200 RGB, lossless PNG, one synthetic
  scene (gradients + 26 soft blobs + horizon/pole edges) with per-frame
  integer shift (-3..+3 px), exposure wobble (+/-2.5) and fresh gaussian
  sensor grain (sigma 3.0) per frame. Seed 99 layout, same shape as the
  `JXL-BATCH.md` burst pattern. Total PNG bytes: **36,290,805**.
  Generator: `/tmp` script (`gen_burst.py`, seed 99 + per-frame seed
  99+i), not in tree.
- Real photo: one carried-over 1000x666 RGB PNG, **912,283 B**
  (md5 `a0962c6673f1a578db71fc4edc330716`).
- Kodak: UNFETCHABLE from this network (mirror returned HTTP 404 for
  every `kodim*.png`; the fetch residue is one 196-byte HTML error
  page). The carried-over real photo above is the real-image leg.
  No Kodak numbers are reported because no Kodak files exist here.

## Legs (burst, 12 frames)

| Leg | Encoder | Total bytes | Wall (12 files) | Ratio vs PNG |
|---|---|---:|---:|---:|
| N1 | NBLI default (lossless) | 33,146,290 | 6.36 s | 1.095x |
| N2 | NBLI `-g` (golomb tree) | 33,223,276 | 8.36 s | 1.092x |
| J1 | `cjxl -d 0` lossless, effort 7 | 33,715,596 | 59.26 s | 1.076x |
| J2 | `cjxl -d 0 -e 8` lossless | 33,305,754 | 113.32 s | 1.090x |

Per-file N1: frame00 2761470 (0.535 s), frame01 2762984 (0.528 s),
frame02 2762362 (0.528 s), frame03 2762138 (0.525 s),
frame04 2761314 (0.528 s), frame05 2762372 (0.527 s),
frame06 2763010 (0.527 s), frame07 2761768 (0.528 s),
frame08 2761882 (0.528 s), frame09 2762906 (0.528 s),
frame10 2761406 (0.524 s), frame11 2762678 (0.522 s).

Per-file N2: frame00 2768010 (0.697 s), frame01 2769378 (0.691 s),
frame02 2768784 (0.691 s), frame03 2768490 (0.692 s),
frame04 2767612 (0.696 s), frame05 2768854 (0.692 s),
frame06 2769352 (0.694 s), frame07 2768122 (0.693 s),
frame08 2768318 (0.689 s), frame09 2769270 (0.685 s),
frame10 2767962 (0.680 s), frame11 2769124 (0.720 s).
N2 is +76,986 B (+0.23%) and +2.0 s slower than N1: the golomb tree
buys nothing on this grainy burst corpus.

Per-file J1: frame00 2807124 (5.096 s), frame01 2809083 (4.769 s),
frame02 2811921 (4.836 s), frame03 2807985 (4.926 s),
frame04 2810404 (4.827 s), frame05 2811427 (4.886 s),
frame06 2812580 (4.930 s), frame07 2807762 (4.963 s),
frame08 2809134 (5.093 s), frame09 2810565 (5.168 s),
frame10 2805231 (4.858 s), frame11 2812380 (4.875 s).

Per-file J2: frame00 2767571 (9.358 s), frame01 2775815 (9.425 s),
frame02 2779089 (9.448 s), frame03 2781012 (9.491 s),
frame04 2771059 (9.431 s), frame05 2773348 (9.489 s),
frame06 2777283 (9.447 s), frame07 2778822 (9.463 s),
frame08 2773424 (9.506 s), frame09 2781386 (9.415 s),
frame10 2776050 (9.388 s), frame11 2770895 (9.426 s).
J2 per-file bytes likewise match the prior attempt's staged files
(frame00 2767571 both runs).

Method note: the carried-over `bench_final.sh` called bare `cjxl`,
whose default is distance 1.0 (lossy) — a first J1 pass here produced
1,858,412 B of lossy output and was discarded. Lossless legs use
`-d 0` (J1 at default effort 7, J2 adds `-e 8`). The corrected J1
per-file bytes match the prior attempt's staged JXL files exactly
(frame00 2807124 both runs), cross-validating the rerun.

## Real-photo leg (1000x666, 912,283 B PNG)

| Leg | Encoder | Bytes | Wall | Ratio vs PNG |
|---|---|---:|---:|---:|
| RN | NBLI default | 626,432 | 0.186 s | 1.456x |
| RJ1 | `cjxl -d 0`, effort 7 | 630,288 | 1.229 s | 1.447x |
| RJ2 | `cjxl -d 0 -e 8` | 628,036 | 4.006 s | 1.453x |

NBLI is the smallest and the fastest on the single real photo too
(-0.6% vs effort-7 JXL at ~7x the speed; -0.3% vs `-e 8` at ~22x).

## Decode-back

51/51 pixel-exact, 0 mismatches: all 12 burst frames through all 4
legs (48) plus the real photo through RN/RJ1/RJ2 (3). NBLI decodes
via `nbli-proto -f <in.nbli> -o <out.png>`, JXL via
`djxl --num_threads=1`, compared with numpy array equality against the
source PNG. Decoded intermediates deleted after the compare (`rt/`
empty); footprint held flat at ~165 MB with ~748 MB free on `/tmp`.

## Build note

Plain `g++ src/NBLI/main.cpp src/imageio/*.c src/imageio/uPNG/*.c
src/NBLI/NBLI.cpp -O3 -Wall` per the upstream README (dynamic link;
no `-fcommon`-class flag needed on gcc-14). The carried-over rebuilt
binary re-verified round-trip exact on one burst frame before the
timed rerun, so no rebuild was needed — upstream sources unchanged
(`git status` clean in `/tmp/nbli-src`, HEAD `6561fc8`).

## Verdict: YES, lane candidate — on these numbers

Head-to-head on the burst corpus (36,290,805 B PNG): NBLI default
33,146,290 B in 6.36 s vs `cjxl -d 0 -e 8` 33,305,754 B in 113.32 s —
NBLI is **0.5% smaller at 17.8x the encode speed**; vs effort-7 JXL
(33,715,596 B, 59.26 s) it is **1.7% smaller at 9.3x**. Note what
"burst" means here and what it does not: NBLI is a SINGLE-image
codec with no inter-frame coding — each of the 12 frames was encoded
independently (~0.53 s/file) and the totals are plain sums. The corpus
is a burst series only in shape (consecutive similar frames), so these
numbers prove consistent per-frame wins on photo-like content, not a
burst/series mode. The real
photo repeats the shape: NBLI 626,432 B in 0.19 s vs `-e 8` 628,036 B
in 4.01 s (**0.3% smaller, 21.5x faster**). The upstream claim (JXL
`-e 8` ratio at ~25x speed on CLIC2021 photos) points the same
direction we measured — parity-or-better ratio at ~18-22x speed — on
a different corpus and one real frame, so treat the multiple as
corroborated-in-spirit, not reproduced-to-the-letter.

Caveats that keep this a *candidate* and not a *decision*: the burst
corpus is synthetic grain that compresses poorly under every codec
here (~1.09x), so the ratio deltas are small in absolute bytes; the
real-image evidence is a single 1000x666 frame (Kodak unfetchable —
HTTP 404 from this network); only single-threaded encode was timed
(no decode-speed comparison); and bit-exactness is proven here on 25
NBLI files, which is a narrow proof for a v0.4 prototype. Engine
wiring, `.so` packaging, and registry work are out of scope and
unstarted — the next WP should re-run these legs on real Kodak/CLIC
frames and measure decode before any lane lands.
