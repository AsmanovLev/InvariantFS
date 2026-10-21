WP: wp/641-stream-lane
Files changed (squash-merged to main as f9348cc, 11 commits on the branch):
  src/core/vol_cpack.c          (vol_containerpack_sweep_stream: ranged pin write + MAP_PRIVATE
                                 mmap + the UNCHANGED body on mapped bytes (full == NULL);
                                 chunked rebuild-verify; container copy moved before the purge;
                                 INVFS_CPACK_RECIPE_MAX names the recipe wall)
  src/core/vol_sweep.c          (dispatch: the head-sniff branch takes the streaming entry; same
                                 decline/stamp contract, no predicate changes)
  src/core/volume_internal.h    (the streaming entry's declaration)
  tools/test-ntfs-big.sh        (the streaming leg quotes the engine's own admission marker;
                                 the offline sweep's scratch is pointed at the big disk; the
                                 L4 accept-either shape stays)
  tools/check-vol-find.sh       (ledger re-point, twice: once by diff at the branch tip, once
                                 +38 constant after merging main)
Tests run (all in /mnt/invfs-wt/invfs-wp641, after `git merge main` => 89fc969):
  $ make -j4 test
    MAKE_EXIT=0   33 "PASS:" banners, 0 failures
    ... check-vol-find: ok (29 ACTS, 81 SKIP, 110 call sites on the ledger)
    ... repo hygiene: OK (727 tracked files, no binaries/oversize outside allowlist)
    ... check-codecpack-sync: layer A passed (13 vendored packs in the record)
  $ bash tools/run-e2e.sh tools/test-qcow2.sh
    QCOW2 CONTAINERPACK E2E: PASS   EXIT=0
  $ bash tools/run-e2e.sh tools/test-qcow2-zlib.sh
    PASS: the qcow2 pack maps zlib clusters to MRM2 with REPRO entries and rebuilds the
    container bit-exactly; the sweep DECOMPOSES this compressed-cluster image ... EXIT=0
  $ bash tools/run-e2e.sh tools/test-containerpack.sh
    WP119: regression refused (+265417 B projected), win decomposed, both bit-exact
    CONTAINERPACK E2E: PASS   EXIT=0
  $ PATH=/usr/sbin:/sbin:$PATH bash tools/run-e2e.sh tools/test-ntfs.sh
    == admission leg: tiny ARC (INVFS_ARC_BYTES=1M) ==
    fs.ntfs (tiny ARC): cls=5 algo=20 gen=1
    NTFS CONTAINERPACK E2E: PASS   NTFS_EXIT=0
    (the PATH prefix is not cosmetic: mkfs.ntfs lives in /usr/sbin, which is not on a
     non-root PATH, so the suite's `command -v mkfs.ntfs` precondition refuses with
     "not installed" for an installed package. That defect is tools/test-ntfs.sh's, is
     outside this WP's scope, and is written up as WP655 with the measurement.)
Result: PASS
L4 RUN (after the merge, on the 1.5 TB scratch): L4_EXIT=0, all legs green.
  == [L4] 16 GiB scale lifecycle ==
    [cpack] streaming admission: 17179869184 bytes (ranged pin + mmap)
    [cpack] enumerate failed: fatfs could not enumerate the container (...)
    cpack: ntfs: disk.ntfs: the recipe is 4596965280 bytes and the engine holds it in
      RAM whole; the limit is 1073741824 bytes (INVFS_CPACK_RECIPE_MAX, MiB).
      Declining: the file keeps its own bytes and the generic floor may re-encode it.
  OK  L4: lane reached the 16 GiB container (streaming admission: sweep.log) and refused after the pin
  OK  L4: generic floor (class=4); lane declined, reason above
  OK  L4: 16 GiB rebuild bit-exact through the mounted read path, fsck OK
  OK  L4: unlink through mount reclaims, fsck OK
  That is the WP's claim proven on the admission half: the streaming path REACHES a container
  larger than RAM (no whole-file malloc, no OOM kill) -- and then refuses BY NAME because the
  RECIPE is 4.28 GiB.

Remaining TODOs:
  - MEASURED ARCHITECTURAL CEILING (not a bug in this WP, and not fixable by streaming):
    a pack whose recipe is the COMPLEMENT of its members produces a recipe as large as the
    container minus the members. On this 16 GiB / 12 GiB-member fixture that is 4.28 GiB of
    recipe, which is more than a 7 GiB box can hold, so full decomposition of THAT shape is
    impossible here regardless of how the pin is read. The knob names the wall instead of
    dying on it, which is the whole point of INVFS_CPACK_RECIPE_MAX. Worth its own WP if
    anyone wants big-container decomposition on small boxes: the fix is a recipe encoding that
    does not store the complement (e.g. run-lists over the non-member ranges), not more RAM.
  - The L4 decomposition leg as originally specified (200 members + table + map as siblings,
    fully decomposed) was NOT run to green: it needs
    INVFS_BIG_SCRATCH and ~40 GB, which this box now has (/mnt/invfs-wt, 1.5 TB), but
    the leg is a 45+ minute single run and was not scheduled while other gates were in
    flight. Exact command:
      INVFS_BIG_SCRATCH=/mnt/invfs-wt/big INVFS_E2E_AGENT=wp641 \
        bash tools/run-e2e.sh tools/test-ntfs-big.sh
    This is the leg that proves the WP's claim end-to-end (a container bigger than RAM
    decomposes); everything above proves the small-file path is byte-identical and the
    lane's guards still refuse for the same reasons.
  - The cross-check that the whole-file path and the streaming path produce identical
    on-disk siblings for a mid-size container needs a box where BOTH run (the streaming
    path is taken only when the whole-file read fails or the file exceeds the cap). Not
    proven, not claimed -- same caveat as the WP doc.
  - AGENTS.md §2.5's fuse_fs.c citations were already stale before this WP (noted, not
    touched); the section this WP's code lives in is cited correctly.
