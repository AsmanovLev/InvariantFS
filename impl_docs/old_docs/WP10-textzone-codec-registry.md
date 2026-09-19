# WP10 design note: codec registry + Text Zone (cross-file PPMd batching) + storage-class flags + memory policy

User directives (2026-08-26, Telegram-style discussion, decisions verbatim where critical):

- "4M block size shown the best result for ppmd. Higher sizes didn't shown any more improvements."
- "Я хотел именно cross-file batching. Именно в этом и особенность Text Zone. Что логика тут должна
  быть похожа на ReiserFS - несколько файлов в одном блоке. И именно поэтому надо сортировать файлы -
  си файлы сжимаются лучше чем пачка файлов на разных яп-ах."
- "Таблица кодеков сразу" (codec registry from day one), classification "Гибрид",
  cache "ARC на декодированные блоки (или файлы в случае если нету cap_seek у декодера|энкодера)".
- "декодеры|энкодеры должны легко интегрироваться, как плагин ... скопировать tar.xz/gz куда-то
  (или обновить initramfs?) и подтянулись новые рецепты ... есть ещё энкодеры|декодеры типа
  'контейнер' - mp3 (id,jpg,mpeg stream), flac(id,jpg,stream), video(meta,videostream,audiostream), zip".
- Mount-param ARC limit (fixed alloc, e.g. 512MB; decoded unit bigger than cache → reject at sweep)
  + decoder memory limit (e.g. 512MB; decoder exceeding it → reject compression, fallback generic).
- "отклоняем на этапе sweep конечно же. А иначе это будет не 'гарантированные условия'" —
  rejection is sweep-time ONLY; read path never refuses stored data. Fallback = generic algo.
- "если пользователь сменит размер - то надо повторно проводить sweep - чтобы найти файлы что могут
  вписаться в увеличенную политику, или что не вписываются в уменьшенную политику" — policy re-sweep
  must work in BOTH directions over already-swept files.
- "надо где-то хранить размеры декодированных блоков|файлов чтобы ускорить этот момент".
- "энкодерам|декодерам ещё потребуется своего рода функция 'сниффера'" — per-codec content recognition.
- Storage-class flag per file: "uncompressable (less than thresold% (for example 0.5%) of benefit),
  codec specific, container specific, generic, generic_due_memlimit, text, возможно что-то ещё".
- "generic_due_guard are first candidates on addition of subencoders (png format features a wide range
  of encoders which produce pixel-exact images but different in terms of bits)".
- "uncompressable кстати надо тоже ретраить если появился новый кодек (сначала по сниффу)".

Pre-req already landed: PPMd8 wrapper `src/ppmd_codec.c` (invfs_ppmd_encode/decode, o=8, 64MB,
CUT_OFF, wire = [2B LE props][range-coded stream w/ END marker]). Historical bug fixed: CPpmd8.Stream
is a UNION {In,Out} — assigning both clobbers; decode assigns only .In, encode only .Out.

## 1. Codec registry (`src/codec.h`, `src/codec.c`)

```c
#define INVFS_CODEC_CAP_SEEK      0x01  /* decode arbitrary range w/o full stream */
#define INVFS_CODEC_CAP_BATCHED   0x02  /* decode unit = shared batch block */
#define INVFS_CODEC_CAP_WHOLEFILE 0x04  /* decode unit = whole file */
#define INVFS_CODEC_CAP_CONTAINER 0x08  /* decomposes into members/siblings */
#define INVFS_CODEC_CAP_EXTERNAL  0x10  /* external tool, may be absent */

typedef struct invfs_codec {
    uint32_t     algo;           /* INVFS_ALGO_* */
    const char  *name;
    uint32_t     caps;
    uint64_t     dec_mem_bytes;  /* peak decoder memory per unit */
    uint16_t     generation;     /* BUMP when encoder gains/improves a sub-encoder */
    int       (*sniff)(const uint8_t *head, size_t head_len, const char *name);
                                 /* 0 = not mine, >0 = confidence (magic > extension > heuristic) */
    int       (*probe)(void);    /* EXTERNAL only: tool availability; NULL = builtin */
    int       (*encode)(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap, size_t *outlen);
    int       (*decode)(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);
} invfs_codec;

const invfs_codec *invfs_codec_by_algo(uint32_t algo);
const invfs_codec *invfs_codec_all(size_t *count);          /* registry order = sniff priority */
uint16_t           invfs_registry_generation(void);         /* max generation over all codecs */
```

