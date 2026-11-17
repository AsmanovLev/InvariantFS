# ADR-013: Computed (View) Inodes — Base Spec + Delta Overlay, Materialize on Sweep

## Status

Proposed — 2026-11-17

Independent of [ADR-012](ADR-012-verbatim-space-dedup-chunk-index.md) (that one is about
dedupe identity); composes with [ADR-010](ADR-010-cross-vm-dedup-via-windowed-recipes.md)
(the reference primitive) and with the existing delta log.

## Context

A user has a library in one canonical compact form — say APE — and does **not** want to
store the other representations (WAV, FLAC). They want those to *exist* in the namespace
and to cost zero bytes until somebody actually reads them.

Almost all of the substrate already exists:

| piece | what it gives us | where |
|---|---|---|
| recipe = AST of opcodes with `algo` + `repro_*` params | "how to build bytes", already a program | `invarifs.h` (`invfs_ast_block_entry`) |
| `INVFS_ALGO_WINDOW_SRC` | a file as bytes of another inode (a view), tested | ADR-010, `vol_read.c:749` |
| delta log + overlay | per-inode append-only modifications over a base | `vol_delta.c`, `vol_cut.c` |
| `helper_exec` + Landlock sandbox | running an external encoder/decoder safely | `helper_exec.c` |
| ARC | cache of decoded content | `vol_read.c` |

What does not exist is a decision about **what a computed inode is**, and that decision
has three hard constraints which are the whole point of this ADR.

### Constraint 1: an encoder is not deterministic unless it is pinned

`decode` is deterministic by definition (the format specification says what the bytes are).
`encode` is deterministic only for a **fixed implementation**: zstd level 3 vs 19 produced
different bytes for the same input (`BENCH-MERKLE-DEDUP.md`), FLAC/APE versions differ,
and **GPU encoders are non-deterministic in principle** — a different driver, GPU or
scheduling order produces different bytes, sometimes run to run.

A file whose bytes change under the user breaks checksums, backups, `rsync`, and any
downstream tooling. So a computed inode must freeze its transform.

### Constraint 2: `stat` must not lie

`size` cannot be "unknown while reading": tools `stat` before reading. The output size of
an encode is not knowable without running the encoder, so composition has to run it once
at creation time.

### Constraint 3: a view is not a datum

The FLAC we generate will not equal the FLAC the user's own `flac` binary would produce.
If a file must exist independently of our tools, it has to be **materialized**. This has to
be said in the documentation, not discovered during a restore.

## Decision

### 1. A computed inode = { base spec, delta overlay }

```
inode
 ├── base spec  : transform(streams, params, pinned version) -> bytes
 └── delta      : the ordinary per-inode delta log overlay

read(off, len)  = apply( decode(base), delta )
write(off, buf) = append to the delta log      (no special path)
```

The base is never stored; the overlay is ordinary delta, so every existing property of the
delta layer (crash consistency, fold, retire, pba refs) applies unchanged.

### 2. The base spec records everything needed to reproduce the bytes

- source streams by **content reference** (inode + range, or a chunk hash once ADR-012
  lands — the spec does not care which, it is an indirection);
- transform identity: codec id, **implementation version**, parameters;
- output `size` and the output Merkle root, both computed **once at creation** (one encode
  run) so `stat` answers immediately and the content has a stable identity;
- whether the transform is `STABLE` (pinned software implementation) or `VOLATILE` (anything
  that cannot be pinned).

### 3. Materialization is a sweep/fold decision, never a read-time surprise

Triggers, all of which already exist as machinery:

| trigger | why |
|---|---|
| `fold` merging the delta into the base | a computed inode has no on-disk base to fold into, so fold **is** materialization |
| delta size beyond a share of the base | rewriting the base once is cheaper than replaying a long overlay |
| the source is about to be compacted away | the derivation is at risk; materialize first or refuse |
| explicit `invf-sweep --materialize[=all|<name>]` | operator decision |

Materialization runs the pinned encoder under `helper_exec`, writes a normal recipe, and
the inode stops being computed — i.e. it stops depending on our code and on any codec
version.

### 4. Deletion and lifetime are refcounts, with a keeper rule

- the spec holds references to its sources (same discipline as `WINDOW_SRC` protecting its
  source, ADR-010 §5);
- a source with live dependents must not disappear. Default policy: **keeper name** — the
  source moves under an internal owner name (`\x01…`, the shape batch owners already use)
  and dies only when the last dependent goes. Alternative where keeper names are
  unacceptable: `EBUSY` and let the operator decide;
- deleting the computed inode releases its references; a source whose refcount reaches 0
  becomes free through the normal paths.

