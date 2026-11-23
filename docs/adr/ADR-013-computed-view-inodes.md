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

### 2. The base spec lives in the RECIPE, as a new opcode

Decided: `INVFS_ALGO_COMPUTED` is a recipe opcode, and the spec is carried in the recipe
stream itself — not in a `!spec` sibling.

Why the recipe and not a sibling: the recipe is already the per-inode program
(`algo` + `repro_*` params + children), so a computed inode changes nothing about the
read path — one dispatch, one place where a spec can be wrong. It is also atomic with the
file's own metadata: the recipe is content-addressed by `recipe_addr`, so changing the
spec changes the address (natural invalidation), and there is no second inode that can
drift out of sync or be deleted on its own. A sibling would buy a smaller recipe at the
cost of a second object to keep alive, refcount and validate.

The spec records everything needed to reproduce the bytes:

- source streams by **content reference** (inode + range, or a chunk hash once ADR-012
  lands — the spec does not care which, it is an indirection);
- transform identity: codec id, **implementation version**, parameters;
- output `size` and the output Merkle root, both computed **once at creation** (one encode
  run) so `stat` answers immediately and the content has a stable identity;
- whether the transform is `STABLE` (pinned software implementation) or `VOLATILE` (anything
  that cannot be pinned).

### 2a. Composition is asynchronous, and "not ready" is a first-class state

Composing a view costs one encode run (needed for `size` and the root), so creations are
queued rather than done inline: composing a library of thousands of tracks must not block
the writer for minutes. An inode therefore starts `NOT_READY` and becomes `READY` when the
first composition finishes.

There is no POSIX "not ready", so the state is reported honestly in three places instead
of being hidden:

| surface | NOT_READY behaviour |
|---|---|
| `open()` for read | fails with `EAGAIN` (retryable — the same errno a non-blocking call uses) |
| `stat()` | returns the inode's recorded values (size 0 until composed) and is **never** silently wrong about a ready view |
| `user.invfs.computed` xattr | carries `not_ready`, `ready` or `volatile`, so a tool can ask the real question |

A `VOLATILE` view (a transform that could not be pinned — see §5) is readable but its
xattr says the bytes are not reproducible, which is what keeps it out of backups by
policy rather than by accident.

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

**What makes a transform `STABLE` — the checklist.** All four must hold, and each one
is a way real encoders go `VOLATILE`:

| requirement | why it fails in the wild |
|---|---|
| pinned implementation **and version** | zstd level 3 vs 19 already produced different bytes for the same 64 KiB (measured, `BENCH-MERKLE-DEDUP.md`); FLAC/APE/x264 builds differ between distros |
| deterministic parameters | preset, quality, `--threads N` (a different thread count changes some encoders' output), rounding modes |
| **no injected metadata** | gzip stores mtime and OS in the header; x264 writes its own version string into an SEI; muxers write a creation date and a "muxing application" string — identical input, different bytes |
| no hardware/driver dependence | GPU encoders (`nvenc`, `vaapi`, `qsv`, AV1 hwenc) are **non-deterministic by nature**: different driver, card or scheduling order gives different bytes, sometimes run to run |

Practical recipe for media, if a `STABLE` view is wanted: pin the encoder build, fix the
preset and thread count, and **normalize the container metadata** (zeroed dates, fixed
muxer/app strings). Anything less lands in `VOLATILE`.

Consequences of the rule, stated plainly:

- storing video in its compact form and exposing a decoded view is safe (decode only);
- transcoding on read to another *encoded* format is allowed only for a pinned software
  encoder, and it is CPU per read (cached in ARC, materialized by policy);
- a `STABLE` view's bytes are frozen; re-composition happens only on explicit request,
  never "during the next sweep";
- **`VOLATILE` cannot be a zero-storage view, and saying otherwise would be a lie.** If a
  transform cannot be reproduced, the only way to guarantee the bytes is to store them.
  Caching a volatile composition is worse than useless: it looks stable while the cache
  holds, then a recomposition after eviction returns different bytes — a player seeking
  inside such a file would read two versions of one file. So the policy is:
  a `VOLATILE` transform either **materializes on the first composition** (it costs
  storage, i.e. no free lunch — but the bytes are then real) or is **refused** at creation.
  The class exists so the operator can see *why* the encoder in question is not usable as
  a view, not to enable it.

**Chains are allowed.** A view may reference another view (decided 2026-11-17): the depth
cap is 4 and cycles are rejected at creation, because the spec names sources and a cycle
would make the inode unreadable by construction. Each level keeps its own pinned
transform, so a chain is only as reproducible as its weakest link — which is why the
composite state is `VOLATILE` if any link is.

**scrub/verify does not re-encode by default.** A computed inode is verified by checking
that its sources verify and that the spec is intact (opcode parse, pinned version present,
root recorded); re-running the transform is expensive and belongs to an explicit
`--verify-views` pass, which recomposes and compares the recorded root — that pass is also
the determinism test for a pinned encoder.

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

## Resolved questions (2026-11-17)

| question | decision |
|---|---|
| Where the base spec lives | in the recipe, as `INVFS_ALGO_COMPUTED` — one read path, atomic with the file, content-addressed (§2) |
| Cost of creation (one encode for size/root) | queue the composition; the inode is `NOT_READY` until done, `open()` says `EAGAIN`, the xattr says why (§2a) |
| ARC budget for decoded views | unchanged: decoded output is charged to the normal budget and evicted like anything else |
| Chains of views | allowed, depth cap 4, cycles rejected (§5) |
| How scrub treats a computed inode | verify sources + spec integrity; re-encode only under `--verify-views`, which compares the recorded root (§5) |

## Still open

1. The sweep's background worker owns bulk composition (decided 2026-11-17): composing a
   library is a sweep job, not a daemon, because the sweep already holds the volume
   exclusively and already has the queue discipline (defer/admission/retry) that a
   composer needs. Consequence to accept: views created on a volume whose sweep never
   runs stay `NOT_READY` — which `open()` reports as `EAGAIN`, so nothing lies.

## See also

- [ADR-012-verbatim-space-dedup-chunk-index](ADR-012-verbatim-space-dedup-chunk-index.md) — the chunk index behind the view
- [ADR-008-fuse-vs-kernel](ADR-008-fuse-vs-kernel.md) — who serves the view
- [all ADRs](README.md)