v1 registers: NONE, LZ4, ZSTD (wrappers over existing calls), PPMD (wraps invfs_ppmd_*;
caps=BATCHED, no SEEK; dec_mem = 64MB model + unit), plus container/external entries
(ZIP/TAR/GZ/PNG/FLAC/MP3/JXL/APE/WV) with sniff + probe — their transcode logic stays in
volume.c in v1, registry formalizes selection only. Probe search order for EXTERNAL:
env INVFS_TOOLS → /usr/lib/invfs/tools → PATH (fixes PB6: absent tool = codec unavailable
at sweep, files stay readable generic instead of permanent EIO).

Sweep selection becomes: collect sniff scores → sort desc → policy filter → transcode →
guard → stamp class. Fallback = generic (per-segment ZSTD-19; always admissible).

## 2. Storage-class flag (`invfs.class` xattr in INO2 ext block)

On-disk value = 4 bytes: `{cls u8, algo u8, gen u16 LE}`. Lives as internal xattr
`invfs.class` (house extension pattern, same as VOLF_META2 — no format change).

```c
enum {
    INVFS_CLASS_UNCOMPRESSIBLE   = 1, /* benefit < INVFS_MIN_GAIN_PCT (def 0.5%) */
    INVFS_CLASS_CODEC            = 2, /* codec-specific (PMP/JXL/APE/WV) */
    INVFS_CLASS_CONTAINER        = 3, /* container (TARR/GZR/PNGR/ZIP-kids/FLACR) */
    INVFS_CLASS_GENERIC          = 4, /* generic ZSTD-19 */
    INVFS_CLASS_GENERIC_MEMLIMIT = 5, /* codec rejected by dec_mem policy */
    INVFS_CLASS_GENERIC_GUARD    = 6, /* codec guard refused (bit-exact/size) */
    INVFS_CLASS_TEXT             = 7, /* PPMd batch member */
    /* absent = unclassified (fresh write; sweep hasn't seen it) */
};
```

`algo`+`gen` semantics: for GENERIC_MEMLIMIT / GENERIC_GUARD = the REJECTING codec and its
generation at rejection time; for UNCOMPRESSIBLE = 0 + `invfs_registry_generation()` snapshot;
for accepted classes = the codec that won.

Lifecycle (verified against volume.c mechanics): content replace → fresh record without INO2 →
class gone → reclassify (correct: content changed). Rename copies whole record incl. ext → class
follows content. Hardlink shares inode → class shared. Stamp via vol_set_xattr (meta_rewrite:
append record + position-kill tombstone); write only on class CHANGE (check-then-write).

Sweep walk predicate per class:
| class                 | action |
|-----------------------|--------|
| absent                | full path: sniff → policy filter → transcode → guard → stamp |
| UNCOMPRESSIBLE        | retry only if some codec with generation > stored gen sniffs positive (sniff first, cheap); else skip |
| CODEC/CONTAINER/TEXT/GENERIC | compliance check vs current policy; violated → downgrade to generic |
| GENERIC_MEMLIMIT      | if stored codec's dec_mem ≤ dec_mem_limit now → retry that codec directly |
| GENERIC_GUARD         | if stored codec's generation > stored gen → retry FIRST (new sub-encoder) |

Side win: today every sweep re-pays full ZSTD-19 encode for files that then fall back to
NONE segments (guard rejections). The stamp ends that.

## 3. Text Zone storage (ReiserFS-style packing)

Batch = up to 4MB logical bytes (user-confirmed PPMd optimum; doc/06 "64KB/100 files" superseded),
concatenation of sorted text file slices.

- Segment framing: standard `[4B csize][4B crc32c]` + payload `[4B usize][2B props][PPMd stream]`.
  `usize` = decoded batch size — needed because a member knows only its slice, and policy
  re-sweep must learn unit sizes WITHOUT decoding (user requirement). Whole-file codecs get
  unit size for free from `AST entry.length`.
- Owner: one permanent internal inode named `\x01tzb` (control-byte prefix; filtered from
  vol_list_dir/readdir). Its AST entries = sealed batches (`zone=TEXT, algo=PPMD,
  block_id=batch_seq`), its L2P keeps batch pbas → fsck counts them live; all existing
  segment-write machinery reused unchanged.
- Member file AST entry: `{file_offset, length=slice_len, zone=TEXT, algo=PPMD,
  block_id=slice_idx, block_offset=offset_in_batch}` (block_offset finally used as reserved)
  + duplicate L2P entry `(member_id, slice_idx) → batch pba` (same trick as offline dedupe).
- Files >4MB: split into multiple slices across consecutive batches (one AST entry per slice).
- vol_retire_inode NEVER frees zone==TEXT blocks on member delete (member L2P dups removed;
  block stays owned). Garbage collected by GC.

## 4. Sweep pipeline (text)

