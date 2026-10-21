WP: wp/657-read-thread-fail
Files changed (branch wp/657-read-thread-fail, 4 commits, nothing pushed):
  src/core/vol_read.c                    (the INVFS_READ_THREAD_FAIL=N fault lever + the fix:
                                          th[t]=0 BEFORE create, the return CHECKED, the slice run
                                          INLINE when create fails -- mirroring vol_sweep.c:1504-1521)
  src/cli/read_parallel_bitexact_test.c  (+110: the thread-fail leg, asserting bit-exactness with
                                          every create forged to EAGAIN, and that the lever actually
                                          fired via vol_read_thread_fail_forged())
  tools/check-vol-find.sh                (9 ledger entries re-pointed after the lever shifted lines;
                                          verdicts preserved -- ACTS stayed ACTS, SKIP stayed SKIP)

THE DEFECT, and it is worse than the WP doc predicted: with the fix absent, an
EAGAIN from pthread_create made the read return SUCCESS with WRONG BYTES.
  red-before (fix reverted, lever on):
    ..    thread-fail leg: 6 workers, 6 create attempt(s) forged to EAGAIN, read rc=0
    FAIL  thread-fail leg: NOT bit-exact; first diff at offset 7 (got 0x00 want 0x1a)
    read_parallel_bitexact_test: FAILED        RC=1
  green-after:
    ..    thread-fail leg: 6 workers, 6 create attempt(s) forged to EAGAIN, read rc=0
    OK    thread-fail leg: 2097152 bytes bit-exact with every pthread_create failing
    read_parallel_bitexact_test: PASS
The slices whose thread never started were simply never decoded, so the caller got
zeros where the file had data -- silent corruption under thread/memory pressure, not
just the UB/SIGSEGV the doc named. On a box that has already been OOMed twice, that
is the realistic failure.

The lever is inert unless set, read once per fan-out, and capped at 32 (the pool's
own bound). vol_read_thread_fail_forged() exists so the gate can PROVE the leg went
through the failure path instead of passing because the knob was ignored.

Tests run:
  $ make -j4 test                    MAKE_EXIT=0, 33 PASS
      check-vol-find: ok (29 ACTS, 81 SKIP, 110 call sites)
      repo hygiene: OK (729 tracked files)
  $ ./bin/invf-read_parallel_bitexact_test        PASS (all legs)
  red-before / green-after as quoted above, both captured on this branch.
Result: PASS
Remaining TODOs:
  - The other WP653 findings were deliberately NOT touched (each needs its own WP):
    the sweep's unbounded e->length before decoding into a 64 KiB stack buffer, the
    five-way duplicated decode switch, the error-granularity and re-encode-cap
    differences.
  - tools/test-writepath.sh and test-meta-multidev.sh were not re-run on this branch:
    the change is confined to the parallel fan-out's failure path and the unit gate
    above drives that path directly. Run them before the merge if wanted.
