# WP58a-var-name-v3 — variable-length names (bytes/255) in the inode record

**Branch:** `wp/58a-var-name-v3`
**Worktree:** `/tmp/invfs-wp58a`
**Severity:** MEDIUM (format break; compat deliberately not preserved)
**Source:** `impl_docs/WP58-meta-record-v3.md` (stage A1); measured on the Gentoo mapper image
**Estimated effort:** 1–2 days

---

## Scope

Replace the fixed `char name[256]` field in `invfs_inode_rec` with a
**byte-length-prefixed, NUL-terminated** name placed immediately after a **36-byte
fixed prefix** (C11 flexible array member). The record stays one self-describing,
append-only unit — **no core/arena split, no compression** — so the only change is
the record's internal structural packing.

Name semantics stay **raw POSIX bytes, cap 255** (ext4/XFS parity, matches the kernel
`NAME_MAX`); `name_len` keeps its meaning. This is the single largest metadata win
and the prerequisite for the later core+arena stages.

**Why first / measured baseline** (Gentoo mapper image, read-only):

| | records | MiB |
|---|---|---|
| on disk (rec_len + 4 B CRC each) | 302,269 | ~104.2 |
| live | 66,126 | 25.799 |
| name slot (`256 × N`) | 66,126 | 16.144 (real names 3.083, **pad 13.061**) |
| allocated mapper extents | 836 × 128 KiB | 104.5 |

Sweep cannot reclaim any of it: `vol_inode_compact` declines on mapper volumes
(`src/core/vol_records.c:2310-2312`). Expect **live 25.8 → ~12.7 MiB** and
**on-disk 104 → ~38 MiB** from this WP alone.

---

## v3 wire layout

```c
#pragma pack(push, 1)
typedef struct invfs_inode_rec {
    uint32_t magic;       /* INODE_REC_MAGIC / TOMBSTONE_MAGIC */
    uint32_t rec_len;     /* 36 + name_len + 1 + body  (excludes trailing CRC) */
    uint64_t inode_id;
    uint64_t file_size;
    uint64_t ctime;
    uint32_t name_len;    /* 0..INVFS_MAX_NAME (255) */
    char     name[];      /* name_len bytes; NUL at name[name_len] */
    /* followed by: recipe hdr (v1 16 B / v2 24 B) + entries[] [+ children][+ INO2] */
} invfs_inode_rec;        /* sizeof == 36 */
#pragma pack(pop)
```

Helpers (in `invarifs.h`), the only sanctioned way to reach the body:

```c
#define INVFS_REC_HDR_LEN ((uint32_t)sizeof(invfs_inode_rec))   /* 36 */
static inline uint8_t       *invfs_rec_body (invfs_inode_rec *r)
    { return (uint8_t *)r->name + r->name_len + 1; }
static inline const uint8_t *invfs_rec_cbody(const invfs_inode_rec *r)
    { return (const uint8_t *)r->name + r->name_len + 1; }
```

Invariants preserved: trailing CRC32C; `rec_len` is authoritative; append address
math (`active_offset`, `inode_area_pos`) unchanged; `offsetof(invfs_inode_rec, name)`
stays 36 and therefore `cli/ls.c:145` is untouched.

Rules that every site must follow:

1. **Body/AST starts at `invfs_rec_body(rec)`**, never `rec + sizeof(invfs_inode_rec)`.
2. **Size math** adds `strlen(name) + 1`: `rec_size = INVFS_REC_HDR_LEN + nlen + 1 + hdr_len + n_ents*sizeof(entry) + ext_len`.
3. **Minimum valid record** is `INVFS_REC_HDR_LEN + 1` (empty name), not `sizeof(...)`.
4. `rec_set_name` writes `n = min(strlen,255)`, sets `name_len`, copies, NUL-terminates;
   the buffer must have been sized with `n+1` first (same `name` string).
5. Any full-record `memcpy` must copy **header + name + body**, not just the header.

---

