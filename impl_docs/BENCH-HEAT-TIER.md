# rheat: what the read-heat actually buys, measured

**Question (2026-11-28).** `rheat` — the per-record read-heat counter — had a
decision hanging on it: *keep the tier promotion it drives, with a measurement, or
retire the whole thing.* The suspicion came from two dead ends found in the code:
`INVFS_HEAT_INIT` parsed into a field nothing read, and `heat_ext_merge()` with zero
callers.

**Answer.** rheat is **not** dead and is **kept**. It has two live consumers, one of
them measured here; the two dead ends were real and were removed.

## What rheat feeds

1. `vol_tier_migrate` (`src/core/vol_tier.c`): after the decay pass, a sweep copies
   canonical **dev1** segments whose `rheat >= INVFS_HEAT_HOT` (8) into the dev0
   arena, and `seg_read_checked` prefers the dev0 copy. Canonical stays — a copy,
   never a move.
2. `heat_extract` (`src/core/vol_heat.c`): read-hot PPMd batch members are extracted
   to standalone per-segment ZSTD, so a hot member does not pay for decoding its
   whole batch slice.

## The instrument

The promotion counters (`tier_promoted`, `tier_blocks`, `tier_demoted`) said what was
**copied**. Nothing said whether a copy was ever **served** — which is the only
number that can decide the question. Added `tier_read_hits` (`volume_internal.h`),
incremented where the dev0 copy is served (`vol_read.c`), printed in the sweep's
`tier:` line and at close under `INVFS_TIER_STATS=1`.

An earlier idea was to use the ARC's `ghost_hits` ("re-decode after eviction"). It
was rejected: ghost hits measure eviction regret in the cache, not whether a
*persisted* acceleration copy paid for its bytes.

## Measurement

Two devices (`invf-mkfs d0 0.125 d1 0.25`), 30 cold + 5 hot files of 1 MiB, the
whole set swept once so the files are canonical on dev1, then 16
`meta_probe --heatpump` sessions per hot file (the sweep-cadence fold), then a sweep.

| | |
|---|---|
| promoted | **80 segments, 1360 blocks (5.3 MiB)** |
| served on re-read (5 hot files) | **80 reads** from dev0 copies |
| served on cold reads (3 control files) | **0** |
| second sweep immediately after | **0 promoted** — idempotent, no re-copy |
| sweep with nothing hot (35 files) | 0.158 s, no tier line |

Precision is 100% in both directions: every promoted copy was served exactly once,
and no cold file was served from an acceleration copy.

## What this does NOT show, and why it matters

**No latency win was measured, and none could be on this box**: both devices are
files on one SSD, so a dev0 hit costs exactly what the canonical read costs. The
benefit of an acceleration copy is bounded by the dev0/dev1 speed difference; with
that difference at zero, the honest statement is *"the mechanism does what it says,
and the payoff is entirely conditional on real device asymmetry."*

The cost side is small and one-off: 1 write per promoted segment, no re-copy on
later sweeps, demotion only when the arena's free share drops under 20%.

So the decision rests on: precision measured, idempotence measured, cost measured,
benefit **unmeasurable here and named as such** — which is not the same as assumed.

## Removed (the dead ends that started this)

| what | why it was dead |
|---|---|
| `INVFS_HEAT_INIT` + `v->heat_init` | parsed at `vol_open`, stored, never read by anything. Seeding read-heat on create needs the INO2 ext blob, which the **v1 inode row does not have** (WP-M7 kept only the fixed row + xattrs). |
| `heat_ext_merge()` | built that same INO2 ext blob; zero callers in the tree. |
| `tools/test-heat.sh` leg 2 | asserted exactly those two behaviours. Now a **named skip** that says they no longer exist; leg 1 (read heat through the `invfs.heat` xattr) and leg 3 (profile ladder) still run. Deleting it is justified by measurement, not by the dead code: a v1 volume rewritten 3x reports `heat=absent` for `wheat` and stays `class=7` after the sweep, so the carry the leg asserted cannot happen on this format. |

A knob that parses and does nothing is worse than no knob: the test written around it
had to skip every run, which is how a real regression would have hidden.

## Two ways this measurement first lied

Both were caught before any conclusion was published, and both are worth keeping:

1. **Reads do not drive heat.** The first workload used `invf-cat` in a loop and got
   **0 promotions** — reading through the tool folds nothing; the fold happens on the
   sweep cadence via `meta_probe --heatpump`. Conclusion would have been "promotion
   never fires".
2. **Raw-zone files are not candidates.** Promotion only considers canonical dev1
   segments (`pba >= shadow_zone_start`), so files still in the dev0 raw zone must be
   swept first. Same 0, same wrong conclusion.

Same class as `AGENTS.md` §1.8: the measurement must name the artefact — here, which
measurement hook actually drives the counter being measured.
