# WP653 Phase A — format knowledge in `src/core`, the read/sweep decompressor pair, and the plugin dispatch rule

STATUS: complete, COMPACT RECONSTRUCTION.

Provenance, stated so nobody over-trusts it: the first version of this audit was
produced by a subagent and then **deleted by the orchestrator before it was
archived** (an `rm` before the `cp`). Three regeneration attempts died before
writing a single section. This file is therefore written from the first version's
own output (which was read in full) plus spot re-verification of the load-bearing
citations against `main` at `8be497f`. Citations below were re-checked unless
marked *(from the audit, not re-verified)*.

The two defects the audit found are ALREADY FIXED in main and are marked as such:
WP657 (`pthread_create` return unchecked in the read fan-out) and WP658 (the
sweep's unbounded `e->length`).

---

## 1. `src/core` files that parse a foreign FILE FORMAT

`wc -l` at `8be497f`. "Foreign format" = a container/wire format whose magic,
header fields or chunk structure the code reads. Volume-format parsers
(`vol_btree.c`, `vol_delta.c`, `vol_records.c` INO2/TLV, `vol_spt0.c`,
`vol_anchor.c`, `vol_metabuf.c`) are excluded by definition.

| file | lines | format parsed | where | callers |
|---|---|---|---|---|
| `vol_png.c` | 1281 | **PNG** (signature, IHDR/IDAT/IEND framing, raw-deflate IDAT, zlib-wrapper detection), PNG rebuild | signature admit `:311`; `png_idat_is_zlib_wrapped` `:214`; spool build `:81`; rebuild+memcmp guard `:416`, `:472` | `png_parse_admit` ← `vol_create_png_file:718`, `vol_create_nbli_file:856`; `invfs_png_from_jxl` ← `vol_read.c:508`; `vol_create_png_file` ← sweep PNG/JPEG lanes `vol_sweep.c:650` |
| `vol_exer.c` | 490 | **JPEG** (SOI/SOFn/marker walk), **PNG** (chunk walk to IEND), **PE/exe-as-container** payload | `jpeg_scan_end:49`; `png_scan_end:95`; `exe_scan_media:119`; `exer_payload_parse:156`; `exer_splice:199` | `exer_payload_parse`/`exer_splice` ← `vol_read.c:1223`,`:1242`; `vol_exer_carve` ← sweep EXER branch |
| `vol_cpack.c` | 4413 | **gzip** (RFC 1952 header walk: FLG/FEXTRA/FNAME/FCOMMENT/FHCRC), **TAR** (ustar at 257), **FLAC** (STREAMINFO), APE/MAC, WAV, plus the containerpack **map blob** | `gz_header_len:1987`; TAR sniff `:2075`; `flac_bits_per_sample:1252`; `cpack_map_parse:2494`, `cpack_map_validate:2591` | `gz_header_len` ← `vol_create_gz_file:2045`; `vol_create_tar_file:1840`; `vol_create_flac_file:1467` ← `vol_sweep.c:582`,`:599`,`:619`; `cpack_map_parse` ← `cpack_map_get:3281` |
| `vol_ast.c` | 695 | **ZIP** (EOCD scan-back, central directory, local header, stored/deflate) | `vol_zip_parse_children:515-578` (EOCD `PK\x05\x06` at `:525`, CD at `:538`); `vol_zip_extract_member:664-695` (method 0 `:678`, method 8 `:683`) | `vol_read.c:1501`,`:1509` — the ZIP window read path. No sweep caller: AGENTS §2.9 says there is no builtin ZIP lane |
| `vol_read.c` | 1862 | **IVFT/IVGZ/IVPN recipe blobs** — InvariantFS's OWN formats, listed only because the GZR arm re-implements the gzip header/trailer layout instead of calling `gz_header_len` | `"IVGZ"` magic `:1002`; level/mem/crc32/isize/hlen `:1005-1013`; `deflateInit2(...,-15,...)` + gzip wrap `:1044-1060` | n/a (the read path itself) |
| `vol_sweep.c` | 2437 | **sniffing only** (magic compare, no field parse) | `"fLaC"` `:582`; `"ustar"` at +257 `:601`; `"\x89PNG\r\n\x1a\n"` `:650` | n/a (the lane dispatcher) |
| `vol_textzone.c` | 1118 | **none.** Contrary to the WP's premise, this file holds zone bookkeeping, the batch accumulator, seal/commit/flush and GC. Its only content inspection is `tz_sniff_any:125`, which delegates to the codec registry's `sniff()` (`src/codecs/codec.c`) and to `invfs_text_family`/`invfs_binary_family` (`:801`,`:839`) — extension+head classification, not a format parser | `tz_sniff_any:125` | `tz_defer`/`bz_defer` ← `vol_sweep.c:287`,`:296`,`:898`,`:1027`, `vol_cpack.c:4388`,`:4397` |

**Corrections to WP653's own premise** (the WP doc said `vol_png.c`, `vol_exer.c`
and `vol_textzone.c` are "the same shape"): `vol_textzone.c` is not a format
parser at all, and the count of duplicated decode implementations is FIVE, not
two (`vol_read.c` alone carries three of them).

---

## 2. Read path vs sweep path: every measured difference

Framing contract is already single-implementation: `ast_frame_ok` is defined once
(`vol_read.c:334`, re-verified) and called from the read serial loop (`:758`), the
read fan-out (`:375`), the sweep worker (`vol_sweep.c:1128`) and the cut salvage
probe (`vol_cut.c:105`). The checked framed read, `seg_read_checked`
(`vol_read.c:74`), is likewise shared.

