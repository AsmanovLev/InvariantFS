# WP-M10 — delta log: append, in-memory index, per-record CRC

**Branch:** `wp/M10-delta-log`
**Worktree:** `/tmp/invfs-wp-M10`
**Severity:** HIGH (the recent tier; torn/replayed state is the crash surface)
**Source:** `impl_docs/design-meta-v3.md` §2, §4, §7, §12, §18.1/D1, §15.3
**Estimated effort:** large

---

## Scope

Implement the **append-only delta log** and its in-memory key index with
per-record CRC. Data structure + unit tests only: no overlay (WP-M11), no
mutation wiring (WP-M12), no replay (WP-M13), no fold (WP-M14).

Files:
- `src/core/vol_delta.h` — new: record/segment structs, append/lookup API.
- `src/core/vol_delta.c` — new: segment append, coalescing index, CRC.
- `src/core/invarifs.h` — frozen delta record + segment framing structs.
- `src/core/volume_internal.h` — per-volume delta state (active segment,
  bump cursor, index).
- `Makefile` — add `vol_delta.o` to `CORE` (lines 30-34) and
  `infv-delta_test` to `test` (lines 133-138).
- `tools/delta_test.c` — new unit harness.

## Why

§2–§4 make the delta the writer's only critical section and the source of
lock-free reads (§9). §18.1/D1 chose **append log + in-memory index** over a
second on-disk tree; §16 requires unit coverage and parser fuzzing.

## Design

**On-disk record (frozen here):**
```
key_len:u16 BE
val_len:u16 BE        # 0 = delete record (shadows base)
flags:u16             # bit0 delete; others reserved 0
crc32c:u32            # over key||val, this field zeroed
key[key_len]
val[val_len]
```
§12 fixes `{key, value, crc}` but is **silent on the delete marker and
length framing**; this WP freezes the above, consistent with WP-M6/M7 keys.
Segments reuse shadow framing (§12); stripe size is this WP's decision,
documented in a comment.

**In-memory index:** `key -> {segment, offset, seq}`, a byte-keyed
open-addressing map (design says "fast in-memory index"/"delta_index" but
does not fix the implementation). Latest append wins (coalescing, §2).

**Append:**
```
delta_append(rec):
    reserve len via atomic bump in the active segment   # like RAW
    write record + CRC
    index[key] = {offset, ++seq}
```
Append is the writer critical section §4/§9 describes; sharding is WP-M20.
The bump allocator for the delta is created here (§15.3).

**Recovery hook:** a torn tail is truncated at the last valid record by CRC
(consumed by WP-M13); this WP exposes `delta_scan_valid()` and a torn-tail
unit case but does not replay at mount.

## Validation

1. `make test` — existing binaries plus `infv-delta_test`: append/lookup
   round-trip; coalescing (latest wins); delete marker; CRC mismatch
   detected; torn tail truncates at last valid; segment rollover;
   index == scan-replay equivalence.
2. `bash tools/run-e2e.sh tools/test-meta-v3.sh` — no v3 regression.
3. If wired, `make fuzz` exercises the record parser (§16).

## Out of scope (do NOT touch)
- Overlay reads / delete shadowing in the read path (WP-M11).
- Wiring chmod/unlink/rename into the delta (WP-M12).
- Mount replay (WP-M13); fold (WP-M14); reclaim (WP-M15).
- Save point (WP-M16); concurrency/sharding (WP-M20).
- Base tree format (WP-M2/M3); namespace key encodings (WP-M5/M6/M7).

## Coordination notes

- Subagent ID: `wp-M10-delta-log`; `INVFS_E2E_AGENT=wp-M10-delta-log`.
- E2E gate: `bash tools/run-e2e.sh tools/test-meta-v3.sh`.
- Dependencies: **WP-M2** (framing/allocator idioms), **WP-M5/M6/M7**
  (key encodings), **D1** decision.
- Blocks: WP-M11, WP-M12, WP-M13, WP-M14, WP-M15, WP-M20.
- The record framing is frozen; reserved flag bits need a new WP.
