# WP75 — v3: persist the dirty bitmap on vol_flush

**Branch:** `wp/75-v3-bitmap-flush`
**Worktree:** `/home/user/invfs-wp75`
**Severity:** HIGH (silent block-accounting leak; v3 durability hazard)
**Source:** `test-dedupe` recon (subagent `10307a22`)
**Estimated effort:** hours

---

## Scope

On Meta-v3 volumes the dirty block bitmap is never flushed by `vol_flush()`,
so allocations/frees that happen after the last `v3_publish()` are lost at
close. Fix the persistence, surface free-block accounting in v3 `invf-fsck`,
and correct the `test-dedupe.sh` assertion.

---

## Bug A — `vol_flush()` drops the dirty v3 bitmap

**File:** `src/core/volume.c` (`vol_flush`, ~:2388-2391)
**Evidence:** `vol_flush` early-returns `if (v->sb.vol_flags & VOLF_V3)
return 0;`. The only v3 bitmap writer is static `v3_bitmap_flush()` called
from `v3_publish()` (`src/core/vol_btree.c:1334,1386`). The sweep's last
bitmap-mutating pass is dedupe (`tools/invf-sweep.c:1031-1034`); its frees go
through `vol_free_blocks()` (`src/core/vol_dedupe.c:267-272`) *after* the last
publish, so they stay dirty in RAM and are dropped at `vol_close()`
(`volume.c:1983`). `--realize` cannot recover them (WP-M21 retired the
checkpoint stubs in `vol_rollback.c:65-91`).

Broader impact: any allocation after the last publish (e.g. a fresh delta
segment) is marked free on disk, so a reopen can re-hand those blocks.

**Fix:**
1. `src/core/vol_btree.c`: rename `static int v3_bitmap_flush(...)` →
   `int vol_v3_bitmap_flush(...)`; update the call at `:1386`.
2. `src/core/volume_internal.h`: declare `int vol_v3_bitmap_flush(invfs_volume *v);`
   (after `invfs_volume` is defined).
3. `src/core/volume.c` `vol_flush`: replace the early return with
   `if (v->sb.vol_flags & VOLF_V3) return vol_v3_bitmap_flush(v);`.
4. Add a `vmux_barrier` after the write for power-loss ordering, mirroring
   `v3_publish`.

Only the dirty range is written (idempotent, cheap). No format change.

---

## Bug B — v3 `invf-fsck` prints no `free blocks:` line

**File:** `src/cli/fsck.c`
The v3 report branch returns at `:108-109`, before the v2 print at
`:200-201`, so `tools/test-dedupe.sh:40` parses an empty value.

**Fix:** in the v3 branch, immediately after the `cycles/shared` printf
(`:101-102`) and before `if (rep.v3_reachable_free)` (`:103`), add:

```c
printf("  free blocks:  %llu\n",
       (unsigned long long)vol_free_blocks_cached(v));
```

(Identical expression to the v2 branch; equals `vol_count_free(v)` on a fresh
open.)

---

## Bug C — `test-dedupe.sh` assertion is not valid on v3

`FREE0 + FREED == FREE1` fails because the sweep also allocates metadata
(recipe/base/delta pages) that is not freed. Measured with the fix:
`FREE0=120596, FREED=57, FREE1=120647` → delta 51 (off by 6).

**Fix (preferred):** compare against a no-dedupe control image with the same
geometry (make the duplicate files distinct), i.e.
`(FREE1_dup - FREE0_dup) - (FREE1_ctl - FREE0_ctl) == FREED` — exact.
**Fallback:** relax `:89-90` with a documented small-overhead bound and a
comment explaining the metadata allocations.

---

## Validation

1. `make test` — must pass.
2. `INVFS_E2E_AGENT=wp75-bitmap-flush bash tools/run-e2e.sh tools/test-dedupe.sh`
   — must PASS.
3. Independent raw-bitmap check: count free bits over
   `[metadata_zone_start, +ceil(total/8/4096))` before/after sweep; the
   dedupe delta must equal `freed N` exactly.
4. Control experiment (no duplicates): sweep must not change free blocks
   beyond the documented metadata overhead.
5. `bash tools/run-e2e.sh tools/test-meta-v3.sh` and
   `tools/test-meta-v3-fsck.sh` — must stay green (regression guard).
6. Reopen the image and confirm the bitmap matches RAM (`invf-fsck` free
   blocks stable across reopen).

---

## Out of scope (do NOT touch)

- SPT0/checkpoint capture (WP77).
- `invf-verify` metadata-region check (WP76).
- Test-helper link lists (WP74).
- Sweep transcode/batch parity (WP78).

---

## Coordination notes

- Subagent ID: `wp75-bitmap-flush`
- E2E gates: `test-dedupe.sh`, `test-meta-v3.sh`, `test-meta-v3-fsck.sh`.
- Dependencies: none. If WP74 has not merged, `test-dedupe.sh` still runs
  (it does not compile a helper).
