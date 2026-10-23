# AGENTS.md

> Development workflow + usage guide for InvariantFS.
> Read this before opening a PR, running an e2e test, or shipping a
> volume to production. The "How to use" section is written for end
> users; the "How to develop" section is written for subagents and
> humans working on the codebase.

---

## TODO / Roadmap

This is todo/roadmap list. Once task is done, erase it:
- [ ] **Direct file access to shadow zone (for pro-users / zero-transcode inspection)**:
  Expose raw shadow/recipe data or alternate view (e.g. `/.invariantfs/shadow` or dedicated interface) so power users can read raw underlying blocks directly without triggering re-encoding or on-the-fly decompression pipelines.
- [ ] **Fix SSH blackout / latency during heavy writes (emerge libjxl)**:
  Investigate `g_io_lock` vs `/dev/fuse` queue hold during merge-to-/, implement reader-priority / lock splitting or write backpressure so SSH remains responsive during package installation.
- [ ] **Emerge `media-libs/libjxl` -> install `cjxl`/`djxl` on Gentoo root**:
  Restore tmpfs portage tree, finish binary merge, verify tools.
- [ ] **Install 20 codecpacks via `invf-pack`**:
  Push packs to box, verify and register into `/.invariantfs/codecpacks`.
- [ ] **Release new codecpacks in registry + clean codecpacks out of the main repository tree**.
- [ ] **WP615**: `jxlest` dual CLI + `.so` (`wp/615-jxl-so-plugin`).
- [ ] **WP616**: content batching (classifier table + composer + read-slicing). RAW/opaque first; photo-transcode-first per `docs/benchmarks/JXL-BATCH.md`.
- [ ] **WP615-followup**: demote FLAC/JXL/PNGR builtins to fat packs (read-forever grandfathering for old recipes).
- [ ] **Pack system (WP614)**: anchored `.invariantfs/` semantics, content-addressed pack cache + `@recommended`/`@all` sets, `install --volume` from cache, worker hardening, memory controller, network default-deny except `invf-pack` fetch.

---

## Part 1 — How to develop

### 1.0 Role Check: Primary Agent vs. Subagent

Before proceeding with any task, determine your operational role:
- **Primary Agent (working alone, no parallelism):**
  If you are the sole agent running the session and the change is a minor bugfix, documentation tweak, or local feature, **formal Work Packages (WP) and worktrees are NOT required**. You may edit directly on `main`, run validation tests, and push commits.
- **Subagents / Parallel Workers:**
  When multiple subagents are dispatched in parallel, or when undertaking major breaking architectural rewrites, **full WP discipline (§1.2 - §1.5) with dedicated branches and validation gates is mandatory** to prevent collisions and uncoordinated regressions.

### 1.1 Discovery: where work starts

Sources of work, in rough order of priority:

- `impl_docs/AUDIT.md` — security / correctness audit findings.
- `INCIDENTS.md` — production incidents, including unresolved ones.
- `tools/test-*.sh` failures.
- Fuzz harness crashes (`make fuzz`).
- Code review (a human reading the source).
- User reports.

Each finding gets a one-paragraph writeup **before** it's scheduled,
naming the file:line and the failure mode. No drive-by fixes.

### 1.2 Work Packages (WP)

Every change goes through a WP, written as
`impl_docs/WP<N>-<slug>.md`, containing:

