# WP78 — v3 sweep transcode/batch parity

**Branch:** `wp/78-v3-sweep-parity`
**Worktree:** `/home/user/invfs-wp78`
**Severity:** HIGH (headline v0.5.0 features silently inactive on the default format)
**Source:** `make e2e` recon (Cause B, 7 suites)
**Estimated effort:** L

---

## Scope

`vol_sweep_one()` (`src/core/vol_sweep.c:1255`) short-circuits every
Meta-v3 volume to `vol_sweep_one_v3()` (`:912`). That function implements only
container-pack decomposition + generic ZSTD recompression. Everything below
`:1270` is unreachable v2 record surgery, and because `invf-mkfs` defaults to
v3 (`mkfs.c:171`) and `vol_open` refuses v2 (`volume.c:1358`), the transcode
lanes are effectively dead code.

Restore parity on v3 for the lanes that are currently missing.

---

## Bug A — v3 sweep never defers text/binary batches

`vol_sweep_one_v3` never calls `tz_defer()` / `bz_defer()`, so:
- text PPMd cross-file batching is gone → `test-textzone`: `text->PPMd lines: 0`
  (expected ≥9); `test-heat` (batch members drive heat promotion);
  `test-writepath` (`swept.txt not TEXT post-sweep`).
- binary ZSTD+BCJ batching is gone → `test-binbatch`:
  `binary->ZSTD lines: 0` (expected 293); `test-conbatch`
  (`bins.tar parts not batched`).

**Fix:** route v3 candidates through the existing accumulators
(`tz_defer`/`bz_defer`, sealed by `vol_tz_flush`) exactly as the v2 path does,
returning rc 9/10 so `tools/invf-sweep.c` prints the same lines.

---

## Bug B — non-container codecpacks and transcode lanes absent on v3

`vol_sweep_one_v3` runs only `is_container` packs. Missing: non-container
codecpacks (e.g. the identity pack used by `test-helper-isolation`:
`contained home run did not transcode`), JXL/JPEG (rc 7),
GZ/TAR/PNG/FLAC (GZR/TARR/PNGR/FLACR), EXER carve (rc 11), and
`test-qcow2` (`diska.qcow2 not decomposed` — verify whether this is the
container path or the plugin path before changing anything).

**Fix:** port the relevant branches from `vol_sweep_one` (`:1400-1660`) into
the v3 path, preserving the bit-exactness guards (decode+memcmp / rebuild).
Prefer refactoring the shared per-file transcode decision into a helper
callable from both paths over duplicating the code.

---

## Validation

1. `make test` — must pass.
2. `INVFS_E2E_AGENT=wp78-sweep-parity bash tools/run-e2e.sh tools/test-textzone.sh`
   — PASS (≥9 PPMd lines, GC reclaims, bit-exact).
3. `tools/test-binbatch.sh`, `tools/test-conbatch.sh`,
   `tools/test-helper-isolation.sh`, `tools/test-writepath.sh` — PASS.
4. `tools/test-heat.sh` — PASS.
5. `tools/test-qcow2.sh` — investigate and report; PASS or document why not.
6. `tools/test-jxl.sh`, `tools/test-pngflac.sh`, `tools/test-exercarve.sh`,
   `tools/test-containerpack.sh` — these need WP74's link fix; if WP74 has
   merged, run them; otherwise record them as blocked.
7. `tools/test-meta-v3.sh` + `test-meta-v3-recipe.sh` — stay green.
8. Bit-exactness spot check: `invf-verify --deep` clean after sweep.

---

## Out of scope (do NOT touch)

- `vol_flush` bitmap persistence (WP75).
- `invf-verify` (WP76).
- SPT0 capture (WP77).
- Test-helper link lists (WP74).
- The v2 code path itself — do not delete it in this WP (deletion is a
  separate cleanup).

---

## Coordination notes

- Subagent ID: `wp78-sweep-parity`
- E2E gates: `test-textzone.sh`, `test-binbatch.sh`, `test-conbatch.sh`,
  `test-heat.sh`, `test-helper-isolation.sh`, `test-writepath.sh`,
  `test-qcow2.sh`, `test-meta-v3.sh`.
- Dependencies: full verification of the codecpack suites needs WP74 merged.
- This is the largest WP; if the shared-helper refactor balloons, stop and
  report rather than duplicating 260 lines.