Classifier (hybrid): extension family first (.c/.h/.cpp→CODE_C, .py→CODE_PY, .js/.ts→CODE_JS,
.json/.xml/.yaml/.csv→DATA, .txt/.md/.log→PROSE...); unknown extension → content sniff
(≤8KB sample: text iff zero NUL bytes AND ≥85% printable/whitespace). Not text → existing
generic path untouched. Sorting: stable sort by (family, size) inside the accumulation window
("не перемешивать, доупорядочивать", doc/06 B29 +26.6% ratio) — this is the language grouping.

Accumulator lives in volume.c (house style: needs statics vol_map/l2p/meta/tombstones; a separate
module can't reach them). [Post-split (14bb2d72): the accumulator and the whole text-zone write path
live in `vol_textzone.c`; the statics argument is why `volume_internal.h` exists.] vol_sweep_one
defers text candidates into accumulator; seals at ≥4MB;
`vol_tz_flush(v)` drains the partial batch at end of CLI run (tools/invf-sweep.c) and daemon drain
(vol_sweep_pending). Seal: PPMd encode → MANDATORY decode+memcmp verify (doc/06 invariant,
in-process, free) → size guard `ppmd_len < raw_len` (else store batch as NONE) → write segment
under owner → per-member: new record version (append-only), L2P dups, tombstone old record,
free old RAW blocks (not shared — safe), stamp INVFS_CLASS_TEXT. Crash safety: batch segment +
owner maps journaled BEFORE member records; crash between → orphan batch reclaimed by GC,
members still RAW. Idempotence: swept files are zone≠RAW → existing first-zone check skips.

Return code 9 = "text → PPMd batch" (sweep.c reporting).

## 5. Read path

- vol_read_inode: PPMD branch — member L2P → pba → read segment → CRC → arc_get(pba); miss →
  invfs_ppmd_decode whole batch (usize from payload) → arc_put(pba, batch) → copy slice
  [block_offset, +length).
- vol_read_range: same; PPMD never touches the stack `uint8_t tmp[SEGMENT_SIZE]` (heap for
  >64KB units). Other algos untouched.
- ARC key = pba (not inode_id): TEXT blocks are freed ONLY by GC, which does
  arc_invalidate(pba) before returning the block to the bitmap — single hermetic point.
- verify --deep / invf-cat / vol_read_file covered automatically via vol_read_inode.

## 6. Memory policy (sweep-time admission only)

Mount options parsed in fuse_fs.c (pre-filtered from -o list): `arc_limit=<K/M/G>` (default
256MB, env INVFS_ARC_BYTES fallback) and `dec_mem_limit=<K/M/G>` (default 512MB). Setters:
`vol_set_arc_budget()`, `vol_set_dec_mem_limit()`; CLI tools read the same env.

Admission at sweep: codec skipped if `dec_mem_bytes > dec_mem_limit`; WHOLEFILE codec skipped
if `file_size > arc_limit`; batch target = `min(4MB, arc_limit/2)` (ARC refuses entries >½
budget, arc.c:263). Rejection → fallback generic + stamp GENERIC_MEMLIMIT. Read path NEVER
rejects stored data ("гарантированные условия": everything stored decodes within the policy
active at last sweep; policy change + re-sweep restores the guarantee).

Policy re-sweep over ALL live files (not just RAW): compliant → skip (cost = AST header read,
+12B pread per TEXT segment for usize); violated → downgrade (decode → store generic; TEXT
members' L2P dups removed, holes GC'd later); generic + now-admissible better codec → upgrade
per §2 table. Batch-shrink corner: arc_limit reduced below 2×existing-batch → those batches
unbatch to generic via downgrade; next normal text pass re-batches them at the new target.
No special repacker needed in v1.

## 7. GC (v1) + compactor (later)

Mark-and-sweep phase in invf-sweep after dedupe: walk all live ASTs, mark pbas of zone==TEXT
entries; owner segments outside the mark set → arc_invalidate(pba) + free + owner record rewrite.
No refcounts needed (WP6 refcount design deferred). Compactor (repacking batches with holes,
doc/06 trigger >30% fragmentation) — next phase, not v1.

## 8. Misc

- sizes.c algo_name += PPMD; stat.c already prints "Text"; invf-stats gains per-class counts.
- fuse_fs.c: hide `\x01`-prefixed names in readdir; same filter in vol_list_dir.
- Makefile: += codec.o to CORE.
- Tests: unit (classifier, registry, class round-trip), engine (mkfs → import mixed tree →
  sweep → verify --deep → invf-cat == sha256 → delete members → GC → fsck clean), mount
  (sequential + random reads, ARC hits in user.invfs.stats), crash test (kill -9 between seal
  and member-commit → members readable from RAW, batch GC'd), limits (dec_mem_limit=32M →
  texts fall back to ZSTD; arc_limit=8M → batch shrinks to 4MB), retry on generation bump
  (GENERIC_GUARD + UNCOMPRESSIBLE).
