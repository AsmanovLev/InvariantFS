# design-meta-v3 — Two-tier metadata: immutable B+-tree base + append-only delta (LST)

**Branch:** `wp/meta-v3-design` (design only; no code)
**Severity:** HIGH (format-breaking; replaces the on-disk metadata model)
**Source:** metadata-layout study; WP58 measurements; `impl_docs/AUDIT.md`;
`INCIDENTS.md`; AGENTS.md §2.3–§2.5, §2.7, §2.11
**Status (2026-09-19):** design draft for review. Supersedes the earlier
COW-snapshot draft of this file (see §11 Rejected alternatives).

> InvariantFS is treated as **unreleased** here: the format is not frozen, there
> is **no migration** and **no v2 reader**. Cutover is v3-only (§10 Legacy
> deletion).

---

## Scope

Replace the append-only per-version metadata model (`invfs_inode_rec` + `DELT`
tombstones + flat mapper + rewrite-position checkpoint) with a **two-tier
metadata store**:

- an **immutable B+-tree base** (stable tier) — the consolidated namespace;
- an **append-only delta log / LST** (recent tier) — changes since the last fold,
  read-visible with a fast in-memory index;
- **overlay reads** (delta wins), **fold** = lazy compaction of delta into base;
- a **single save point** for rollback.

Goals: O(1) mount, no tombstones / no dead-record accumulation, O(log N)
lookup/readdir, lock-free reads (immutable base), hardlinks, and a sweep that
walks the live set instead of the whole log. The DATA plane is unchanged: writes
still land in RAW (append-only) and are consolidated into Shadow by the sweep.

## Non-goals

- Many long-lived snapshots / writable branches / snapshot DAG.
- Reflink (needs block refcounts) — deferred.
- Data-zone format, codecs, containerpacks, parity/seal algorithms.
- Wasm, S3, OverlayFS/WP66, splice zero-copy, network/multi-host.
- Migration from v2.

---

## 1. Motivation (measured, code-grounded)

| Problem | Evidence |
|---|---|
| Metadata bloat | WP58 on 15 GiB: 302,269 records on disk, 66,126 live — **75% dead**, 104.2 MiB vs 25.8 MiB (118,073 `DELT`). |
| Compaction disabled | `vol_inode_compact` returns 0 on mapper volumes (`vol_records.c:2425-2427`); only extent-granular reclaim exists. |
| Compaction blocked by checkpoints | rewrite-position checkpoint (`vol_records.c:2428-2435`). |
| Mount O(N) | `nbuck`/`dbuck`/`ibuck` rebuilt by a full record scan at `vol_open` (`volume_internal.h:331-337`); no persisted index. |
| Update amplification | every `chmod`/`setattr`/xattr/rename appends a full record + tombstone; recipe can reach ~384 MiB (`invarifs.h:982-999`). |
| No hardlinks | no block refcounts (`volume.h:200-204`, WP6). |
| Fully serialized | global `g_io_lock` (`fuse_fs.c:31`). |

Conclusion: fix **mount, amplification, reclaim** with a persisted tree; the
useful split is **recent vs stable**, not data-zone.

## 2. Layer model

```
                 reads                         writes
                   │                              │
       ┌───────────┴───────────┐                 ▼
       ▼                       ▼         ┌─────────────────┐
  DELTA (LST, fast tier)  BASE (Shadow) │ append to delta │
  recent changes          immutable     └─────────────────┘
  in-mem index            B+-tree
       └───────────┬───────────┘
                   ▼
             merge: delta wins
                   │
        fold (lazy compaction, during sweep/idle)
                   ▼
        new immutable base + delta reset
```

- **BASE** — a B+-tree, **immutable between folds**. Root in the superblock
  (double-slot + seq/CRC, the `l2p_replay` idiom, `volume.c:644-669`).
- **DELTA** — a dedicated append-only metadata log (fast tier / dev0), a single
  coalescing stream keyed by namespace key, with an in-memory index. Bounded;
  replayed at mount.
- **READ** — delta index first, then base; delta wins.
- **WRITE** — append to delta; update the in-memory index; **base untouched**.
- **FOLD** — merge delta into base, producing a new base; reset delta.

## 3. Read path

```
lookup(key):
    if delta_index has key: return delta value      # recent wins
    else:                   return base lookup      # O(log N)
```

Consistency with a concurrent fold (add-before-remove rule):

1. fold writes the key into the **new base** first;
2. only then removes it from the **delta**.

With `read = delta → base`, any interleaving yields the correct (delta-ahead)
value: if the delta no longer has the key, the new base must already have it.
No read lock is required on either structure.

Ordered `readdir` is a **merge** of the delta's key range and the base's key
range; the delta is small, so the merge is cheap.

## 4. Write path

