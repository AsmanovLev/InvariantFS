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

- `impl_docs/old_docs/AUDIT.md` — security / correctness audit findings.
- `INCIDENTS.md` — production incidents, including unresolved ones.
- `tools/test-*.sh` failures.
- Fuzz harness crashes (`make fuzz`).
- Code review (a human reading the source).
- User reports.

Each finding gets a one-paragraph writeup **before** it's scheduled,
naming the file:line and the failure mode. No drive-by fixes.

### 1.2 Work Packages (WP)

Every change goes through a WP. The WP doc lives at
`impl_docs/WP<N>-<slug>.md` (template: `impl_docs/WP-TEMPLATE.md`)
and contains:

- **Scope:** the smallest set of files the change touches.
- **Why:** the audit finding / incident / bug it addresses.
- **Design:** the fix shape, with pseudocode where it helps.
- **Validation:** exact test commands and expected outcomes.
- **Out of scope:** explicit list (so reviewers don't ask "did you also fix X?").
- **Coordination notes:** subagent ID, e2e gates to run.

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

### 1.7 Anti-patterns

- Direct commits to `main`.
- Bundling unrelated fixes into one branch.
- Skipping `make test` "because it's just a docs change" — docs build
  rules sometimes break.
- Running an e2e suite without acquiring the lock.
- Reaching into another WP's worktree to "fix one small thing".
- Touching `INCIDENTS.md` / `impl_docs/old_docs/AUDIT.md` without updating the
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
  untransformed when in doubt, which means **it does not compress
  well by default** — the compression happens offline in the sweep.

### 2.3 Zone layout

> **v0.5.0 (Meta-v3):** the four zone fields are *advisory policy*, not hard
> regions — there is one shared free-block pool. Raw-class allocation prefers
> the RAW extent and overflows into shadow-space blocks with the class tag
> **unchanged** (`zone=0`), so placement never decides what a block is. The
> `L2P journal` / `Inode area` / `Mapper` rows below were the v2 model and are
> gone: v3 metadata is a COW B+ tree base plus an append-only Delta Log.

| Zone | Role | Lifecycle |
|---|---|---|
| **Bitmap** | one bit per 4 KiB block | part of the metadata zone; the dirty range is flushed on `vol_flush` |
| **Meta-v3 area** | `RT30` descriptor + COW B+ tree base + append-only Delta Log | base pages are copy-on-write; the delta is merged by the background **fold**; the base-page pool may be free inside the metadata zone |
| **RAW** | content-class tag for freshly written segments | new writes land here; drained into Shadow by the sweep |
| **Shadow** | consolidated, type-clustered, deduplicated storage | grows as RAW drains into it |
| **Seal parity** | optional XOR/RS parity stripes over the shadow pba extent | written by `invf-sweep --seal` |

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
   surgery (the v2 "tombstone" model is gone).
2. The file's blocks are **not** freed immediately; the sweep reclaims them,
   and fold drops the delta entry.
3. `df` reflects the reclaimed space only after the sweep (and the bitmap
   flush) have run.

**Implications:**

- A volume that sees lots of writes-then-deletes will appear to fill up until
  the sweep runs.
- `df` reports the free space after sweep; mid-sweep it can look scary, and
  that is normal.
- Don't write directly to RAW without going through the volume — the format
  isn't a block device you can dd to.

### 2.5 The sweep

`invf-sweep` is the background worker that:

1. Drains RAW segments into Shadow (data movement).
2. Re-clusters text/binary content by type.
3. Reclaims dead blocks: per-segment dedupe (BLAKE3) and GC of dead
   text/binary batches.
4. Re-encodes data with stronger codecs where bit-exactness is proven
   (text → PPMd batches, binaries → ZSTD+BCJ batches, plus the
   container/codecpack lanes).
5. Publishes the new recipes via inode-id-keyed publication.

Manual invocation:

```bash
invf-sweep /path/to/volume.img                # offline (volume unmounted)
invf-sweep /path/to/volume.img --dry-run      # show what would happen
invf-sweep /path/to/volume.img --seal         # also write parity seals

# In FUSE: trigger sweep by signal or xattr
kill -USR1 $(pidof invf-fuse)                  # request sweep
setfattr -n user.invfs.sweep -v 1 /mount/point  # same, via xattr
```

Background sweep in FUSE: set `INVFS_SWEEP_INTERVAL=<seconds>` before
mounting. Default is OFF — opt in explicitly.

### 2.6 Recovery and rollback

After a crash, the volume opens DIRTY and `vol_open` replays the **Delta Log**
(plus the legacy L2P journal on pre-v3 volumes) and scans the metadata. If the
result is anomaly-free the volume transitions to CLEAN automatically;
`invf-fsck [-f]` can also be run explicitly.

To **undo** the last sweep (e.g., a sweep that mis-clustered data):

```bash
invf-rollback /path/to/volume.img
```

On v3, rollback is built on **SPT0 savepoints** (`vol_spt0.c`): a savepoint
records `{base_root, delta_end, flags}` and a restore returns the metadata to
that generation. Without a savepoint, rollback refuses — the volume's history
is gone. (The v2 `CKP0` sweep-checkpoint + `\x01reten` retention registry were
retired with the v2 metadata machinery.)

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

### 2.8 Tooling environment

External helper executables (for audio/image transcoding) are looked
up in this order:

1. `$INVFS_TOOLS/<name>` if set
2. `/usr/lib/invfs/tools/<name>`
3. (POSIX) `PATH` — disabled by default for root; set
   `INVFS_REQUIRE_HELPER_PATH=0` to re-enable

On Windows, paths are hardcoded — see `docs/SECURITY.md`. Known risk;
deferred fix.

### 2.9 The bit-exactness contract

**What it means:** every byte of every file written to the volume can
be recovered bit-for-bit, regardless of what codec was applied.

**What it does NOT mean:**

- It does not mean "lossless compression of everything". A file that's
  already compressed (ZIP, JPEG, MP4) is stored verbatim because no
  useful codec applies — the volume is acting like a dedup'd tarball.
- It does not mean "metadata is preserved exactly". mtime is
  truncated to seconds (ctime64 second resolution); xattrs and POSIX
  ACLs are stored as opaque blobs; uid/gid are preserved.
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
  recipe is corrupt; run `invf-fsck -f` to repair.
- **"Why does mkfs refuse to format my image?"** — probably the
  metadata zone is too small for the volume size. Use
  `INVFS_META_FRAC=16` or smaller.
- **"Why is FUSE returning EROFS?"** — the volume hit the hard_min
  floor (ENOSPC policy flipped it to read-only). Run `invf-sweep`
  offline to reclaim, then `invf-fuse` again. Check
  `getfattr -n user.invfs.stats /mount/point` for the reason.

### 2.11 When NOT to use InvariantFS

- High-throughput write workloads (it's write-append, not
  write-in-place).
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
- `impl_docs/old_docs/AUDIT.md` — security / correctness audit findings.
- `docs/GENTOO-INSTALL.md` — full install guide (VM + bare metal).
- `tools/test-*.sh` — executable examples of every operation.
- `impl_docs/old_docs/WP*.md` — design rationale for non-trivial features.
- For new contributors: pick a `WP*` from the discovery list above,
  open a draft, ask for review.

---

## Versioning

This file (`AGENTS.md`) lives at the repo root. Changes to the
workflow sections (§1.*) require a WP — they're load-bearing.
Changes to the usage section (§2.*) can land directly with a docs
review.
