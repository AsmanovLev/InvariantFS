# WP16: Containerpacks — external container decomposition (WP16a)

ABI v1.1 (WP16b) adds **seekable containers** (the `map` command + local
splice reads), the **DEFER_ENOSPC** storage class, and **codec profiles**.
See "ABI v1.1" below; the v1 text is the foundation and still holds.

A **containerpack** is a codecpack (WP10 §10, WP13) that *decomposes* a
container file into member inodes instead of transcoding it whole. The
members become first-class files and flow through the ENTIRE normal
pipeline: RAW -> text PPMd batching / binary ZSTD(+BCJ) batching / generic
ZSTD-19, nested container decomposition included, recursively. The FS
learns nothing about the container format; the pack owns it through four
commands.

Fixture pack: `tools/codecpacks/splt_test.codecpack/` (trivial SPLT format:
`"SPLT"` + u32 n + n x u64 lengths + concatenated payloads). E2E:
`tools/test-containerpack.sh`. Unit: `test_packs` in `src/codec_test.c`.

## 1. Manifest ABI

A pack directory `<name>.codecpack/` with a `manifest` (key = value).
`type = container` selects the container ABI; absent = the WP13 codec ABI.
A container pack declares the FOUR decomposition commands instead of
`encode`/`decode` (a container manifest carrying encode/decode registers
them but they are never called; a container manifest missing any of the
four commands does not register at all):

```
name = rawdisk
type = container
algo = 16                 # on-disk AST algo id (6 bits, 0..63; must be free)
pack_version = 1
generation = 1            # bump to re-arm GUARD-stamped retries (WP10 §2)
caps = container|external # CONTAINER|EXTERNAL|WHOLEFILE are forced anyway
dec_mem = 0               # constant decode working set; 0 = use the default
sniff.magic = ...         # hex, optional sniff.offset
sniff.ext = img, ...      # comma list
enumerate = python3 {pack}/rd.py enumerate {in} {out}
extract   = python3 {pack}/rd.py extract {in} {idx} {out}
strip     = python3 {pack}/rd.py strip {in} {out}
rebuild   = python3 {pack}/rd.py rebuild {recipe} {dir} {out}
estimate  = python3 {pack}/rd.py estimate {in}     # OPTIONAL (WP13 grammar:
                                                  # prints a bare byte count)
```

Command contract (fixed argv, no shell; exit 0 = ok, anything else =
refuse/fail; scratch files live in a fresh mkdtemp dir per operation):

| command   | argv                          | semantics |
|-----------|-------------------------------|-----------|
| enumerate | `{in} {out}`                  | writes the member table to {out}: one line per member, `idx<TAB>suggested_name<TAB>usize` |
| extract   | `{in} {idx} {out}`            | writes member idx's raw bytes to {out}; the FS stats the result and refuses the file if it is not exactly `usize` bytes |
| strip     | `{in} {out}`                  | writes the recipe to {out}: everything needed to rebuild besides the member bytes; pack-owned format, the FS never parses it |
| rebuild   | `{recipe} {dir} {out}`        | {dir} holds one file per member named by its decimal idx; must reproduce the ORIGINAL bit-exact |
| estimate  | `{in}`                        | optional; prints the decode working set in bytes (header-derived, never a trial decode) |

Placeholders `{in} {out} {idx} {dir} {recipe} {pack}` substitute per argv
token; unused slots substitute as empty strings. `probe()` resolves argv[0]
of all four commands (+ estimate, + `requires`) — a pack whose tools are
missing is skipped (the file waits RAW and unstamped).

Limits and validation (FS side, `cpack_parse_table`): at most **65536
members**, idx values **0..65535**, unique; blank lines tolerated;
suggested_name is sanitized to `[A-Za-z0-9._-]`, 24 chars max, and is
advisory only — members are identified by INDEX. `usize` is NOT bounded by
the container's size (a compressing container can hold members larger than
the archive); the sum is overflow-checked. Honesty is enforced where the
bytes move: the sweep stats every extracted member against its announced
usize, the read side compares what it read against it.

