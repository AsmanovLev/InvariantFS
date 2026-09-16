# WP35-import-meta-coords — WP30 mapper coordinates propagate to writers

**Branch:** `wp/35-import-meta-coords`
**Worktree:** `/tmp/invfs-wp35`
**Severity:** HIGH
**Source:** user report — `invf-import` failed with "meta failed: c/ (Success)" for every directory on a fresh v0.3.0+ two-device volume; downstream `invf-ls` / `invf-stats` saw zero entries despite the import reporting success.
**Estimated effort:** 4 hours

---

## Scope

The WP30 v0.3.0+ mapper routes inode records through dynamic metadata extents
(`vol_meta_merge.c:618` `meta_get_append_pos`), but the legacy append path in
`vol_create_file`, `vol_create_file_with_meta`, and `meta_rewrite` still wrote
directly to the linear cursor `v->inode_area_pos`. On a fresh volume that
mismatch leaves the records in a position the linear scanners cannot find, and
on a two-device volume the resulting hint is rejected by `meta_read_record_by_id`
because `v->inode_area_pos` was never advanced past the linear default. This WP
moves every record append onto `meta_get_append_pos`, syncs the mapper state
on every write, and re-points the open-time cursors onto the active mapper
extent so the legacy linear scans find the records.

Five bugs, all in the WP30 coordinates contract:

- Bug A: `meta_get_append_pos` calls `meta_mapper_get` (a read-lock acquisition)
  while the function itself holds the write lock — guaranteed deadlock on the
  second call.
