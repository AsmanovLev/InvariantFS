# WP-M24 — v3 symlinks, special files, and mkfs v2 retirement

**Branch:** `wp/M24-symlinks-and-specials`
**Severity:** HIGH (distro / rootfs compatibility: symlinks, mknod, device nodes, pipes)
**Source:** Review of v3 cutover (WP-M21b regression) & Linux rootfs support (WP67)

---

## Scope

Restore full symlink and special file (FIFO, SOCK, CHR, BLK, rdev) support on metadata-v3 volumes:
1. `src/core/vol_records.c`:
   - Wire `vol_create_symlink` on v3: store symlink target in immutable recipe store (`vol_v3_recipe_store`), populate inode row (`INVFS_ITYP_LNK`, `size = strlen(target)`, `recipe_addr`), insert dirent.
   - Wire `vol_create_special` on v3: set `INVFS_ITYP_*`, mode, rdev, and call `vol_v3_create_node`.
   - Update `vol_get_meta` on v3: for `INVFS_ITYP_LNK`, load target bytes from `in.recipe_addr` via `vol_v3_recipe_load` into `out->target`.
   - Wire `vol_v3_set_meta` in `vol_dirs.c`: update recipe store if `INVFS_ITYP_LNK` target is modified, ensure `rdev` and `type` are persisted.
2. `src/core/vol_read.c`:
   - In `vol_read_inode`, if `in.type == INVFS_ITYP_LNK`, load recipe blob directly and return it as target content.
3. `src/cli/fuse_fs.c`:
   - In `fill_stat_from_meta`, set `st->st_rdev = (dev_t)m->rdev` for CHR and BLK devices.
4. `src/cli/mkfs.c`:
   - Retire `INVFS_V2=1` hatch: refuse v2 format creation with informative error message since v2 engine code was removed.
5. `src/cli/symlink_v3_test.c`:
   - Add unit test verifying creation, readlink, stat, getattr, read_inode, and special files (mknod chr/blk/fifo/sock) on v3 volumes.

## Validation

1. `make test` — all existing and new unit tests pass.
2. `bash tools/run-e2e.sh tools/test-meta-v3.sh` — passes.
