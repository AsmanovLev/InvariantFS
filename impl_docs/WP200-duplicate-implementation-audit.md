# WP200 — duplicate-implementation audit (read-path + sweep framing twins)

Branch: `wp/duplicate-implementation-audit`
Worktree: `/srv/bench/worktrees/wt-dupaudit`

## Scope

Audit only, plus one fix for the one site that diverges with a demonstrable
red control.

* Audited read-only: every parallel/serial pair, every v2/v3 fork, every
  near-duplicate helper, every length-vs-buffer `memcpy` in `src/` and
  `tools/`. Verdict per site is reported to the orchestrator, not filed here.
* Fixed:
  * `src/core/vol_read.c` — promote `ast_frame_ok()` from a file-static to
    a shared declaration (`src/core/volume_internal.h`).
  * `src/core/vol_sweep.c` — `sweep_thread_worker()` now calls it, so the
    sweep's segment decode cannot be weaker than the read path's.
  * `src/cli/read_parallel_bitexact_test.c` — new sweep leg, the red control.

## Why

`ast_frame_ok()` was extracted in `49b39c6` because the WP94 parallel whole-file
decode was a *second, weaker* implementation of the serial loop's contract: it
did `memcpy(dst, blob, e->length)` where `blob` was an `hdr`-byte allocation,
which is a silent heap over-read that **returns success** — 672 wrong bytes out
of a 314 MB container laundered into a CRC-valid stored segment.

The same structural class is still live one file over.
`sweep_thread_worker()` (`src/core/vol_sweep.c:1458`) is the sweep's own copy of
"decode one stored segment into plaintext":

```c
uint8_t orig[SEGMENT_SIZE];                 /* 64 KiB, on the stack         */
...
} else {                                     /* ALGO_NONE, and anything else */
    memcpy(orig, a->tasks[k].blob_old, orig_len);   /* no framing check    */
}
```

`orig_len` is `e->length` from the recipe; `blob_old` is a `csize_old`-byte
allocation read by `seg_read_checked()`. Nothing checks `csize_old ==
orig_len`. Identical shape to the defect that was just fixed, one layer down,
and it is worse in outcome: the over-read bytes are not merely returned to a
caller, they are **re-framed, re-CRC'd and written back as a stored segment**.
A file the read path refuses becomes, after one sweep, a file that reads
successfully with wrong bytes.

The LZ4/ZSTD arms of the same worker are self-validating (the decompressor is
given `orig_len` as the destination capacity and its return value is compared),
so the divergence is the `else` arm — but the fix is to call the one shared
validator so the arms cannot drift again.

## Design

1. Move `ast_frame_ok()` from `static` in `vol_read.c` to
   `src/core/volume_internal.h` as a shared declaration; keep the definition in
   `vol_read.c` (it is the read path's contract; the sweep now *calls* it
   rather than re-deriving it).
2. `sweep_thread_worker()` calls `ast_frame_ok()` on the same
   `(algo, csize_old, orig_len)` triple before it decodes, and on a refusal
   marks the task invalid exactly as the decompress-failure arms already do —
   the segment is simply not swept, which is the "leave the file alone"
   outcome every other failure in that worker produces.
3. New test leg `run_sweep_frame_disagreement_leg()` in
   `src/cli/read_parallel_bitexact_test.c`, reusing the tamper the existing
   `run_frame_disagreement_leg()` already builds: one entry of an ordinary
   multi-segment file is relabelled `ALGO_NONE` while its length stays the
   original, so the stored LZ4 frame is genuinely shorter than the entry
   claims. The leg then runs `vol_sweep_file()` and re-reads.

   The pinned invariant is not "the sweep fails" — it is that the sweep must
   not turn a frame the read path refuses into stored, CRC-valid bytes.

## Validation

```
$ make test
$ INVFS_E2E_AGENT=wp-duplicate-implementation-audit bash tools/run-e2e.sh tools/test-writepath.sh
```

Red control, before the fix (recorded in the commit message): the new leg
prints

```
  FAIL  sweep leg: sweep ACCEPTED a frame shorter than its entry length; the
        re-read returned 16777216 bytes with the first wrong byte at ...
```

## Out of scope

* `src/core/arc.c`'s missing lock — another agent owns it (per task brief).
* Every other site in the audit that comes back `agrees` or `cannot-tell`:
  reported, not touched.
* The `orig[SEGMENT_SIZE]` **overflow** direction (`e->length > SEGMENT_SIZE`
  for a RAW entry). Reported as its own site; see the audit report — no
  writer in the tree produces such an entry, so it has no red control here.

## Coordination notes

* Subagent: `wp/duplicate-implementation-audit`.
* E2E: `tools/test-writepath.sh` (isolated), serial, with the runner.
* Never committed to `main` (§1.3); the orchestrator merges.