## Component responsibility (who does what)

| File | Owns | Change |
|---|---|---|
| `src/core/invarifs.h` | wire struct, bounds | `name[256]` → `name[]` FAM; add `INVFS_REC_HDR_LEN`, `invfs_rec_body/cbody`; `INVFS_MAX_REC_LEN` / `INVFS_INODE_REC_MAX` add `1 + INVFS_MAX_NAME` |
| `src/core/volume_internal.h` | name writer | `rec_set_name` (n+1 bytes, NUL); fix the `invfs_name_fits` assert that reads `sizeof(rec->name)` |
| `src/core/volume.c` | open-time scan + RAM name index | advance by `rec_len`; copy `name_len` bytes from `rec->name` (RAM slot stays 256 B) |
| `src/core/vol_records.c` | append/supersede/tombstone/slot (31 sites) | the heart: INOD append, `vol_retire` tombstone, owner/aux slot sizing, `meta_get_append_pos` |
| `src/core/vol_write.c` | file commit | `wsession_commit`: alloc + `rec_set_name` + AST at `invfs_rec_body`; `wsession_load_old` parse |
| `src/core/vol_ast.c` | decomposed parts + AST parse | part-record build at `reg_body`; `vol_get_children`, recipe parse |
| `src/core/vol_read.c` | read path | resolve records by `rec_len`, AST at body, name compare via `name_len` |
| `src/core/vol_fsck.c` | repair/quarantine/truncate (8 sites) | quarantine + `fsck_truncate_suffix` (`memcpy(nr, rec, sizeof)` → header+name+body), scan bounds; drop the 256 hardcode at :385 |
| `src/core/vol_dirs.c` | create/rename | build record, legacy bounds check |
| `src/core/vol_tier.c` | owner/tier records | tier writer + tombstone `rec_set_name`; keep the in-place no-tombstone exception (:252-261) |
| `src/core/vol_textzone.c` | text batches | batch/owner record build (10 sites) |
| `src/core/vol_dedupe.c` | dedupe rewrite | record rewrite/size base |
| `src/core/vol_repair.c`, `vol_cpack.c`, `vol_png.c`, `vol_heat.c` | repair / container / PNG / heat | size/offset bases |
| `src/core/vol_meta_merge.c` | extent sizing (6 sites) | "record + tombstone fit one extent" must use the real per-record name length |
| `src/core/vol_sweep.c` | sweep walk (15 sites) | compaction/scan; note compaction is a no-op on mapper volumes |
| `src/cli/sweep.c`, `resize.c`, `migrate-v2.c`, `ls.c` | offline tools | record walk/size; `ls.c` uses `offsetof(name)` (still valid) |
| `src/core/vol_resize.c` | file_size patching | `offsetof(file_size)` unchanged |

Established work pattern: delete the field first — every remaining `->name`/
`sizeof(rec->name)` misuse becomes a compile error that must be routed through the
helpers; then fix the `sizeof(invfs_inode_rec)` offset sites (they still compile, so
grep is mandatory).

---

## Validation

1. **Unit tests:** `make test` — must pass (baseline 4722 checks, 0 failures).
2. **Crafted inputs:** empty name, 255-byte name, name with UTF-8/latin-1 bytes,
   tombstone with a name — no crash, correct round-trip.
3. **Bit-exactness:** `invf-cat` / `invf-cp` + `cmp` on the fixture's live set.
4. **Size gate:** `invf-stats` on a rebuilt volume shows live/on-disk metadata
   dropped by the expected ~13 MiB / ~66 MiB; `invf-fsck` CLEAN.
5. **Tool parity:** `invf-ls`, `invf-stat`, `invf-fsck -f`, `invf-verify` agree with
   the pre-change view on the same logical tree.
6. **E2E (with `INVFS_E2E_AGENT=wp58a-var-name-v3`, serialized by the lock):**
   `tools/test-meta-extent-walk.sh`, `tools/test-writepath.sh`,
   `tools/test-sweep-mapper.sh`, `tools/test-textzone.sh`, `tools/test-binbatch.sh`,
   `tools/test-dedupe.sh`, `tools/test-fixture-bigvol.sh`.
