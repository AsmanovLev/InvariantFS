# Architectural Defect Audit Program (2026-11-19)

Motivation: one-off checks have already produced both real defects and false positives. What is needed
is a reproducible procedure, not a lucky hunt. Below are five passes, each answering
its own class of question, and each with a "found/not found" criterion.

Code snapshot for all passes: `f0f9924` (see `CODE-MAP.md`).

## Pass 1. Guard Coverage (funnel analysis)

Question: every guard condition has N paths that are expected to respect it —
how many of them actually do?

Method: for each guard, find the **central function** (funnel) and the **bypass paths**
(functions that clear bits/free without going through the funnel).

Working example: `spt0_block_pinned` lives inside `vol_free_blocks()`
(`volume.c:3843`), so 44 of 46 free sites are protected by construction.
Bypasses: `vol_free_run()` (static, below the funnel), `mbuf_free()`
(`vol_metabuf.c:337` — goes THROUGH the funnel, plus mirrors the counters under
`!v->retain_release`), and the **deliberate** bypass `v->retain_release = 1`
(`vol_spt0.c:1030`, `vol_reclaim.c`): reclaim must free pinned blocks — this is
debt repayment, not a defect.

Candidates for guards of the same type: `pba_ref_count()==0` before freeing,
`vol_name_is_container_sibling()`, `ast_frame_ok()`, `seg_extent_checked()`,
`tz_flush_class_ok()`, `vol_inode_get()!=1` before publishing.

Criterion: a list of `(guard, funnel, bypass paths, which of them are WITHOUT a check)`.
False positives are expected: intentional bypasses need to be marked, not "fixed".

## Pass 2. Path Symmetry

Question: where do paired paths have different sets of guards?

Pairs to compare line by line:
- write (`vol_write_range`) ↔ read (`vol_read_range`);
- one device ↔ two (`INVFS_RAW_MIRROR`, `vmux_*`);
- sealed ↔ unsealed (`vol_seal_*`);
- `newino == inode_id` (in-place supersede) ↔ `newino != inode_id`;
- payload in a blob (`!mbrblob`) ↔ member inode;
- error ↔ success at each commit step (what is rolled back).

Already found by this class: `vol_cpack.c:3342-3349` (in the `newino==inode_id`
branch, superseded segments are not freed, while in the neighboring branch they are).

Criterion: a table of pairs and a list of asymmetries with `file:line`.

## Pass 3. Error Paths (alloc/free on all returns)

Question: on every exit path, is what was allocated freed?

Method: for each function with 2+ allocations — enumerate `return`/`goto` and for
each note what is freed. Found: `vol_png.c:949-953` (`cblob` is not
freed on a serialization error).

Criterion: a list of functions with incomplete freeing.

## Pass 4. Object Lifecycle

Question: every object has four transitions — is each one guarded?

Objects: write session, save point (pin), delta segment, batch owner, extent,
pack handle, plugin client. For each: creation → publish → invalidation →
free; who sets/clears it and what prevents double-free.

Found by this class: `vol_spt0.c:861` (the old pin is freed BEFORE the rejection at
`:886` and before the new one fails to allocate), `vol_delta.c:2225-2236` (blocks of new
segments are freed before the final barrier), `vol_metabuf.c:381-383` (the RT30 slot
is selected before the increment, while reads interpret `seq&1` differently).

Criterion: an "object × 4 transitions × guard" table.

## Pass 5. Mutation Testing of Contracts

Question: if a guard is removed, will at least one test fail? If not, the contract is unprotected.

Method: for each guard from pass 1, temporarily replace the condition with `false` (in a
worktree, not in the main tree), build, and run the affected tests. A guard without
a failing test = a hole in the tests.

Two such cases are already visible: `test-helper-isolation` passes with the
`dlmopen→dlopen` fallback present (`ivpack_loader.c:291,295`), and `test-pack-sign` passes even though
pack registration from env/`.ivpack` does not check the signature (`codec.c:1748`).

Criterion: a list of guards without a witness test; each one is a request for a new test.

## Order

1 and 2 are mechanical and yield a list of specific places; 3 is cheap and almost always
finds something; 4 is the most time-consuming, but catches architectural windows; 5 —
turns what was found into protected contracts.

Passes 1-4 are performed by **blind agents** (a tree without docs and comments, see
`CODE-MAP.md`); pass 5 is local only, because it requires a build and tests.
