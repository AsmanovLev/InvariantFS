# WP-AG — allocation groups for the append planes (v0.7)

Status: design, step 1 in progress.
Scope owner: engine (`src/core/vol_delta.c`, `vol_write.c`).

## What "AG" means here, and what it does not

AG (allocation group) is borrowed from XFS as a **physical** concept: an
independently owned slice of allocation and append space with its own lock.
XFS partitions the disk into AGs and keeps **one logical namespace** across
them; inode numbers merely encode which AG owns the block.

InvariantFS adopts exactly that split and nothing more:

| Plane | Sharded? | How |
|---|---|---|
| Delta append plane (recent-tier metadata log) | **yes** | per-AG reserve windows in the existing chain |
| RAW / Shadow append arenas (data writes) | **yes** | same reserve discipline, shared shard identity |
| Metadata namespace (inode + dirent + xattr, base B+-tree + delta keys) | **no** | one logical namespace |

Namespace sharding stays rejected for the reasons already recorded in
`impl_docs/design-meta-v3.md` §9: metadata is one logical namespace, and
partitioning it makes `rename`/`link` cross-domain and turns `fold` into a
distributed transaction. AG shards **space and locks**, never ownership of a
key.

The user-visible goal is the one the Gentoo stand measured: bulk creation
(`rsync`, `tar`) serializing on one lock and one bump pointer.

## Hard constraint found before designing

`invfs_rt` (RT30, block-0 offset 0x9D0) is **48 bytes with no spare field**:

```
_Static_assert(sizeof(invfs_rt) == 48,
               "RT30 is 48 bytes: 0x9D0..0xA00 ...");
```

It carries a single `delta_pba` head. A "one segment chain per AG" layout
would need N heads and therefore RT30 v2 plus relocation of SPT0 (0xA00) and
SPN0 (0xA20). That is a second, larger format break for no functional gain,
so:

**Decision: one chain, N reserve windows.** AG identity costs no on-disk
bytes. RT30 keeps naming the single active segment; the AGs are windows into
it.

## Why windows are legal at all

The delta log already tolerates holes, by construction:

* `INVFS_DELTA_FLAG_PAD` records are valid but **inert** — replay advances
  past them without indexing (`vol_delta.c`, replay loop), and the comment
  there states the contract: *"the log stays continuous, nothing
  resurrects"*.
* `dl_pad_span` / `vol_delta_pad_build` already exist to fill exactly this
  kind of abandoned span, and `dl_staged_heal` pads doomed spans ahead of
  the next fence.
* A failed append already leaves a consumed-but-unindexed span, and the next
  fence pads it. An abandoned AG window is the same shape, one step wider.

So an AG window that never gets its record written is not a crash: it is a
pad, and the pad machinery is already load-bearing.

## Step 1 is subtler than "move the pwrite out": the fence owns the ordering

The first draft of this WP proposed reserving under the lock, writing the
record outside it, and publishing under the lock. That is WRONG as written,
and the reason is the group commit, not the write:

* `vol_delta_append` stages every record (`dl_staged_keep`) and then fences
  the whole group when the caps are reached: `vmux_barrier` ->
  `dl_staged_free` -> `delta_pending = 0` -> `dl_retire_collect`.
* That fence means "every staged byte is durable". It is true today only
  because staging happens after the `pwrite` **inside the same lock** — so
  no staged byte can still be in flight when the fence is issued.
* Drop the lock across the `pwrite` and a second thread can reach the caps
  and fence while the first thread's bytes have not been written yet. The
  barrier then covers nothing, the staged set is freed, and the record lands
  after the fence: a durability hole that no CRC can see.

The fix is an **in-flight counter**, not a wider lock:

```
delta_inflight            /* reservations taken, records not yet written */
```

Rules:

1. Under the lock: rollover, reserve `off`, advance `bump`, take `seq`,
   `delta_inflight++`. Unlock.
2. `pwrite` the record with no lock held.
3. Under the lock: `delta_inflight--`, then publish (`di_put`) on success or
   doom on failure; `pending` accounting.
4. The fence may be claimed only by a thread that finds the caps reached
   **and** `delta_inflight == 0` after its own decrement. That thread takes
   the fence token, releases the lock, and runs the barrier **outside** it,
   then re-takes the lock for the bookkeeping (`dl_staged_free`, counter
   reset, `dl_retire_collect`).

`vmux_barrier` is an fsync; today it runs with `g_delta_lock` held, so every
fence blocks all appenders for the duration. Moving it out is worth more
than moving the `pwrite` out, and both fall out of the same counter.

Invariants that must not move:

* bytes before index (`di_put` last, always after the `pwrite` returned);
* a reserved-but-never-written span is consumed and parked to a PAD by the
  next fence — the WP606 shape that already exists for a failed append;
* at most one fence in flight, and never while `delta_inflight > 0`;
* `dl_staged_keep` must still run **before** `di_put`, because the rollback
  it captures (`has_prev` / `prev_seg` / `prev_off` / `prev_seq`) is what
  makes `dl_doom_entry` able to undo a published record.

## Third invariant: `di_put` overwrites unconditionally

