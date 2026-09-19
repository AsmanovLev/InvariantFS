# WP-M19 — two-device placement + mirror

**Branch:** `wp/M19-two-device`
**Worktree:** `/tmp/invfs-wp-M19`
**Severity:** MEDIUM (misplacement costs capacity, not bytes; mirroring is correctness)
**Source:** `impl_docs/design-meta-v3.md` §12, §14, §2, §15
**Estimated effort:** large

---

## Scope

Place the v3 tiers on the right device: **base + delta + RAW on dev0** (fast
tier), **Shadow data on dev1** (cold archive), metadata mirrored via the
existing `DEVT` descriptor. Adapts WP25 (`vol_tier.c`); no new mirror
algorithm.

Files:
- `src/core/vol_tier.c` — RAW mirror (`\x01rawm`) and dev0 tier arena
  (`vol_tier.c:8-35`, `vol_tier_migrate`), retargeted for v3.
- `src/core/volume.c` — open/mount with `DEVT` for v3 volumes.
- `src/cli/mkfs.c` — v3 two-device layout (the WP25 form).
- `src/core/invarifs.h` — `DEVT` unchanged; v3 placement constants.
- `src/core/vol_delta.c` / `vol_metabuf.c` — delta/base allocations
  dev0-resident.
- `src/core/vol_fold.c` — fold writes the new base on dev0 (mirrored).

## Why

§14 defines the deployment: one volume over dev0 (SSD: RAW + delta + base)
and dev1 (HDD: Shadow/cold archive). §12 restates "base/metadata on dev0,
mirrored (existing DEVT); Shadow data on dev1". Without this WP the v3 tiers
inherit v2 placement assumptions and the required split is undefined.

## Design

**Placement (frozen):**

| Structure | Device | Mirror |
|---|---|---|
| RT30 root + base pages | dev0 | yes (`DEVT`/`\x01rawm`) |
| delta log segments | dev0 | yes |
| RAW data | dev0 | yes, per WP25 rules |
| Shadow / cold data | dev1 | canonical |
| recipe blobs | dev0 | yes |

The design says "mirrored (existing `DEVT`)" — reuse the WP25 mirror,
including its crash ordering (data durable before the mirror map entry) and
its dev1-failure policy (`vol_tier.c:14-15`). Do not invent a second format.

**Fold interaction:** fold writes COW pages through `mbuf_write` (dev0,
mirrored) and publishes RT30 there; readers never touch dev1 for metadata.
The sweep migration pass (`vol_tier_migrate`) keeps moving cold DATA, not
metadata.

**Limits:** WP25's v1 limits (max 65535 mirrored raw keys,
`vol_tier.c:35`) still apply; report them rather than silently degrading.

## Validation

1. `make test` — unit case that metadata allocations resolve to dev0 and the
   mirror map entry precedes the data reference.
2. `INVFS_V3=1 invf-mkfs` on a two-device volume → write/read, fold, sweep,
   remount; content bit-exact; metadata on dev0; Shadow on dev1.
3. Simulated dev1 failure: reads fall back to the dev0 mirror for mirrored
   data; metadata unaffected.
4. `bash tools/run-e2e.sh tools/test-multidev.sh` — adapted to v3;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Multi-host / network / SAN filesystems.
- A new mirror or refcount scheme; reuse `DEVT`/WP25.
- DATA codecs/transcodes and seal algorithm.
- Delta/base on-disk layout (WP-M1/M2/M10).
- Concurrency (WP-M20).

## Coordination notes

- Subagent ID: `wp-M19-two-device`; `INVFS_E2E_AGENT=wp-M19-two-device`.
- E2E gates: `test-multidev.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M1/M2** (root/base), **WP-M10** (delta residency),
  **WP-M14** (fold publishes on dev0), **WP-M18** (migration pass).
- Blocks: WP-M22's device-matrix e2e legs.
- The design does not specify a dev0-failure fallback; existing WP25
  per-device latch behavior applies — state it rather than inventing one.
