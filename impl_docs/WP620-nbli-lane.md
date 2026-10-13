# WP620 — NBLI photo lane with a two-way codec handler (jxl | nbli)

## Scope

Smallest file set (expected):

- `registry/codecpacks/nbli/<ver>/` (new fat pack: NBLI prototype source pin +
  static build via `registry/tools/build-payloads.sh` pattern, manifest, sha256)
- `src/core/vol_sweep.c` (photo lane dispatch: two-way handler)
- `src/cli/fuse_fs.c` or volume mount options (user-visible codec choice; exact
  knob TBD by implementer — mount option preferred, default `jxl`)
- `tools/test-nbli-lane.sh` (new e2e: both handler positions, bit-exactness)
- `docs/benchmarks/NBLI-BENCH.md` (append: real-corpus + decode-speed legs)

Do not touch: JXL builtin lane (stays default), PNGR, container lanes,
avrecode (parked, WP618), fNBLI closed binary (disqualified, never stage it).

## Why

WP617 benched the open NBLI prototype (WangXuan95/NBLI @6561fc8) against the
JXL lane: 0.5% smaller at 17.8x encode speed on a 12-frame burst corpus,
0.3% smaller at 21.5x on a real photo, 51/51 decode-back exact
(`docs/benchmarks/NBLI-BENCH.md`). Orchestrator decision 2026-10-13: adopt as
an opt-in lane. NBLI is single-image-only (no inter-frame coding) — the lane
is a per-file codec choice, not a series mode.

## Design

- NBLI arrives as a **fat pack**, not a builtin (follows the WP615-followup
  direction: builtins demote to packs; new codecs land as packs). Reuse the
  WP619 static-payload machinery (`build-payloads.sh`, registry index).
- Two-way handler: photo lane dispatches to `jxl` or `nbli` per an explicit
  user choice. Proposed shape (implementer confirms): mount option
  `-o photo_codec=jxl|nbli`, default `jxl` (grandfathering: existing volumes
  behave identically until the user opts in). The choice must be visible in
  `user.invfs.stats` or equivalent so an operator can tell which codec owns a
  file.
- Bit-exactness guard per file: decode-back `memcmp` before the recipe is
  trusted (same category as the JXL lane guard); refusal leaves the file RAW,
  never the generic floor silently.
- Recipes must record which codec owns each segment (forward-read + rollback
  must not need the choice knob to decode).

Pseudocode (dispatch):

```
if (photo_lane_eligible(file)):
    codec = user_choice()  # jxl (default) | nbli
    if codec == nbli and pack_loaded("nbli"):
        try nbli_encode(file) -> guard(decode_back_memcmp) -> commit or RAW
    else:
        existing jxl path unchanged
```

## Validation

- Real corpus legs: Kodak + CLIC frames (fetch; WP617 could not — 404 from
  its network; if still unfetchable, document and use the carried-over photo
  + synthetic burst, explicitly).
- Decode-speed timing for both handler positions (WP617 timed encode only).
- `tools/test-nbli-lane.sh` via `bash tools/run-e2e.sh` (both positions +
  refusal legs + fsck/host-clean), `make test`, `check-repo-hygiene.sh`.
- Expected outcome: lane lands with `nbli` opt-in, default path byte-identical
  behavior to pre-WP (prove with a before/after sweep comparison on a photo
  corpus with default settings).

## Out of scope

- fNBLI closed binary in any form (policy: no binaries, no audit).
- Inter-frame/series coding (does not exist in NBLI; not invented here).
- Demoting the JXL builtin (WP615-followup owns that).
- Decode-path ARC special-casing for NBLI segments (generic path unless
  measured slow — then a new WP).
- Re-benching avrecode (parked).

## Coordination notes

- Subagent ID: `wp620-nbli-lane`. Worktree: `/tmp/invfs-wp620`, branch
  `wp/620-nbli-lane`, created from main post-WP619.
- E2E gates: `tools/test-nbli-lane.sh` (new) + `tools/test-writepath.sh`
  regression (photo lane shares the write path).
- Disk discipline (load-bearing after the 2026-10-13 ENOSPC deaths): all
  bench/build scratch under `/tmp/invfs-wp620-*`, ≤200M, clean per leg;
  commit after every leg.
- Report format: WP / Files changed / Tests run / Result / Remaining TODOs.