What differs:

- **Sweep bounded `e->length` against nothing.** `orig_len` came straight from the
  recipe entry (`vol_sweep.c:1483` *(from the audit)*) into
  `uint8_t orig[SEGMENT_SIZE]` (64 KiB, stack). **FIXED by WP658** — and the fix's
  own gate measured that the case is unreachable in practice: the shared frame
  check refuses a lying length first (`ast_frame_ok` requires `hdr == len` for
  NONE), and reaching the arm needs a frame larger than `SEGMENT_SIZE`, which no
  writer can emit (`vol_write.c:193`, re-verified).
- **Read fan-out joined an uninitialised `pthread_t`.** `vol_read.c:603-627`:
  `th[32]` uninitialised, `pthread_create`'s return discarded, `th[t]=0` only in
  the skip branch. **FIXED by WP657**, whose gate measured the consequence as
  silent corruption (rc=0 with wrong bytes, first diff at offset 7) rather than
  only UB.
- **Error granularity**: the read path reports per-file `EIO`; the sweep skips the
  failed segment and continues with the rest of the batch
  (`valid = 0; continue;`). The files end up in different states for the same
  damage.
- **Re-encode destination cap differs**: the sweep calls
  `ZSTD_compress(a->tasks[k].cbuf + 8, SEGMENT_SIZE + 64, ...)`
  (`vol_sweep.c:1152` *(from the audit)*) against 65824 elsewhere — not a bug,
  but the number is not shared.
- **Coverage**: the two paths do not decode the same population — the sweep runs
  the lane dispatcher first, so a file it re-encoded never reaches the arms the
  read path would have used.
- **`plen` / `seg_extent_checked` / heat / ARC**: the sweep reads extents with
  `seg_extent_checked` and tracks heat; the read path does not.

The five decode-switch sites (plus the narrow sixth):

| # | site | destination | frame check first | unknown algo |
|---|---|---|---|---|
| 1 | `vol_read.c:766`, `:775`, `:1252` (serial) | heap, `malloc(ah.file_size)` | yes (`:758`) | codec registry / pack decode (`:1253`) |
| 2 | `vol_read.c:386`, `:392`, `:383` (fan-out) | `a->data` | yes (`:375`) | `err = -1`, unreachable by construction |
| 3 | `vol_read.c:1795`, `:1803`, `:1809` (ranged) | `tmp[SEGMENT_SIZE]` + heap fallback | no — the NONE arm re-derives the rule inline | loud refusal (`:1815`) |
| 4 | `vol_sweep.c:1138`, `:1143`, `:1147` | `orig[SEGMENT_SIZE]` stack | yes (`:1128`) | memcpy arm |
| 5 | `vol_write.c:440`, `:449`, `:456` (read-back) | `plain[SEGMENT_SIZE]` stack | no, gated by `wsession_simple_old` (`vol_write.c:193`) | NONE arm |
| 6 | `vol_cut.c:105-145` (salvage) | its own | yes + `e->length <= 128 MiB` (`:110`, re-verified) | refusal |

Only #6 bounds the entry against a constant; #4 bounded nothing until WP658. That
asymmetry is the answer to WP200's open question: **the pair diverges in
behaviour at named points, not merely in wording** — and the duplication is what
let the bound go missing in one copy. Consolidation is WP659, deferred.

---

## 3. Plugin dispatch rule (inproc vs IPC vs exec)

*(from the audit; the call sites are in `vol_cpack.c` around the estimate and
per-member paths — not re-verified line by line for this reconstruction)*

| mechanism | chosen when | why |
|---|---|---|
| **in-process** (`vol_plugin_inproc.c`, WP625) | `-o allow_inprocess_plugins=<pack>` (or `$INVFS_ALLOW_INPROCESS_PLUGINS` offline) AND the pack passes the three gates: pilot allowlist, anchored origin (realpath both sides), per-dispatch ed25519 re-verify | the estimate call is the hot one; ~142 µs in-process vs ~339 µs per fork+exec of the CLI helper (WP625 measured), and it is the floor of the win |
| **IPC** (`vol_plugin_client.c`) | the in-process probe child fails (crash, hang, wrong ABI) → fall back, daemon survives | a hostile or broken plugin must not take the mount with it |
| **exec** (`helper_exec.c` + `tool_scratch.c`) | no in-process eligibility, or a command the pack only offers as an executable; scratch is chosen by `tool_scratch.c` under the capacity rules in AGENTS §2.8b | the default and the fallback: files-in/files-out, no trust in the pack's code |

The rule as a decision procedure: *try in-process only for a pack the operator
named explicitly and that passed the gates; on any failure fall back to IPC; exec
is what remains.* Nothing about this is discoverable from the docs before this
table — that is the Phase B item.

---

## What this implies for Phase B (WP653 proper)

1. Move the parsers (PNG, JPEG/EXE, gzip/TAR/FLAC, ZIP) to `src/recipes/` or merge
   them into the existing `pngx.c`/`tarx.c`/`zip.c`, so `src/core/` keeps only
   segments, BLAKE3 and opaque recipe blobs.
2. `vol_textzone.c` stays in core — it is bookkeeping, not format knowledge, which
   the WP's premise got wrong.
3. The five-way decode switch is WP659, after the backup: it is a data-path
   rewrite, and the two defects this audit found were the urgent part.
4. Write the §3 table into `docs/architecture/` with `file:line` citations (§1.7),
   because the dispatch rule exists only in the code today.