### 5. Determinism contract: decode views always, encode views only pinned

| operation | example | deterministic | verdict |
|---|---|---|---|
| decode / view | AV1 → frames, APE → WAV, ZIP member → file | yes, by specification | always allowed |
| encode, pinned | WAV → FLAC via a **software** encoder at a recorded version | yes, for that implementation | allowed, marked `STABLE` |
| encode, GPU or unpinned | frames → AV1 on GPU, "whatever `flac` is installed" | no | **forbidden** as a base spec |

Consequences of the rule, stated plainly:

- storing video in its compact form and exposing a decoded view is safe (decode only);
- transcoding on read to another *encoded* format is allowed only for a pinned software
  encoder, and it is CPU per read (cached in ARC, materialized by policy);
- a `STABLE` view's bytes are frozen; re-composition happens only on explicit request,
  never "during the next sweep".

### 6. Opt-in, and degradation must be loud but harmless

`INVFS_VIRTUAL=1` gates the whole feature. Without it a computed inode is not created and
an existing one reports a clear error. If the pinned encoder is missing at read time, the
file fails to read with a diagnostic — it is **not** silently materialized from a different
encoder, because that would change its content.

## Consequences

### Positive

- one canonical form stored, many representations visible: the storage the user explicitly
  does not want (duplicate FLACs) costs nothing;
- writes are ordinary delta writes, so the feature adds no new write path and inherits
  crash consistency, retire, and fold;
- materialization is decided where there is time to decide (sweep/fold), not on the read
  path;
- composes with ADR-012: a source stream is content-addressed either way, so a computed
  inode's sources can be deduplicated like any other content.

### Negative

- a read may now execute an external program: latency, and a dependency on a binary that
  can be removed from the machine. The declarative answer is `STABLE` + a loud failure, not
  a fallback;
- `du`/`df` reporting becomes ambiguous: the view's logical size is visible but its cost is
  zero. Reporting must say so explicitly rather than pretend;
- the namespace grows objects whose content depends on our codec registry, which is a
  support surface (a "why is this FLAC different from mine" question that must be answered
  by documentation, not by surprise);
- chains (a view over a view) need a depth cap and cycle rejection.

### Out of scope

- GPU encoders in any form (non-deterministic by nature);
- "similar content" (fuzzy) matching — a view is exact by construction, not approximate;
- automatic materialization of every view at sweep time: that defeats the purpose and is
  opt-in only.

## Alternatives considered

| alternative | verdict |
|---|---|
| Materialize eagerly at creation | Rejected: it stores exactly the duplicates the user refused to keep. |
| Symlinks / hardlinks to a canonical file | Rejected: they cannot change format, so the user's player still cannot open a FLAC view. |
| A FUSE-side overlay outside the FS (a userspace generator) | Rejected: loses namespace integration, crash consistency, refcounts and dedupe; two tools would disagree about what exists. |
| Generate bytes at every read without caching | Rejected: FLAC/AV1 encode per read is minutes of CPU per file; ARC + materialization policy exist for this. |
| "Whatever encoder is installed" (no pinning) | Rejected: the file's bytes would drift with the system, breaking checksums and backups. |

## Testing plan

1. **Pinning** — a spec records version; a missing/mismatched encoder fails loudly and
   never substitutes.
2. **Composition** — `read` of a computed inode over a delta overlay equals
   `decode(base) ⊕ delta` byte for byte, including holes and torn-tail cases already
   covered for ordinary files.
3. **Materialization equivalence** — after `--materialize`, the file reads **bit-identically**
   to what it read before, and no computed spec remains.
4. **Deletion** — source deleted with a live dependent: keeper name appears, dependent still
   reads; last dependent deleted: keeper disappears; refcounts reach 0 exactly once.
5. **Fold** — folding an inode with a delta triggers materialization, not a recipe rewrite.
6. **Reporting** — `du`/`stat`/`invf-stats` show the view as zero-cost and say so.

## Open questions

1. Where the base spec lives: a new recipe opcode (`INVFS_ALGO_COMPUTED`) with the spec in
   the recipe stream, or a `!spec` sibling inode? The opcode keeps one read path; the
   sibling keeps the recipe small.
2. Creation cost: one encode run for `size`/root — acceptable per file, but what about
   composing thousands of views at once (a whole library)? Probably a background queue.
3. ARC eviction for decoded views: current ARC keys on inode id; a view's decoded output is
   large (WAV of a whole album), so it needs its own budget.
4. Should a view be allowed to reference another view (depth 2), or is one level enough?
5. How does `scrub`/`verify` treat a computed inode — verify the sources and the root, or
   re-run the encoder?
