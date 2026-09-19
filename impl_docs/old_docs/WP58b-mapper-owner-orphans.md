# WP58b — mapper-volume owner-record orphans + append-after-rollback visibility

**Branch:** `wp/58b-mapper-owner-orphans` (or land directly on main — see Status)
**Worktree:** `/tmp/invfs-wp58b`
**Severity:** HIGH (silent space leak / false fsck damage; data stays bit-exact)
**Source:** `INCIDENTS.md` candidate; found while clearing the pre-existing
`test-rollback.sh` / `test-dedupe.sh` red suites.
**Estimated effort:** Bug A already fixed (committed); Bug B ~3-6 h.

---

## Status snapshot (read this first)

Two distinct mapper-volume (WP30, `format_version>=1`, `met0_present &&
meta_mapper`) bugs. **Bug A is fixed and committed; Bug B is open.**

- **Bug A — superseded owner records resurrect and orphan swept blocks.**
  Committed on `main` as **`ecf66ff`**
  (“sweep/rollback: fix mapper-volume owner-record orphan + missing
  accounting”), files `src/core/vol_records.c`, `src/core/vol_fsck.c`,
  `src/core/vol_rollback.c` (+252/-9). Not yet pushed to origin.
  Verified: `orphans=0 missing=0` through 4 sweeps + realize, files
  bit-exact 5/5; `make test` PASS (4467/86/169/22, 0 failures). Baseline
  `main` (`d522cdd`) showed `orphans=64 missing=66` on the same 5-file
  volume, and `test-rollback.sh` failed even earlier (at `[C] --realize`).

- **Bug B — appends after a rollback are invisible on a mapper volume.**
  **FIXED** (working-tree commit, see “Bug B” below). Root cause was the
  extent-relative append cursor `met0.active_offset` not being rebased on
  rollback. `tools/test-rollback.sh` is now fully **PASS** (A1–G, E2/E3
  included).

---

## Background: how records live on a mapper volume

`format_version=1` volumes (`VOLF_META2|VOLF_ASTV2`) keep inode records in
**dynamic metadata extents** tracked by the Mapper table, NOT in a linear
inode area.

- File records are appended through `vol_append_slot`
  (`src/core/volume.c:4147`) → `meta_get_append_pos`
  (`src/core/vol_meta_merge.c:641`) into the **shared active extent**
  (`v->met0.active_extent` / `active_offset`).
- **Owner-class records** (`\x01reten`, `\x01tzb`, `\x01tier*`,
  `\x01rawm`, `\x01parity*`) are appended through `vol_append_owner_slot`
  (`src/core/volume.c:4193`) → `meta_get_owner_append_pos`
  (`src/core/vol_meta_merge.c:803`) into a **dedicated per-owner extent**,
  rewritten **in place** at the extent's offset 0 (`tz_owner_write`,
  `src/core/vol_textzone.c:161`; tier at `src/core/vol_tier.c`).
- `vol_records_walk_ex` (`src/core/volume.c:4007`) walks mapper slots in
  **slot-index order** (`ei` ascending), then by increasing pba within an
  extent. So walk order is NOT append order across extents.
- Each record's prefix is 36 bytes (`INVFS_REC_HDR_LEN`), name is a
  variable FAM (`INVFS_NAME_CAP=256`), AST lives at
  `invfs_rec_body()`; trailing CRC32C. No `free_meta_extent` exists —
  extents are never returned (only `pba==0` slots are reusable;
  `extend_meta_extent` grows in place).

---

## Bug A — superseded owner records resurrect (FIXED, `ecf66ff`)

**Files:** `src/core/vol_records.c`, `src/core/vol_fsck.c`,
`src/core/vol_rollback.c`

### Failure mode

A tombstone deleting an owner record was appended to the **shared**
file-record extent, which is visited **before** the owner's dedicated
extent (higher slot). `scanset_delt` was applied before `scanset_inod`, so
the id-kill hit nothing and the owner INOD **re-added itself afterwards**.
The resurrected owner's AST pinned blocks the sweep had already freed for
real → `missing` (used && bitmap-free); after realize the reverse →
`orphans` (bitmap-set && unreferenced).

Proven walk order (debug `[scan]` output) on the 5-file repro:

```
140946070 INOD id=11 fs=0    \x01reten placeholder (vol_create_file)
140946133 DELT id=11 fs=0    sweep2 ret_registry_delete id-kill (shared extent)
141340672 INOD id=11 fs=66   real registry AST (owner extent, LATER)
141340799 DELT id=11 fs=...  v2 position-kill from tz_owner_write
```

### Fixes landed

1. `vol_records.c`: `vol_delete_owner_overwrite(v, name, inode_id, pos)`
   writes a **v2 position-kill tombstone in place at the owner's own
   position** (owner extents are single-record, in-place). `vol_retire_inode`
   routes `name[0]==0x01 && mapper` deletes through it and returns early.
2. `vol_fsck.c`: in the bitmap rebuild, mark **every live mapper extent**
   used (`v->meta_mapper_n` slots, `invfs_meta_ext_pba`/`invfs_meta_ext_size`).
   Prevents dead owner extents from reading as orphans.
3. `vol_rollback.c`:
   - `vol_ckp_begin` / `vol_ckp_realize` / final rollback CKP0 clear: free
     the checkpoint journal **staging run** with the descriptor
     (`ckp_free_direct`), fixing staging leaks.
   - `ret_registry_delete`: free each shard's ranges from its **own AST**
     (`tz_owner_load` + `vol_free_blocks`), falling back to the L2P scan.
   - Phase-1b in `vol_rollback`: `ret_registry_delete` the `\x01reten*`
     registry **and** `rollback_purge_owners()` (new) which collects all
     live `\x01*` names via `vol_records_walk` and `vol_delete_file`s them,
     before the phase-2 rebuild.

### Reproduce / verify Bug A

```bash
# /tmp/opencode/orph-repro.sh — mkfs 0.5 GiB, import 5x50000-byte random
# files, fsck after fresh/sweep1-4/realize, then bit-exact check.
bash /tmp/opencode/orph-repro.sh
# EXPECT (fixed): orphans=0 missing=0 on every line; bit-exact ok=5 bad=0
# On broken baseline (d522cdd): sweep2+ orphans=64 missing=66
```

Gotcha: `/dev/shm` here is devtmpfs and `blkio_looks_like_device` treats
`/dev/*` as a raw device → use `/tmp/opencode/*.img`, or relative paths
after `cd /dev/shm` (as `tools/test-rollback.sh` does).

---

## Bug B — appends after a rollback are invisible on a mapper volume (FIXED)

**Symptoms:** `tools/test-rollback.sh` E2 failed with
`'a.c.v2' not found` after `INVFS_ROLLBACK_ABORT_AT=rebuilt` + re-run of
`invf-rollback`. Earlier legs A1–D and E1 passed.

**Minimal repro (mapper volume):**

```bash
bin/invf-mkfs m.img 0.5
bin/invf-cp m.img a.c a.c          # inode record 124 B at extent0 off 0
bin/invf-cp m.img b.bin b.bin      # ... off 124..218
bin/invf-sweep m.img               # rewrites into extent0, grows extent
                                   # table, active_offset 218 -> 1092
bin/invf-rollback m.img
bin/invf-cp m.img a.c new.txt      # rc=0, "stored ... as inode 3"
bin/invf-ls m.img                  # new.txt MISSING
```

**Root cause (proven with `-DINVFS_DEBUG_META_EXTENTS`):** on a mapper
volume the file-record append cursor is extent-relative —
`v->met0.active_extent` / `v->met0.active_offset` — and the sweep moves
it forward (it rewrites records into the active extent and may create a
new extent). `vol_rollback`'s phase 1 restored `v->inode_area_pos` to the
checkpoint's `iapos` and **zeroed the dead tail `[iapos, old_pos)`**, but
left `met0.active_offset` at the sweep's end. Debug trace after rollback
of the repro:

```
[flush.met0] write ext_count=2 active_extent=0 active_offset=1092
[mga] in: extent_idx=0 off=1092 ...      <- stale sweep cursor
```

The next `vol_append_slot` therefore handed out `extent0_pba*BS + 1092`,
which sits **past** the zeroed region (pre-sweep records end at
offset 218). The record stream became
`[a.c][b.bin] 0x00...0x00 [new.txt]`; `vol_records_walk_ex` /
`vol_inode_next` stop at the first non-magic bytes, so the new record
was written but never found on reopen (verified: `invf-cp` is happy, but
this walk stops at 218 and `invf-ls` shows only the pre-sweep names).

