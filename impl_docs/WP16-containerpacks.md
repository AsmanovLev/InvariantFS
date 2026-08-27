# WP16: Containerpacks — external container decomposition (WP16a)

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
