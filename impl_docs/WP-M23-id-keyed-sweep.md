# WP-M23 — Inode-ID-Keyed v3 Sweep Publication & Nested Corpus Safety

**Branch:** `wp/M23-id-keyed-sweep`
**Worktree:** `/tmp/invfs-wpM23`
**Severity:** HIGH (P0 data corruption / silent data loss)
**Source:** Static review of v3 merge window (M21b/c integration review)
**Estimated effort:** 4 hours

---

## Scope

Fix data-safety vulnerabilities in the v3 sweep engine where sweeping nested files either overwrote root-level files or created stray root entries while leaving nested files un-swept. Make v3 blob publication purely inode-id-keyed, implement active write session guards by inode ID, make `vol_sweep_file` function on v3 volumes, and provide offline nested-corpus test validation.

---

## Bug A — Leaf name publication in `vol_sweep_one_v3` / `vol_create_blob_file`

**File:** `src/core/vol_sweep.c`, `src/core/vol_png.c`, `src/core/vol_dirs.c`
**Function:** `vol_sweep_one_v3`, `vol_create_blob_file`
**CWE:** CWE-668 (Exposure of Resource to Wrong Sphere), CWE-436 (Interpretation Conflict)

The M21c iterator and `vol_sweep_name_of` hand out leaf names. In `vol_sweep_one_v3`, the sweep called `vol_create_blob_file(v, name, enc, enc_len, full_len, INVFS_ALGO_ZSTD)`. On v3, `vol_create_blob_file` invoked `vol_v3_create_content_node(v, name, orig_size, addr)`. That function parsed `name` with `v3_split_path`: when `name` was a leaf (e.g., `"photo.jpg"` from `"subdir/photo.jpg"`), the parent resolved as `""` (root directory `/`). Consequently:
- If a root-level file `/photo.jpg` existed, its content was silently overwritten by `subdir/photo.jpg`.
- If no root-level `/photo.jpg` existed, a stray `/photo.jpg` was created at root, while `subdir/photo.jpg` remained unswept in RAW.
- In addition, publishing via `create_content_node` stomped file attributes (mtime, atime) and broke hardlinks.

**Fix:**
Introduce `vol_v3_publish_blob_inode(invfs_volume *v, uint64_t inode_id, const uint8_t *blob, size_t blob_len, uint64_t orig_size, uint32_t algo)`. This stores the shadow blocks using `io_pwrite`, serializes and writes the recipe, and directly updates `in.recipe_addr` on `inode_id` using `vol_v3_inode_delta_put`. The dirents, hardlinks, inode ID, file mode, uid, gid, and mtime remain completely untouched and intact.

---

## Bug B — Sweep vs active write session race on nested files

**File:** `src/core/vol_write.c`, `src/core/vol_sweep.c`
**Function:** `vol_write_active_name`, `vol_sweep_one`
**CWE:** CWE-362 (Concurrent Execution using Shared Resource with Improper Synchronization)

`vol_write_active_name` only compared strings against `s->name`. Because daemon sweeps pass leaf names to `vol_sweep_one`, an active write session on `subdir/photo.jpg` was never matched by `"photo.jpg"`, permitting the background sweep to race live write sessions.

**Fix:**
Add `vol_write_active_id(invfs_volume *v, uint64_t inode_id)`. Check both `vol_write_active_id(v, inode_id)` and `vol_write_active_name(v, name)` across sweep entry points (`vol_sweep_one`, `vol_sweep_file`).

---

## Bug C — `vol_sweep_file` / manual sweep broken on v3

**File:** `src/core/vol_sweep.c`
**Function:** `vol_sweep_file`, `vol_sweep_file_inner`, `vol_sweep_file_generic`
**CWE:** CWE-670 (Always-Incorrect Control Flow Implementation)

`vol_sweep_file` called `idx_get_id` (a no-op on v3) and `vol_sweep_file_inner` which attempted to read legacy v2 inode records via `sweep_locate_record`. On any v3 volume, every single file failed with `-1`. Manual sweeps triggered by SIGUSR1 or xattr failed 100% of files.

**Fix:**
Add a v3 branch in `vol_sweep_file` and `vol_sweep_file_generic` to route to `vol_sweep_one_v3(v, inode_id, NULL)`. Return `0` on success (swept), `1` if skipped / nothing to do, and `-1` on error.

---

## Bug D — Missing offline nested corpus test for `vol_sweep_pending`

**File:** `src/cli/sweep_pending_test.c` (or test harness)

Unit test suites only tested flat directories or offline `invf-sweep` CLI. The daemon's `vol_sweep_pending` path with nested directory structures was untested.

**Fix:**
Add a comprehensive offline unit test exercising `vol_sweep_pending` with nested subdirectories (`dir1/sub2/file.bin`), root files, collisions, bit-exact verification, and verify that no stray root entries are created.

---

## Validation

1. `make test` — all unit tests pass.
2. `bin/invf-sweep_pending_test` — nested files sweep to Shadow without root corruption.
3. `tools/run-e2e.sh tools/test-textzone.sh` and related e2e gates.

---

## Out of scope

- Symlinks/special files on v3 (handled in separate WP).
- Large-file streaming / chunked buffering in `vol_sweep_one_v3`.
- Removal of `INVFS_V2=1` hatch.

---

## Coordination notes

- Subagent ID: `wpM23-id-keyed-sweep`
- Branch: `wp/M23-id-keyed-sweep`
