WP: wp/629-debian-boot-stall
Files changed:
  tools/boot-debian-qemu.sh   (kernel-cmdline block HOISTED above every preflight check -- it reads five env vars and touches nothing, so print mode no longer needs a built bin/, an initramfs or a staged root; INVFS_DEB_PRINT_CMDLINE=1 prints the assembled line and exits; the assembly's diagnostics and the four preflight refusals now go to STDERR so stdout in print mode is the cmdline and nothing else; the poweroff-red message now names the WP630 fix as regressed rather than an unknown wedge, and points at INVFS_DEB_SHUTDOWN_DEBUG at the point of failure)
  tools/test-debian-cmdline.sh (NEW, 136 lines, in `make e2e`, ~1 s, ISOLATED: no root, no QEMU, no kernel, no initramfs, no volume. 35 checks / 9 assertion groups: byte-exact default line; INVFS_DEB_BOOT_MASK= means NO mask; a custom mask REPLACES the default list; the shutdown-debug trio appended whole, after the masks, in order; every off spelling is off and prints no "ON" note; stdout stays one line with no "note:"; hermetic run against an empty REPO. Ends with a NEGATIVE CONTROL -- no print mode, same bogus env -> exit 2 and EMPTY stdout -- which is what distinguishes "print runs before the preflight" from "the preflight was deleted")
  Makefile                    (one line in the `e2e` list + a comment saying why; the engine-ci e2e-full job extracts its suite list from `make -n e2e`, so it is picked up there with no workflow edit, and a new tools/test-*.sh also trips that workflow's force_full scope rule)
  docs/DEBIAN-INSTALL.md      (INVFS_DEB_BOOT_MASK had NO row at all -- shipped in b848d04 and documented nowhere -- and gets one, including the `${VAR-default}` vs `${VAR:-default}` distinction and why masking is a kernel-cmdline mask and not written into the staged root; INVFS_DEB_PRINT_CMDLINE row rewritten to say stdout-only/-append-able; paragraph pointing at the new gate)
  INCIDENTS.md                (CI debian shutdown saga closed against the WP630 entry, with Layer 3's writeback-stall reading recorded as NEVER separately re-tested instead of refuted; the WP630 entry's own heading flipped from "OPEN —" to "FIXED (WP630) —" because the file contradicted its own Status field)
Tests run:
  $ git merge main
  Already up to date.   (branch already at c4a9727 == main; no conflicts)
  $ make -j2 test                                   # log /tmp/wp629-maketest2.log
  test-shard-check: OK (64 prerequisite binaries invoked across shards)
  ... 1558 assertion lines "OK"/"ok", zero "FAIL"/"not ok"
  check-codecpack-sync: SKIPPED layers B and C -- no registry checkout
  MAKE_EXIT=0
  $ INVFS_E2E_AGENT=wp629-cmdline bash tools/run-e2e.sh tools/test-debian-cmdline.sh
  ... 35 checks, "PASS: debian boot cmdline assembly (9 assertion groups)"   -> GATE_EXIT=0
  $ INVFS_E2E_AGENT=wp629-cmdline-rerun bash tools/run-e2e.sh tools/test-debian-cmdline.sh
  ... identical, 35 OK, PASS                                                -> RERUN_EXIT=0  (2/2, no flake signal)
  $ bash tools/check-repo-hygiene.sh
  citation check: OK (line numbers in range across 36 doc(s))
  repo hygiene: OK (714 tracked files, no binaries/oversize outside allowlist)   -> RC=0
  $ INVFS_DEB_PRINT_CMDLINE=1 bash tools/boot-debian-qemu.sh 2>/dev/null   # vs the kernel's own record
  diff -u printed.txt <serial.log "Command line:" after CR strip -> IDENTICAL
  (i.e. the hoisted string is byte-for-byte the cmdline the guest kernel actually booted with)
  $ INVFS_DEB_VOLUME=/tmp/wp629-work/deb.img INVFS_DEB_ACCEL=kvm bash tools/run-e2e.sh tools/boot-debian-qemu.sh
  the guest booted the initramfs and refused the volume: "This volume has no root filesystem in it"
  -> "FAIL: systemd did not reach multi-user.target in a single boot attempt"   BOOT_EXIT=1
Result: PASS (unit tier + the new hermetic gate + repo hygiene; the REAL Debian boot leg is NOT RUN and could not be run here -- see below)
Remaining TODOs:
  1. The provisioned boot leg (`installation-bootstrap-debian`) is NOT RUN, and it is the only evidence that could confirm the gate still ends at `reboot: Power down`. It is not runnable in this container: `debootstrap` is not installed (network is up, so that is the only blocker), `/tmp/wp629-stage` is empty, and the leftover 4 GiB volume `/tmp/wp629-work/deb.img` was never imported with a Debian root -- booting it is what produced BOOT_EXIT=1 above, and the failure is the volume's, not the harness's: the initramfs said so by name and the harness then reported it as H1's boot-timeout, which is its documented wording for "systemd never announced itself". What that partial run DOES prove is that the restructure is boot-path-neutral: the kernel logged exactly the hoisted string as its command line (all six `systemd.mask=` tokens, in order), so assembly, QEMU launch and the mask passing are all unchanged. Closing this needs either a provisioned stage or CI -- scope maps `tools/boot-debian-qemu.sh` to the debian leg alone (so the harness change itself runs there), but the new `tools/test-*.sh` also trips `force_full`, so the one PR runs both.
  2. The staged-root provisioning leaks recorded in the WP doc (guest hostname and `/etc/resolv.conf` inherited from the build host, no `/etc/hosts`) are still untouched -- deliberately, and they were never shown to matter.
  3. `impl_docs/WP629-debian-boot-stall.md` keeps its own STATUS line; it is untracked scratch in the orchestrator's tree (not in this worktree, not in git), so nothing here rewrites it. Its "Next" paragraph still reads as an open problem ("getting visibility INSIDE systemd-shutdown is the first thing a future attempt should fix") although `INVFS_DEB_SHUTDOWN_DEBUG` shipped that instrument and WP630 closed the defect -- worth one edit by the orchestrator, not by me, since the doc is outside the scope I was given.
