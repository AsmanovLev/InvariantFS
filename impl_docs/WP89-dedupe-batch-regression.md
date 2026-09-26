# WP89-dedupe-batch-regression — a three-way B+-tree split dropped a separator, and the qcow2 e2e gate was a coin flip

**Branch:** `wp/89-dedupe-batch-regression`
**Worktree:** `/tmp/invfs-wp89`
**Severity:** HIGH (the QCOW2 e2e gate was red on `main`, deterministically, and red/green depended on the filesystem's readdir order)
**Source:** `WP89-TASK.md` — `tools/test-qcow2.sh` failing at `e745838` in the sweep's `[5/7] dedupe` and `[6/7] batches` stages
**Estimated effort:** S (done)

---

## Scope

Four files:

- `src/core/vol_btree.c` — the root cause (a shift loop that is one slot
  short whenever a three-way split is spliced into an internal page) and a
  guard that makes a malformed internal page unpublishable.
- `src/cli/btree_test.c` — a regression test that pins the invariant at the
  level the WP88 test missed (the splice into a *parent*, not the split of a
  *leaf*). Verified failing with the fix reverted.
- `tools/test-qcow2.sh` — the fixture walk is sorted, so "which busybox
  source is the text fixture" stops being a property of how the submodule
  directory happened to be created.
- `INCIDENTS.md` — the entry, with the reproduction and the verification.

No dedupe or batching code is touched. The dedupe pass and the batch flush
were the messengers.

---

## Bug A — the three-way split's parent splice shifted the tail by one slot

**File:** `src/core/vol_btree.c`
**Function:** `bt_ins_rec` (the internal-node branch, the `cu.nptr > 1` splice)
**CWE:** CWE-787 (out-of-bounds read of an uninitialised slot), CWE-672
(operation on an uninitialised value)

`5979bfd` ("an oversized B+ tree record") generalised the two-way splice to
the three-way split. It changed the loop's *bound* and nothing else:

```c
/* before 5979bfd (two-way only, add == 1) */   /* after 5979bfd */
for (j = n; j > i + 1; j--)                     for (j = n; j > i + add; j--)
    e[j] = e[j - 1];                                 e[j] = e[j - 1];
```

With `add == 1` the two are the same. With `add == 2` (a three-way split,
`nptr == 3`) the loop moves every tail record up by exactly ONE slot, so:

- the topmost destination `n + add - 1` is never written — it keeps
  whatever the `malloc` gave it, and
- the two records that should have moved to `i+3`, `i+4` are each one slot
  too low, orphaning the subtree of the old `e[i+1]`.

The page is then written and published with `n += 2` records. Its own CRC
and generation both check out, so nothing downstream can tell: the tree is a
legal page holding an illegal child. Every later `btree_search` for a key
past that separator walks into the hole and returns `-1`.

**Fix** — read the source `add` slots lower, not one:

```c
for (j = n + add - 1; j > i + add; j--)
    e[j] = e[j - add];
```

`add == 1` reduces to the old loop exactly, so the two-way path is
byte-for-byte unchanged.

### How it reached the sweep

`vol_v3_recipe_store` → `btree_search` → `-1`, so:

```
[5/7] dedupe     100.0%  1099/1099 ... failed; sweep data intact
dedupe: pass failed (sweep results are intact)
[6/7] batches ... batch flush failed (rc=-1)
```

`vol_sweep_dedupe_ex` and `tz_v3_commit_member` are both "store a fresh
recipe, then delta-put the inode row", and both got the same `-1`. The sweep
exits non-zero, `test-qcow2.sh` dies on the sweep's status, and it never
prints a `FAIL:` line — which is why the failure looked like it belonged to
dedupe and batching rather than to the tree.

Instrumented trace of the production shape (the `nmi`/gen sequence is from
`bt_ins_rec` itself):

```
ins_split: level=0 n=6 g=3 cut0=1 cut1=5 node=34677 mid=34678 right=34679
splice:    level=1 i=20 n_old=23 add=2 cu.nptr=3
bt_write:  NULL CHILD level=1 idx=24/25 gen=91
bt_search: NULL child at depth=1 (level=1 n=27 ci=26)
```

A leaf eight bytes from a three-way split, spliced into the level-1 root at
record 20 of 23: the two new separators went in, the tail moved one slot
instead of two, and record 24 of the 25-record page was never written.

---

## Bug B — `bt_write` sealed a page with a null child

**File:** `src/core/vol_btree.c`
**Function:** `bt_write`
**CWE:** CWE-672

Not a second bug: the missing seatbelt for Bug A. Every internal page in the
tree leaves through `bt_write`, and a record with no child is a hole in the
key space, so this is the one place that can refuse such a page instead of
sealing it.

```c
if (level != INVFS_PAGE_LEVEL_LEAF)
    for (i = 0; i < n; i++)
        if (e[i].child.pba == 0)
            return -1;
```

With the guard in place and Bug A still present, the same run that produced
23 silent read failures now produces 5 loud ones
("every wide upsert succeeds (no lost separator)") — the corruption is
converted into a failed insert, which is the failure mode a filesystem can
report.

---

## Bug C — the e2e fixture was chosen by readdir order

**File:** `tools/test-qcow2.sh`
**Function:** the fixture generator's `os.walk`

```python
for root, _dirs, files in os.walk(os.environ["REPO"] + "/tools/busybox-src"):
    for n in sorted(files):
```

`files` is sorted, `dirs` is not: `os.walk` yields directories in readdir
order, so which `.c` file above 20 kB became the text fixture depended on
how `tools/busybox-src` had been created. A real
`git submodule update` checkout and a `cp -a` of byte-identical content
enumerate differently:

| worktree | busybox-src | fixture picked | result |
|---|---|---|---|
| `/home/user/InvariantFS` (real checkout) | readdir order A | `archival/dpkg.c` | FAIL |
| `/tmp/invfs-wp89` (`cp -a`) | readdir order B | `scripts/kconfig/expr.c` | PASS |

`diff -r` between the two `busybox-src` trees reports only `.git/index`. The
gate was therefore a coin flip on the filesystem, which is why the bb96b2a
worktree "passed" at a commit that has the same bug as `main`.

**Fix:** `dirs.sort()` in the loop header. One line, and the fixture is now
the same in every worktree.

---

## Why the other three "ruled out" experiments all came back negative

They were all run in `/home/user/InvariantFS`, whose `bin/invf-sweep` is
built from a source that is not the one on disk: `build/obj/invf-sweep.o`
(13:30:24) is older than `tools/invf-sweep.c` (13:30:37), and the binary
carries no dashboard CSS strings while the source emits them. So the reverts
rebuilt some objects and not that one, and the three experiments shared a
broken control. A `make clean && make` in the worktree was the control that
made the rest of the hunt possible: 77 of 80 objects are byte-identical
between the two trees, and the three that differ are `invf-sweep.o`
(stale), `mkfs.o` and `blkio_test.o` (build-date string only).

Worth stating plainly: the tree was NOT the problem. The bug is in the
engine, and it reproduces in a from-scratch build of `e745838`.

---

## Validation

1. **Unit tests:** `make test` — passes. `invf-btree_test` gains
   `test_wide_split_internal` (23 failures with the fix reverted, 0 with it).
2. **Failing input, fixed engine:** the fixture that used to fail
   (`tools/busybox-src` = `archival/dpkg.c`, i.e. the main worktree's pick)
   now sweeps clean: `dedupe 986/986 ... merged=986`, `text batches flushed
   (6 deferred)`, sweep exit 0.
3. **E2E:** `bash tools/run-e2e.sh tools/test-qcow2.sh` → `QCOW2
   CONTAINERPACK E2E: PASS`, migration leg included, run twice.
4. **Neighbouring gates:** `tools/test-ivpacks.sh`, `tools/test-writepath.sh`
   stay green.
5. **Hygiene:** `bash tools/check-repo-hygiene.sh` OK.

### The invariant the new test pins

After an internal page absorbs a three-way split it must still name every
child it held *plus* the two new pieces: the whole key space stays
reachable from the root (`btree_check` reports `nkeys` equal to the record
count) and every key still reads back byte-for-byte. The WP88 test
(`test_wide_records`) pins the *child* half — a leaf that needs three pages
— in a tree one level deep, so the parent's splice never ran; that is the
gap this test closes.

---

## Deliverables

- `src/core/vol_btree.c` — the splice fix (Bug A) + the `bt_write` guard
  (Bug B).
- `src/cli/btree_test.c` — `test_wide_split_internal`.
- `tools/test-qcow2.sh` — `dirs.sort()` (Bug C).
- `impl_docs/WP89-dedupe-batch-regression.md` — this doc.

---

## Out of scope (do NOT touch)

- `sw_duration()` in `tools/invf-sweep.c` (from `e745838`) prints the
  millisecond remainder with an `s` suffix, so any sub-minute stage reads
  `941s` for 941 ms, `26s` for 26 ms. Display only — a one-line fix, but it
  belongs to whoever owns the sweep dashboard, and it is what made the
  original failure report look like two slow stages.
- The other 13 suites that pick their busybox fixture with the same unsorted
  walk (`test-containerpack.sh`, `test-conbatch.sh`, `test-ext4fs.sh`,
  `test-fatfs.sh`, `test-ntfs.sh`, `test-p7z.sh`, `test-rawdisk.sh`,
  `test-resize.sh`, `test-vdi.sh`, `test-xfs.sh`, `bench-fs.sh`,
  `check-repo-hygiene.sh`, and `test-qcow2.sh`'s siblings). One line each,
  but a separate WP: it is a test-hygiene sweep, not this regression.
- The v2→v3 e2e rot (flakey legs 3/6, test-rollback legs A-G).
- The 1-block-per-commit leak at `src/core/vol_write.c:870` (WP-M15).
- `/home/user/InvariantFS`'s stale `bin/invf-sweep`: the orchestrator owns
  the main worktree's build state.

---

## Coordination notes

- Subagent ID for this WP: `wp89-dedupe-batch-regression`
- Pass via `INVFS_E2E_AGENT=wp89-dedupe-batch-regression` when invoking e2e.
- E2E gates run:
  - `bash tools/run-e2e.sh tools/test-qcow2.sh` (twice)
  - `bash tools/run-e2e.sh tools/test-ivpacks.sh`
  - `bash tools/run-e2e.sh tools/test-writepath.sh`
- `tools/busybox-src` is a gitlink, unpopulated in fresh worktrees — copy it
  in from `/home/user/InvariantFS/tools/busybox-src` or a containerpack
  suite dies in fixture generation.
- Dependencies: none. The bug arrived with `5979bfd` (already on `main`).
