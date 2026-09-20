# WP-M21 — TASK: legacy deletion (v2 code removal)

## Branch / Worktree
- Branch: `wp/m21-legacy-deletion`
- Worktree: `/tmp/invfs-wp-m21`
- Base: `main` @ `c900f35`

## Context
The programme is v3-only ("no migration, no v2 reader"). WP-M21 deletes the v2 
metadata machinery so the codebase has a single metadata model. This is a 
DELETION task — high risk, no new code.

## Scope (from WP-M21 spec, impl_docs/WP-M21-legacy-deletion.md)

**Delete v2 metadata machinery: invfs_inode_rec, DELT, INO2, flat mapper, 
vol_records.c append/tombstone paths, vol_meta_merge.c, CKP0/reten registry, 
nbuck/dbuck/ibuck mount-scan indexes, vol_inode_compact, migrate-v2 CLI.**

Files (DELETE or adapt):
- `src/core/invarifs.h` — v2 inode record structs, mapper structs
- `src/core/vol_records.c` — v2 record append/tombstone/retire paths
- `src/core/vol_meta_merge.c` — meta mapper merge (delete entire file)
- `src/core/vol_rollback.c` — CKP0/\x01reten registry (keep data-plane functions)
- `src/core/volume.c` / `volume_internal.h` — nbuck/dbuck/ibuck, vol_inode_compact
- `src/cli/migrate-v2.c` — DELETE (remove from CLI_MAINS and Makefile)
- `tools/` — v2-only format tests (test-astv2.sh, etc.)

DATA plane (DO NOT DELETE): vol_ast.c, vol_read.c, vol_write.c, ARC, codecs, 
containers, seals, L2P journal, bitmap, tier.

## Critical Safety Rules

1. **Data plane is off-limits**: Functions like `vol_ast.c`, `vol_read.c`, 
   `vol_write.c`, `arc.c`, `vol_cpack.c`, `vol_tier.c` etc. contain BOTH v2 
   and v3 code. Delete only the metadata-specific paths within them (e.g., 
   `vol_inode_compact`, v2 record format handling). Use `grep` to find callers 
   of v2 structures and delete those call sites only.

2. **Never delete by filename**: Always delete by function/structure within a 
   file that also serves v3 or the data plane.

3. **Let the compiler find dangling references**: After each deletion pass, 
   rebuild (`make -j`). Fix only what the compiler reports.

4. **Test after each major deletion**: Run `make test` after deleting each 
   major structure to catch breakage early.

## Implementation approach

Do this in safe order:

**Pass 1 — Identify v2-only structures and functions:**
```bash
grep -rn "invfs_inode_rec\|DELT\|INO2\|meta_mapper\|nbuck\|dbuck\|ibuck\|vol_inode_compact\|migrate-v2" src/core/*.c src/core/*.h --include="*.c" --include="*.h"
```

**Pass 2 — Delete migrate-v2 (easiest):**
- Remove `migrate-v2` from `CLI_MAINS` in Makefile
- Remove `tools/migrate-v2.c`
- Delete any v2-only test scripts (test-astv2.sh, etc.)

**Pass 3 — Delete v2 record format:**
- `invfs_inode_rec`, `DELT` record type, `INO2` inline data from `invarifs.h`
- Delete v2-specific functions in `vol_records.c`
- Delete `vol_inode_compact` calls

**Pass 4 — Delete mapper/metadata merge:**
- `vol_meta_merge.c` — entire file or metadata-specific portions
- Mapper structs from `invarifs.h`

**Pass 5 — Delete mount-scan indexes:**
- `nbuck`/`dbuck`/`ibuck` from `volume_internal.h`
- The O(N) scan in `volume.c` that builds them (v3 doesn't need this)

**Pass 6 — Delete CKP0/reten registry:**
- Keep functions that the data plane still uses (if any)
- Delete the retention registry logic in `vol_rollback.c`

## Validation

1. `make test` — all unit binaries pass after each deletion pass

2. `grep` audit — no remaining references to:
   ```
   invfs_inode_rec, DELT, INO2, INVFS_MAX_REC_LEN, meta_mapper,
   vol_meta_merge, \x01reten, open_cuts, nbuck, dbuck, ibuck,
   vol_inode_compact, migrate-v2
   ```

3. `INVFS_V3=1 invf-mkfs t.img` → write/read/fold/rollback/sweep → 
   `invf-fsck` clean

4. `make e2e` — full suite passes

## Deliverable format (AGENTS §1.6)

```
WP: wp/m21-legacy-deletion
Files deleted: (list)
Files modified: (list)
Tests run:
  $ make test ... (output)
  $ grep -r "invfs_inode_rec|DELT|meta_mapper|nbuck" src/core --include="*.c" --include="*.h" ... (should be empty)
  $ INVFS_E2E_AGENT=wp-M21-legacy-deletion bash tools/run-e2e.sh tools/test-meta-v3.sh ... (output)
Result: PASS
Remaining TODOs: (list of anything not deletable without v3 replacement)
```

## Key constraint
**DO NOT delete code that the data plane uses.** The data plane (RAW, Shadow, 
codecs, ARC, L2P journal) is v3's foundation and must not be touched.