```
change(key, value):            # e.g. permchange (chmod/touch/xattr/rename)
    append{key, value, crc} to delta log
    delta_index[key] = offset
```

No tombstone, no base rewrite. `unlink` appends a `delete` delta record; the
key is shadowed in reads and physically removed at fold. Because the base is
untouched between folds, **readers never block** — this is where `g_io_lock`
dies (readers lock-free; the delta append is the only writer critical section).

## 5. Fold (lazy compaction)

Trigger: delta size/age threshold, or the sweep. Fold is incremental, not a full
rebuild: applying K delta keys to a COW base costs `O(K log N)` and shares all
untouched pages with the old base.

```
fold():
    new_base = base
    for each live key in delta:
        new_base = btree_upsert(new_base, key, value)   # COW path
    publish(new_base)          # atomic double-slot root swap (+ CRC)
    reset(delta)               # after publish is durable
    reclaim(old_base, delta)   # respecting the save point
```

`fold` is the "lazy compaction": there is no tombstone sweep and no metadata
log compaction beyond it. Fold is **not** required for correctness — the delta
can grow until a threshold — only for read latency and space.

## 6. Save point (single)

A save point records `{base_root, delta_end}`. Rollback loads that base and
truncates the delta to `delta_end`. One save point at a time:

- while a save point is live, fold may still run, but the pre-fold base root is
  **retained** (pinned) until the save point is dropped or replaced;
- rollback after a fold restores the pinned base and discards the delta tail;
- dropping the save point releases the pinned base.

This matches today's single-CKP0 semantics (`vol_records.c:2428-2435`,
`volume.c:4335`) but on a clean root instead of absolute rewrite positions. It is
a **rollback point**, not a browsable snapshot (§11).

## 7. Recovery

1. Read the base root from the double-slot area (higher seq; torn → fall back).
2. Replay the delta log from the last durable base; per-record CRC; a torn tail
   is truncated at the last valid record.
3. Mount cost = O(1) base + O(delta). Fold cadence bounds the delta, so mount is
   bounded rather than O(total records).
4. Power-loss claim (AGENTS §2.2) is **not** made until crash-injection soak
   (`tools/test-flakey.sh`) passes.

## 8. Reclaim

- Delta segments are freed after a fold publishes and the save point no longer
  needs them.
- Base pages unreachable from the current base **and** not pinned by a save point
  are freed. With a single save point this is a reachability diff (mark from both
  roots) or epoch-based retention — **no general refcount tree**.
- Reclaim is incremental/background.

## 9. Concurrency

- **Base immutable between folds** → lookup/readdir need no lock.
- **Delta** append is the writer's critical section; shardable (per-thread log
  segments + atomic reserve) exactly like RAW.
- **Fold** builds the new base aside and publishes atomically; readers drain the
  old base naturally.
- Stage out `g_io_lock`:

```
g_io_lock  ->  base lock-free + delta append lock
           ->  AG-sharded delta/RAW arenas
           ->  (future) per-shard metadata
```

- Full metadata sharding is **rejected**: metadata is one logical namespace;
  sharding makes rename/link cross-domain and turns fold into a distributed
  transaction.

## 10. Legacy deletion (hard cutover)

- `invfs_inode_rec`, `DELT`, inline `INO2`, `INVFS_MAX_REC_LEN` growth paths.
- flat **mapper** (`invarifs.h:1041-1057`), `vol_meta_merge.c`, `meta_mapper`.
- `vol_inode_compact` / WP58-D scheme-E.
- WP22d consistent cut (`open_cuts`), `\x01reten` registry (`volume.h:555`),
  hidden owner-inodes.
- `CKP0`/rollback absolute positions — replaced by save point.
- `nbuck`/`dbuck`/`ibuck` mount-scan indexes.
- legacy `format_version=0` replay / `migrate-v2`.

## 11. Rejected alternatives

**COW-snapshot DAG / L2 writable branches.** Frozen snapshots pin the *physical
encoding* of every block they reference, so the sweep can neither move nor
re-encode them; with cross-file batches one member pins a whole batch. Reclaim
then waits on the oldest snapshot, and recompression stalls. Making snapshots
cheap instead requires logical data indirection (segment id → encoding map) and
a general refcount graph + GC. Rejected: the requirement is a **single rollback
save point**, not browsable history.

**Single in-place B+-tree + WAL.** Viable and slightly simpler on the read path
(one structure after checkpoint), but it gives up the immutable base and its
**lock-free reads**, and needs torn-page redo on every page. The two-tier design
keeps writes append-only and reads lock-free, at the cost of a merge read.

**Pure LSM / leveled levels.** Compaction write-amplification across levels and
merge reads; the two-level base+delta shape is all that is needed here.