## 2. Sweep pipeline (volume.c `vol_containerpack_sweep`)

Placement in `vol_sweep_one`: AFTER every builtin container magic
(ZIP/TAR/GZ/PNG/FLAC, and the MP3 codec branch), BEFORE the WP13
whole-file codec-pack loop / text / generic. First sniff hit wins.

1. sniff hit + probe -> enumerate (any failure: fall through, NO stamp);
2. admission (WP10 §12, sweep-time only): container size vs the ARC budget
   (`INVFS_ARC_BYTES`; the rebuild is a whole-file read unit), then the
   decode working set vs `INVFS_DEC_MEM_LIMIT` (default 512 MiB) — the
   pack's estimate command if present, else the manifest `dec_mem`
   constant, else the ABI default `sum(member usize) + container size`
   (the recipe is not in hand yet and the container's size bounds it; the
   default also matches the read path's true peak: the whole-file output
   buffer plus one member in flight). Over -> stamp
   `GENERIC_MEMLIMIT{algo, gen}`, fall through. An estimate command that
   cannot size the job stamps `GENERIC_GUARD{algo, gen}` (the WP13
   estimate convention);
3. strip -> recipe;
4. per member: extract into the scratch dir as `<idx>`; the produced size
   must equal the announced usize (a lying pack refuses the file);
5. GUARD (the house 1:1 invariant): rebuild over recipe + extracted
   members, memcmp against the original BEFORE anything reaches disk. Any
   failure abandons the decomposition silently — no pack stamp; the file
   falls through to text/generic and the stamp it earns there is terminal
   until the content changes;