- Bug B: `vol_create_file` / `vol_create_file_with_meta` write records at the
  legacy linear `v->inode_area_pos` instead of using `meta_get_append_pos`. On
  a v0.3.0+ volume the records land in an unrelated block, `idx_put_id`
  stores the wrong position, and every subsequent `vol_apply_meta` /
  `meta_rewrite` fails because `meta_read_record_by_id`'s hint is rejected
  (the hint's block is past `inode_area_pos`).
- Bug C: `meta_rewrite` (the metadata-update path) uses the same legacy
  `v->inode_area_pos` cursor for writing — even after Bug B is fixed, the
  first directory `apply_meta` still writes to the wrong position.
- Bug D: `vol_open_inner` initialises `v->inode_area_pos` and
  `v->inode_area_end` from the legacy linear layout (`v->inode_area_start *
  BLOCK_SIZE` → `metadata_zone_end * BLOCK_SIZE`). On a v0.3.0+ volume the
  records live in the mapper extents, which sit outside that range. The open
  scan walks the wrong range and the linear scanners in `vol_compute_stats`
  and `invf-ls` never reach them.
- Bug E: `tools/invf-import.c:142` has an inverted mkdir condition
  (`vol_mkdir(vol, vname) != 0 && vol_is_dir(vol, vname) == 0`). `vol_mkdir`
  returns 0 on failure and a non-zero inode id on success, so the original
  test meant "if mkdir succeeded AND the name is not a directory, fail" —
  which is the opposite of the intended check. Failure is silent and the
  import silently proceeds to `apply_meta_or_die` on a non-existent anchor.

---

## Bug A — meta_get_append_pos deadlocks on second call

**File:** `src/core/vol_meta_merge.c`
**Function:** `meta_get_append_pos`

`meta_get_append_pos` acquires `v->meta_lock` with `pthread_rwlock_wrlock` at
the top of the function (line 633) and then calls `meta_mapper_get` (lines 645,
650, 662, 680, 699, 712). `meta_mapper_get` is implemented with a read-lock
acquisition (`vol_meta_merge.c:110`):

```c
uint64_t meta_mapper_get(const invfs_volume *v, size_t i) {
    if (!v->meta_mapper || i >= v->meta_mapper_n) return 0;
    pthread_rwlock_rdlock(&((invfs_volume *)v)->meta_lock);   /* <-- deadlock */
    ...
}
```

A writer holding the lock cannot acquire a reader lock — POSIX rwlock semantics
guarantee this. The first call to `meta_get_append_pos` (which allocates
extent 0) succeeds; the second call hangs forever waiting for the lock it
still holds.

On a single-device volume the legacy cursor in `vol_create_file` keeps
`meta_get_append_pos` from being called twice per record. On two-device
volumes the meta extent can fill and trigger lazy allocation of extent 1,
which exercises the same code path again — and the import hangs at that
point. The first symptom that surfaced was `invf-cp` and `invf-import` hanging
on the first write; `invf-stats` reported `state: CLEAN` and zero live files
because the import had silently produced no records before hanging.

**Fix:** replace every `meta_mapper_get(v, idx)` inside the write-locked
section with direct array access (`v->meta_mapper[idx]`). The caller already
holds the write lock; the read-lock wrapper is wrong for that context. Also
removed the spurious `pthread_rwlock_unlock` on the `merge_in_progress`
early-return path — the lock is acquired *after* that check, so unlocking
there unlocked a lock the thread never held.

---

## Bug B — vol_create_file ignores the WP30 mapper

**File:** `src/core/vol_records.c`
**Functions:** `vol_create_file` (×2 paths), `vol_create_file_with_meta` (×2 paths)

The legacy record-append logic uses `v->inode_area_pos` for seek/write:

```c
if (io_seek(&v->io, v->inode_area_pos) != 0 ||
    io_write(&v->io, rec, rec_size) != 0 || ...)
v->inode_area_pos += rec_size + 4;
idx_put(v, name, strlen(name), inode_id,
        v->inode_area_pos - rec_size - 4, ...);
idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
```

On a v0.3.0+ volume the inode area lives in the WP30 mapper extents, not the
linear legacy region. The records go to wherever `inode_area_pos` happens to
point (the legacy linear default at open time), which is outside any mapper
extent. `idx_put_id` then stores a position that the next `meta_read_record_by_id`
cannot use: the hint check at line 904 is

```c
if (hint >= p && hint + sizeof(invfs_inode_rec) <= v->inode_area_pos)
    p = hint;
```

If `v->inode_area_pos` is still the legacy default (~33 MB) and the record
actually lives at the mapper extent position (~1.6 GB on a two-device volume),
the upper-bound check fails and the scan walks from `p` (which falls in the
zero-filled linear region) — no records found, `meta_rewrite` returns 0,
`apply_meta_or_die` reports "meta failed: <name> (Success)".

**Fix:** every append now calls `meta_get_append_pos` to get the actual write
position from the mapper, writes at `abs_pba + offset`, and updates BOTH
`v->met0.active_offset` and `v->inode_area_pos` so the legacy scanners can
locate the record through `idx_get_id`.

All four paths affected: `vol_create_file` empty-file (vol_records.c:78-95),
`vol_create_file` non-empty (vol_records.c:256-283), `vol_create_file_with_meta`
zero-length (vol_records.c:341-356), `vol_create_file_with_meta` non-empty
(vol_records.c:472-486).

---

## Bug C — meta_rewrite uses legacy cursor

**File:** `src/core/vol_records.c`
**Function:** `meta_rewrite`

`meta_rewrite` is the metadata-update path used by `vol_apply_meta`,
`vol_update_ino2`, `vol_meta_flush_dir`, `vol_delink`, `vol_rename`. The
write-side of `meta_rewrite` (lines 1073-1085) used the same legacy cursor as
Bug B, with the same outcome. This is the path that fires for every
directory after `vol_mkdir` creates the anchor — and is why "meta failed"
appears once per directory even after Bug B is fixed.

**Fix:** `meta_rewrite` now branches on `met0_present && meta_mapper`: if the
volume is on the WP30 layout, it calls `meta_get_append_pos` for the new
position and tracks the cursor the same way as Bug B. The legacy format
(format_version=0) path is preserved verbatim.

---

## Bug D — vol_open_initial cursors ignore the WP30 mapper

**File:** `src/core/volume.c`
**Function:** `vol_open_inner`

After Bug B writes records into the mapper extents and Bug C updates the
in-memory mapper state, the open-time cursor still pointed at the legacy
linear region (block 8201, ~33 MB). `vol_open_inner`'s scan walks from
`v->inode_area_pos` to `v->inode_area_end`, both of which were derived from
the linear layout. The records lived in the mapper extents (block 399783+ on
a two-device volume) — outside that scan range. Every tool that reopens the
volume (`invf-ls`, `invf-stats`, `invf-fsck`) saw zero records, even though
the import itself reported success and the records were durable on disk.

**Fix:** when `met0_present && meta_mapper && met0.extent_count > 0 &&
active_extent < extent_count`, rebind `v->inode_area_pos`, `v->inode_area_end`,
and `v->inode_area_start` onto the active extent (`first_pba * BLOCK_SIZE`,
extent end, `first_pba`). The mapper-aware `vol_inode_next` and the
extent-aware scanner in `vol_compute_stats` can now reach the records.

The mapper point itself (the source of the bug) was preserved by the lazy
allocation in `meta_get_append_pos` (line 642-654): extent 0 was allocated
from the shadow zone on the two-device volume and the mapper entry was set
in memory; `meta_mapper_flush` (line 653) only no-ops because at allocation
time `meta_mapper_pba` is still 0 from a fresh mkfs (the mapper table at
block 17 is *intended* to be lazy-loaded). The persistence story works out
because the mapper pointer itself is in the superblock and persists at
mkfs; the mapper *contents* are flushed on every meta extent allocation, and
on the next open `meta_mapper_load` reads them into RAM before the scan.

---

## Bug E — invf-import inverted mkdir check

**File:** `tools/invf-import.c`
**Function:** `import_entry`

Original code at line 144:

```c
if (vol_mkdir(vol, vname) != 0 && vol_is_dir(vol, vname) == 0) {
    fprintf(stderr, "mkdir failed: %s\n", vname);
    n_skipped++; return;
}
```

`vol_mkdir` returns 0 on failure and the new inode id on success. The
original condition tested "mkdir succeeded AND name is not a directory" as
the error path — i.e., the failure path was never entered. The success path
fell through to `apply_meta_or_die`, which then called `vol_apply_meta` on
an anchor that didn't exist (because mkdir had failed). Combined with Bug C
above, every directory ended up as "meta failed: <name> (Success)" with
zero skip counts.

**Fix:** capture the return value, then check `mid == 0 && !vol_is_dir` —
failure when `vol_mkdir` returned 0 *and* the name isn't already a directory
from a previous run. Braces added around the case body so the case-local
variables don't fall through into `S_IFREG`.

---

## Validation

1. **Unit tests:** `make test` — passes (4467 checks, 0 failures).
2. **Single-device fresh image:** `dd if=/dev/zero of=test.img bs=1M count=1024`
   → `invf-mkfs test.img 4` → `invf-import test.img /tmp/test-src` →
   `invf-ls test.img` shows 5 entries; `invf-stats test.img` shows 1 file,
   3 dirs, 1 symlink.
3. **Two-device fresh image:** `invf-mkfs dev0.img 1 dev1.img 1` →
   `invf-import dev0.img /tmp/test-src` (with `INVFS_DEV1=dev1.img`) →
   `invf-ls dev0.img` shows 5 entries; `invf-stats dev0.img` shows 2 files
   (the extra one is the `\x01rawm` device-sidecar which `invf-ls` filters
   out by name prefix).
4. **No `meta failed: ...` lines** in either case.
5. **Fuzz:** `make fuzz` — already-known pre-existing manifest failures
   (143) reproduce cleanly; no new failures introduced.

---

## Deliverables

- Patch series (one commit per bug A–E):
  - `meta_get_append_pos: avoid read-lock deadlock inside write-lock section`
  - `vol_create_file/_with_meta: write through meta_get_append_pos`
  - `meta_rewrite: write through meta_get_append_pos on WP30`
  - `vol_open_inner: rebind cursors to active mapper extent`
  - `tools/invf-import: fix inverted mkdir check`
- `impl_docs/WP35-import-meta-coords.md` — this document.

---

## Out of scope (do NOT touch)

- The `merge_in_progress` early-return path inside `meta_get_append_pos`
  (different concern: WP30 Phase 6 merge concurrency, not the write path).
- `invf-ls` symlink display (separate display bug, not the import path).
- The `\x01rawm` sidecar being counted as a regular file by `invf-stats`
  (it *is* a regular file in the volume; the sidecar prefix is an
  implementation detail of WP25, not a stats bug).

---

## Coordination notes

- Subagent ID for this WP: `wp/35-import-meta-coords`
- Pass via `INVFS_E2E_AGENT=wp/35-import-meta-coords` when invoking e2e.
- E2E gates to run:
  - `bash tools/run-e2e.sh tools/test-writepath.sh`
- Dependencies: none. WP30 (mapper) is already on `main`.