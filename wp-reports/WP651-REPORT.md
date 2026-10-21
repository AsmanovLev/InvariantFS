WP: wp/651-domain-headers  (SLICE 1 of 6 only — vol_storage.h)
Files changed:
  src/core/vol_storage.h     (NEW, 110 lines: invfs_vol_storage + the 5-accessor surface)
  src/core/volume_internal.h (the 14 WP25 device/mirror fields leave the open struct for `invfs_vol_storage st;`; +vol_storage.h include; 2 stale comments re-pointed)
  src/core/volume.c          (102 field references -> v->st.*; 5 accessor definitions appended after vol_mirror_stale)
  src/core/vol_crash.c       (v->degraded -> vol_st_degraded(v); +include)
  src/core/vol_tier.c        (v->degraded -> vol_st_degraded(v); +include)
  src/core/vol_write.c       (v->degraded -> vol_st_degraded(v); +include)
  src/core/vol_sweep.c       (v->dev0_blocks -> vol_st_dev0_blocks(v); +include)
  src/core/vol_read.c        (v->dev_skip[0] -> vol_st_dev_skipped(v,0); +include)
  src/core/vol_resize.c      (dev0_blocks/devt/dev1_blocks -> accessors; the two-assignment pair -> vol_st_devt_commit)
  tools/check-vol-find.sh    (mechanical ledger re-point, WP635/641 discipline)

Commits (branch wp/651-domain-headers, 3, nothing pushed):
  5200822 wp651 slice 1: vol_storage.h owns the device/mirror state of a volume
  6d776ff wp651 slice 1: re-point the check-vol-find ledger at the shifted lines
  8d4b891 wp651 slice 1: fix the stale v->degraded prose in volume_internal.h

WHAT MOVED (14 fields, all of the WP25 DEVT/mirror state):
  path2, dev_skip[2], degraded, devt, devt_present, resync_pending,
  resync_winner, dev0_blocks, dev1_blocks, meta_end_bytes, mux_pos,
  raw_mirror, rawio_logged, metaread_logged
  Field text and comments are unchanged; only the storage location and the
  access path moved.

WHAT DID *NOT* MOVE (deliberate, stated in the header):
  io, io2, io_open[2], ndev, dev0_present stay in the open struct. They are
  storage-domain in principle but have non-owner readers that this slice
  would have had to edit: vol_cpack.c:1811-1834 (io2 commit-idle sampling),
  vol_fold.c:201-203 (io2/io_open), src/cli/btree_test.c:149-151,
  src/cli/metabuf_test.c:98-100, tools/delta_test.c:108-153. Also still in
  the open struct: the bitmap cursors and block-pool allocator state the
  WP651 slice table names — those are the next storage increment, not this
  one. `io` must also stay the FIRST member after `path`: v_of_blk
  (volume_internal.h:843) does pointer arithmetic on offsetof(invfs_volume,
  io), and the io_* macros every engine call site uses depend on it. Moving
  it would be a logic change, which this slice is not allowed to make.

ACCESSOR SURFACE — 5 accessors, every one with a caller outside volume.c:
  vol_st_degraded(v)        3 callers: vol_crash.c:82, vol_tier.c:806, vol_write.c:1039
  vol_st_dev0_blocks(v)     3 callers: vol_resize.c:50, :53, vol_sweep.c:2419
  vol_st_dev_skipped(v,d)   1 caller:  vol_read.c:89 (out-of-range index reads as "not skipped")
  vol_st_devt(v)            1 caller:  vol_resize.c:340
  vol_st_devt_commit(v,dt)  1 caller:  vol_resize.c:345
  NO accessor for: devt_present, resync_pending, resync_winner,
  meta_end_bytes, mux_pos, raw_mirror, rawio_logged, metaread_logged,
  path2. Each is owner-private in practice — grep shows zero non-volume.c
  readers — so WP651 rule 2 (no speculative getters) leaves them
  accessor-less.

INVARIANT NOW ENCODED (WP651 rule 3): vol_st_devt_commit. dev1_blocks is a
cached copy of devt.dev_blocks[1]; vol_resize.c used to assign the table and
then the copy on two back-to-back lines, and so did the two open legs in
volume.c. A table write that moved dev_blocks[1] and left the copy behind
would keep routing on the old span for the rest of the mount. The resize
apply now makes both changes in one call. dev0_blocks is deliberately NOT
touched by it (mkfs fixes it; the resize grows the tail device only) — that
is what the old code did and what it still does. The two owner-side sites
(volume.c:866/872, :1130/1141) still write the pair directly, because
volume.c is the owner; only the non-owner site went through the accessor.