`di_put` has no sequence comparison — it assigns `s->seq = seq` and
returns, which is correct only because today `seq = ++delta_seq` and the
`di_put` happen in the same critical section, so the index always ends up
holding the highest sequence.

Reserving the sequence earlier (step 1) breaks that: thread A can take
`seq=7`, thread B take `seq=8`, B publish first, and A then publish over it,
leaving the index on the older record. The fix is one guard in the coalesce
branch:

```c
if (s->klen == klen && memcmp(s->key, key, klen) == 0) {
    if (s->seq > seq) {           /* a newer record already published */
        if (inserted) *inserted = 0;
        return 0;                 /* older append loses; retry-free no-op */
    }
    break;                        /* coalesce: latest wins */
}
```

The caller's `delta_records++` / anchor bookkeeping must agree with that
"lost the race" return, so it has to be told (a third out-param or a
distinct return code), not inferred.

## Implementation checklist for step 1

1. `volume_internal.h`: add `int delta_inflight;` and `int delta_fencing;`
   (both under `g_delta_lock`; diagnostics only, never persisted).
2. `vol_delta.c`: build the record image before the lock (it reads only
   `key`/`val`), keep rollover under the lock.
3. Reserve (`off = bump; bump += rl; seq = ++delta_seq; inflight++`) under
   the lock; `pwrite` outside it.
4. Re-lock: `inflight--`, `dl_staged_keep` (captures `prev`), guarded
   `di_put`, `pending` accounting.
5. Fence claim: caps reached && `inflight == 0` && `!fencing`; set
   `fencing`, unlock, `vmux_barrier`, re-lock, `dl_staged_free` + counter
   reset + `dl_retire_collect`, clear `fencing`, unlock, free retire list.
6. Failure paths keep today's shape: consume the span, doom the set, pad at
   the next fence.
7. Tests: `tools/test-meta.sh`, `tools/test-meta-dirent.sh`,
   `tools/test-fold-delta-read.sh`, plus a forced `pwrite` failure after a
   reservation (fault seam) asserting replay resumes past the pad and later
   records are still found.

## Concurrency model

Today `vol_delta_append` holds one global `g_delta_lock` for the whole
operation, **including the `io_pwrite` of the record and the group fsync**.
That is the serialization the stand measured.

Step 1 does not yet split the index; it removes the write and the fsync
from the critical section, using the in-flight counter above.

```
lock:
    rollover if the window does not fit      (rare, still global)
    off  = bump ; bump += rl                 (reservation)
    seq  = ++delta_seq ; inflight++
    pending accounting
unlock
    pwrite(record bytes at off)              ← parallel
lock:
    inflight--
    di_put(index, key, ...)                  (publish AFTER the bytes)
    fence claim iff caps reached && inflight == 0
unlock
    vmux_barrier                             ← outside the lock, once
    lock: staged_free, pending reset, retire_collect ; unlock
```

Ordering rule that must not change: **bytes before index**. A reader may
resolve a ref the instant `di_put` returns, so the record must already be on
the device. Reserving before writing is safe precisely because an unwritten
reservation is invisible (no index entry) and a pad at the next fence.

Failure rule: a failed `pwrite` dooms the set exactly as it does today; the
reserved span is consumed and padded by the next fence. No new failure mode.

Step 2 (after step 1 is measured) makes the reservation itself per-AG:
`bump[ag]` with a CAS reserve of one record-sized span, `ag = hash(owner) % N`.
That removes the remaining lock from the reservation path; rollover stays
global because it publishes RT30.

## Shard identity

`ag = hash(owner key) % INVFS_AG_COUNT`, `INVFS_AG_COUNT` a power of two so
the modulo is a mask. Hashing by owner keeps related records in one window
(an owner's records are appended together anyway) and keeps a replay's
locality.

Namespace keys are unaffected: two AGs may hold keys of the same namespace;
that is the point.

## Not in scope

* Split of the in-memory index (`di_put`) into per-bucket locks — step 3.
* Namespace sharding — rejected, see above.
* RAW arena sharding lands on the same reserve primitive, after delta.
* Any change to `INVFS_DELTA_SEG_BLOCKS` (32 blocks / 128 KiB), record
  header, PAD encoding, RT30, SPT0/SPN0.

## Validation

* `make -j12`, `bin/invf-v2_open_test`, `tools/test-meta.sh`,
  `tools/test-meta-dirent.sh`, `tools/test-fold-delta-read.sh` (the
  fold/read interleave seam exists: `invfs_test_delta_read_hook`).
* Deliberate-partial-window case: force a `pwrite` failure (fault seam) after
  a reservation and assert replay resumes past the padded hole and later
  records are still found.
* A/B measure: bulk create of N files through FUSE/engine with and without
  the reservation split; report files/s, not wall clock alone.

## Staged delivery

1. In-flight counter + reservation split: reserve under lock, `pwrite`
   outside, publish under lock, fence claimed outside. No format change.
   **Next.**
2. Per-AG reservation (`bump[ag]`, CAS), rollover still global.
3. Index bucket locks; then RAW arena on the same primitive.
