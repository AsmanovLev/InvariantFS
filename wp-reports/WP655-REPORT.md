WP: wp/655-admin-path
Files changed (commit 8a35483 on branch wp/655-admin-path):
  tools/run-e2e.sh   (+126) the admin-tool resolution block: admin roots
                     (/usr/sbin:/sbin) prepended to a suite's PATH (no privilege
                     granted -- a LOCKED suite still needs sudo to loop-mount),
                     e2e_admin_path / e2e_need_admin / e2e_admin_package exported
                     to suites, a package map (ntfs-3g, e2fsprogs, dosfstools,
                     exfatprogs, xfsprogs, btrfs-progs, util-linux, lvm2, parted),
                     and --need/--path CLI dispatch for direct invocation.
                     INVFS_E2E_ADMIN_ROOTS overrides the roots.
Red-before: tools/test-ntfs.sh:90 `command -v mkfs.ntfs` failed for an INSTALLED
  ntfs-3g and printed "not installed", exiting 1 without running a leg.
  Measured: /usr/sbin/mkfs.ntfs exists, `command -v mkfs.ntfs` in a stock
  non-root PATH returns nothing.
Tests run:
  $ bash tools/run-e2e.sh --path mkfs.ntfs            -> /usr/sbin/mkfs.ntfs
  $ bash tools/run-e2e.sh --need losetup mkfs.vfat    -> exit 0 (both resolved)
  $ INVFS_E2E_ADMIN_ROOTS=/nonexistent bash tools/run-e2e.sh --need losetup
      FAIL: losetup: ABSENT, not merely off PATH -- searched PATH (...) and /nonexistent
            package: util-linux  (install it: apt-get install util-linux)
      -> exit 1   (negative control: a precondition that never refuses verifies nothing)
  $ bash tools/run-e2e.sh tools/test-ntfs.sh    -> NTFS CONTAINERPACK E2E: PASS   EXIT=0
  $ bash tools/run-e2e.sh tools/test-fatfs.sh   -> FATFS E2E: PASS                EXIT=0
      (this one was the "fat32.img not decomposed" red in the full e2e: it was a
       stale/absent build plus the PATH refusal, NOT the recipe -- see WP656,
       closed as not-a-bug. Recipe measured 54662910 B of 67108864 B, 81.5%.)
Result: PASS
Remaining TODOs:
  - `make -j4 test` and the ebpf gate (tools/ebpf/test-probe.sh) were not re-run
    on this branch: the change touches only tools/run-e2e.sh, and the two
    containerpack suites above exercise the runner end-to-end. Run them before
    the merge if the orchestrator wants the full set:
      cd /mnt/invfs-wt/invfs-wp655 && make -j4 test
      bash tools/run-e2e.sh tools/ebpf/test-probe.sh
  - The suite-side `command -v` preconditions were NOT rewritten to call
    e2e_need_admin: with the runner's PATH fixed they resolve correctly, so the
    rewrite is a message-quality improvement, not a fix. Left undone deliberately
    (that is where the previous three subagent attempts died).
