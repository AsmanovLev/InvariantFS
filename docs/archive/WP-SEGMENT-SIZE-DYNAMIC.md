# WP: dynamic chunk/segment size (audit 2026-11-18)

Source: delegate audit `d3ab001b…` covering all 64+144 uses of
`INVFS_SEGMENT_SIZE` / `SEGMENT_SIZE` in `src/` and `tools/`. The task is to understand what
is needed to make the chunk size a **session field** rather than a constant.

## Audit result: three classes

### (a)+(b) Should become a session field

`src/core/vol_write.c` — ten functions:

| function | lines | what it does |
|---|---|---|
| `wsession_seal_child` | 252,284,288,291 | child span `child_segs*SEGMENT_SIZE` |
| `wsession_write_seg_n` | 534,552 | `file_offset = j*SEGMENT_SIZE`, full write length |
| `wsession_seg_current` | 566,583-599,639-654 | RMW decode with zero-pad to chunk |
| `wsession_zero_fill` | 672 | `calloc(1,SEGMENT_SIZE)` |
| `wsession_load_old_impl` | 773-787 | materialization in 64-KiB windows |
| `vol_write_range` | 836-837,857,868-877,886 | offset→segment, split, ENOSPC estimate |
| `vol_write_truncate` | 915-917,949,956,977-984,994 | keep/child boundaries, zeroing the middle |
| `vol_write_read` | 1032-1035 | session read |
| `vol_write_commit_impl` | 1079-1094,1102-1114 | commit tail math |

Plus outside `vol_write.c`:
- `src/core/vol_sweep.c:1162` — `cbuf` in the worker's task structure (heap, but size from the constant);
- `src/core/vol_sweep.c:1251` — `ZSTD_compress` bound from the constant;
- `src/core/vol_read.c:1828,1886` — stack `tmp[]`/`ctmp[]` during decode;
- `src/cli/fuse_fs.c:2088,2525-2531,2542` — FUSE write staging.

**Stack arrays (direct stack-overflow risk at >64 KiB):**
`vol_write.c:877,980,1035,1094,1114` (`uint8_t plain[SEGMENT_SIZE]`),
`vol_read.c:1828,1886`, `vol_sweep.c:1184`. These must be moved to the heap (or to a
reusable session scratch buffer) **before** enabling dynamics.

### (c) Must remain a constant — or get an explicit value source

| location | why |
|---|---|
| `vol_write.c:213` | predicate for "old per-segment recipe" (`length<=SEGMENT_SIZE && block_id==i`) |
| **`vol_sweep.c:1230`** | **validation: a recipe cannot declare `length > SEGMENT_SIZE`** — this is a writer-invariant check |
| `vol_heat.c:848` | ENOSPC estimate by record count assumes 64 KiB |
| `sweep_orig_len_test.c:89,112,121` | test explicitly asserts that the writer does not emit >64 KiB |
| `volume.h:895` / `volume_internal.h:146` | default value |

### (d) Tests/tools only
`write_create_path_test`, `chld_producer_test`, `pbaref_test`,
`arc_concurrency_test`, `read_parallel_bitexact_test`, `dedupe_test`,
`dedupe_symlink_test`, `dirs_free_before_publish_test`, `spn_skip_recipe_test`.

## False positives
Comments/strings: `volume.h:883`, `vol_seal.h:4,47`, `vol_write.c:424,556`,
`vol_sweep.c:1215-1222`, `invf-reconsolidate.c:4-5`, `sweep_orig_len_test.c` (part
of strings).

## Risks the audit found beyond expectations

1. **`vol_seal.h:48` — `SEAL_SYM_BYTES 65536`** is defined independently, and seal
   assumes that **segment == symbol**. A large chunk breaks seal geometry
   without even appearing in a name grep. Either the symbol stays 64 KiB and is cut by
   the logical stream (not by segment boundaries), or a symbol-size field goes
   into the format.
2. **`MAX_SEGMENTS_V2 = 1<<24`, `MAX_FILE_SIZE = 1<<48`** (`invarifs.h:1155,1167`):
   a large chunk changes the maximum representable file size and EFBIG behavior.
3. **`s->child_segs` is already per-session** (`vol_write.c:114`): child recipes
   saved with an old chunk size must be invalidated or marked.
4. **A name grep misses literal 64 KiB**: `vol_spt0.c:1440
   buf[64*1024]`, `sweep_publish_rollback_test.c:103 SEG_BYTES 65536`,
   `entry_v3_test.c:163-296`, `vol_cpack.c:2741 CPACK_LEAF_HEAD`. Check before
   any format change.
5. **The recipe does not store the chunk size** — only `length` and `pba`. Mixing
   sizes within one volume is safe only if all readers use `e->length`
   as the decode boundary; these hits do not prove that.

## Recommended path (a decision is needed)

The invariant at `vol_sweep.c:1230` ("record length ≤ SEGMENT_SIZE") is a check, not
a format property. Three options:

