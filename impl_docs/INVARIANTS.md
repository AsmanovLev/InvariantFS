# InvariantFS Invariants

A list of statements that must hold, and **how each is checked**.
The point of the document: three bugs lived for months not because the architecture was bad, but
because the invariant existed only in a comment. A comment does not fail.

Entry format: `statement` → check → what is printed on violation.

---

## I1. Pairing Passport Matches on Both Devices (Mode A)

**Statement.** After every flush, `DEVT.sync_seq` on dev0 equals `DEVT.sync_seq`
on dev1, and both are on disk (not in the device cache).

**Check.** At runtime: immediately after the write, `vol_write_devt` rereads both
passports and compares them (`INVFS_PERF_ADD(PERF_DEVT_WRITES)` is right there).

- printed: `devt: MISMATCH right after the bump: dev0=N dev1=M (wrote N)`
- limit: 20 messages per process, then a single line "further
  mismatches are not logged"

**Why this way.** A certificate that is checked only when the volume is opened
has only one way to report breakage — copy gigabytes.
A check at the write point tells the truth immediately and costs two reads of 188 bytes each.

**Scope.** The invariant applies to mode A. In mode B, the passports are independent and
there is nothing to compare them with.

---

## I2. Barrier Comes After Writing the Passport

**Statement.** Between the write of `DEVT` and the return from `vol_commit_mirror` there is
`vmux_barrier`.

**Check.** By observation: HDD has no FUA and write cache is enabled, so without
the barrier, `sync_seq` diverges forever (observed 693544/693543, consistently).
There is no automatic check — this is verified by code inspection and I1.

---

## I3. Writing the Passport Does Not Lose to Resync

**Statement.** `DEVT` is written to every device **directly**, not through
the multiplexer.

**Check.** I1 catches a violation at flush time, where resync runs
concurrently (observed `dev0=697511 dev1=697510` with a successful return).

---

## I4. Block 0 Does Not Lose Other Fields

**Statement.** Any writer of block 0 patches **its own** field and does not rewrite the
block from an earlier-read buffer without re-serializing the neighbors.

**Check.** Reference implementation — `vol_write_rdp0`: reads through the mux, updates `DEVT` from
memory with CRC recalculation, writes the whole block through the mux (i.e. to both
devices).

**History.** Before the fix, `vol_write_rdp0` wrote the block only to dev0: shadow did not
receive `RDP0` at all, and a stale `DEVT` could return from the old buffer.

---

## I5. Resync Does Not Copy More Than Is Occupied

**Statement.** The resync boundary = the highest **allocated** block of
metadata, not the end of the reserved zone; pages that match on both
devices are not written.

**Check.** Always printed:
```
vol: mirror resync: dev1 <- dev0 (N bytes in use of the M reserved; ...)
vol: mirror resync: X of N bytes already equal, Y written
```
Expected: `Y` is a few megabytes for a one-block mismatch; `N` is noticeably smaller
than `M`.

**Why safe.** A live metadata page is an allocated block,
so nothing live can exist above the last occupied one. If the bitmap is not loaded —
fall back to the full span: a slow resync is correct, a guessing one is not.

---

## I6. Data Reaches Shadow Before Metadata References It

**Statement.** With `raw_mirror=1` (and with a sweep migration), the segment's copy on
dev1 is written **before** the recipe/inode row is published on dev1.

**Check.** By observing the order in `write_segment_blocks` /
`vol_sweep` (data → recipe → publish → free). There is no automated test —
**debt**: a crash test is needed, aborting at each stage
(`INVFS_*_ABORT_AT`, as with fold).

---

## I7. Old Block Is Freed Only After Publishing

**Statement.** During a segment migration, the old `pba` is not freed and its
refcount is not touched until `publish` has completed.

**Check.** The comment in `vol_sweep.c` describes the transaction; the behavior
is checked by a crash test (see I6, debt).

---

## I8. Namespace Walk Does No Repeated Work

**Statement.** `walk_dir` does not resolve the path from the root for every entry: the inode
number and type are taken from the directory listing.

**Check.** `[perf-trace] build_file_table: total=… (walk=…, qsort=…)`.
A regression is immediately visible: before the fix, walk took 103.8 s, after — 20.9 s on 322,888
files.

---

## I9. The Walk Does Not Stay Silent About Incompleteness

**Statement.** A walk that did not reach the end does not pretend to be complete:
`vol_walk_result` / `vol_walk_commit` set the latch, `vol_walk_abandon`
prints the decision, `build_file_table` marks the table as degraded.

**Check.** The `walk_status*` tests and the `invf-fuse: the namespace walk
did not complete…` messages.

---

## I10. Invariants Are Not Checked "by Eye"

**Statement.** Every statement above is either checked by a runtime line,
or has a test, or is explicitly marked as debt.

**Check.** This file. If an invariant is described in `CONCEPTS.md` but is not here,
it is not checked.

---

## Invariants We Do Not Have but Would Find Useful

- a check that `RDP0` matches on both devices after flush (after
  the I4 fix it should, but nobody looks);
- a check that shadow contains no references to RAW blocks after sweep (this is
  the condition for a "clean" loss of dev0);
- a check that after seal, shadow metadata is complete for all swept files
  (the condition for shadow being readable on its own).
