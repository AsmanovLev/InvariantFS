# ADR-014: the container map is the ABI, and it declares its vocabulary

## Status

Accepted. Implements the rule that a decomposed container's readability is
governed by one FS-owned structure, and that structure names what a reader must
know before it touches an entry.

## Context

A decomposed container stores three things: the pack's recipe, the members, and
the map (`<name>!mbrmap`) that says which byte range of the container is what.
The map is FS-owned — wire format, parser, validator and serve path all live in
`src/core/vol_cpack.c` — while the recipe is pack-owned (opaque bytes, read only
by the pack's own `rebuild`). That asymmetry was implicit, and it produced a real
incident.

Elision of all-zero regions needed a map entry meaning "these bytes are zeros",
added as `kind 3` (`f66ca44`). Nothing else changed: the wire layout, the
validator and the serve path already had room. But every engine built before
that commit capped a v1 (`MRMP`) map at `kind 1`:

```c
if (ents[i].kind > (v2 ? CPACK_MAP_KIND_REPRO : 1)) { free(ents); return -1; }
```

so a volume decomposed by the current sweep, read by a reader one commit older,
gave

```
[cpack] get disk.img: map read/parse failed
ext4fs: mapped container read failed for disk.img
```

and 0 of 134 217 728 bytes — while the same volume read bit-exactly with the
current binary. Two separate defects made that expensive to diagnose:

- the message covered three different failures (sibling unreadable, blob does not
  parse, entry names an unknown kind) with one line, so the operator could not
  tell a newer map from a corrupt one or from a missing sibling;
- the pack manifest key that exists for exactly this — `min_read`, "minimum
  reader version that can decode" (`src/core/invarifs.h:558,574`) — is dead: the
  pack loader never assigns it, `invf-pack` prints it as a string
  (`src/cli/pack.c:220`), and nothing compares it with the running engine.

## Decision

1. **The map's kinds are the ABI vocabulary.** `0` recipe bytes, `1` member
   bytes, `2` re-deflate on read, `3` zeros (`CPACK_MAP_KIND_ZERO`), `4` the
   bytes live in `<name>!mbrblob` at `src_off` (`CPACK_MAP_KIND_BLOB`). A pack
   may *produce* any of them; only this file may *define* one.

2. **A new kind comes with a new declaring magic.** One magic per "an older
   reader cannot parse this" step, no version table:

   | magic | kinds it may carry |
   |---|---|
   | `MRMP` (v1), `MRM2` (v2) | 0..2 — historical, still read |
   | `MRK1` (v1), `MRK2` (v2) | 0..4 — *declares* that kinds ≥ 3 may appear |

   A baseline magic carrying `kind 3` is still ACCEPTED, because that is how
   every map written between `f66ca44` and this ADR looks; refusing them would
   strand volumes that read fine. New maps carrying any kind ≥ 3 are stamped
   with the declaring magic at write time (`cpack_map_declare_kinds`,
   `src/core/vol_cpack.c:2851`, called from the commit block at `:5307`).

3. **The map is the only source of truth for member layout.** `kind 4` made the
   `!mbrleaf` list redundant: it is no longer written, and a volume that still
   has one reads as before (the legacy branch of `cpack_member_read`,
   `:3711`). One invariant, one encoding.

4. **Read-time interpretation does not depend on the environment.**
   `INVFS_CPACK_BLOB`/`INVFS_CPACK_LEAF` are *write* policy (`cpack_leaf_mode`,
   `:2995`; `cpack_packed_layout`, `:3043`); the read path probes the volume. A
   volume whose meaning changed with the shell would be a volume that cannot be
   read back on another day.

5. **One funnel.** `cpack_member_read` (`:3711`) is the only function that may
   read a member's payload. It was not before — the rebuild path read the
   payload sibling itself — and `tools/test-cpack-map-consolidation.sh` now
   greps the source and fails if a third `vol_read_range()` call site appears in
   this file.

6. **Every refusal names the failing step.** `cpack_map_parse` takes an optional
   `why`; `cpack_map_get` distinguishes sibling unreadable from does-not-parse,
   and an unknown kind says it is a map written by a newer core and what to do.

## Consequences

- An engine older than a map's vocabulary refuses at byte 0 by name
  (`... does not start with a magic this build knows (MRMP/MRM2 = map kinds
  0..2, MRK1/MRK2 = map kinds 0..4)`), instead of failing entry-by-entry on
  "unknown kind" and leaving the operator to guess whether the map is newer or
  damaged. It still cannot say "upgrade" — a shipped binary cannot be improved
  retroactively — but the current binary does say it against a future map.
- The validator grows one branch per kind, each with its own invariant:
  `kind 3` requires `idx == 0 && src_off == 0`; `kind 4` requires a payload
  sibling, a member that exists, `src_off + len` inside that sibling (resolved
  *before* the validate, so a map naming a missing sibling cannot validate
  clean), no byte shared with a `kind 1` entry, and no byte shared with another
  `kind 4` entry.
- A maximally fragmented map now costs ~40 B per entry up to
  `CPACK_MAP_MAX_ENTS` = `4 * CPACK_MAX_MEMBERS + 4` = 4 194 308 entries
  (`src/core/vol_cpack.c:2646`). That
  bound is the engine's member bound (`src/core/volume_internal.h:2335`); the
  decomposition price (`cpack_size_proj`) is what keeps a real container from
  getting there.
- A pack may change its recipe freely (it ships with its reader); a pack may NOT
  change the map without this ADR.

## Alternatives rejected

- **A version table with 001→002 migrations.** One operator, no volumes in the
  field; a migration path nobody exercises is a liability.
- **Enforcing `min_read`.** The key exists in every manifest and in the pack
  struct, but it is a *per-pack* number for a *per-format* problem (the map is
  the engine's), and filling it in retroactively cannot fix volumes already
  written by packs that never set it. The declaring magic carries the same
  information at the only layer that can enforce it.
- **Skipping unknown kinds and serving zeros.** That would read a `kind 4`
  payload region as zeros and return plausible-looking garbage.

## See also

- [ADR-007-plugin-architecture-ivpack](ADR-007-plugin-architecture-ivpack.md) — the pack ABI this map belongs to
- [ADR-015-pack-io-is-ranged-or-streamed](ADR-015-pack-io-is-ranged-or-streamed.md) — what a pack reads *through*
- [all ADRs](README.md)