Tests run:
  $ make -j4 test   (foreground, log /tmp/wp651-mt.log)
  MAKE_EXIT=0
    test-shard-check: OK (65 prerequisite binaries invoked across shards)
    citation check: OK (line numbers in range across 37 doc(s))
    check-vol-find: ok (29 ACTS, 81 SKIP, 110 call sites on the ledger)
    repo hygiene: OK (729 tracked files, no binaries/oversize outside allowlist)

  $ make clean; make -j4            (clean build, log /tmp/wp651-full-build.log)
  WP651_FULL_EXIT=0; 6 warnings — IDENTICAL set to the pre-slice clean build
    (base /tmp/wp651-base from git archive HEAD~3: 6 warnings, diff empty)

  $ make clean; make -j4 test       (clean, log /tmp/wp651-mt.log)
  MAKE_EXIT=0; 69 warnings, IDENTICAL set to base clean `make -j4 test` (69)
    — diff of {file: warning text} multisets is empty. No new warnings.

  $ bash tools/check-vol-find.sh
  VOLFIND_EXIT=0  (ok, 29 ACTS / 81 SKIP / 110 sites)

  $ bash tools/check-repo-hygiene.sh
  HYGIENE_EXIT=0  (repo hygiene: OK; the 12 "cite: WARN" lines are pre-existing
    docs/ references to files outside this repo, identical at base)

  $ INVFS_E2E_AGENT=wp651-storage bash tools/run-e2e.sh tools/test-writepath.sh
  E2E_EXIT=0 -> WRITEPATH E2E: PASS

  $ INVFS_E2E_AGENT=wp651-storage bash tools/run-e2e.sh tools/test-resize.sh
  E2E_EXIT=0 -> RESIZE E2E: PASS   (run because vol_resize.c holds the only
    invariant-encoding accessor and the only devt rewrite outside the owner)

  $ INVFS_E2E_AGENT=wp651-storage bash tools/run-e2e.sh tools/test-meta-multidev.sh
  E2E_EXIT=0 -> PASS: v3 two-device (dev0 base + metadata mirror + degraded
    read) — the suite that actually exercises the moved state: degraded
    mount, dev1-mirror reads, read-only write refusal, dev0 reattach.

  $ INVFS_E2E_AGENT=wp651-storage bash tools/run-e2e.sh tools/test-flakey-multidev.sh
  E2E_EXIT=0 -> SKIP: dm-flakey unavailable (sudo/dmsetup). Reported, not
    substituted: this is the suite that would drive dev_skip/resync_* under a
    dropped-write window, and it cannot run in this environment.

ONE FINDING FOR THE ORCHESTRATOR (not fixed here — out of slice scope):
  test-writepath.sh FAILS AT BASE too, before any of my edits. Its leg [C]
  assertion `class_of "$IMG1" swept.txt = 7` calls
  `$B/meta_probe` (tools/test-writepath.sh:220), and bin/meta_probe is not a
  `make test` prerequisite — it is built only by `make e2e` / `make test-bins`
  (Makefile:753, :830, :1593). On a fresh worktree with only `make -j4 test`
  run, meta_probe does not exist, class_of returns empty, and the leg fails
  with "FAIL: swept.txt not TEXT post-sweep". Measured: base tree
  (git archive HEAD~3) -> EXIT=1 with the same message; my tree -> EXIT=1 with
  the same message; after `make -j4 bin/meta_probe` -> EXIT=0 PASS on my tree.
  So the gate is not broken by this slice, and the slice is not masked by it.
  The fix belongs to whoever owns the harness: either add meta_probe to the
  test prerequisite list, or have run-e2e.sh build it.

Result: PASS
Remaining TODOs:
  - slices 2-6 (vol_meta.h, vol_datapath.h, vol_tiering.h, vol_resilience.h,
    vol_plugins.h) untouched, as instructed. One slice only.
  - io/io2/io_open/ndev/dev0_present: the second storage increment. Needs the
    vol_cpack.c / vol_fold.c / src/cli test-binary edits listed above.
  - bitmap cursors + block-pool allocator state: still in the open struct.
  - volume_internal.h is 2352 lines (was 2360) and still included by 114
    files. vol_storage.h is included by 9 (the owner + 5 core callers +
    volume_internal.h itself, transitively reaching all 114). The WP's exit
    criterion — the open definition reachable from one domain header only —
    is not met by slice 1 and cannot be; it is the whole-WP criterion.