6. commit, children first: members as `<name>!mbr<NNNN>`[-sname] RAW via
   vol_create_file (member meta uid/gid/mode copied from the container
   record), then the member table verbatim as `<name>!mbrt`, then the main
   record replaced by the recipe blob (vol_create_blob_file: one BINARY
   whole-file segment, algo = the pack's, orig_size = container size);
   retire the old record last; stamp `CONTAINER{algo, gen}`. Mid-commit
   failure purges the created siblings (`vol_transcode_abort`); a leftover
   `!mbrt` from a killed run is purged before the next attempt;
7. the fresh members defer into THIS run's batching accumulators
   (WP14b pattern: head sniff -> `tz_defer`/`bz_defer`; the flush
   re-reads the live records).

Return convention mirrors `vol_pack_sweep`: `100+algo` on commit, `1` =
tools absent (wait RAW, unstamped), `0` = declined (fall through;
`GENERIC_*` stamps carry the retry semantics). invf-sweep prints
`<name>: <pack> (codecpack)` for rc >= 100.

Name budget: the pack branch declines when `strlen(name) + 40 >
INVFS_MAX_NAME` (255) — `!mbr` + 4 digits + `-` + 24 sname; this doubles
as the nesting depth guard (each nesting level costs >= 9 name bytes, and
`vol_sweep_one` skips names > 240 anyway).

## 3. Read path

The recipe record's algo maps to the pack. `algo_is_whole_file` covers it
(WHOLEFILE is forced at registration), so `vol_read_range` diverts to the
inode-keyed ARC whole-file path. `vol_read_inode` -> `pack_container_rebuild`:

1. read the member table `<name>!mbrt` through vol_read_file (it is an
   ordinary inode — batched or generic, decoded transparently);
2. materialize `<idx>` files in a scratch dir, reading each member sibling
   THROUGH ITS CURRENT STORED FORM (vol_read_file — the pack's extract
   cannot help, the original container no longer exists; a member may
   itself be a decomposed container and rebuilds recursively);
3. exec `rebuild`, check the output size == the recorded file size, hand
   back the bytes (ARC-cached by inode id).

A missing/corrupt member, a bad table, or a pack error fails the read
LOUDLY: `-1` -> the FUSE boundary maps it to **EIO** (`fuse_fs.c` read
path), `invf-cat` prints the cause + `read failed` and exits 1. A read
with the pack NOT LOADED hits the WP13 missing-pack branch ("algo N
requires a codecpack that is not loaded") — same loud failure. The 1:1
invariant is never served silently.

## 4. Delete / rename

`record_owns_siblings` answers 1 for a pack-container algo (and for an
algo no loaded pack can resolve — conservative), so `vol_unlink` /
`vol_replace_file` retire every `<name>!...` sibling — members, the table,
and nested grandchildren (prefix match), mirroring TAR. Rename moves the
siblings with the container (the existing `!`-aware walk).

## 5. Storage shape recap

| piece | name | storage |
|-------|------|---------|
| main record | `<name>` | recipe blob, one BINARY whole-file segment, algo = pack's, CONTAINER{algo,gen} |
| member i | `<name>!mbr<NNNN>`[-sname] | ordinary inode (RAW -> batching / generic / nested container) |
| member table | `<name>!mbrt` | enumerate output verbatim, ordinary inode |

## 6. Coverage

- `codec_test` (`test_packs`): container registration, the four commands
  exposed via `invfs_pack_def`, caps forced CONTAINER|EXTERNAL|WHOLEFILE,
  no encode/decode trampolines, a container pack missing a command is not
  registered, registry order (builtin magics first, packs after, text
  LAST), probe/reset unload.
- `tools/test-containerpack.sh`: multi-member container with an empty
  member, guard refusal on a corrupt table (falls to generic, no
  siblings), same-run member batching (TEXT + BATCHED_BIN stamps),
  verify --deep, sha256 bit-exact containers AND direct member reads,
  pack-absent read fails loudly, nested container decomposition on a
  later sweep with rebuild-through-rebuild read-back, idempotent
  re-sweep, delete cascade incl. nested grandchildren, fsck clean,
  `INVFS_DEC_MEM_LIMIT` admission leg (GENERIC_MEMLIMIT{algo,gen},
  bit-exact generic read).

## 7. Pack-author gotchas

- **rebuild must be bit-exact.** The FS memcmps the guard rebuild against
  the original at sweep time and abandons the file on ANY mismatch (no
  stamp, no siblings, generic storage). Test your rebuild against junk
  tables too: the FS treats a mismatch as "the pack is broken for this
  content".
- **enumerate's usize column is the contract.** extract must produce
  exactly that many bytes per member; the read path fails the whole
  container loudly if a stored member's size disagrees later.
- **Members are identified by index**, not name: `<dir>/<idx>` at rebuild,
  `<name>!mbr<NNNN>` on disk. The suggested_name only rides inside the
  sibling name (sanitized, 24 chars) for human/debug orientation.
- **idx need not be dense** — the FS iterates the table, not 0..n-1.
- **Zero members = decline** (the FS refuses an empty table; a 0-member
  container goes generic — it is tiny by definition). Zero-length members
  are fine.
- **estimate is optional but load-bearing for policy.** Without it the FS
  uses `sum(usize) + container size` as the working set; with a
  compressing container (members larger than the archive) an honest
  estimate keeps big archives admissible.
- **Stream, never mmap-the-world.** Members may be huge (partitions):
  read `{in}` in windows, write `{out}` incrementally. The FS hands you
  file paths exactly so that no member has to fit in RAM on YOUR side.
- **Exit codes are the only channel**: 0 = ok, anything else =
  refuse/fail. Stdout/stderr are /dev/null (except estimate's stdout).
- Scratch dirs are fresh per call — never read a leftover.

# ABI v1.1 (WP16b): seekable containers, DEFER_ENOSPC, profiles

## 8. The `map` command (seekable containers)

A container pack may declare one more command:

```
map = python3 {pack}/rd.py map {in} {out}
```

Its presence puts `INVFS_CODEC_CAP_SEEK` on the registry entry (the ONLY
source of that bit for a container pack — a `caps` line claiming `seek`
without a map command is stripped at registration; a `map` line on a
codec pack is parsed but never wired). probe() covers the map tool like
the other four. The map lets the FS serve ANY byte range of the original
container WITHOUT running the pack: no rebuild exec, no whole-file
reconstruction, and — the point — **reads keep working with the pack
uninstalled** (the map on disk is self-describing).

### The map format (FS-owned, binary, little-endian)

The pack renders it; the FS defines, validates and owns it:

```
[4B "MRMP"][u32 count]
count x { u64 orig_off, u64 len, u8 kind, u32 idx, u64 src_off }  (29 B)
```

Entries are sorted by `orig_off` and must partition `[0, container_size)`
exactly: contiguous, no gaps, no overlaps, `len > 0` (a zero-length
member simply has no entries). `kind` 0 = RECIPE (the bytes live at
`[src_off, +len)` of the recipe blob; `idx` must be 0), 1 = MEMBER (the
bytes are member `idx`'s `[src_off, +len)`; `idx` must exist in the
member table, `src_off + len <= usize`). The blob length must be exactly
`8 + count * 29`; count is capped at `4 * CPACK_MAX_MEMBERS + 4`
(262148). For SPLT the map is trivial: one RECIPE range for the header,
one MEMBER range per non-empty member (members are contiguous chunks).

### Sweep flow with a map (vol_containerpack_sweep)

enumerate -> admission (arc / dec_mem, unchanged) -> DEFER_ENOSPC (§9) ->
strip -> extract (size-checked) -> **map: parse + validate the shape**
(partition, recipe/member bounds) -> commit members RAW + `!mbrt` ->
commit the recipe record (the name flips to it; the old RAW record is NOT
retired yet) -> **map guard**: every map entry's source range is read
back THROUGH THE REAL READ PATH (the recipe segment of the fresh record,
member siblings via `vol_read_range`) and chunked-memcmp'd (8 MiB
windows) against the original — streaming, constant memory, **no rebuild
exec** (map-less packs keep the v1 rebuild-exec guard) -> store `!mbrmap`
LAST via `vol_create_file` (it compresses itself later) -> retire the old
record -> stamp CONTAINER{algo,gen}.

`!mbrmap` lands last on purpose: its presence on disk is the "the guard
passed" marker, so a crash mid-commit can only leave a container that
falls back to the pack's rebuild exec (or fails loudly with the pack
absent) — never one that serves an unguarded map. A guard/write failure
rolls the name back onto the untouched old record and purges every
sibling: abandon to generic, no pack stamp, exactly the v1 guard
semantics. Map-capable packs skip the rebuild guard entirely.

### Read path (seekable)

`algo_is_whole_file` excludes CAP_SEEK container packs, so
`vol_read_range` no longer diverts them to the whole-file ARC rebuild.
Instead (both in `vol_read_range` and in the whole-file `vol_read_inode`
pack branch): pack algo + CAP_SEEK, OR an algo no loaded pack resolves,
PLUS a live `name!mbrmap` sibling -> `cpack_map_read`:

1. load-or-cache the parsed map + member table (idx-sorted) + the recipe
   blob (segment read, CRC-checked), re-validated against the live record
   (it is disk data now); cached per open volume keyed by container name
   (freed at vol_close; retiring the container or any "name!..." sibling
   invalidates the entry);
2. serve the range by walking the map: RECIPE ranges copy from the cached
   recipe, MEMBER ranges `vol_read_range` the member sibling (batched /
   generic / nested-container reads all just work — a nested seekable
   container splices through its own map recursively).

NO pack exec on read, ever. A missing `!mbrmap` (a pre-v1.1 sweep, or a
deleted sibling) falls back to the whole-file rebuild exec when the pack
is loaded, and fails loudly (EIO) when it is not — the v1 semantics. A
map present but invalid at load fails the read LOUDLY (like a corrupt
member table); the sweep-time guard makes this unreachable for content
the FS itself wrote.

The WP16a admission rules are unchanged in v1.1 (a seekable container
still obeys the whole-file ARC budget and the decode working set at sweep
time; relaxing them for the map shape is future work — a pack with a big
but honestly estimable container should ship an `estimate` command).

## 9. INVFS_CLASS_DEFER_ENOSPC (9)

A sweep that must hold the NEW shape while the OLD one is still stored
has a worst-case space price; paying it only to unwind at a mid-commit
ENOSPC is wasted work. The containerpack sweep (and the codec-pack sweep,
and the generic ZSTD-19 sweep) now price it up front against
`vol_free_blocks_cached()` and, when short, stamp
`DEFER_ENOSPC{algo, gen}` and wait RAW — silently (the tools-absent
convention), never falling through to generic.

The heuristic (documented in `sweep_enospc()`): the containerpack sweep
requires `free_blocks >= (sum(member usize)/2 + 64 MiB) / 4096` — member
csizes are unknowable pre-write (they compress through the pipeline
later), so the charge is a conservative fraction of the announced total;
the 64 MiB margin covers the recipe/table/map and the records. The
codec-pack sweep charges the encode scratch bound (full + full/4 + 64K)
plus the margin; the generic sweep charges the original's own size raw
plus per-segment framing plus the margin.

DEFER_ENOSPC is re-evaluated EVERY sweep (the `vol_sweep_one` class
predicate breaks straight into the full path, like an absent stamp):
space now suffices -> the file processes; still short -> re-stamp, which
the check-then-write stamp makes free. It names the declining codec
(pack algo, or ZSTD for the generic floor) so the sweep log can
attribute the wait.

## 10. Codec profiles (INVFS_PROFILE)

Four effort levels — `fast` / `balanced` / `dense` / `archive` — which
are also the future heat scale (hot<->fast, warm<->balanced,
cold<->dense, frozen<->archive). `vol_open` parses `INVFS_PROFILE`
(default `balanced`; an unknown value keeps the default with a
complaint), stores it on the volume (`vol_get_profile`), and publishes
the effective name back to the environment, so every pack subprocess
(exec'd with fixed argv from the sweep/read paths) inherits it and pack
helpers can map effort. mkfs/sweep CLIs inherit the env — no CLI flags
in v1; invf-sweep logs `profile: <name> (generic zstd level N)` when the
env is set (unset: no line, byte-identical logs).

v1 effects: (1) the generic sweep's ZSTD level maps fast=6,
**balanced=19 (the historical default — an unset env reproduces existing
volumes' bytes)**, dense=22, archive=22 (archive reserves the slot for a
future LZMA2 backend swap, not a higher zstd level); (2) the
`INVFS_PROFILE` env for pack execs. **PPMd wrapper params stay fixed
(o8/64M) regardless of the profile in v1**, and the text/binary batch
payloads keep the registry ZSTD level.

## 11. Pack-author recipe for a seekable pack (v1.1)

Everything from §7 still holds. To make reads stop needing your pack at
runtime:

1. Add `map = <tool> {in} {out}` to the manifest. The tool reads the
   ORIGINAL container and writes the MRMP map (§8): which stored source
   reproduces each byte range — the recipe blob (what your `strip`
   writes) for container metadata, a member idx + offset for member
   payload. Members need not be contiguous in the original; a member
   fragmented across the archive is several MEMBER entries with rising
   `src_off`s, and recipe/member runs may interleave freely as long as
   `orig_off`s partition `[0, container_size)`.
2. Registration adds CAP_SEEK; the sweep validates the map's shape, then
   proves it byte-exactly by reading every source range back through the
   real FS read path and memcmp'ing against the original — a map that
   lies is a guard refusal (abandon to generic, no stamp), exactly like a
   rebuild mismatch. **Test your map against junk tables too.**
3. Keep `rebuild` anyway: it is the fallback when the map sibling is
   absent (a pre-v1.1 sweep, a deleted `!mbrmap`), and registration still
   requires the four v1 commands.
4. The map command must stream like the others (a member may be a
   partition); the FS side never holds more than the recipe + one 8 MiB
   window.

(The 4-pack wave — rawdisk/ext4/fat/xfs — builds on this recipe: their
maps are filesystem-structure walks rendered to MRMP runs.)

# WP16e: builtin -> pack migration (the jxl.codecpack proof)

The JPEG->JXL lane (WP11) was the last whole-file codec hardcoded in
volume.c; WP16e moved it to `tools/codecpacks/jxl.codecpack/` (algo 4)
without changing the on-disk shape or the class-stamp semantics, proving
the migration pattern every remaining builtin external (pmp/ape/wv) can
follow:

1. **The pack claims the builtin's algo.** A manifest whose `algo` names a
   builtin **EXTERNAL placeholder** (sniff+probe only: pmp/jxl/ape/wv)
   **overrides** it — the placeholder drops out of the materialized
   registry and the pack entry (manifest sniff, dec_mem, generation, the
   encode/decode trampolines) becomes the algo's only entry. Precedence
   rule as implemented in `pack_register`: **pack wins over
   builtin-external**; builtin **stream** codecs (NONE/LZ4/ZSTD/PPMD) and
   builtin **CONTAINER** entries (zip/tarr/gzr/pngr/flacr/exer) can never
   be claimed — a manifest naming their algo is silently skipped;
   pack-vs-pack collisions keep the first-registered (readdir order).
2. **The placeholder earns CAP_PACKONLY** (`INVFS_CODEC_CAP_PACKONLY`,
   codec.h): with no pack loaded, the sweep DEFERS sniff-positive content
   RAW and unstamped (vol_sweep_one's WP13 loop) instead of letting it
   fall to the terminal generic floor; the first sweep with the pack
   installed picks it up. The defer only fires when no *real* pack claimed
   the content (builtins sit before packs in registry order, so the loop
   remembers the placeholder hit and decides after the pack loop ran).
3. **The builtin sweep branch is deleted**; the generic pack path
   (`vol_pack_sweep`: probe -> `estimate` admission -> encode ->
   decode-back memcmp guard -> size guard -> blob -> CODEC{algo,gen})
   carries the file, and the class-predicate retry (vol_jxl_retry for
   GUARD/MEMLIMIT{JXL}) re-enters through the same `vol_pack_sweep`. For
   jxl the manifest is direct argv (`cjxl {in} {out} --lossless_jpeg=1` /
   `djxl {in} {out} --output_format=jpeg` — the flag replaces the
   extension the FS scratch paths do not carry) plus a C SOF-walk
   `estimate` helper (`jxlest`, prints w*h*3, 0 = geometry unknown).
4. **Reads dispatch through the registry**: algo-4 blobs decode via the
   pack trampoline when the pack is loaded (the WP13 read branch's shape);
   the builtin djxl wrapper remains as the pack-absent fallback so
   pre-migration blobs and EXER-carved JXL parts stay readable.

WP12(d) landed with the same change: every POSIX tool child now runs under
RLIMIT_AS — a pack with manifest `dec_mem` > 0 gets
`max(2*dec_mem, 256 MB)`, everything else (builtin tool callers, packs
without dec_mem) the 2 GB default — and the pack exec resolves a bare
argv[0] through `$INVFS_TOOLS` -> `/usr/lib/invfs/tools` -> PATH, matching
the probe (before, probe could pass on INVFS_TOOLS while exec ran the PATH
tool of the same name). E2E: tools/test-jxl.sh (parity legs + guard /
tool-absent / pack-absent legs + an RLIMIT-kill fixture pack).

