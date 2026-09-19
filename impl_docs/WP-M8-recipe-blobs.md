# WP-M8 — content-addressed recipe blobs + verified read path

**Branch:** `wp/M8-recipe-blobs`
**Worktree:** `/tmp/invfs-wp-M8`
**Severity:** HIGH (bit-exactness: a bad recipe must never be trusted)
**Source:** `impl_docs/design-meta-v3.md` §12, §13, §15.2, §16
**Estimated effort:** large

---

## Scope

Make the recipe an **immutable, content-addressed blob** outside the inode
row and wire the read path to fetch and **verify** it before reassembly.
WP-M5 left a `invfs_blkptr recipe`; this WP turns it into a content address
(dedup identical recipes) and owns verification.

Files:
- `src/core/vol_ast.c` — recipe serialize/deserialize
  (`vol_serialize_children` `vol_ast.c:19`, `vol_deserialize_children`
  `:41`, encode/decode `:90`, `:425`).
- `src/core/vol_read.c` — `read_locate_record` (`vol_read.c:298`),
  `vol_read_inode` (`:316`): resolve addr, fetch + verify, then reuse the
  existing segment decoder.
- `src/core/vol_btree.c` — recipe keyspace codec.
- `src/core/invarifs.h` — row recipe field becomes a content address, via
  WP-M5 `row_version`. No delta; segment codecs unchanged.

## Why

§12 is explicit: recipes are immutable AST blobs, content-addressed and
**verified on read**, kept out of the inode row. The v2 recipe can reach
~384 MiB inline (`invarifs.h:982-999`); content addressing shrinks the row to
a fixed reference and dedups identical recipes. Verification is the
bit-exactness contract (AGENTS §2.9): trusting an unverified recipe can
return wrong bytes.

## Design

**Content address:** the design says "content-addressed" but is **silent on
the hash and encoding**. The tree already vendors BLAKE3 (used by
`vol_dedupe.c`), so this WP freezes `addr = BLAKE3-256(serialized recipe)`
and stores `{addr, blkptr}` in the row (fetch = O(log N) + one page read).
A different function is a new WP — do not change this silently.

**Recipe keyspace:** `key = 0x04 || addr[32]`. Identical recipes map to one
key; a second writer stores only the reference. On read, recompute BLAKE3
and compare to the key; mismatch is a hard error (quarantine/refuse, never
best-effort), per §16.

**Read path:**
```
row = read_locate_record(name)
addr = row.recipe.addr
blob = btree_lookup(recip_key(addr))       # WP-M3
if blake3(blob) != addr: hard error
segment decode(blob)                        # existing v2 decoder, reused
```

**Durability:** recipe blob pages are written + barrier'd **before** the
inode row referencing them ("structure before reference", WP-M2/M5). A crash
can orphan a blob (reclaimed by WP-M15) but never publish a live reference
to an unwritten recipe.

## Validation

1. `make test` — existing binaries plus WP-M2/M3 unit binaries.
2. `INVFS_V3=1 invf-mkfs t.img`; write files covering each AST shape;
   `invf-cat` is bit-exact (`cmp`) vs source; reopen reads identically.
3. Crafted corruption: flip a byte in a recipe blob → read fails loudly,
   does not return wrong bytes.
4. `bash tools/run-e2e.sh tools/test-astv2.sh`;
   `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

## Out of scope (do NOT touch)

- Data-zone codecs, transcode policy, containerpack formats.
- The recipe AST internal encoding (unchanged from v2 `vol_ast.c`).
- Delta/overlay/fold; data-segment dedup (`vol_dedupe.c`) — recipes only.
- Reclaim of orphaned blobs (WP-M15).

## Coordination notes

- Subagent ID: `wp-M8-recipe-blobs`; `INVFS_E2E_AGENT=wp-M8-recipe-blobs`.
- E2E gates: `test-astv2.sh`, `test-writepath.sh`, `test-meta-v3.sh`.
- Dependencies: **WP-M5** (row), **WP-M3** (keyspace lookup), **WP-M2**.
- Blocks: WP-M9 (write must publish through this store), WP-M15 (orphans).
