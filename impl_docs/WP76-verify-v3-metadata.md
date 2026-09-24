# WP76 — invf-verify: v3-aware metadata-region allocation check

**Branch:** `wp/76-verify-v3-metadata`
**Worktree:** `/home/user/invfs-wp76`
**Severity:** MEDIUM (false-positive integrity alarm; trains operators to ignore verify)
**Source:** `test-multidev` recon (subagent `f1a16943`)
**Estimated effort:** small

---

## Scope

`invf-verify` asserts the **entire** metadata region is allocated. That is a
v2-era invariant. On a two-device Meta-v3 volume, mkfs deliberately leaves the
metadata-zone tail free as the dev0-resident v3 base-page pool (WP-M19), so
verify prints a spurious `block N in metadata region is free (should be
allocated)` and exits 1.

---

## Bug A — metadata-region check is not format-aware

**File:** `src/cli/verify.c:300-313`
**Evidence:** the loop at `:301-307` reads the on-disk bitmap and requires
`[0, sb.metadata_zone_start + sb.metadata_zone_blocks)` all allocated; a
second check at `:309-313` uses the same bound. `git blame` dates this to
2026-08-24 (pre-v3). On v3 two-device, `mkfs.c:588-598` (WP-M19, commit
`555f2fc`) clears `[v3_root_pba + 2, metadata_zone_start + metadata_blocks)`;
`mb_alloc_meta_zone` (`vol_metabuf.c:193-224`) hands those blocks out as base
pages. `invf-fsck` does not share the bug: `vol_fsck_scan` dispatches to
`fsck_v3_scan` (`vol_fsck.c:457-458`) before the v2 rule.

Single-device v3 volumes do not trigger it (the base pool lives in shadow);
two-device empty volumes already trigger it at block 38. `--deep` content
verification is clean (4/4), fsck is OK.

**Fix:** make the reserved bound format-aware.

```c
uint64_t meta_reserved_end = sb.metadata_zone_start + sb.metadata_zone_blocks;
if ((sb.vol_flags & VOLF_V3) && sb.meta_mapper_pba && sb.meta_mapper_blocks) {
    uint64_t root_end = sb.meta_mapper_pba + sb.meta_mapper_blocks
                      + INVFS_MBUF_BOOT_PAGES;
    if (root_end < meta_reserved_end) meta_reserved_end = root_end;
}
/* use meta_reserved_end in BOTH the :301-307 loop and the :309-313 check */
```

Add `#include "vol_metabuf.h"` for `INVFS_MBUF_BOOT_PAGES` (or use literal 2
with a comment). Keep the whole-region check for non-V3, and fall back to the
full region when `meta_mapper_pba`/`meta_mapper_blocks` are 0.

**Do NOT** "fix" mkfs by marking the tail allocated — that defeats WP-M19.

---

## Validation

1. `make test` — must pass.
2. `INVFS_E2E_AGENT=wp76-verify bash tools/run-e2e.sh tools/test-multidev.sh`
   — must PASS.
3. Two-device empty volume: `invf-mkfs d0.img 0.125 d1.img 0.25` then
   `INVFS_DEV1=d1.img invf-verify d0.img --deep` — exit 0.
4. Single-device v3: `invf-mkfs s0.img 0.0625` → `invf-verify s0.img --deep`
   — stays exit 0.
5. A genuinely corrupted bitmap (clear an in-use prefix block) must still be
   flagged — prove the check is not simply disabled.
6. `bash tools/run-e2e.sh tools/test-meta-v3-multidev.sh` — stays green.

---

## Out of scope (do NOT touch)

- `src/cli/mkfs.c` layout.
- `vol_fsck.c` v3 scan.
- Bitmap flush (WP75), SPT0 (WP77), sweep parity (WP78), link lists (WP74).

---

## Coordination notes

- Subagent ID: `wp76-verify`
- E2E gates: `test-multidev.sh`, `test-meta-v3-multidev.sh`.
- Dependencies: none.