- **Scope:** the smallest set of files the change touches.
- **Why:** the audit finding / incident / bug it addresses.
- **Design:** the fix shape, with pseudocode where it helps.
- **Validation:** exact test commands and expected outcomes.
- **Out of scope:** explicit list (so reviewers don't ask "did you also fix X?").
- **Coordination notes:** subagent ID, e2e gates to run.

The WP doc is a **work-in-progress scratchpad, not a deliverable**: it is
deleted in the same commit that ships the change, because §1.7 makes the
shipped commit the design rationale. Only the long-lived design decisions
deserve to outlive it, and those go in `docs/adr/`.

**Rule:** one WP per logical change. Don't bundle unrelated fixes into
one branch — it makes bisect and revert miserable. Trivially small
fixes can be combined (e.g., WP32's three parser bounds bugs in one
WP), but the WP doc must still list each bug separately.

### 1.3 Worktrees

Each WP gets its own branch + worktree. The orchestrator (a human or
the primary agent) creates them with:

```bash
cd /home/user/InvariantFS
git worktree add /tmp/invfs-wp<N> -b wp/<N>-<slug> main
```

Worktrees live under `/tmp/invfs-wpN/` (one directory per WP).
`main` stays green; WPs land via squash-merge after review.

**Rule:** never commit directly to `main`. Never push to a WP branch
that's still owned by another subagent.

### 1.4 Subagents

A subagent is launched with:

- The WP doc path (`/tmp/invfs-wp<N>/WP<N>-TASK.md`).
- The branch name (`wp/<N>-<slug>`).
- The worktree path (`/tmp/invfs-wp<N>`).
- The scope constraint: "do not modify files outside the listed scope".

Subagent responsibilities:

- Implement the change in their worktree only.
- Run `make test` in their worktree before reporting completion.
- If e2e is required, acquire the e2e lock (see §1.5).
- Report back: list of files changed, exact commands run, snippets of
  pass/fail output, any TODOs they couldn't resolve.

Subagent responsibilities they do **not** have:

- Merging to main (orchestrator only).
- Resolving merge conflicts in other WPs (orchestrator only).
- Running the full e2e suite (each subagent runs only the gates named
  in their WP's Coordination notes).

### 1.5 E2E test coordination

E2E suites use `/dev/shm` image names. The runner at `tools/run-e2e.sh`
is **parallel-safe**: it isolates each suite in its own private mount
namespace with a fresh tmpfs on `/dev/shm`, so the same suite can run
concurrently in different worktrees (parallel subagents) without
colliding on images.

**Two execution modes (chosen automatically per suite):**

- **ISOLATED** (default): suites that do not need the real uid/root and do
  not touch shared `/tmp` paths run in a private `unshare -rm` namespace
  with their own `/dev/shm`. Fully parallel, bounded by
  `INVFS_E2E_SLOTS` (default 4).
- **LOCKED**: suites that use `sudo`/`losetup`/loop mounts, check the uid,
  or use generic `/tmp` paths are serialised on the legacy global lock
  (`/tmp/invfs-e2e.lock`) and run as the invoking user. `/tmp` is not
  isolated because AGENTS worktrees live under `/tmp`.

**Three invocation modes:**

```bash
# Foreground (waits only if the suite is LOCKED and the lock is held)
bash tools/run-e2e.sh tools/test-writepath.sh

# Background (returns immediately; log /tmp/invfs-e2e-bg/<name>.<pid>.log)
bash tools/run-e2e.sh --bg tools/test-writepath.sh

# Wait (blocks until all background suites finish; prints results)
bash tools/run-e2e.sh --wait
```

**Subagent ID attribution:** every subagent sets
`INVFS_E2E_AGENT=<wp-id>` before invoking e2e. The runner records it with
the suite/pid/branch in the holder info (`/tmp/invfs-e2e-slots/slot<N>.info`
for isolated runs, `/tmp/invfs-e2e.lock.info` for locked runs).

Example:
```bash
INVFS_E2E_AGENT=wp31-readpath-bounds bash tools/run-e2e.sh tools/test-writepath.sh
```

**If a LOCKED suite is queued behind the global lock:**

- Subagent MUST NOT skip the runner and invoke the suite directly
  (corrupts shared `/dev/shm` or `/tmp` state).
- Subagent MUST NOT busy-loop or poll aggressively.
- Subagent SHOULD call `bash tools/run-e2e.sh --bg <suite>` and record
  the log path; it runs when the lock frees.
- Subagent SHOULD report back to the orchestrator that e2e is queued,
  with the log path so the orchestrator can check it.

A "hang" in e2e is acceptable; a corrupted `/dev/shm` is not.

**Knobs:** `INVFS_E2E_SLOTS=N`, `INVFS_E2E_NO_NS=1` (force the locked
path), `INVFS_E2E_FORCE_NS=1`, `INVFS_E2E_FORCE_LOCK=1`.

### 1.6 Reporting back

Subagent output format (no variation; orchestrator parses this):

```
WP: wp/31-readpath-bounds
Files changed:
  src/core/vol_read.c        (Bug A: bounds check; Bug B: INVFS_MAX_REC_LEN; Bug C: GZR cap)
  src/cli/resize.c           (Bug D: NULL rz2.io2 after commit)
Tests run:
  $ make test
  ... (output)
  $ bash tools/run-e2e.sh tools/test-writepath.sh
  ... (output)
Result: PASS
Remaining TODOs: none
```

### 1.7 Documentation policy

**The code is the spec. The docs are a map to the code.** This is not a
style preference — the repo carried 18 design docs under `src/doc/` for
months, 15 of which never mentioned Meta-v3 and all of which described the
retired format. They read like contracts, so
people and agents built on them and were wrong. They are now deleted; git
history keeps every revision.

Consequences, and they are binding:

- **A doc that contradicts the code is a bug in the doc**, never a reason to
  change the code. Fix the doc or delete it in the same commit as the code
  change that made it wrong.
- **A subsystem doc cites `file:line`.** A claim that cannot be pointed at in
  the tree is deleted, not softened. `src/core/invarifs.h` is the on-disk
  format authority; `docs/architecture/META.md` is the concept layer over
  it; `impl_docs/DOCMAP.md` is the subsystem→code map.
- **Shipped design rationale lives in the commit that shipped it**
  (`git log -p -- <path>`), not in a side file. The WP docs were deleted for
  this reason; do not recreate that directory.
- **New docs go in `docs/`** (architecture, ADRs, guides, benchmarks), not
  next to the source and not in `impl_docs/` except the generated
  symbol indexes and the live trackers (`AUDIT.md`, `INCIDENTS.md`).
- **Codec/container-packs shall be in "registry" folder (see repo root).**
- Windows and NFS support are parked: document them as parked, do not
  maintain recipes for them.
- `tools/check-repo-hygiene.sh` fails the build when a doc under `docs/`
  references a path that no longer exists. Keep it green.

Rationale and the full inventory of what was deleted and why: git history
for that prose (`git log --diff-filter=D --name-only -- src/doc/`).
`docs/architecture/OVERVIEW.md` is NOT part of that inventory — it is a
65-line zone summary that still exists and is still current.

### 1.8 Anti-patterns

- Direct commits to `main`.
- Bundling unrelated fixes into one branch.
- Skipping `make test` "because it's just a docs change" — docs build
  rules sometimes break.
- Running an e2e suite without acquiring the lock.
- Reaching into another WP's worktree to "fix one small thing".
- Touching `INCIDENTS.md` / `impl_docs/AUDIT.md` without updating the
  status fields — these files are the source of truth for what's been
  fixed and what hasn't.

---

## Part 2 — How to use (with caveats)

### 2.1 What InvariantFS is, in one paragraph

A content-addressed filesystem with a bit-exactness invariant:
**what you write comes back byte-identical**. Compressions are
verified by decompress-and-compare before being trusted; containers
(ZIP/TAR) are stored byte-original with members exposed as on-demand
windows; transcodes happen only where bit-exactness is proven per
file.

### 2.2 What it is NOT

- Not a general-purpose production filesystem. **Experimental.**
- Not crash-safe against arbitrary power loss. (Crash recovery works
  for normal shutdown; power-loss soak tests live in `test-flakey.sh`
  and are not part of `make e2e`.)
- Not a SAN/network filesystem. Single-host, FUSE-based.
- Not magic. The bit-exact invariant is achieved by storing things
  untransformed when in doubt, which means **it compresses poorly by
  default** — the write path stores fast LZ4 (ZSTD only once RAW fill
  passes 80%, via the adaptive effort ladder) and falls back to verbatim
  when a segment does not shrink; the sweep is what re-encodes into the
  strong codecs (`src/core/vol_write.c:251`, `:351-354`; see §2.4).

### 2.3 Zone layout

> **v0.5.0 (Meta-v3) is the only format.** The four zone fields are *advisory
> policy*, not hard regions — there is one shared free-block pool. Raw-class
> allocation prefers the RAW extent and overflows into shadow-space blocks
> with the class tag **unchanged** (`zone=0`), so placement never decides
> what a block is. Metadata is a COW B+ tree base plus an append-only Delta
> Log; there is no mapping journal and no inode-record stream.

| Zone | Role | Lifecycle |
|---|---|---|
| **Bitmap** | one bit per 4 KiB block | part of the metadata zone; the dirty range is flushed on `vol_flush` |
| **Meta-v3 area** | `RT30` descriptor + COW B+ tree base + append-only Delta Log | base pages are copy-on-write; the delta is merged by the background **fold**; the base-page pool may be free inside the metadata zone |
| **RAW** | content-class tag for freshly written segments | new writes land here; drained into Shadow by the sweep |
| **Shadow** | consolidated, type-clustered, deduplicated storage | grows as RAW drains into it |
| **Seal parity** | native v3 seal (WP201, par2-inspired, no byte-compat): per-chunk RS groups + footer written last | `invf-sweep --seal [5\|10\|20\|25]` seals (default 10); `--unseal` removes |

### 2.4 The write path (most important caveat)

**Writes are write-once at the segment level; metadata is a COW B+ tree plus
an append-only Delta Log.** When you `write()` through FUSE:

1. Data lands in RAW as **new segments** — LZ4 by default (verbatim when
   incompressible; ZSTD under fill pressure via the adaptive effort ladder).
   A ranged write forks the recipe and allocates fresh segments for the
   touched ranges; untouched segments are aliased, never overwritten in place.
2. The inode/dirent mutation is appended to the **Delta Log**. An in-memory
   overlay makes it visible to readers immediately, without blocking.
3. The background **fold** merges accumulated delta records into the immutable
   COW B+ tree base and publishes the new root atomically through the `RT30`
   double slot.
4. The bitmap is updated in RAM; the dirty range is persisted on flush/close.

When you `unlink()`:

1. A delta delete entry is appended — there is no immediate in-place record
   surgery.
2. The file's data segments are **retired at `unlink`, freed at the next
   fence**: `vol_unlink` stages the row+dirent deletes and files a retire
   entry on the row (`vol_retire_recipe`, `src/core/vol_dirs.c:900`;
   resolution via `vol_free_recipe_blocks`, `src/core/vol_ast.c:136`).
   The first successful fence covering the deletes frees the blocks -- and
   only while the row is still absent (a fence-failure doom restores the
   row and drops the entry, so a resurrected file keeps its blocks; an
   unloadable recipe restores the name right away with `-1`, WP604).
   Fold drops the delta entry. Three things still defer the space, and
   they are the reason a delete-then-check cycle can look like a leak:
   - a **live SPT0 savepoint** (§2.6) pins every block the captured
     generation's recipes named, so the blocks stay allocated until the
     next capture drops that window and reclaims what no live recipe
     names (`spn_reclaim`, `src/core/vol_spt0.c`). On a mount with
     `raw_watermark` set, the ladder takes that capture itself (§2.5); on
     a mount without one, it waits for the next sweep — by hand, via
     `kill -USR1`, or at the next maintenance window.
   - a `zone == TEXT` entry names a *shared* batch segment, which the
     targeted free deliberately skips — `tz_v3_gc` reclaims it when the
     last live member drops (`src/core/vol_ast.c:153`).
   - the retire window itself: between the unlink and the next fence the
     blocks are still allocated (a sync/fsync, the next grouped commit,
     or close drains them; `df` converges then, not at the unlink).
3. The bitmap is a derived cache, so `df` reflects the free only after the
   bitmap has been flushed.

**Implications:**

- A volume that sees lots of writes-then-deletes can still appear to fill up
  until a sweep drops the savepoint pin described in §2.4 step 2 — which, on
  a `raw_watermark` mount, the ladder does by itself (§2.5).
- `df` reports the free space after the bitmap flush; mid-sweep it can look
  scary, and that is normal.
- Don't write directly to RAW without going through the volume — the format
  isn't a block device you can dd to.

### 2.5 The sweep

`invf-sweep` is the background worker. A single-device, non-`--dry-run` run is
eight core stages, in this order (`tools/invf-sweep.c`):

| # | stage | what it does | cite |
|---|---|---|---|
| 1 | `prepare` | checkpoint + policy (F9: the capture used to live here; it moved to after `collect` validates, so a refused sweep leaves the volume untouched) | `invf-sweep.c:1787` |
| 2 | `collect` | walk the live inodes | `:2130` |
| 3 | `transform` | per-file lane dispatch: builtin container lanes, codecpack/containerpack lanes, text/binary batching, EXER carve, generic ZSTD floor — each only after its own bit-exactness guard passes | `:2292` |
| 4 | `heat` | promote hot batch members to standalone segments | `:2477` |
| 5 | `dedupe` | per-segment BLAKE3; cross-file and intra-file merge | `:2513` |
| 6 | `batches` | GC of dead text/binary batches + the shared batch flush | `:2558` |
| 7 | `migrate` | tier migration between devices (WP601) | `:2603` |
| 8 | `finalize` | the durability point: checkpoint and volume flush | `:2612` |

Two variants: a two-device volume inserts `tier` (hot/cold balancing) between
`heat` and `dedupe` (`invf-sweep.c:2494`), and `--seal` appends `seal`
(a 9th stage, native v3 since WP201)
(`:2649`). Under `--dry-run`, stage 3 reports as `plan`.

Note the ordering: **re-encoding (stage 3) happens before reclaiming
(stages 5–6)**, not after. A sweep therefore needs free space for the new
encoding before it gets the old space back.

> **v3 space accounting when a lane supersedes a file.** Both the builtin
> container lanes and the containerpack lane retire the superseded recipe
> through the same shared helper, `vol_release_superseded_blob`
> (`src/core/vol_ast.c`; called from six lane sites in
> `src/core/vol_sweep.c:487-737` and via `cpack_release_superseded`,
> `src/core/vol_cpack.c:3448-3452`, at `:4082` and `:4106`). The old
> two-sided shape (builtin lanes skipping the release on v3 while cpack
> branched `if (VOLF_V3)`) is gone; every lane retires the same way now.
> Measured on a 12-file / 1.8 MiB
> TAR corpus (v3 image, `invf-mkfs` + `invf-import` + `invf-sweep`,
> `invf-fsck` free-block counts): a lane sweep left **478 free blocks
> stranded** as "unclaimed — allocated, no live reference"; the next bare
> sweep reclaimed 456 of them and the count then stayed flat across further
> sweeps. Part of that swing is inherent, not a defect: a successful
> container lane stores the members *and* the recipe before the old
> segments are given up (§2.9). So this is a **one-sweep lag, not an
> unbounded leak** — the space comes back when the next capture drops
> the savepoint pin (§2.4 step 2), which on a `raw_watermark` mount the
> ladder arranges by itself (§2.5) — and `invf-fsck -f` does **not** recover
> it on v3 (measured: unchanged after a full `-f` pass).
>
> **CORRECTION — the 478 was not measured to be lane retirement.** A later
> four-cell measurement on a named corpus (12 TARs, 122,880 B each,
> 2,836,480 B total, v3 image, `invf-fsck` free blocks) implemented the
> shared retire-helper shape (`vol_release_superseded_blob`) four ways --
> with and without the savepoint pin, with and without the helper -- and **all four cells were
> identical** (249413 free after the lane sweep). The shape reclaimed
> **zero** additional blocks on that corpus. The 253-block swing between
> sweep 1 and sweep 2 is **the savepoint pin being discharged at the next
> capture, not lane retirement.** What drives the swing was not established;
> the two candidates are the pin and the superseded recipe blob no longer
> loading. Keep the 478 figure as the historical observation it is, and do
> not attribute it to a lane policy.
> `INVFS_RECLAIM_ORPHANS=0` turns the orphan collector OFF. It is **on by
> default** since 2026-09-28 (`src/core/vol_reclaim.c:29`); this line used to
> say "default-off" and did not describe the build.

Manual invocation:

```bash
invf-sweep /path/to/volume.img                # offline (volume unmounted)
invf-sweep /path/to/volume.img --dry-run      # show what would happen
invf-sweep /path/to/volume.img --seal         # seal with default menu (pct 10)

# In FUSE: full pass, and it arms a savepoint — see the note below
kill -USR1 $(pidof invf-fuse)                  # request sweep
setfattr -n user.invfs.sweep -v 1 /mount/point  # same, via xattr
```

> **Which triggers arm a rollback savepoint (v3).** All of them. The
> offline `invf-sweep` captures an SPT0 savepoint after the live set
> validates, before `transform` (F9: capturing in `prepare` meant a sweep
> refused at `collect` kept its prepare-time frees -- a refused sweep must
> be bit-identical to one never run, and `collect` is read-only so the
> window still brackets every mutation), and so does every full pass the
> FUSE daemon runs: the watermark pass (`-o raw_watermark=<pct>`) and,
> since WP134, `kill -USR1` and the `user.invfs.sweep` xattr too. All three
> in-FUSE triggers set the same in-process flag
> (`g_sweep_now`, set at `src/cli/fuse_fs.c:2183` by the USR1 handler and
> at `:3458` by the xattr, consumed by the tick at `:2512`), and the sweep thread's
> `invf_sweep_worker` takes the same capture under `g_io_lock`, after the
> sweepable list validates and before the first mutation (F9-worker: the
> capture used to run at entry, so every refuse path mutated first and
> refused second -- same fix as the offline reorder).
> (`src/cli/fuse_fs.c:2326-2340`, the walk at `:2300-2307`). **So a USR1 or
> xattr sweep on v3 CAN be rolled back**: unmount, then `invf-rollback`.
> The daemon prints that command when it arms the window — but only on a
> mount started with `-f`, because `fuse_daemonize` sends the daemon's own
> stdout and stderr to `/dev/null`; on a default mount the xattr
> `savepoint=` view is the only place that output can be read.
> `getfattr -n user.invfs -m- /` reports `savepoint=live|none` so the
> operator can see the window instead of taking the daemon's word for it.
>
> **The window costs the blocks it pins, for one generation.** A capture
> pins every block the pre-sweep generation's recipes named, so those
> blocks stay allocated while the window is live; the next **capture** —
> from a sweep, whether the operator ran it or the watermark ladder did —
> drops the old window and reclaims what no live recipe still names
> (§2.4 step 2). Measured on a 12-file / 3.0 MiB text corpus plus a
> 3.0 MiB TAR (v3 image, `invf-mkfs` + `invf-import`, then `kill -USR1`):
> the pass pinned **414 blocks** in a 33-block mark set, and the entire
> free-block difference against the same sweep run with no window was
> **447 blocks** — the pin plus the mark set, and nothing else. Same order
> of magnitude as the 478-block lane lag above: one generation, not a leak.
>
> **The watermark ladder repays that generation itself.** The debt is
> created by a pass and collected by the *next* capture (`spn_reclaim`),
> so a re-arm rule based on the fill alone can deadlock: the hold is what
> keeps the fill up, so the pass that armed the window is the last one
> that ever runs and the space only came back when an operator swept by
> hand. The ladder therefore takes the one capture it owes per window, and
> stops on two measured conditions rather than any constant: the **mark**
> (an owed capture is not taken on a volume that is not over its own
> watermark — under it there is no pressure to relieve, and the debt waits
> for the next capture of any kind), and the first capture that reclaims
> **nothing**, which proves the live window is holding only live blocks
> because a pass that superseded nothing created no debt. That is what
> makes the cycle finite instead of a sweep that re-pins forever
> (`src/cli/fuse_fs.c:2569-2614`). Measured on a 1535-block RAW zone with
> 697 blocks staged offline: **2 passes, the second reclaiming all 697,
> RAW fill back to 0, and quiet from ~5 s on** — with no `invf-sweep`, no
> write, no signal and no unmount in between.
>
> The USR1/xattr pass **fails closed**: if the capture is refused it
> abandons the pass rather than rewrite the data with no way back. The
> watermark pass keeps its older fail-open behaviour.
>
> **Every trigger fails closed when the window cannot be proven** (F7).
> A capture that returns 0 may still not exist on disk: a device can
> acknowledge a write it discarded (dm-flakey `drop_writes`), and the old
> `spt0_info` check only reported in-memory state, so a sweep that started
> in a drop window published a torn generation with no savepoint behind
> it. All triggers now re-read the SPT0 descriptor from the device --
> through a fresh O_DIRECT open, since the page cache would return the
> sweep's own bytes -- and require the captured identity *including a
> per-capture generation nonce* (without it, a dropped store over an
> unchanged volume reads back the previous identical capture and verifies
> falsely). On mismatch the pass is abandoned and the volume is untouched.
> On *explicit* capture refusal the older split remains: USR1/xattr abandon,
> while the watermark pass and the offline explicit-refusal path proceed
> windowless.

`INVFS_SWEEP_INTERVAL=<seconds>` controls periodic background sweeping:
- **`-1` (or unset, default):** never swept automatically in background. Sweeping only occurs via explicit commands (`invf-sweep`, `kill -USR1 $(pidof invf-fuse)`, or the `user.invfs.sweep` xattr).
- **`0`:** always sweep on every 1 Hz tick.
- **`>0`:** sweep periodically every N seconds when idle (no open file handles).

The on-demand pending drain (delta commits and stamps) operates independently from this periodic full pass.

> **The drain YIELDS to a live foreground write path (WP626), and is
> CHUNKED so reads are served between files (Oct 22 2026 bare-metal
> incident).** Two independent properties, both about the drain's hold
> of `g_io_lock`, which every `FUSE_WRITE`, `.flush`, `.fsync` AND every
> metadata/read must take before it can be served:
> - **WHEN it starts (WP626):** the drain does not start on a tick where
>   the write path arrived within the last second (`g_fg_write_ns`
>   stamped at write ENTRY — a request queued on the lock is still a
>   request the drain would be making wait); it runs on the first tick
>   after arrivals stop, announcing the yield once per streak
>   (`on-demand drain: yielding to a live write path`).
> - **HOW LONG the hold lasts (chunking):** the tick drains at most ONE
>   pending entry per hold (`vol_sweep_pending_cap(g_vol, 1)`) and
>   releases the lock (`unlock; usleep(500); lock`) between entries,
>   re-checking the write gate and shutdown each round. Worst-case hold
>   is one file's sweep, not the whole pending set — which is what used
>   to freeze READS (an sshd child reading its host keys, getty's login)
>   for the length of a 13–49 s drain on a 110 GB two-device rootfs. A
>   write arriving mid-drain aborts the remainder to the next tick
>   (`PARTIAL` in the log line).
>
> Identical work, reclaim and codecs in both cases — only the hold shape
> changed. Nothing is lost by waiting: the pending list is RAM-only
> bookkeeping, and USR1, the `user.invfs.sweep` xattr and the
> `raw_watermark` ladder are separate triggers the gates do not touch.
> `INVFS_SWEEP_DRAIN_DEFER=0` turns the start-gate off (the A/B control
> leg of `tools/test-writeback-stall.sh`, which also asserts LIVENESS —
> the drain resumes on its own once the write path goes quiet).
> `INVFS_SWEEP_DRAIN_DEFER=0` turns it off (`src/cli/fuse_fs.c:2614`) and
> that is the A/B control leg of `tools/test-writeback-stall.sh`, which also
> asserts LIVENESS — the drain resumes on its own once the write path goes
> quiet, because a deferral that never ends would be a leak, not a yield.

### 2.6 Recovery and rollback

After a crash, the volume opens DIRTY and `vol_open` replays the **Delta Log**
and scans the metadata. If the result is anomaly-free the volume transitions
to CLEAN automatically; `invf-fsck [-f]` can also be run explicitly.

A volume whose superblock does not carry `VOLF_V3` is refused by name, by
every tool, with the image untouched. There is no conversion path in-tree and
there never was: the only route is `invf-mkfs` a new volume and re-import, or
restore from a backup.

> **What `invf-fsck -f` will and will not do on a damaged v3 volume.** An
> unreadable base page is *contained*, not fatal: its key range is
> **quarantined**, every other subtree stays readable, and a key inside a
> quarantined range reads `EIO` rather than answering "absent". `-f` then
> *excises* the quarantined ranges — it drops a key **range**, not a page — but
> only after it has shown that no live, readable object requires a key inside
> one: an inode row requires its recipe blob (keyed `0x04 || BLAKE3(recipe)`)
> and its xattrs (`0x03 || inode || …`), and a directory entry requires the
> row it resolves to. A range it cannot clear is **left alone**: the volume is
> changed not at all, stays `DAMAGED`, exits 3, and prints
> `CANNOT REPAIR` naming the files at stake. The keys stay on the page they are
> on and keep reading `EIO`, which a restore can undo — an excised key cannot
> be, so the pass will not take that decision for you. `-f --discard-reachable`
> is the separate, explicit way to say "drop them anyway"; it names the cost
> first, and the volume still reads `DAMAGED` afterwards. A damaged volume is
> therefore never reported clean, whatever the flag.
>
> One consequence worth knowing: a torn **rightmost** leaf is quarantined as
> `[its first key, +inf)`, and every `0x03`/`0x04` key sorts above such a
> bound, so nothing in those namespaces can be enumerated while the page is
> unreadable. `-f` therefore always refuses that case. It is the honest answer,
> not an oversight — the flag is the way past it.

To **undo** the last sweep (e.g., a sweep that mis-clustered data):

```bash
invf-rollback /path/to/volume.img
```

> **Check there is something to roll back to.** On v3 a savepoint only
> exists if something *captured* one, and since WP134 that is every sweep
> trigger: the offline `invf-sweep`, the in-FUSE watermark pass, `kill
> -USR1`, and the `user.invfs.sweep` xattr (§2.5). The USR1/xattr pass
> fails closed, so a data-rewriting pass that ran is a pass that got its
> window. If `invf-rollback` still prints `no save point` and exits 1
> (`tools/invf-rollback.c:75-78`), no trigger armed one on this volume —
> check `getfattr -n user.invfs -m- /` for `savepoint=none` (a live
> window from a *previous* pass may also have been consumed by an earlier
> rollback, which is the point of no return).

On v3, rollback is built on **SPT0 savepoints** (`vol_spt0.c`): the 32-byte
`SPT0` descriptor at block-0 offset `0xA00` records
`{base_root, delta_end}` (plus a reserved `flags` and a `crc32c`
over the descriptor, `src/core/invarifs.h:683-703`), and a restore publishes
`base_root` and truncates the delta to `delta_end`. Without a savepoint,
rollback refuses — the volume's history is gone.

There is no read-only "mount at the checkpoint" view: `-o at_checkpoint` and
`vol_open_at()` were removed, because the only thing they could restore was
the retired mapping journal's staged prefix, so they had never succeeded and
returned err -11 for every mount. A mount that passes the option now mounts
normally, read-write; libfuse rejects the unknown option itself.

### 2.7 Capacity and the metadata reservation

For a Linux rootfs with ~50k+ small files, the default `INVFS_META_FRAC=64`
is too small. The comment in `src/cli/mkfs.c` says **"rootfs images want
16-24"** — use:

```bash
INVFS_META_FRAC=16 invf-mkfs /path/to/rootfs.img 30
```

Symptoms of an undersized metadata zone: ENOSPC on writes even though
`df` shows plenty free, and v3 base-page allocation falling back to the
shadow pool (`mb_alloc_meta_zone` → shadow) in the FUSE log.

> **This knob does NOT bound inode capacity — measure before you rely on
> it.** `mb_alloc_meta_zone`'s own comment says why: mkfs marks
> the whole metadata zone allocated (bitmap + mapper + record area), so in
> practice the zone scan finds nothing and `mbuf_alloc` falls through to
> the **shared pool**. Measured on a 4 GiB image: 60 files → 106 META
> blocks, 120 → 228, 240 → 471, i.e. **2.03 blocks (8,309 B) of META per
> file, flat** — and the *same* 240-file run gave **471 blocks at
> `INVFS_META_FRAC=16` and 472 at `INVFS_META_FRAC=64`**, identical. The
> inode count a volume can present is therefore set by **total volume
> size**, not by this fraction: at 4 GiB that is roughly 480,000 files.
> The per-file figure is extrapolated past 240 files; the three-point
> linearity is clean but the extrapolation is an extrapolation.
> A 32 MiB reserved gap between the mapper and the record area is dead
> weight, and a lower fraction reserves less of it — which is the honest
> reason the older advice said "16-24".

### 2.8 Developer Notes

#### Tooling Environment & External Helpers
External helper executables (for audio/image transcoding) are looked up in this order:
1. `$INVFS_TOOLS/<name>` if set
2. `/usr/lib/invfs/tools/<name>`
3. (POSIX) `PATH` — disabled by default for root; set `INVFS_REQUIRE_HELPER_PATH=0` to re-enable
*(On Windows, paths are hardcoded — see `docs/SECURITY.md`).*

#### Codecpack Storage & Precedence
The **packs** themselves have their own store on the volume, **not in `/usr/lib`**:
- **The canonical store is the volume's own `/.invariantfs/codecpacks/`** (`<name>.codecpack/` dirs). `vol_open` loads them into the registry at mount, read-only and best-effort (`vol_load_volume_packs`, `src/core/volume.c:922`). This is what makes a volume self-hosting: the packs travel with the data.
- **Install packs there with the manager** `invf-pack` (`src/cli/pack.c`): installs to host pack root `/.invariantfs/codecpacks` by default, or `<mnt>/.invariantfs/codecpacks` with `--on-volume <mnt>` (`--root` / `INVFS_PACK_ROOT` overrides).
- **`/usr/lib/invfs/codecpacks` is only a host-dir FALLBACK**, scanned by the codec probe *when `INVFS_CODECPACKS` is unset* (`src/codecs/codec.c:741`). Packaging seeds it into initramfs so rootfs finds packs before the volume store is readable.
- In-process plugin origin gate anchors on `/.invariantfs/codecpacks` first (`src/core/vol_plugin_inproc.c:178`). Set `INVFS_CODECPACKS` (colon dirs) to replace host fallback; volume store always loads independently.

#### Tool Scratch Staging
External helpers stage intermediates in a scratch directory:
- `$INVFS_TOOL_SCRATCH` (forced, if set)
- `/dev/shm` (1st default: `min(statvfs, allocation ceiling)`)
- `/tmp` (2nd default: same)
- `/var/tmp` (3rd default: `statvfs`)
- `allocation ceiling`: `min(cgroup memory.high|max - memory.current, /proc/meminfo MemAvailable) * INVFS_SCRATCH_TMPFS_MAX_FRAC` (default 50%). If member totals exceed root capacity, scratch dynamically migrates to another root (`src/core/tool_scratch.c`).

#### The Bit-Exactness Contract
Every byte of every file written to the volume can be recovered bit-for-bit, regardless of codec applied:
- **JPEG → JXL lossless**: built-in JXL lane (`vol_sweep.c`). Transcode guarded by decode-back `memcmp`. Optional: tools absent leaves file RAW.
- **PNG → JXL lossless**: built-in PNGR lane (`vol_sweep.c:1100`). Whole-path rebuild + `memcmp` guard. Opt-in: `-o photo_codec=nbli` routes via NBLI pack.
- **TAR / GZIP**: stored byte-original but *decomposed*. Headers in IVFT/IVGZ recipes, payloads in `name!partN` sibling inodes, rebuilt byte-for-byte on read (`vol_cpack.c`).
- **MP4 / Video**: stored verbatim.
- **Metadata**: mtime truncated to seconds; plain xattrs opaque blobs; **POSIX ACLs are evaluated daemon-side on every entry point** (`perm_check_cred` / `perm_check_traversal_cred`) since the mount does not negotiate `default_permissions`.

#### Common Pitfalls
- **Volume full after small writes?** Background sweep is not running; dead segments and delta entries accumulate. Run `kill -USR1 $(pidof invf-fuse)` or `invf-sweep`.
- **Read path slow?** Default ARC cache is 256 MB; tune `arc_limit=<MB>` on mount.
- **`invf-cat` byte count mismatch?** Corrupted recipe. Run `invf-fsck` to identify unreadable recipes.
- **mkfs refuses image?** Metadata zone too small for volume size. Use `INVFS_META_FRAC=16` or smaller.
- **FUSE returning EROFS?** Hit hard_min floor (ENOSPC policy flipped read-only). Run `invf-sweep` offline, then inspect `getfattr -n user.invfs.stats <mnt>`.

#### When NOT to Use InvariantFS
- High-throughput write workloads (updates allocate fresh segments instead of in-place patching).
- Workloads dominated purely by empty metadata files.
- As a generic drop-in ext4 replacement on production servers.
*Best for:* archival storage, container/VM base images, rootfs with read-mostly workloads, build roots.

#### Getting Help
- `INCIDENTS.md` — production incidents and root cause analyses.
- `impl_docs/AUDIT.md` — security and correctness audit findings.
- `docs/GENTOO-INSTALL.md` — full installation guide (VM + bare metal).
- `tools/test-*.sh` — executable validation suites.

---

## Versioning

This file (`AGENTS.md`) lives at the repo root. Changes to the
workflow sections (§1.*) require a WP — they're load-bearing.
Changes to the usage section (§2.*) can land directly with a docs
review.

# The GitNexus block used to live here (<!-- gitnexus:start --> ... <!-- gitnexus:end -->).
#
# Removed: it cited the per-skill markdown under .claude/, and that whole
# directory is now untracked (it is
# regenerated by `npx gitnexus analyze` on each machine that wants it). A tracked file
# must not cite an ignored path -- that is what made `repo hygiene` fail in CI with
# DOCROT while passing locally, twice, because the local tree always HAS .claude/.
#
# The tool itself is still wired up for agents: `~/.pi/agent/mcp.json` registers the
# gitnexus MCP server, and .gitnexus/run.cjs is the CLI shim for it.
