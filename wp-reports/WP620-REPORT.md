WP: wp/620-nbli-lane
Files changed:
  tools/test-nbli-lane.sh    (leg-2 header `CONTAINER{NBLI=16,gen=1}` -> `{NBLI=25,gen=1}`;
                              WP627 moved INVFS_ALGO_NBLI 16 -> 25 and rewrote every assert
                              plus most comments, but missed this one summary line. Comment
                              only, no behaviour change -- commit c539bbb)

  Continuation context (no new work needed, verified):
    The WP620 lane itself is already on `main` as 609ef4b and corrected there by WP627
    (b9db773: INVFS_ALGO_NBLI 16 -> 25, rawdisk collision) + registry pointer 72b1edf.
    This branch was merged up to main at 12d205e and its tree was BYTE-IDENTICAL to main
    before c539bbb: `git diff main` = the single comment line above. Merging that stale
    branch over main would have re-introduced the algo-16 collision; the merge is the fix,
    not the lane.

  Residue audit for stale algo ids around NBLI (16 vs 25):
    grep -rn "NBLI=16|NBLI 16|algo 16|algo=16|ALGO_NBLI 16|{16}" over src/ tools/ registry/
    docs/ AGENTS.md | grep -i nbli   ->  exactly one hit (tools/test-nbli-lane.sh:23),
    now fixed. Functional asserts were already correct (leg2/3/4 all compare
    "cls=3|6 algo=25 gen=1"), and registry/codecpacks/nbli/1.0.0/manifest opens with
    "NBLI prototype v0.4 (algo 25, INVFS_ALGO_NBLI)" agreeing with
    src/core/invarifs.h:114 `#define INVFS_ALGO_NBLI 25`.

  Two-way-handler pairing audit (read-only, every PNGR-special site is NBLI-paired):
    src/core/vol_read.c      decode branch + algo_is_whole_file           paired
    src/core/vol_sweep.c     sweep_dispatch PNG branch (jxl|nbli knob)    paired
    src/core/vol_records.c   sibling list handling                        paired
    src/cli/fuse_fs.c        -o photo_codec= / INVFS_PHOTO_CODEC=nbli     paired

Tests run:
  $ make -j2 test            (log /tmp/wp620-maketest2.log)
    ...
    test heredocs: OK (139 python block(s) compile)
    citation check: OK (line numbers in range across 36 doc(s))
    check-vol-find: ok (29 ACTS, 81 SKIP, 109 call sites on the ledger)
    repo hygiene: OK (713 tracked files, no binaries/oversize outside allowlist)
    check-codecpack-sync: layer A passed ("all 11 vendored packs are in the record");
      layers B/C SKIPPED -- no registry checkout at $INVFS_CODECPACK_REGISTRY,
      /tmp/invfs-registry, ../invfs-registry (by design; strict mode is opt-in)
    MAKE_EXIT=0

  Pack payload (needed by the nbli gate; not built by `make test`):
  $ registry/tools/build-payloads.sh build nbli
    -> registry/codecpacks/nbli/1.0.0/bin/nbli, static ELF, payload sha256 verified
       against .../1.0.0/sha256

  $ INVFS_E2E_AGENT=wp620-nbli-lane bash tools/run-e2e.sh tools/test-nbli-lane.sh
    (log /tmp/wp620-e2e-nbli.log)
    == leg 1: default (jxl) ==
    leg1 OK: PNGR claims + refusals as specified, JPEG JXL, bytes exact, re-sweep idle
    == leg 2: nbli position ==
    leg2 OK: NBLI claims + RGBA contrast + JPEG untouched, bytes exact, re-sweep idle
    == leg 3: pack absent (nbli mode waits, never pins) ==
    leg3 OK: packless wait is unstamped; staging the pack claims on re-sweep
    == leg 4: broken PATH namesake (guard, not corruption) ==
    leg4 OK: broken tool refuses into GUARD, RAW bytes intact, settles
    == leg 4b: truly tool-less (unstamped wait) ==
    leg4b OK: tool-less lane waits RAW, bytes intact
    == leg 5: FUSE (mount option + stats visibility) ==
    leg5 OK: stats reports photo_codec=nbli
    == leg 6: fsck clean, host untouched ==
    leg6 OK
    PASS: nbli-lane (two-way photo handler, opt-in nbli, bit-exact)
    NBLI_EXIT=0

    Note: this run used the SYNTHETIC photo-like real-pixels frame, not CLIC --
      "NOTE: no CLIC zip at /tmp/invfs-wp620-corpus/clic-prof-valid-2020.zip"
      Set INVFS_NBLI_CLIC_ZIP to run that leg on a real CLIC frame.

  $ INVFS_E2E_AGENT=wp620-nbli-lane bash tools/run-e2e.sh tools/test-writepath.sh
    (log /tmp/wp620-e2e-writepath-r3.log)
    ...
    == [C] mmap read of swept forms (TEXT batch, container, ZSTD/UNCOMPRESSIBLE) ==
      classes: swept.txt=TEXT swept.tar=CONTAINER swept.bin=UNCOMPRESSIBLE
      mmap read bit-exact: swept.txt
    ...
      armed (region1 msynced+fsynced, region2 dirty only)
      post-crash: fsynced region intact; unsynced region also landed (kernel wrote it
        back before the kill)
    WRITEPATH E2E: PASS
    WRITEPATH_R3_EXIT=0

  Flake discipline -- every red was chased to its cause, and one of them was NOT the code:
    test-nbli-lane.sh run twice: green, green.
    test-writepath.sh run 1 and 2: RED at the first [C] assert,
      "FAIL: swept.txt not TEXT post-sweep" (/tmp/wp620-e2e-writepath{,-r2}.log).
    Cause: `bin/meta_probe` is built by `make e2e` / `make test-bins`
      (Makefile:1586, :830) and NOT by `make test`. The suite's class_of() calls it with
      2>/dev/null, so a missing probe yields an empty class and the leg reports a
      classification failure instead of a setup failure. `make -j2 bin/meta_probe`, then
      re-run -> PASS. Proven independent of this branch: `git diff main` was one comment
      line in a different file at run time.
    Minimal cross-check that the text lane itself was never broken:
      mkfs 0.5 + invf-cp 3 MB text + invf-sweep -> "t.txt: text -> PPMd batch",
      "text batches flushed (1 deferred)", and meta_probe --heat reports class 7.

Result: PASS

Remaining TODOs: none for WP620.

  Two follow-ups the orchestrator may want, both deliberately NOT done here
  (AGENTS 1.8: no drive-by fixes; each needs its own WP):
  1. tools/test-writepath.sh mis-reports a missing bin/meta_probe as a semantic failure.
     One-line precondition beside `need()` would turn it into
     "missing bin/meta_probe (run make e2e first)" -- and meta_probe should arguably be
     a dependency of the e2e runner's precondition check for every suite that calls it
     (test-dynzone.sh, test-flushfail.sh, test-heat-mapper.sh have the same exposure).
  2. The CLIC real-corpus leg of test-nbli-lane.sh was exercised only synthetically in
     this environment (no /tmp/invfs-wp620-corpus/clic-prof-valid-2020.zip). The lane's
     real-frame evidence lives in docs/benchmarks/NBLI-BENCH.md:138 (WP620 appendix,
     51/51 decode-back exact), so this is a local-coverage gap, not a code gap.

Scope note: nothing outside tools/test-nbli-lane.sh was touched on this branch, and no
commit went to main. WP620-REPORT.md is the only file written outside the worktree.
