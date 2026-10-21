WP: wp/644-backup-pilot   (worktree /tmp/invfs-wp644, HEAD 2ad4ece, 5 commits from main)

Files changed:
  tools/test-backup-pilot.sh (NEW, 1153 lines)  hermetic chunked pilot: phases
      plan/run/verify/reclaim/report + gate legs L0-L6. Fault-injected resume
      (L2/L3), bit-exact deterministic stride sample (L4), unlink-and-settle
      reclaim with the free-block deltas printed (L5), three-rate report that
      REFUSES extrapolation under 512 MiB / 500 files (L6).
  docs/guides/BACKUP.md      (NEW, 241 lines)  operator procedure: preflight,
      chunk sizing from the RAW window, sweep-then-fsck-per-chunk loop,
      resumable manifest, single-copy reality on dev1 stated not hidden, and
      the measured calibration table (import 7.37 files/s / 2.29 MiB/s,
      sweep 1.45 MiB/s, fsck 1548 inodes/s => 13.7 days for 1 TB, NO-GO
      inside 7 days with sweep binding).
  No src/ file changed. No file outside the WP scope touched.

Tests run:
  $ cd /var/tmp/wp644-mt && make -j2 test      # copy of the same commit, see NOTE 1
  test-shard-check: OK (64 prerequisite binaries invoked across shards)
  ... 159 isolated unit invocations, 32 PASS banners, 0 FAIL/FAILED lines
  citation check: OK (line numbers in range across 37 doc(s))
  repo hygiene: OK (715 tracked files, no binaries/oversize outside allowlist)
  MAKE_EXIT=0                            (log /tmp/wp644-maketest.log, 4838 lines)

  $ INVFS_E2E_AGENT=wp644 bash tools/run-e2e.sh tools/test-backup-pilot.sh
  OK L0 two generations, one corpus digest fff8009466c6751f356980a28e428461932dd9e67aaf73398ce111e589375fb1
  OK L1 3 chunks (chunk-a/b/c), bytes match a fresh walk, all PENDING
  OK L2 DONE(a) FAILED(b) PENDING(c); c untouched after b failed
  OK L3 a imported once (not again), b retried once, c imported; all DONE
  OK L4 sampled paths bit-exact through invf-cat
  OK L5 the unlinked chunk's space came back and fsck stayed CLEAN
  OK L6 report: three rates present, extrapolation refused at this sample size
  PASS: wp644 hermetic legs L0-L6          E2E_EXIT=0   (15 OK assertions)

  Flake re-run, identical command: E2E_RERUN_EXIT=0, 15 OK assertions, same
  corpus digest fff8009...fb1 (fixture determinism holds across runs).

  $ bash tools/check-repo-hygiene.sh
  repo hygiene: OK                         HYGIENE_EXIT=0
  (the two cite: WARNs are pre-existing ADR-007/ADR-009 drift, not this WP)

  Calibration leg (real corpus, ran end-to-end this round on /mnt/wp640-big):
  corpus /usr/lib/x86_64-linux-gnu, 3332 files / 1034.38 MiB, fresh 8 GiB
  volume, 54 chunks, all 54 DONE with the per-chunk fsck gate CLEAN,
  `verify` bit-exact (44 of 58 sampled paths compared through invf-cat /
  readlink, the other 14 rows are directories, which have no bytes to
  compare; zero DIFFs):
  import : 3332 files / 1034.38 MiB in 452.232 s = 7.37 files/s, 2.29 MiB/s
  sweep  : same bytes in 712.936 s = 1.45 MiB/s (transform+dedupe+batches+finalize)
  fsck   : 2.677 s over 4144 live inodes = 1548 inodes/s
  1 TB / 1M files => import 127.2 h, sweep 200.9 h, fsck 0.2 h, TOTAL 13.7 d.
  Verdict: NO-GO inside 7 days; binding stage is SWEEP, not codec choice.

Result: PASS (gates green at 2ad4ece; one measured deliverable + one named
bottleneck, per the WP's expected outcome).

Remaining TODOs (named, none blocking merge):
  1. NOTE — make test host workaround. In /tmp/invfs-wp644 the gate cannot run
     in place: tools/run-unit-isolated.sh stages the repo out of /tmp with
     `mount --bind`, and this worktree carries two ext4 bind mounts inside it
     (bin/ and build/ on /mnt/wp640-big, made to keep the 3.8 GiB /tmp tmpfs
     from filling), which makes the bind fail EINVAL => exit 3 "could not
     stage". Reproduced on a bare directory too (plain --bind fails, --rbind
     succeeds), so it is the mount layout, not the tree. The gate was
     therefore run on a byte-identical copy of 2ad4ece at /var/tmp/wp644-mt,
     where staging works and the same wrapper isolates /tmp as intended.
     Two follow-ups this WP does not own: (a) run-unit-isolated.sh could fall
     back to the existing UNISOLATED path when staging fails instead of
     exiting 3 (the fallback exists for the ns-unavailable case); (b) the
     worktree bin/build mounts should not sit inside a repo the wrapper binds.
  2. Real-corpus RECLAIM was not run. The leg unlinks the chunk with the most
     verbatim bytes and, with every chunk at 0 verbatim, picks the LAST DONE
     row (= `toplevel`, 810 files / 958 MB); the settle then needs >=1 full
     sweep of 1034 MiB at the measured 1.45 MiB/s (~12 min/pass, cap 4) on top
     of an 8 GiB volume, i.e. a 25-45 min leg. The property itself is gated
     hermetically by L5 (space returns, fsck CLEAN after). Follow-up if the
     orchestrator wants a real-corpus number: run `--reclaim` on a corpus whose
     chunks are a few hundred MiB, or add a knob to name the reclaim chunk.
  3. Scope growth not taken (correctly, per the WP): the import whole-file-RAM
     wall (invf-cp/invf-import malloc each file) and the lane whole-file-RAM
     wall (WP641) were routed around, not fixed. The plan phase counts
     over-RAM files and BACKUP.md routes them through a FUSE mount.
  4. The daemon-shutdown wall (a lingering FUSE daemon holds the volume flock
     past unmount) is worked around by waiting on the LOCK and killing by PID;
     fixing it is WP623 territory.
