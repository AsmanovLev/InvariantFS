WP: wp/658-sweep-orig-len
Files changed (branch wp/658-sweep-orig-len, 2 commits):
  src/core/vol_sweep.c                   the guard: `orig_len > SEGMENT_SIZE` refuses the segment before
                                         any decode arm touches the 64 KiB stack buffer, with the reasoning
                                         and the honesty note written in the code
  src/cli/sweep_orig_len_test.c (NEW)    14 checks; the fixture builds the poisoned recipe the way a
                                         damaged/foreign one would arrive (healthy file, rewrite one
                                         entry's length, re-store under its own BLAKE3 address, publish a
                                         second inode on it)
  Makefile                               CLI_MAINS + shard dep + run line
  tools/check-vol-find.sh                ledger re-point (+24, one entry 1891 SKIP -> 1915)

RESULT THAT CHANGES THE WP'S PREMISE: the red-before does NOT exist. Measured:
  $ (guard removed, rebuilt) ./bin/invf-sweep_orig_len_test   -> 14 checks, 0 failures
The length lie is refused one step EARLIER by the shared frame check: for
INVFS_ALGO_NONE, ast_frame_ok requires hdr == len (vol_read.c:341-347), so a
recipe entry claiming more than its frame holds never reaches a decode arm.
Reaching an arm with orig_len > SEGMENT_SIZE needs a FRAME larger than
SEGMENT_SIZE, and no writer can emit one -- vol_write.c:193 caps the read-back at
SEGMENT_SIZE and every segment write passes exactly that (leg 2 of the gate
asserts the writer's own shape).

So the guard lands as a cheap invariant / defence in depth, NOT as a bug fix, and
the code says so. The gate is a regression guard on the refusal, not a crash repro.
This is the outcome the WP doc anticipated ("if the red-before fixture turns out to
be impossible to build, that is a RESULT").

What the gate DOES prove (green, 14/14):
  leg 1  a 3-segment file writes, its row reads, its recipe parses to 3 entries
  leg 2  every entry at write time is exactly SEGMENT_SIZE
  leg 3  the poisoned recipe serializes and re-stores under its own address; a
         second inode is published on it
  leg 4  the sweep SURVIVES an over-long entry, and the poisoned inode never
         returns a truncated or clamped payload (the clamp this WP refuses)
  leg 5  the healthy inode sharing the same data stays bit-exact (no collateral)
  leg 6  the recipe AUDIT NAMES the incoherence: 1 fault over 2 checked -- this leg
         replaced a first version that asserted fsck's damaged flag, which was
         VACUOUS here (the scan reported 0 live files, so the check could not fail)
  leg 7  a legal full-size segment still decodes and sweeps bit-exactly: the bound
         is '>' not '>=', so it cannot quietly stop decompressing every normal
         segment

Tests run:
  $ make -j4 test          MAKE_EXIT=0, 33 PASS; sweep_orig_len_test 14/14;
                           check-vol-find ok (29 ACTS/81 SKIP/110 sites);
                           repo hygiene OK (730 tracked files)
Result: PASS
Remaining TODOs:
  - The WP's Design point 4 (enumerate every SEGMENT_SIZE stack array in
    vol_sweep.c and report each one's guard) was NOT done: the task was taken over
    by the orchestrator after the subagent died, and the enumeration was dropped
    for time. It is the one deliberate gap. Command to finish it:
      grep -n "\\[SEGMENT_SIZE\\]" src/core/vol_sweep.c
    and check each against its decode/copy site.
  - tools/test-sweep-lane-retire.sh, test-orphan-reclaim.sh and test-writepath.sh
    were not re-run on this branch; the change is one refusal in the sweep worker
    and `make test` covers the unit tier.