**Zone-partitioned metadata (metadata split by data zone).** Not well-defined: a
recipe is one record whose segment list can span RAW and Shadow simultaneously
(partial write to a swept file), and metadata-only changes (`chmod`) do not move
data at all. The meaningful split is **recent vs stable**, not zone. The
data-zone split remains, correctly, a DATA-plane property.

## 12. On-disk format v3 sketch

- `INVFS_VERSION = 3`; root area holds the base root (double-slot, seq + CRC).
- **Base pages**: fixed size (default 4 KiB; 16 KiB variant evaluated), header
  `{magic, gen, level, nentries, checksum}`; packed leaf entries; extent-relative
  internal addressing.
- **Block pointers**: `{pba, checksum, gen, flags}`.
- **Delta records**: `{key, value, crc}` append-only; coalesced by key in the
  in-memory index; segment/stripe framing reusing shadow framing.
- **Save point record**: `{base_root, delta_end, flags}`.
- **Recipes**: immutable AST blobs, content-addressed, verified on read
  (bit-exactness non-negotiable); keep them out of the inode row.
- **Two-device**: base/metadata on dev0, mirrored (existing `DEVT`); Shadow data
  on dev1.

## 13. Rootfs readiness / daily usage

Removes the two biggest rootfs blockers (metadata bloat/ENOSPC, mount scan) and
adds hardlinks:

| Daily operation | Today (v2) | v3 |
|---|---|---|
| Boot/mount | full record scan | O(1) base + bounded delta replay |
| `stat`/lookup | RAM hashes rebuilt by scan | delta index + O(log N) base |
| `readdir` big dirs | dir hash in RAM | merge of ordered ranges |
| create/delete/rename | full record + tombstone | delta append |
| package installs | 75% dead → ENOSPC spam | fold reclaims; no tombstones |
| rollback | coarse CKP0 | single save point |
| hardlinks | unsupported | `nlink` |

Not fixed here: write-heavy throughput, verify/compress CPU tax, sweep latency,
field maturity. Profile: read-mostly / immutable / archive.

## 14. Target deployment (personal reference)

| Device | Layout | Role |
|---|---|---|
| s1 | EFI + Swap + 128 GB xfs (**OS root**) + Test area | OS + e2e/scratch/reserve |
| s0 | 128 GB xfs (AI) + remainder | InvariantFS **dev0**: RAW + delta + base |
| HDD | whole device | InvariantFS **dev1**: Shadow / cold archive |

- One volume spanning dev0 (SSD) + dev1 (HDD); AI hot path stays on xfs/erofs,
  not InvariantFS.
- Consumer NVMe is not ZNS; RAW/delta are logical append streams. AG is a
  software concept (sharding RAW + delta).

## 15. Staging

1. **v3 skeleton**: descriptors, root area, base page format + CRC, bootstrap
   allocator, base B+-tree (inode) + unit tests.
2. **Namespace**: inode + dirent + xattr; recipes as content-addressed blobs.
3. **Delta + overlay + fold**; mount replay; bump-allocator for delta.
4. **Save point + rollback**; reclaim.
5. **Hardlinks** (`nlink`).
6. **Cleanup**: delete v2 paths; final e2e + crash soak.

## 16. Test strategy

- Rewritten format tests (~10–12): `test-migrate-v2.sh`, `test-astv2.sh`,
  `test-meta-extent*.sh`, `test-meta-overflow.sh`, `test-stats-mapper.sh`,
  `test-mapper-crash.sh`, `test-sweep-mapper.sh`, `test-compact.sh`,
  `test-l2p.sh`, `test-rollback.sh`, `test-registry.sh`.
- ~46 e2e unchanged if CLI behaviour is preserved.
- New (~100–150): base B+-tree unit; delta append/index; overlay ordering
  (add-before-remove); fold; save point/rollback; reclaim; mount replay; crash
  injection; hardlinks.
- Invariants: delta-vs-base value ordering; root double-slot; refcount-free
  reclaim (reachability) checks; fuzz the record/page parsers.

## 17. Effort & risks

- Smaller than the COW-snapshot programme: no refcount graph, no DAG, no logical
  data indirection. Still a format-breaking multi-WP effort.
- Risks: fold correctness under concurrent reads; ordering rule; save-point ×
  fold interaction; delta bounding (mount latency); reclaim without refcounts.

## 18. Open decisions

1. Delta representation — append log + in-memory index (replay at mount) vs a
   small persisted on-disk tree (O(1) mount).
2. Fold trigger — size/age threshold, sweep-driven, or idle.
3. Base page size — 4 KiB vs 16 KiB.
4. Reclaim — reachability diff vs epoch retention.
5. Save point — `{base_root, delta_end}` vs delta position only.
6. Recovery claim timing (needs soak evidence).

## 19. Coordination

- Review artifact only; implementation WPs cut from §15.
- Supersedes `WP58-meta-record-v3.md` (deleted; recoverable from git history).
