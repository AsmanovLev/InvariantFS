# AGENTS.md

> Development workflow + usage guide for InvariantFS.
> Read this before opening a PR, running an e2e test, or shipping a
> volume to production. The "How to use" section is written for end
> users; the "How to develop" section is written for subagents and
> humans working on the codebase.

---

## Part 1 — How to develop

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
  strong codecs (`src/core/vol_write.c:190`, `:203-204`; see §2.4).

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
| **Seal parity** | *not implemented* — the XOR/RS implementation went with the format it belonged to | `invf-sweep --seal` refuses and says so |

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
2. The file's data segments are freed **at `unlink`**: `vol_v3_unlink`
   walks the inode's recipe and frees every non-shared segment through
   `vol_v3_free_recipe_blocks` → `vol_free_blocks`
   (`src/core/vol_dirs.c:598-600`, `src/core/vol_ast.c:125`). Fold drops
   the delta entry. Two things still defer the space, and they are the
   reason a delete-then-check cycle can look like a leak:
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
seven core stages, in this order (`tools/invf-sweep.c`):

| # | stage | what it does | cite |
|---|---|---|---|
| 1 | `prepare` | policy, and on v3 the **savepoint capture** that brackets the run | `invf-sweep.c:1574`, `:1689` |
| 2 | `collect` | walk the live inodes | `:1734` |
| 3 | `transform` | per-file lane dispatch: builtin container lanes, codecpack/containerpack lanes, text/binary batching, EXER carve, generic ZSTD floor — each only after its own bit-exactness guard passes | `:1822` |
| 4 | `heat` | promote hot batch members to standalone segments | `:1955` |
| 5 | `dedupe` | per-segment BLAKE3; cross-file and intra-file merge | `:1991` |
| 6 | `batches` | GC of dead text/binary batches + the shared batch flush | `:2036` |
| 7 | `finalize` | the durability point: checkpoint and volume flush | `:2067`, `:2106` |

Two variants: a two-device volume inserts `tier` (hot/cold balancing) between
`heat` and `dedupe` (`invf-sweep.c:1972`), and `--seal` appends `seal`
(an 8th stage, which **refuses on v3** — see §2.3)
(`:2124`). Under `--dry-run`, stage 3 reports as `plan`.

Note the ordering: **re-encoding (stage 3) happens before reclaiming
(stages 5–6)**, not after. A sweep therefore needs free space for the new
encoding before it gets the old space back.

> **v3 space accounting when a lane supersedes a file.** The builtin
> container lanes do **not** release the superseded recipe's data segments
> on v3 — the `vol_delete_inode` after each `vol_create_*_file` is guarded
> `if (!v3 && ...)` (`src/core/vol_sweep.c:1058`, `:1075`, `:1093`,
> `:1132`, `:1184`). The containerpack lane is two-sided by contrast: it
> branches the *other* way, releasing the superseded recipe on v3
> (`if (v->sb.vol_flags & VOLF_V3) cpack_release_superseded(...) else
> vol_delete_inode(...)`, `src/core/vol_cpack.c:3162-3165` and
> `:3189-3192`; the release is a targeted `vol_v3_free_recipe_blocks`,
> `src/core/vol_cpack.c:2784`). Measured on a 12-file / 1.8 MiB
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
> shared `sweep_retire_superseded()` shape four ways -- with and without the
> savepoint pin, with and without the helper -- and **all four cells were
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
invf-sweep /path/to/volume.img --seal         # not implemented; refuses