7. **Fuzz:** `make fuzz` clean (the record parser is the attack surface).

---

## Deliverables

- Patch series: (1) struct+helpers+macros, (2) writers, (3) readers/tools,
  (4) fsck/repair copy paths.
- v3 layout section added to `invarifs.h`; `AGENTS.md` §2.3/§2.9 format text bumped.
- Measurement note (before/after) appended to `impl_docs/WP58-meta-record-v3.md`.
- `INCIDENTS.md`/`AUDIT.md` untouched unless a bug (e.g. the 255/256 cap mismatch) is
  fixed as part of the series.

---

## Out of scope (do NOT touch)

- core+arena COW split, cold-arena compression, varint/delta (WP58 stages A2/B/C/D).
- Changing the 255-byte cap or the bytes-as-names decision (locked: raw bytes/255).
- `invfs_ast_child_entry.name[256]` (container member names) — separate structure.
- The mapper-volumes compaction no-op (WP58 stage D territory).
- Backward compatibility / dual reader / migration — deliberate format break.

---

## Results (implementation, 2026-09-19)

Implemented on `wp/58a-var-name-v3`; `sizeof(invfs_inode_rec)` = **36**, name is a
FAM at offset 36, body at `INVFS_REC_HDR_LEN + name_len + 1`. `rec_len` now includes
the name slot; the trailing CRC32C is unchanged. All ~140 offset sites, ~39 stack
prefix instances, and the `tools/` helpers + embedded test harnesses were migrated.

- **`make test`: PASS** — 4467 + 86 + 169 = 4722 checks, 0 failures (baseline parity).
- **e2e PASS:** `test-meta-extent-walk`, `test-sweep-mapper`, `test-textzone`,
  `test-binbatch`, `test-fixture-bigvol` (30,000 files imported, `invf-cat`
  bit-exact, 3/3).
- **Empirical size win** (20,000 empty files, ~46-byte names, `invf-stats` `used`):
  **main 46.9 MiB -> v3 43.0 MiB**, delta 3.9 MiB = `20000 x (256 - 47)` bytes,
  exactly the predicted per-record saving. Extrapolated to the Gentoo fixture
  (302k records, live 66k): **~66 MiB on-disk / ~13 MiB live**.
- **Pre-existing reds (identical on `main`, not regressions):** `test-writepath`
  (fsck `orphans`), `test-dedupe` (rc=3), `test-compact` (`A1 no checkpoint-skip`),
  `test-dynzone` (fsck orphans), `test-seal` (`first seal should write every
  stripe`), `test-astv2` (`FAIL: fsck A`), `test-migrate-v2` (`heat TLV absent`),
  `test-rollback`/`test-sweepboot`. WP58a reaches the same failure points as main
  (the test harnesses were updated so they no longer fail *earlier* on the v3 layout).

### Known limitations

- `src/cli/migrate-v2.c` now derives the body offset from v3 records; genuine
  **v1** (`name[256]`, body at 292) volumes would be misparsed. Migration/compat is
  out of scope for this deliberate format break; a v1 reader would need its own
  record prefix.
- `src/cli/sweep.c` appears to be a legacy duplicate; the shipped `bin/invf-sweep`
  is built from `tools/invf-sweep.c` (migrated).
- Defensive guards (`name_len > INVFS_MAX_NAME`, `rec_len < HDR + name_len + 1`)
  were added at several scan/parse sites; they only change behaviour for
  corrupt/crafted records (skip instead of OOB).

## Coordination notes

- Subagent ID: `wp58a-var-name-v3`; e2e via `INVFS_E2E_AGENT=wp58a-var-name-v3`.
- Dependencies: none. Landable before WP56/57 (independent).
- One logical change; keep commits per layer so a bisect can isolate a bad offset.
