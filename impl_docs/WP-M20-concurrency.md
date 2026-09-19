# WP-M20 — concurrency: drop `g_io_lock`, lock-free base reads + delta append

**Branch:** `wp/M20-concurrency`
**Worktree:** `/tmp/invfs-wp-M20`
**Severity:** HIGH (a torn/racing metadata read returns wrong bytes)
**Source:** `impl_docs/design-meta-v3.md` §3, §4, §5, §9, §15, §17, §18
**Estimated effort:** large

---

## Scope

Stage out the global `g_io_lock` (`fuse_fs.c:31`) to match §9: base reads are
lock-free (immutable between folds), and the delta append is the only writer
critical section, shardable per-thread like RAW. Full metadata sharding stays
rejected (§9).

Files:
- `src/cli/fuse_fs.c` — remove/shrink `g_io_lock` from read and metadata
  paths (`fuse_fs.c:31`, `:85`, `:518`, …).
- `src/core/vol_delta.c` — per-shard append arenas + atomic reserve; an
  append lock or CAS, not the FUSE global.
- `src/core/vol_btree.c` — assert lookup takes no lock; no shared mutable
  read-path state.
- `src/core/vol_fold.c` — publish is atomic; readers drain the old base.
- `src/core/volume_internal.h` / `vol_write.c` — shard state; RAW arena
  sharding (existing idiom) shared with delta sharding.

## Why

§9 and §18: today every op serializes on `g_io_lock` (`fuse_fs.c:31`), which
§1 lists as a motivation. The two-tier design exists partly so readers never
block: the base is immutable between folds, so lookup/readdir need no lock
and writes contend only on the delta append. §13's daily operations are
concurrent by nature.

## Design

**Staging (frozen order):**
```
g_io_lock  ->  base lock-free + delta append lock
           ->  AG-sharded delta/RAW arenas
           ->  (future) per-shard metadata
```
This WP lands the first two arrows; full metadata sharding is explicitly
rejected (§9). The design gives the shape but **not** the reservation
granularity; this WP picks the delta append granularity (per-record vs
per-batch) from measured contention and documents it.

**Read path:** `btree_search` (WP-M3) reads only immutable pages;
`delta_lookup` (WP-M10) reads the index. The index entry is published only
after the record bytes are written, and the WP-M10 CRC covers the replay
path. The design does **not** specify the memory-ordering primitive; this WP
uses release/acquire and documents it.

**Fold vs readers:** fold publishes the new root atomically (WP-M14) and never
mutates old pages; readers holding the old root finish naturally (§9).
Reclaim (WP-M15) must not free an old page until its reader-drain condition
is met.

**Sharded append:** mirror RAW's per-thread segments + atomic reserve (AGENTS
§2.4/§14: "AG is a software concept — sharding RAW + delta"). Each shard gets
its own segment; the index is shared and updated atomically per key.

## Validation

1. `make test` — a concurrency test: N reader threads vs one writer appending
   delta records, no torn value, no crash; fold concurrently with readers.
2. `INVFS_V3=1 invf-mkfs t.img` → multi-threaded create/write/stat/readdir
   stress; content bit-exact; fsck clean. Run under TSan if supported (report
   if not).
3. `bash tools/run-e2e.sh tools/test-writepath.sh`;
   `bash tools/run-e2e.sh tools/test-meta-v3.sh`;
   `bash tools/run-e2e.sh tools/test-flakey.sh` (queued if the e2e lock is
   held, AGENTS §1.5).
## Out of scope (do NOT touch)

- Full per-shard metadata/namespace sharding (rejected, §9).
- Network/multi-host; new on-disk formats (runtime locking only).
- Fold/reclaim algorithms (WP-M14/M15) beyond their atomicity contracts.
- The v2 path (WP-M21 deletes it).
## Coordination notes

- Subagent ID: `wp-M20-concurrency`; `INVFS_E2E_AGENT=wp-M20-concurrency`.
- E2E gates: `test-writepath.sh`, `test-meta-v3.sh`, `test-flakey.sh`.
- Dependencies: **WP-M3**, **WP-M11**, **WP-M14**, **WP-M15**, **WP-M10**.
- Blocks: WP-M22 (race/soak gates).
- `g_io_lock` may not be fully deleted if a residual non-metadata path needs
  it; remaining uses must be justified. The end state is no metadata
  serialization.