- Doc status updates: doc/02:105, doc/06:174, doc/16:406 → implemented-by-WP6.

## 9. Execution split (non-conflicting)

- Agent A: src/codec.h + src/codec.c + unit test (new files only).
- Agent B: fuse_fs.c mount opts + sweep.c reporting + sizes.c/stat.c names (compile TU-only;
  links after C lands vol_set_*).
- Agent C (after A): all volume.c/volume.h internals — class helpers, PPMD read branches,
  accumulator, owner inode, sweep wiring + policy admission + stamping, GC.
- Orchestrator: contracts (invarifs.h enum, volume.h decls, Makefile), docs, integration,
  full build + tests.

## 10. Plugin packaging: codecpack (user directives 2026-08-26)

Goal restated: "декодеры|энкодеры должны легко интегрироваться, как плагин ... скопировать
tar.xz/gz куда-то (или обновить initramfs?) и подтянулись новые рецепты". Final decisions:

- **Packs only — the self-describing single binary idea is DROPPED.** A pack must be able
  to carry SEVERAL encoders (e.g. png: fpng + zopfli + optipng variants).
- **Two version numbers, never conflated**: `pack_version` = manifest/pack format version
  (for the parser; bumping it must NOT trigger re-sweeps); `generation` = encoder-set
  version, consumed by the class-flag retry logic (§2). Adding a sub-encoder bumps
  `generation` → sweep retries GENERIC_GUARD/UNCOMPRESSIBLE files automatically.
- **Sub-encoder router lives INSIDE the pack, not in the registry.** The AST algo field
  is on-disk namespace (6 bits); which encoder produced a blob must not leak into it —
  the blob is a valid PNG regardless, decode doesn't care. Registry has one `pngr` entry;
  the pack's `pngr-route {in} {out}` tries sub-encoders in manifest `prio` order (with a
  time budget), bit-exact guard inside the pack, emits the smallest valid result.
- **Neural parameter predictor = pack-local, drop-in** (user idea: NN predicts PNG
  encoding params → priority table → right settings chosen faster). The FS side needs
  NOTHING from it: the predictor replaces the static `prio` table inside the pack's
  router. Applies to PNG (filter strategy/chunking/level), FLAC (apodization/LPC),
  later video. Not applicable to PPMD text (fixed params). v1 = static prio.
- Process isolation stays: encoder crash ≠ FS crash; RLIMIT_AS=dec_mem_limit gives real
  runtime memory enforcement (exceed → killed → guard → generic fallback).

Pack = tar/tar.xz archive OR directory:
```
<name>.codecpack/
  manifest            # key=value; fields map 1:1 onto invfs_codec
  bin/<helper> ...    # any language
```
Manifest keys: `name`, `algo` (INVFS_ALGO_*), `pack_version`, `caps` (seek|batched|
wholefile|container|external), `dec_mem` (bytes), `generation` (u16), `sniff.magic` (hex)
+ `sniff.offset`, `sniff.ext` (comma list), `encode`/`decode` = fixed argv with
`{in}`/`{out}` placeholders (no shell), repeated `subencoder=<id> <path> prio=<n>` lines.

probe() search order: `$INVFS_CODECPACKS` (colon-separated) → `/usr/lib/invfs/codecpacks`
→ PATH (tool names from manifests). Manifest deserializes into an invfs_codec entry; a
future `type=dlopen` key can fill encode/decode via dlsym without registry changes.

Future container candidates (theory, v2+): filesystem-image containers — an NTFS/ext4
.img decomposed via MFT/extent parse into per-file members that recursively flow through
the WHOLE codec pipeline (VM images lose cross-file context when compressed as raw
sectors; wimlib-vs-sector-image is the existence proof). ext4 first (open spec).
MAX_AST_DEPTH=16 already admits the recursion shape. Bit-exact rebuild must capture
slack/journal state byte-exact — heavy parser, hence v2+.

Security note: packs execute at sweep/mount time, possibly as root — fixed argv only, no
shell interpolation; Landlock/seccomp for helpers is a later hardening phase.

## 11. Dedup interplay (audit PB7 adjacency)

- TEXT members reuse the offline-dedupe L2P-dup trick; member delete MUST NOT free
  zone==TEXT blocks (anti-PB7 pattern by design; GC owns batch blocks).
