# HANDOFF — removal of the 32-byte AST wire format (migration to 40 B)

The session is long; this is a checkpoint so the next agent can start with a
clean context and not re-discover what has already been found.

## git / CI state

- remote: `origin git@github.com:AsmanovLev/InvariantFS.git`, branch `main`.
- Already PUSHED (3 cleanup commits):
  - `7029f6f` format: unified gate + cleanup of version markers
  - `019743a` chore: removed v3 mentions from prose and names
  - `b33663e` chore: renamed *_v3 implementations to *_impl
- In the working tree (DO NOT commit): `tools/test-png.sh` (broken symlink,
  git mode 120000 → file), `src/codecs/ChangeLog.md` (autocrlf CRLF noise).
- Build environment: WSL2 + Void Linux, gcc 14.2.1, make, `ctags` (universal-ctags
  6.2.1, installed). `make -j12` builds `all`; test binaries are NOT part of
  `all` — build them explicitly: `make bin/invf-v2_open_test bin/invf-entry_v3_test`
  or `make test-bins`.

## Already done — do not touch

- Unified format gate: `src/core/vol_format.{h,c}` — `invfs_format_accept(sb,
  path, why, why_sz)` returns 0 / `-12` (no VOLF_META) / `-13` (generation
  mismatch) / `-14`. All 6 openers (vol_open primary+degraded, verify,
  resize, vnode) go through it.
- `INVFS_FORMAT_VERSION = 1` (was 0); `VOLF_META2`/`VOLF_ASTV2` retired (do not
  reuse the values 0x02/0x08); the decorative `INVFS_VERSION` was removed.
- Prose: `metadata-v3/Meta-v3` → `metadata store`; `*_v3` identifiers →
  `*_impl` (implementations), unsuffixed wrappers/dispatchers remain public API.

## Remaining: OPTION C — remove the 32 B wire format (the task of this handoff)

Goal: make the 40-byte recipe entry the only one, remove v1/v2 (32 B),
rename `*_v3` → base. This is **format-breaking** (current v3 volumes write
32 B — they will stop being readable), the user has agreed (reinstall).

### Key facts (already established, do not re-investigate)

1. `invfs_ast_block_entry` (32 B) — **the current default**, NOT dead. Fields:
   `file_offset, length, zone:2, algo:6, block_id:24, block_offset, pba`.
2. `invfs_ast_block_entry_v3` (40 B) — the new stride, fields:
   `file_offset, length, zone:6, algo:10, block_id:32, rsv:16, block_offset,
   crc32c, pba`. Enabled only by `INVFS_RECIPE_FORCE_V3=1` (test force).
3. Format selection in `src/core/vol_ast.c:67-93`:
   ```c
   const char *f3 = getenv("INVFS_RECIPE_FORCE_V3");
   ...
   hlen = f3 ? invfs_ast_hdr_write_v3(...) : invfs_ast_hdr_write(...);
   ```
4. Headers: `invfs_ast_recipe_header_v1` (16 B, ver 1),
   `invfs_ast_recipe_header_v2` (24 B, ver 2), `invfs_ast_recipe_header`
   (24 B, ver 3). v2 and v3 are **identical in layout**; only `version` differs.
5. Versions/lengths: `INVFS_AST_VERSION_V1=1, V2=2, V3=3`;
   `INVFS_AST_HDR_V1_LEN=16, INVFS_AST_HDR_V2_LEN=24, INVFS_AST_HDR_LEN=24`
   (HDR_LEN has already been renamed from the former V3_LEN).
6. `invfs_ast_entry_stride(version)`: V1/V2→32, V3→40, otherwise 0.
7. The 40 B on-disk form is assembled **arithmetically** via
   `invfs_ast_entry_pack/unpack/verify` (invarifs.h ~1680-1760), NOT memcpy
   (bitfield layout stability). `unpack` takes `invfs_ast_block_entry*`
   (currently 32 B in-memory) and **rejects** `zone>3 || algo>63 ||
   block_id>0xFFFFFF` — these narrow checks must be removed when widening.
8. `invfs_ast_block_entry` (32 B) is mentioned ~163 times (the in-memory type
   throughout vol_write/vol_read/vol_dedupe/vol_records/...).
9. `invfs_ast_hdr_parse` (invarifs.h:1233) — the unified reader, already handles
   all three versions (V1→16 B, V2→24 B, V3→24 B). V2 and V3 check the same caps
   (`MAX_FILE_SIZE`, `MAX_SEGMENTS_V2`). With v1/v2 removed, one branch remains
   (version==3), and `hdr_len` is always `INVFS_AST_HDR_LEN`.