# In FUSE: full pass, and it arms a savepoint — see the note below
kill -USR1 $(pidof invf-fuse)                  # request sweep
setfattr -n user.invfs.sweep -v 1 /mount/point  # same, via xattr
```

> **Which triggers arm a rollback savepoint (v3).** All of them. The
> offline `invf-sweep` captures an SPT0 savepoint in `prepare`, before the
> walk (`tools/invf-sweep.c:1689`), and so does every full pass the FUSE
> daemon runs: the watermark pass (`-o raw_watermark=<pct>`) and, since
> WP134, `kill -USR1` and the `user.invfs.sweep` xattr too. All three
> in-FUSE triggers set the same in-process flag
> (`src/cli/fuse_fs.c:1990`, `:3169`), and the sweep thread's
> `invf_sweep_worker` takes the same capture at the same point: under
> `g_io_lock`, after policy and before the live-set walk
> (`src/cli/fuse_fs.c:2055-2086`, the walk at `:2116`). **So a USR1 or
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
> (`src/cli/fuse_fs.c:2361-2447`). Measured on a 1535-block RAW zone with
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

`INVFS_SWEEP_INTERVAL=<seconds>` is **not** the worker above, and it does
**not** switch the pending drain on. The drain — and only the drain, never
`transform`, never dedupe, never re-encode, never a savepoint — is what the
sweep thread's 1 Hz tick does on every tick
(`src/cli/fuse_fs.c:2294`, the drain at `:2340-2350`). The variable gates how
often the thread re-evaluates the *handle-open deferral* around it
(`:2306-2334`): at `N` it checks every Nth tick, and with the default (unset,
so `N == 0`) it never checks at all, which means the drain runs even while a
file handle is open. Both facts used to be invisible, because until the thread
was created in the daemon and not in the `fuse_daemonize` parent, none of this
ran. Reported, not changed; see the WP.

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

### 2.8 Tooling environment

External helper executables (for audio/image transcoding) are looked
up in this order:

1. `$INVFS_TOOLS/<name>` if set
2. `/usr/lib/invfs/tools/<name>`
3. (POSIX) `PATH` — disabled by default for root; set
   `INVFS_REQUIRE_HELPER_PATH=0` to re-enable

On Windows, paths are hardcoded — see `docs/SECURITY.md`. Known risk;
deferred fix.

### 2.8b Where the tool scratch goes

External helpers run on real files, so every transcode and every
containerpack decomposition stages its intermediates in a scratch
directory. The containerpack forward pass pins the **whole container**
there and then extracts **every member** into the same place, so its peak
is roughly **twice the container** — for a 3.5 GB rootfs container, ~7 GB.
Where that lands is a decision, not a constant
(`src/core/tool_scratch.c`):

| root | order | offered |
|---|---|---|
| `$INVFS_TOOL_SCRATCH` | forced, nothing else searched | free space, still sized |
| `/dev/shm` | 1st | `min(statvfs, allocation ceiling)` |
| `/tmp` | 2nd | same |
| `/var/tmp` | 3rd | `statvfs` (a real filesystem on most hosts) |

The **allocation ceiling** is what makes the tmpfs entries safe: a tmpfs
page is charged to the *writing cgroup*, so a tmpfs inside a cgroup with a
finite `memory.max` fails `ENOSPC` while `df` still reports gigabytes free.
It is `min(cgroup memory.high|max − memory.current, /proc/meminfo
MemAvailable) × INVFS_SCRATCH_TMPFS_MAX_FRAC` (default 50) — so a scratch
that *fits* still cannot be the thing that OOMs the box.

tmpfs stays first because for a small job it is genuinely the fast path,
and the intermediates are written once and read once. What changed is that
a root is only eligible if it can hold **this job's byte count**, and when
the count only becomes known mid-job (the member total arrives after
`enumerate`) the scratch **migrates** to a root that can. If nothing can,
the lane refuses and prints the demand, the margin, every root's number
and the remedy — instead of dying later as an opaque `ENOSPC` from the
middle of an extract, leaving the file silently undecomposed.

| variable | default | effect |
|---|---|---|
| `INVFS_TOOL_SCRATCH` | unset | force one root; still capacity-checked |
| `INVFS_SCRATCH_ROOTS` | `/dev/shm:/tmp:/var/tmp` | `:`-separated list, searched in order |
| `INVFS_SCRATCH_MARGIN_MB` | `128` | headroom kept on top of the demand |
| `INVFS_SCRATCH_TMPFS_MAX_FRAC` | `50` | percent of the allocation ceiling a tmpfs may be offered; `0` forces a real filesystem |
| `INVFS_SCRATCH_VERBOSE` | unset | log the decision even when the first-choice root won |

The decision is asserted by `bin/invf-scratch_policy_test` (`make test`).

### 2.9 The bit-exactness contract

**What it means:** every byte of every file written to the volume can
be recovered bit-for-bit, regardless of what codec was applied.

**What it does NOT mean:**

- It does not mean "lossless compression of everything" — but it also does
  not mean "already-compressed files are stored verbatim". What actually
  happens per kind, on v3:
  - **JPEG → JXL lossless**, via the `jxl.codecpack` lane
    (`tools/codecpacks/jxl.codecpack/manifest`;
    `src/core/vol_sweep.c:333-336`, `:866-882`, `:1100`). It is a
    *transcode*, guarded by a decode-back
    `memcmp`, so the bytes come back identical through a different codec.
  - **PNG → JXL lossless**, via the built-in PNGR lane
    (`src/core/vol_sweep.c:1100-1136`). The whole-path rebuild+memcmp guard
    refuses anything it cannot prove, and a refused PNG is simply left RAW
    (measured: `PNGR ...: refused (IDAT is zlib-wrapped, not a raw deflate
    stream); original kept RAW`).
  - **MP4 → stored verbatim.** There is no video lane; the lossless H.264
    model was built, measured, and shelved (`impl_docs/AUDIT.md` WP15).
  - **TAR / GZIP → stored byte-original but *decomposed*.** The lane keeps
    the header bytes verbatim in an IVFT/IVGZ recipe blob, splits the
    payloads into `name!partN` sibling inodes (each compressed by its best
    algorithm), and records the original length as `file_size`; the read
    path rebuilds the container byte-for-byte
    (`src/core/vol_cpack.c:1399-1402`, `:1430-1454`, `:1482-1483`; the
    GZIP sibling is `vol_create_gz_file`, `src/core/vol_cpack.c:1580`). So
    "verbatim" is imprecise: the bytes are exact, but nothing is left as
    one opaque segment. Note this lane's guard is a *size* guard
    ("transcode only when smaller", `src/core/vol_cpack.c:1464-1470`), not
    a runtime `memcmp` — same category as FLAC in
    `impl_docs/AUDIT.md` H10.
  - **ZIP → there is no builtin lane at all.** On v3 a ZIP falls to the
    generic floor or to a containerpack.
- It does not mean "metadata is preserved exactly". mtime is
  truncated to seconds (ctime64 second resolution); plain xattrs are stored
  as opaque blobs; uid/gid are preserved. **POSIX ACLs are the exception
  to "opaque"** — they are stored verbatim as the standard
  `system.posix_acl_access` / `system.posix_acl_default` blob *and
  evaluated daemon-side on every entry point* (`perm_check_cred` /
  `perm_check_traversal_cred`; the evaluation contract is written out at
  `src/cli/fuse_fs.c:621-626`, the two functions are at
  `src/cli/fuse_fs.c:717` and `:787`), because the
  mount deliberately does not negotiate `default_permissions` and so is
  the sole object-level permission authority. ACLs are not decorative.
- It does not mean "files are stored in their original form". They're
  stored in segments with explicit per-segment codecs; the FUSE read
  path reassembles them. Reading the same file twice from the same
  mount may go through different code paths (ARC cache) — both give
  identical bytes.

### 2.10 Common pitfalls

- **"Why is my volume full after a few small writes?"** — the sweep is
  not running, so dead segments and delta entries accumulate. Trigger
  `kill -USR1` or run `invf-sweep` offline.
- **"Why is my read path slow?"** — the ARC cache default is 256 MB;
  tune `arc_limit=<MB>` on mount. Cold reads always go through the
  full decode.
- **"Why does `invf-cat` give me a different byte count than
  `wc -c` on the source?"** — it shouldn't. If it does, the file's
  recipe is corrupt. Run `invf-fsck` first: it names the inode and
  says which live recipes will not load. A recipe blob is stored
  under the BLAKE3 hash of its own contents, so `-f` cannot rebuild
  one that is gone — it reports the file as unreadable and leaves it
  alone. Restore the content from a backup or the original image.
- **"Why does mkfs refuse to format my image?"** — probably the
  metadata zone is too small for the volume size. Use
  `INVFS_META_FRAC=16` or smaller.
- **"Why is FUSE returning EROFS?"** — the volume hit the hard_min
  floor (ENOSPC policy flipped it to read-only). Run `invf-sweep`
  offline to reclaim, then `invf-fuse` again. Check
  `getfattr -n user.invfs.stats /mount/point` for the reason.

### 2.11 When NOT to use InvariantFS

- High-throughput write workloads (updates allocate fresh segments instead of
  patching in place, plus delta/fold overhead and offline consolidation).
- Filesystems where metadata space dominates (e.g., 1M empty files).
- Anything where a kernel panic mid-syscall must not lose data (the
  crash recovery is good but not POSIX-perfect).
- As a drop-in ext4 replacement on production servers. It's not.

When to **do** use it:

- Archival storage where you want bit-exact preservation.
- Container/VM image bases that are mostly read.
- Build roots where the same files are reconstructed many times.
- Anywhere you want dedup + content addressing + on-the-fly codec
  selection without giving up bit-exactness.

### 2.12 Getting help

- `INCIDENTS.md` — production issues and their fixes.
- `impl_docs/AUDIT.md` — security / correctness audit findings.
- `docs/GENTOO-INSTALL.md` — full install guide (VM + bare metal).
- `tools/test-*.sh` — executable examples of every operation.
- `git log -p -- <path>` — design rationale for past changes. The WP docs
  were deleted once shipped: the commit that landed a change is its rationale.
- For new contributors: pick a `WP*` from the discovery list above,
  open a draft, ask for review.

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