| option | what it does | cost |
|---|---|---|
| **(A) `max_chunk` field in the superblock** (recommended) | readers validate `e->length <= sb.max_chunk`; writer takes `seg = min(policy, sb.max_chunk)`; old volumes read as 64 KiB | one field in the format (v0.7), one check |
| (B) size tag in each record on write | self-contained on write, but duplicates `length` and does not answer "what maximum is allowed" | field in every record, still needs a limit |
| (C) simply raise the constant | no read-code changes | breaks the 64-KiB invariant for **old** volumes and seal |

## Work order (after audits B and C)

1. Move stack buffers (`plain[]`, `tmp[]`, `ctmp[]`, `cbuf`) to heap/scratch — no behavior change; gate: same tests + bit-for-bit.
2. Introduce `max_chunk` into the superblock (option A) and migrate the three (c)-checks to it.
3. Add `seg` to the session (default = `sb.max_chunk`), replace (a)-arithmetic with `s->seg`.
4. Sync FUSE staging and the `vol_sweep` worker with `s->seg`.
5. Seal: cut symbols by the logical stream, not by segment boundaries (or add a symbol-size field).
6. Measurement: `invf-reconsolidate` plan vs. reality; expected 8.5 % → ~0.7 % rounding.

---

## Appendix: recipe format funnel (self-audit while delegate B was failing)

Delegate `d4b4eeb0…` failed on the provider ("operation was aborted"), not on the task.
Below is what was captured by direct grep so as not to block; the restart
(`dc9cd25f…`) will refine the details.

| stage | location | what matters for a v4 record |
|---|---|---|
| PARSE | `vol_ast_recipe_parse()` `vol_ast.c:247`; `invfs_ast_hdr_parse()` `invarifs.h:1238` | version is declared in the header; an unknown version must fail loudly, not be read as v3 |
| STRIDE | `invfs_ast_entry_stride()` `invarifs.h:1210-1218` | 32 B for V1/V2, **40 B for V3**, 0 for unknown — the single point where record stride is set |
| DECODE | `seg_read_once()` `vol_read.c:43`; `csize` read — `vol_read.c:64` (`pba*BLOCK+8`) | **this very line is where `stored_len` from the record will replace reading the segment header** |
| SERIALIZE | `vol_ast.c`, `vol_cpack.c`, `vol_cut.c`, `vol_dedupe.c`, `vol_exer.c`, `vol_migrate.c` | six recipe writers: each constructs records itself, so `stored_len`/`sub_off` will have to be filled in all six |
| HEADER LEN | `hdr_len` from `invfs_ast_hdr_parse()` (`invarifs.h:1257,1273`), `INVFS_AST_HDR_V1_LEN/_V2_LEN` | records start at `hdr_len`; a new header field means a new length constant, not an in-place shift |

Plan takeaway: a v4 record has **one parser, one stride table and one decoder**,
but **six writers** — i.e. reading changes in one place, while writing must be
fixed with discipline, otherwise some paths will start writing records with the "old"
field meaning under the new header version.

### Clarified by the delegate (`dc9cd25f…`) — corrections to the table above

| what | clarification |
|---|---|
| SERIALIZE | **one** builder: `vol_ast_recipe_serialize_tail()` `vol_ast.c:56` + flat/window/tree wrappers `:17,28,41`. The six files from my table merely **fill in records** and call the funnel: `vol_write.c:534-540` (session, commits `:1127,1141`), `vol_png.c:1055-1062,1142-1149,1256-1274` (blob/WINDOW_SRC, via `vol_create_blob_file()` `:955`), `vol_cpack.c:4919,4960`, `vol_cut.c:432`. So there are 5 places to edit, not 6 "independent" ones |
| STRIDE | **40 B are used only under `INVFS_RECIPE_FORCE_V3`** (`vol_ast.c:90-91`), default 32 B; V3-slot packing with CRC — `invfs_ast_entry_pack()` `invarifs.h:1696`. So a "v4 record" without the flag will not appear and will not break old paths |
| DECODE | not only `seg_read_once()`: the loop itself — `vol_decode_ast_entries()` `vol_read.c:592` (fan-out `:620`), storage length via `seg_read_checked()` `:846`, byte-by-byte `seg_read_once()` `:48-50` (`csize` from `pba*4096`), `plen = ceil((csize+8)/4096)` in `seg_extent()` `volume.c:2877`; a separate variant for a batch member `:211` |
| PARSE | `vol_ast_recipe_parse()` `vol_ast.c:247` → `invfs_ast_hdr_parse()` `invarifs.h:1238` (V1 dispatch `:1245`, V2 `:1260`, V3 `:1279`, unknown → -1); plus **tail tables** `vol_ast_recipe_tail()` `vol_ast.c:158` (WINDOW/CHLD) — their layout also depends on record stride |
| HEADER LEN | `INVFS_AST_HDR_V1_LEN=16`, `V2=24`, `V3=24`; `invfs_ast_hdr_write()` returns 16/24 (`invarifs.h:1308,1323,1333,1352`); records start at `hdr_len` (`vol_ast.c:294`) |

**Format-change cost summary:** read — one loop + one access to the storage length;
write — one serializer + five fill-in sites + tail tables +
the `INVFS_RECIPE_FORCE_V3` gate. This is cheaper than it looked from the first grep.