### Plan (in one pass)

1. In-memory struct: rename the body of `invfs_ast_block_entry_v3` (40 B) to
   `invfs_ast_block_entry`; delete the old 32 B struct. Verify that all 163
   sites compile (the `zone/algo/block_id` fields became wider — names unchanged).
2. `invfs_ast_entry_unpack`: remove the checks `zone>3/algo>63/block_id>0xFFFFFF`
   (in-memory is now wide).
3. `invfs_ast_hdr_write_v3` → `invfs_ast_hdr_write`; delete the old v1/v2
   version. In `vol_ast.c` remove `getenv(FORCE_V3)` and the branch, always 40 B.
4. `invfs_ast_entry_stride` → always 40 (remove dispatch), or remove the function.
5. Reads in `vol_ast.c` (~164, ~262, ~268): remove dispatch on
   `hdr->version` / `version == INVFS_AST_VERSION_V3` — always 40 B.
6. Delete: `invfs_ast_recipe_header_v1/v2`, `INVFS_AST_VERSION_V1/V2`,
   `INVFS_AST_HDR_V1_LEN/V2_LEN`, `MAX_SEGMENTS` (0xFFFF, v1 u16 boundary),
   `INVFS_RECIPE_FORCE_V3`.
7. `INVFS_AST_VERSION_V3` → `INVFS_AST_VERSION` (value 3 stays).

### IMPORTANT: do NOT delete `MAX_SEGMENTS_V2` (correction to an earlier version of the notes)

`MAX_SEGMENTS_V2 = (1u << 24)` (invarifs.h:1162) — this is the **currently active
cap** on the number of recipe entries, not a legacy one. Used in ~24 places:
`vol_write.c` (834, 913, 1058), `vol_ast.c` (83, 98, 102, 180, 190),
`fuse_fs.c:2442`, `recipe_tree_test.c:3`, `invarifs.h` (137, 1153, 1159, 1231,
1270, 1291, 1307, 1351, 1584) — including `INVFS_RECIPE_BLOB_MAX`.
The `_V2` suffix is historical (block_id became 24-bit back in v2).
Action: **rename** to `MAX_RECIPE_ENTRIES` (or `MAX_SEGMENTS_BLOCKID`),
do NOT delete. What should be deleted is `MAX_SEGMENTS` (0xFFFF) — it remains
only in 2 places (invarifs.h:1161 definition and :1596, where it is already
questionable to use together with the 40 B stride).
8. Update `src/cli/entry_v3_test.c` (and `tools/test-entry-v3.sh`): remove the
   FORCE_V3/32B legs, keep 40 B; rename the file `entry_v3_test.c` →
   `entry_test.c` (Makefile `CLI_MAINS` + `tools/test-entry-v3.sh`) — the file
   rename can be postponed if it drags in an extra git mv.

### Verification after the pass

```
make -j12
make bin/invf-v2_open_test bin/invf-entry_v3_test
bash tools/test-meta.sh
bash tools/test-meta-dirent.sh
./bin/invf-v2_open_test
./bin/invf-entry_v3_test
```
Expected: everything green; `invf-mkfs` + `invf-cp` write a 40 B recipe.

### Related, NOT part of this task

- `TZV3` (magic 0x33565a54 in batch_owner_test.c) — on-disk magic, do not touch.
- Roadmap: the item “Metadata Sharding & Partitioning (XFS-style AG & dcache
  hierarchy, v0.8 target)” **has been removed from `README.md`** — the feature
  is shipping in v0.7, so it has no place in the distant roadmap. The AG
  wording remains in `impl_docs/design-meta.md` §9 and §14 (where it is accurate).
- Scope boundary for v0.7 (so as not to reopen the `design-meta.md:187`
  debate): shard the **RAW/delta append arenas (AG)** and the **FUSE mirror of
  names + locks** (per-directory buckets instead of the flat `g_entries` and
  `sync_parents_locked`), but do **NOT partition the on-disk namespace** —
  otherwise rename/link/fold become cross-domain.
- Gentoo stand `192.168.0.2`: the Portage rsync is stuck (~58/184 categories,
  invf-fuse is squeezing 1 core). DO NOT touch — there will be a reinstall on
  the new architecture. SSH: `ssh -i ~/.ssh/id_ed25519_invfs root@192.168.0.2`.
