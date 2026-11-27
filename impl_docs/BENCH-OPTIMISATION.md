# The optimisation level is a measurement, not a reflex

**Bottom line: `-O3` is ~28% slower than `-O2` on this engine's CPU-heavy path, with
27% more code. The default stays `-O2`.** This document exists so that the next
person who reaches for `-O3` "for speed" finds the number instead of repeating the
experiment — and so that anyone who has a workload where that is wrong can prove it
with `tools/bench-opt.sh`.

## What was measured

Both builds from the same commit, same machine, same corpus, medians of 3 runs:

| stage | `-O2` | `-O3` | |
|---|---|---|---|
| import (write + fold) | 257 ms | 296 ms | −15% |
| **sweep** (codecs, batches, dedupe) | **26.1 s** | **33.4 s** | **+28% worse** |
| `invf-verify --deep` (decode) | 1481 ms | 1745 ms | −18% |
| `.text`, 6 tools | 5.89 MB | 7.48 MB | **+27%** |
| deep-verify result | `359 of 360 live inode(s): 359 ok, 0 corrupt` | identical | — |

Corpus: 79 MiB / 356 files — the tree's own sources (compressible text, hits the
PPMd batch lane), shared libraries and binaries (zstd / incompressible lanes), and
4 MiB of `/dev/urandom`.

## Why it is slower, and not just "not faster"

`-O3` inlines and vectorises the codec loops until they stop fitting the i-cache,
and the work inside them is not arithmetic — it is table lookups, byte shuffling and
branching in PPMd/zstd. So the trade is: more instructions resident, no arithmetic
win. The 27% larger `.text` is the mechanism, not a side effect.

## The trap that makes this measurable only if you insist

The first attempt measured **11 MiB / 348 files** and produced a perfect tie
(import 70 ms vs 71 ms; decode 15 ms vs 14 ms). That corpus is syscall-bound: nearly
all the wall time is open/read/write, so the generated code barely matters. The real
difference only appears at a size where the codec path dominates — 79 MiB.

Two consequences, both encoded in `tools/bench-opt.sh`:

1. **Time the sweep separately.** `invf-import` writes and folds; it does not
   compress. Timing import alone answers a question nobody asked. The compression,
   batching and dedupe live in `invf-sweep`.
2. **Assert both variants produce the same result.** A speed difference measured on
   different output is not a result. The script fails if the deep-verify lines
   differ, and prints the line it compared.

## What would change the answer

If your workload is dominated by a single hot decoder over large text (no sweep, no
batching), `-O3` may well win — that is a different measurement, not a refutation.
Build both and run the same corpus:

```
tools/bench-opt.sh -O2 -O3
REPS=5 tools/bench-opt.sh -O3 -O2
```

If a variant wins by more than noise on *your* corpus, change `OPT` in the Makefile
and update the table above in the same commit. That is the whole protocol: the
default is a claim, and a claim without a measurement rots.

## The other reason this is not a knob to flip casually

A flag change is a change to generated code inside a filesystem. On 2026-11-27 the
same volume was healthy under one build and lost its directory under another, because
`vol_spt0.c` compared a 32-byte recipe address against a nine-byte string literal:
23 bytes of whatever `.rodata` followed joined the answer, so "does this inode have a
recipe" was a question about the link layout. `tools/check-literal-lengths.py` now
audits that shape in every hygiene run, and `make SAN=address,undefined` exists to
find the rest. Before trusting any optimisation change, run the full matrix — not
because it is policy, but because this engine has already been bitten by exactly
that.
