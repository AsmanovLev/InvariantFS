# WP58-meta-record-v3 — hot COW core (RAW, uncompressed) + compressed cold arena (shadow)

**Branch:** `wp/58-meta-record-v3`
**Worktree:** `/tmp/invfs-wp58`
**Severity:** MEDIUM (space + churn; format-breaking)
**Source:** metadata-layout study (explore `ses_f49f01f5d`, general `ses_f49f01208`) +
read-only measurements of `invfs-root.img` (this WP's baseline)
**Estimated effort:** large (multi-WP programme; see "Staging")

**Status (2026-09-19):** **compression dropped** — the win comes from structure
alone. First implementation split out as **WP58a-var-name-v3**
(`impl_docs/WP58a-var-name-v3.md`; branch `wp/58a-var-name-v3`, worktree
`/tmp/invfs-wp58a`). The codec/framing sections below are kept as an
evaluated-but-deferred record. Name encoding locked to **raw bytes, cap 255**.

---

## Scope

Replace the single append-only per-version record (292 B fixed header, name inline,
AST recipe + entries + INO2 tail, one trailing CRC) with **scheme B**:

- a compact, fixed-slot **COW inode core** in the **RAW (hot, uncompressed)** region;
- append-only **cold arenas** (names, AST recipe+entries+children, non-heat INO2)
  in the **shadow (cold, compressed)** region, reusing the existing shadow segment
  framing/codec path;
- per-file **heat stays hot/uncompressed** (RAW) and is *not* part of the compressed
  cold arena — the one explicit exception.

Metadata-only changes (rename, chmod/chown, setattr, xattr, heat decay/stamp) rewrite
or append **only the core** and emit **no DELT tombstone**; data rewrites replace the
core's AST pointer. This is the scheme-B claim being tested; this WP is the design +
minimal prototype, not the full migration.

**Non-goal:** this is *not* scheme D (varint/delta) and *not* a columnar SoA. Varint
packing would sit *on top* of the cold arena and is explicitly deferred (§Out of scope).

---

## Why — measured on the real mapper volume (read-only)

Volume: `total_blocks=3,932,160` (15 GiB), `format_version=1`,
`VOLF_META2|VOLF_ASTV2`, 836 mapper extents. Liveness replayed exactly
(`final.py`, 0 residual, 0 CRC failures).

| Quantity | Value |
|---|---|
| All records on disk | 302,269 (184,196 INOD + 118,073 DELT), **104.194 MiB** |
| Live set (latest non-broken per name) | 66,126 records, **25.799 MiB** |
| Dead/superseded + tombstones | **77.2 MiB (75%)** |

Live component breakdown: fixed header 292 B × N = 18.414 MiB; `name[256]` slot
16.144 MiB (real names only 3.083); AST recipe 1.009; AST entries 2.008; children
0.202; INO2 4.166.

### Scheme-B hot/cold split (this design)

**HOT — RAW, uncompressed:** core slots only.

| core slot | RAM/hot bytes |
|---|---|
| 32 B | 2.018 MiB |
| 48 B | 3.027 MiB |
| 64 B | 4.036 MiB |
| heat TLV (current image) | ~0 (only 28 records have `invfs.heat`) |

**COLD — shadow, compressed:** names 3.083 + AST 3.219 + INO2-minus-heat 4.166 =
**10.468 MiB raw**:

| codec | whole stream | 64 KiB frames |
|---|---|---|
| zstd-1 | 0.851 | 0.873 |
| zstd-3 | 0.800 | 0.839 |
| zstd-9 | 0.717 | 0.773 |
| zstd-19 | **0.632** | 0.703 |
| gzip-9 | 0.877 | — |

**Valid totals (hot uncompressed + cold zstd-19 whole):**

| | total |
|---|---|
| B32 | **2.65 MiB** |
| B48 | **3.66 MiB** |
| B64 | **4.67 MiB** |

vs 25.799 MiB live (compaction only) and 104.194 MiB on disk. Note: the earlier
"1.31 MiB whole B64" figure **compressed the hot core too** and is invalid under the
zone model; discard it. The study's 6.536 MiB "scheme B" silently included scheme-D
varints (`astent 0.527`, `ino2 2.009`), so raw B is 12.5–14.5 MiB — compression, not
the core/arena split, is what beats the projection.

### The posix problem (open — §Design decision 3)

Of the 4.166 MiB INO2, the 48 B posix header is **3.026 MiB** and is needed on every
`getattr`. Yet it compresses to ~0.05 MiB cold because the image has only **6,423
distinct INO2 blobs across 66,110 records** (clustered mtimes + default
uid/gid/mode). Pulling it into a fat hot core costs ~3 MiB hot to save ~0.05 MiB
cold; leaving it cold costs a decompress per `stat`. Dedup into a hot
content-addressed table is the middle path.

Risks to hold: this fixture's INO2 ratio is workload-specific. Name/AST ratios
(~10×) are workload-independent; assume a pessimistic 4× on INO2 in any projection.

---

## Design

### Zone placement (the rule this WP enforces)

- **RAW / hot / uncompressed:** core array (fixed slots, COW), heat.
- **Shadow / cold / compressed:** names arena, AST arena, non-heat INO2 blob arena.
- This mirrors the existing data model (RAW landing → shadow consolidation,
  `volume_internal.h:1004-1012`), reusing the shadow framing during decode
  (`[4B csize][4B crc]`, `volume_internal.h:1275-1284`).

### Core slot

Proposed 48 B (`invarifs.h` `invfs_inode_rec` is the 292 B baseline to retire):

```c
typedef struct {              /* fixed slot, RAW, COW */
    uint64_t inode_id;        /* identity / id-index key */
    uint32_t gen;             /* generation; position-kill becomes gen-compare */
    uint64_t size;            /* logical file size (DELT keeps old semantics) */
    uint64_t ctime;           /* second resolution */
    uint16_t type_mode;       /* INVFS_ITYP_* + permission bits (hot getattr) */
    uint16_t name_len;
    uint32_t name_off;        /* into cold name arena */
    uint32_t ast_off;         /* into cold AST arena */
    uint32_t ast_len;
    uint32_t ino2_off;        /* into cold INO2 arena (or hot dedup table idx) */
    uint32_t ino2_len;
    uint32_t crc32c;          /* core integrity */
} invfs_inode_core;           /* 48 B packed */
```

32 B variant drops `ctime`/`ast_len`/`ino2_len` at the cost of derived/again-stored
fields; decide in the prototype and record which fields are truly hot.

### Cold-arena frame geometry (mkfs-time, self-describing)

The frame size and codec are chosen when the volume is created and stored in the
superblock/MET0 under `VOLF_REC_V3`, so the reader uses no compile-time constant:

```c
typedef struct {              /* in the sb / MET0 */
    uint8_t frame_log2;       /* frame = 1 << frame_log2; 16 = 64 KiB */
    uint8_t codec_id;         /* INVFS_ALGO_ZSTD (future: PPMd/LZ4) */
    uint8_t codec_level;      /* zstd level, or 0 = "follow the volume profile" */
    uint8_t reserved;
} invfs_cold_geom;            /* 4 B */
```

- **Default:** `frame_log2 = 16` (64 KiB), `codec_id = ZSTD`, `codec_level = 19`.
- `codec_level = 0` binds the level to the existing sweep profile
  (`INVFS_PROFILE_*`, `codecs/codec.h:153`; `invfs_profile_zstd_level`), so a
  `fast`/`balanced` volume pays z3/z9 and only `dense`/`archive` pay z19 — avoids
  the z19 ~2 MB/s / large-window cost as a blanket default.
- **mkfs knob** (env, matching the `INVFS_META_FRAC` precedent in
  `src/cli/mkfs.c:295`): `INVFS_META_FRAME=<log2|16k|64k|256k|1m>` and
  `INVFS_META_ZSTD=<0..22>`; mkfs prints the chosen geometry and stores it. No
  per-record cost — geometry is volume-wide.
- **Reader:** unknown `codec_id`/`frame_log2` → refuse the volume, same gate as
  `VOLF_REC_V3`. Sweep/compaction re-emits frames at the stored geometry.
- **Alignment:** 64 KiB tiles evenly into the 128 KiB minimum mapper extent
  (`invarifs.h:605-608`); frames >min size class span several extents — allowed,
  but the allocator must not assume one-frame-per-extent.
- **Scaling heuristic:** prefer an explicit default + override over silent
  auto-scaling with volume size; if we ever auto-pick, base it on the *estimated
  metadata size*, print it, and keep it overridable.
- **Per-arena geometry** (name/AST/INO2) is a later refinement; v1 stores one
  volume-level value.

### Codec evaluation (metadata cold arena)

Measured on the 10.468 MiB live cold arena (read-only; `btest.py`,
`bench_latency.py`, `ppmd_bench`, `brotli_bz2_bench.py`). zstd decompress is
whole-stream; PPMd is whole-stream order-8 / 64 MB model; brotli via libbrotli.

| codec | cold size | ratio | compress | decompress |
|---|---|---|---|---|
| zstd-19 | 0.632 | 16.6× | ~2 MB/s | ~1.2 GB/s |
| zstd-3 | 0.800 | 13.1× | ~400 MB/s | ~1 GB/s |
| **brotli q11** | **0.606** | **17.3×** | 0.38 MB/s | 473 MB/s |
| brotli q5 | 0.685 | 15.3× | 33 MB/s | 529 MB/s |
| PPMd o8/64M | 0.792 | 13.2× | 16 MB/s | 18 MB/s |
| bzip2 -9 | 0.715 | 14.6× | 4.5 MB/s | 47 MB/s |

- **zstd stays the default** (`codec_level=0` → profile): best speed on both
  axes, already vendored, RFC 8878 stable.
- **brotli q11** is the only codec that beats zstd-19 on size, by just **26 KiB
  (4%)**, at 0.38 MB/s compress (offline only). Decode 473 MB/s is fine and the
  format is RFC 7932. Best candidate for a *future* `archive` cold-arena codec
  (`codec_id` slot) — not the default, not worth a dependency now.
- **brotli q5/q9** are dominated by zstd-19 (worse size, slower both ways).
- **bzip2 -9** is dominated on every axis (ratio worse than zstd-19, decode
  47 MB/s); reject.
- **PPMd** wins only on the `names` sub-stream (0.200 vs zstd 0.269) but decodes
  at ~10 MB/s with a 64 MB model and is frame-hostile; keep it as the text-batch
  codec where it already is.
- **OpenZL** (format-aware graph framework, BSD, C++17) evaluated: strong on
  structured data in principle, but the project states its format/API "will
  change" and only gives a multi-year decode guarantee for release tags — a poor
  fit for an archival FS's decode-forever invariant. Deferred; `codec_id` left
  reserved.

**brotli q11 by frame size** (`brotli_latency.py`, same cold arena; whole-stream
q11 = 0.602 MiB / 455 MB/s):

| frame | zstd-19 size | brotli q11 size | zstd-19 ns/frame | brotli q11 ns/frame |
|---|---|---|---|---|
| 4K | 0.968 | 0.934 | 22,400 | 33,800 |
| 16K | 0.765 | 0.740 | 39,500 | 64,100 |
| 64K | 0.703 | 0.669 | 63,900 | 176,200 |
| 256K | 0.676 | 0.633 | 222,700 | 530,700 |
| 1M | 0.653 | 0.622 | 990,300 | 2,143,000 |

Brotli q11 is consistently ~5% smaller but **1.5–2.8× slower per frame**, and it
needs **256K–1M** frames to realize that size advantage (zstd already saturated
at 64K). Compression is ~0.55 MB/s regardless of frame size (~4× slower than
zstd-19). So if a future `archive` profile adopts brotli it should use large
frames and accept ~0.5–2 ms decode; for the hot random-access path it is strictly
worse.

### Workstreams

- **A — core + arenas (new format, `VOLF_REC_V3`).** Writer emits core + cold
  arena blocks; reader resolves. `magic`/`rec_len`-based skipping is replaced by
  explicit core→arena references; every scanner that assumed `sizeof(invfs_inode_rec)`
  as the AST offset (~40 sites: `vol_read.c:372`, `vol_fsck.c:82,224,257,264,267,316`,
  `vol_dedupe.c:113,169`, `vol_tier.c:198,286`, `vol_textzone.c:99,183,208`,
  `cli/{sizes,stat,ls,sweep,resize,migrate-v2}.c`) must route through one helper.
- **B — COW core + no-DELT metadata rewrite.** `meta_rewrite` / `vol_set_xattr` /
  heat stamp write only the core (new `gen`, CRC), no 292 B tombstone. Preserve the
  owner in-place exception (`vol_tier.c:252-261`, no tombstone when reused in place).
- **C — shadow-compressed cold arena.** Frame cold bytes with the existing shadow
  segment framing; decode on demand with an ARC-style cache keyed by arena block.
- **D — mapper-aware inode compaction (scheme E).** `vol_inode_compact` returns 0
  immediately on mapper volumes (`vol_records.c:2310-2312`); the only reclamation is
  extent-granular `vol_meta_extent_shrink/merge`. Add a compaction pass that rewrites
  the live set into fresh mapper extents behind a two-phase mapper swap. **This is
  independently the single largest win and can land first** (103→25.8 MiB, no format
  change).

### Crash / durability ordering (must hold)

1. Cold arena bytes (and any new core's referenced blobs) durable **before** the core
   that names them — preserves the existing "maps before record" rule
   (`volume_internal.h:959-989`).
2. Core slot is COW: new `gen` + trailing CRC; old generation freed only after the
   new core is durable. Position-kill becomes gen-compare (`idx_del_at` analogue).
3. Owner records keep their dedicated extent-sized, flush-safe append
   (`vol_append_owner_slot` → `meta_get_owner_append_pos`, WP52); the one-record-per-
   extent sizing invariant (`vol_meta_merge.c:626-766`) still applies to the core.
4. No `DELT` on metadata-only change, so tombstone volume drops from 32.9 MiB toward
   0 — but the consistent-cut rules and the legacy `format_version=0` reader stay.

### Migration

New `VOLF_REC_V3` flag; v2 readers refuse v3 loudly; offline conversion in the
`migrate-v2.c` style (live set only — position-kill tombstones name absolute
positions, so the compacted stream is self-consistent).

### Design decisions to close before coding

1. **Core width / field set** (32 vs 48 vs 64) — prototype both, measure.
2. **Cold arena framing & codec — DECIDED:** 64 KiB zstd frames (0.703 MiB z19,
   ~1 GB/s decode, ~40–64 µs/frame). Geometry is **mkfs-time and self-describing**
   (`invfs_cold_geom`); default 64 KiB / zstd-19, with `codec_level=0` meaning
   "follow the volume profile". 4K/16K/256K/1M rejected for the hot path;
   brotli q11 reserved for a future `archive` codec; bzip2/PPMd/OpenZL rejected
   (see "Cold-arena frame geometry", "Latency input", "Codec evaluation").
3. **Posix placement** — (a) hot dedup table of distinct blobs (~0.42 MiB hot,
   `getattr` no decompress), (b) cold + decoded cache (`getattr` decompresses),
   (c) fat core (+~3 MiB hot). The measurements favour (a); decide with the WP owner.
4. **Name arena layout** — raw vs front-coded (front-coded 0.770 MiB raw, and
   zstd still helps; zstd does most of the work, so prefer raw + zstd for simplicity
   unless the front-coded+compressed win is needed).
5. **COW core versioning** — gen-compare vs slot double-buffer.
6. **Frame geometry — DECIDED:** mkfs-time, stored in `invfs_cold_geom`; default
   64 KiB / zstd-19, `codec_level=0` follows the sweep profile. See "Cold-arena
   frame geometry".

### Latency input (measured)

`/tmp/opencode/bench_latency.py` (log `bench_latency.log`, 145 s). Cold arena
10.468 MiB raw; one random record fetches one frame (≈166 B cold/record, so a
64 KiB frame is ~395× amplification).

| frame | z1 | z3 | z9 | z19 | decompress z19 ns/frame | MB/s |
|---|---|---|---|---|---|---|
| 4K | 1.123 | 1.052 | 1.017 | 0.968 | ~22,400 | ~174* |
| 16K | 0.921 | 0.874 | 0.823 | 0.765 | ~39,500 | ~396* |
| 64K | 0.873 | 0.839 | 0.773 | **0.703** | ~63,900 | ~975 |
| 256K | 0.855 | 0.830 | 0.745 | 0.676 | ~222,700 | ~1119 |
| 1M | 0.855 | 0.808 | 0.732 | 0.653 | ~990,300 | ~961 |
| whole | — | — | 0.717 | **0.632** | — | 1226 |

\* small-frame MB/s is inflated by per-call ctypes/Python overhead; the 64K and
whole-stream figures are the representative ones (in C, 4K/16K per-frame cost is
much lower than shown). Compress speed: z19 ≈ 2 MB/s (offline only), z9 ≈ 50 MB/s.

**Conclusion:** 64 KiB frames with zstd are the chosen point — +11% over
whole-stream (0.703 vs 0.632 MiB) and ~1 GB/s decode, with a single frame
decompress (~40–64 µs) per cold access. `getattr`/`read` already cost one I/O,
so this keeps the hot path at "one core read (RAW) + one cold frame decode".
The mkfs default stores 64 KiB / zstd-19; for volumes that rewrite the arena in
the background, `codec_level=0` (profile) drops to z9 (0.773 MiB, 50 MB/s) with a
~9% size penalty. 4K/16K frames are not worth the size for this workload.

---

## Validation

1. **Unit tests:** `make test` — must pass (current baseline 4722 checks, 0 failures).
2. **Bit-exactness:** `invf-cat`/`invf-cp` of the measured fixture vs source
   (`cmp`) for the live set — the bit-exact invariant is non-negotiable.
3. **Reader/tool parity:** `invf-fsck -f`, `invf-ls`, `invf-stat`, `invf-stats`,
   `invf-sizes`, `invf-verify` must see the same view on v3 and on a v2 image.
4. **Format gate:** a v2 reader must refuse `VOLF_REC_V3` loudly; offline conversion
   round-trips (`v2 -> v3`) with identical listing + bytes.
5. **Compaction (WP58-D) can land alone:** after compaction,
   `invf-fsck` CLEAN, population unchanged, `invf-cat` bit-exact, and `df` reclaims
   the 77.2 MiB of dead records + tombstones.
6. **Latency gate:** `getattr`/`read`/`readdir` on a cold-cache mount not worse than
   baseline by more than an agreed budget; report ns/op for both.
7. **Fuzz:** `make fuzz` clean (record parser is the attack surface).
8. **E2E:**
   - `bash tools/run-e2e.sh tools/test-meta-extent-walk.sh`
   - `bash tools/run-e2e.sh tools/test-sweep-mapper.sh`
   - `bash tools/run-e2e.sh tools/test-mapper-crash.sh`
   - `bash tools/run-e2e.sh tools/test-writepath.sh`
   - plus `test-textzone.sh` / `test-binbatch.sh` (owner/cold-arena interaction).

Fixture of record: the 15 GiB `invfs-root.img` measurements above
(66,126 live / 104.194 MiB), plus `tools/test-fixture-bigvol.sh`.

---

## Deliverables

- Format design + core/arena wire spec appended to `invarifs.h` (`VOLF_REC_V3`,
  `invfs_inode_core`, arena framing).
- Prototype encoder/decoder + `meta_probe`/`invf-sizes` extension to print the
  hot/cold split (the measurement harness is `/tmp/opencode/btest.py`).
- Compaction pass (WP58-D) with e2e coverage.
- `INCIDENTS.md` / `impl_docs/AUDIT.md` entries for the retired walkers + the format
  bump; `AGENTS.md` §2.3/§2.4 zone/format text updated.
- Squash-merged commits, one per workstream.

---

## Out of scope (do NOT touch)

- Varint/delta packing (scheme D) of core/entries/INO2 — deferred; orthogonal on top.
- Columnar SoA (scheme C) — rejected for this workload (adds per-op gather).
- Owner-record / seal / parity semantics beyond the flush-safe append already in WP52.
- WP56 (batch-commit/dup-pba) and WP57 (heat persistence) — independent; coordinate
  if the heat split (hot vs cold) touches `vol_heat.c`.
- Legacy `format_version=0` reader changes (must keep working).

---

## Staging (recommended order)

1. **WP58-D** mapper-aware inode compaction (no format change; biggest, safest win).
2. **WP58-A** core + cold arena v3 behind `VOLF_REC_V3` + dual reader + migrate tool.
3. **WP58-B** COW core no-DELT metadata rewrite.
4. **WP58-C** shadow-compressed cold arena (wire the codec; gather latency numbers).

Each stage runs `make test` + the named e2e gates; stage 1 can merge before the
format exists, so `main` stays green.

---

## Coordination notes

- Subagent ID: `wp58-meta-record-v3`; pass `INVFS_E2E_AGENT=wp58-meta-record-v3`.
- Dependencies: none strictly; **WP52 must be green** before touching owner appends
  (WP58-B/C interact with `vol_append_owner_slot`).
- Independently, the open findings WP56 (`bad records` / dup pba / `--realize`
  orphans) and WP57 (heat persistence) should not block stage 1.
- Measurement baseline command: `python3 /tmp/opencode/btest.py` (read-only).