- The sweep dedupe pass MUST skip zone==TEXT entries: batches are already shared, their
  pbas belong to the owner inode, and they are never dedup candidates.
- ~~PB7 itself (BINARY dedup delete frees shared blocks) stays open (old roadmap WP6);
  WP10 neither fixes nor worsens it.~~ CLOSED 2026-08-28: vol_retire_inode (now
  vol_records.c) skips any pba another live MAP entry references (two pba-indexed
  bitmaps per retire); refcounts were never needed. WP6 is moot.

Open defaults (accepted unless vetoed): owner name `\x01tzb`; GC v1 reclaims only fully-dead
batches; batch size guard = `ppmd < raw` (no ZSTD comparison, ~3s/4MB too expensive);
UNCOMPRESSIBLE threshold 0.5% via INVFS_MIN_GAIN_PCT env.

## 12. Nested containers under memory policy (user questions 2026-08-26)

Scenario: zip[zip[huge_png, small_png]] — policy must admit small_png and reject
huge_png independently, and matryoshkas must never force reconstruction through a thick
(non-seekable, large) decode unit.

Two container storage shapes already exist in the codebase:
- **windows** (ZIP): original bytes kept verbatim, members are (method,offset,len) views;
  per-member read cost = that member's decode only (deflate windows are seekable, WP5);
- **extraction** (TAR/GZ/PNG/FLAC): members become sibling inodes ("name!partN"); the
  original is rebuilt on demand from recipe+parts.

Rules:
1. **Per-member admission at every nesting level.** huge_png rejected, small_png
   processed — the decision is made per member; rejection keeps that member
   raw/verbatim, siblings unaffected.
2. **The budget that can overflow is the DECODE-TIME working set** (user m0085): e.g. a
   PNGR member inflates to raw pixels at read time. Admission must compute the decode
   working set **from cheap headers, never by trial decode**:
   - PNG IHDR → raw pixels ≈ w·h·channels·bitdepth/8 (PNGR part decode unit);
   - zip central directory → member csize/usize without decoding;
   - xz stream header/footer → dict size (decode mem ≈ dict + overhead);
   - zstd frame header → window size (decode mem ≈ window + unit);
   - PPMd props → model size (our wrapper: fixed 64 MB) + unit.
   Registry: `dec_mem_bytes` is the constant case; add optional
   `dec_mem_of(head, len, usize) → bytes` for size-dependent codecs (xz/zstd/png).
   Composed member cost = Σ over enclosing layers: a SEEKABLE layer (zip window)
   contributes only the member slice + inflater window (~32 KB); a NON-SEEKABLE layer
   (gz stream) contributes the whole layer stream. If the composed working set exceeds
   dec_mem_limit → the member keeps a cheaper form (verbatim window / generic
   per-segment ZSTD with a bounded 64 KB unit).
3. **"Light" xz/zstd levels (m0088, optional):** light compression levels mean small
   dict/window ⇒ small decode working set ⇒ such members pass admission where heavy ones
   fail. The header-driven estimate above is exactly what distinguishes them; per-member
   xz/zstd processing inside containers itself is v2 (needs the extraction-container
   flow), the estimate hook is v1.
4. **Probing is cheap:** the container directory yields member names+sizes without
   decoding — a new optional registry hook `enumerate(blob, cb)` generalizes
   vol_zip_parse_children; a member head for sniff = a few-KB decode of a seekable
   window.
5. Depth guard MAX_AST_DEPTH=16 stands; add a total-member sanity bound.
6. **v1 boundary:** window members are NOT individually codec-processed (a zip member has
   no inode → no class flag, no sweep visibility); per-member treatment happens only via
   extraction containers (parts are inodes — class flag + policy apply as usual). The
   container as a whole still gets class CONTAINER.
7. ~~Sibling parts ("!"-names) are excluded from TEXT batching in v1~~ — the v2 revisit LANDED as
   WP14b: parts of a freshly-exploded container defer into THIS run's accumulators
   (vol_sweep.c:defer_container_parts), and the walk's absent-stamp branch re-batches parts stored
   per-segment generic (binary batches too, WP14a). The coupling concern is handled by the flush
   re-reading each part's live record before sealing it.
8. **Async decode-ahead (m0089, design-only for v1):** when the budget allows (decoded
   unit ≤ arc_limit/2 per the ARC refusal rule; decode working set ≤ dec_mem_limit),
   reads may prefetch — touching one TEXT member already decodes and ARCs its whole
   batch; sequential patterns can additionally background-fill ARC with next units.
   Natural implementation point: a readahead helper in the fuse layer calling
   vol_read_range/vol_read_file ahead of the cursor.