**Fix** (`src/core/vol_rollback.c`, phase 1, right after
`v->inode_area_pos = iapos`): rebase the extent-relative cursor onto the
restored pre-sweep append pointer.

```c
if (v->met0_present && v->meta_mapper &&
    v->met0.active_extent < (uint64_t)v->meta_mapper_n) {
    uint64_t e = meta_mapper_get(v, (size_t)v->met0.active_extent);
    if (e) {
        uint64_t base = invfs_meta_ext_pba(e) * (uint64_t)INVFS_BLOCK_SIZE;
        v->met0.active_offset = iapos > base ? iapos - base : 0;
    }
}
```

**Verification:**

- Minimal repro now shows `new.txt` in `invf-ls`, bit-exact.
- `bash tools/run-e2e.sh tools/test-rollback.sh` → **ROLLBACK E2E: PASS**
  (A1–G; E2/E3 crash legs green).
- Regression: `bash /tmp/opencode/orph-repro.sh` still
  `orphans=0 missing=0`, bit-exact 5/5; `make test` PASS
  (4467/86/169/22).
- `tools/test-mapper-crash.sh` → 21 passed, 3 failed; the 3 failures are
  **pre-existing** (reproduced with the fix stashed on `ecf66ff`: leg3/leg4
  report “1 descending step”, leg1 identical). Not caused by this change.

**Why it mattered:** any write after `invf-rollback` (or after the crash
recovery path that shares this code) on a mapper volume was silently lost
from the namespace — a correctness bug, not just a leak.

---

## Validation

1. `make test` — must pass (currently PASS: 4467/86/169/22).
2. `bash /tmp/opencode/orph-repro.sh` — Bug A regression (recreate if
   `/tmp/opencode` was cleaned).
3. `bash tools/run-e2e.sh tools/test-rollback.sh` — Bug A legs pass
   (A1–D, E1); Bug B must turn E2 green.
4. `bash tools/run-e2e.sh tools/test-seal.sh` — first seal writes 8/9
   stripes (separate pre-existing seal issue; see `INCIDENTS.md`).

---

## Out of scope (do NOT touch)

- The `--seal` first-run stripe count (8/9) mismatch — separate WP.
- `invf-fsck -f` before first boot breaking runtime FUSE lookups
  (documented in `docs/VOID-INSTALL.md` / `docs/ARCH-INSTALL.md`).
- The `src/cli/migrate-v2.c` v1-source handling (WP58a note).
- Compression/codec work (WP58 deferred).

---

## Coordination notes

- Subagent ID: `wp58b-mapper-owner-orphans`; pass via
  `INVFS_E2E_AGENT=wp58b-mapper-owner-orphans`.
- E2E gates: `tools/test-rollback.sh`, `tools/test-seal.sh`,
  `tools/test-dedupe.sh`, `tools/test-mapper-crash.sh`,
  `tools/test-meta-extent-walk.sh`, `tools/test-fixture-bigvol.sh`.
- Dependencies: none (Bug A already on `main` at `ecf66ff`).
- Bug A commit for reference: `ecf66ff`.

---

## Key file:line index

- `vol_records_walk_ex` mapper walk: `src/core/volume.c:4007`
- `vol_append_slot` / `vol_append_owner_slot`: `src/core/volume.c:4147`, `:4193`
- `meta_get_append_pos` / `meta_get_owner_append_pos`:
  `src/core/vol_meta_merge.c:641`, `:803`
- `vol_retire_inode` + `vol_delete_owner_overwrite`:
  `src/core/vol_records.c` (owner delete path, ~`:700`)
- `tz_owner_write` (owner in-place combo INOD+v2 kill):
  `src/core/vol_textzone.c:161`
- fsck rebuild + mapper marking: `src/core/vol_fsck.c:~750-800`
- rollback phases: `src/core/vol_rollback.c` (`vol_ckp_begin` ~`:176`,
  `ret_registry_delete` ~`:502`, `rollback_purge_owners` ~`:496`,
  `vol_rollback` ~`:880-1125`)
