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

E2E suites share `/dev/shm` image names — running two in parallel
corrupts each other's state. The lock infrastructure already exists
at `tools/run-e2e.sh` and serialises suites with `flock` on
`/tmp/invfs-e2e.lock`.

**Three invocation modes:**

```bash
# Foreground (blocks until the lock is free, then runs)
bash tools/run-e2e.sh tools/test-writepath.sh

# Background (returns immediately; log goes to /tmp/invfs-e2e-bg/<name>.log)
bash tools/run-e2e.sh --bg tools/test-writepath.sh

# Wait (blocks until the lock is free AND all background suites finish;
# prints results)
bash tools/run-e2e.sh --wait
```

**Subagent ID attribution:** every subagent sets
`INVFS_E2E_AGENT=<wp-id>` before invoking e2e. The runner reads it
and includes it in `/tmp/invfs-e2e.lock.info` so concurrent runs are
distinguishable in `who_running` output.

Example:
```bash
INVFS_E2E_AGENT=wp31-readpath-bounds bash tools/run-e2e.sh tools/test-writepath.sh
```

**If the lock is held when a subagent wants to run e2e:**

- Subagent MUST NOT skip the lock and run the suite directly (corrupts
  shared state).
- Subagent MUST NOT busy-loop or poll aggressively.
- Subagent SHOULD call `bash tools/run-e2e.sh --bg <suite>` and record
  the log path; the suite runs in order when the lock frees.
- Subagent SHOULD report back to the orchestrator that e2e is queued,
  with the log path so the orchestrator can check it.

A "hang" in e2e is acceptable; a corrupted `/dev/shm` is not.

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

| Zone | Role | Lifecycle |
|---|---|---|
| **Bitmap** | one bit per block | part of metadata zone; rewritten on every alloc |
| **L2P journal** | append-only logical-to-physical mapping | grows; compacted by sweep |
| **Inode area** | append-only file records (+ INO2 meta-ext) | grows; compacted by sweep |
| **Mapper** | metadata extent table (v0.3.0+) | small fixed-size table; extents grow dynamically |
| **RAW** | linear landing area for new writes | grows toward shadow; swept into shadow |
| **Shadow** | consolidated, type-clustered storage | grows as RAW drains into it |

### 2.4 The write path (most important caveat)

**Writes are append-only at the zone level.** When you `write()`
through FUSE:

1. Data lands in RAW, LZ4-compressed (or verbatim under TURBO profile).
2. An inode record is appended to the inode area with the new pbas.
3. The bitmap is updated and the journal is appended-to.

When you `unlink()`:

1. The file's pbas are NOT freed immediately.
2. The inode record gets a **tombstone** (position-kill DELT).
3. The blocks become freeable when the next sweep runs and rewrites
   the inode area without the dead entries.

**Implications:**

- A volume that sees lots of writes-then-deletes will appear to fill
  up until the sweep runs.
- `df` reports the **virtual** free space after sweep + compaction.
  Mid-sweep it can look scary; that's normal.
- Don't write directly to RAW without going through the volume — the
  format isn't a block device you can dd to.

### 2.5 The sweep

`invf-sweep` is the background worker that:

1. Drains RAW into Shadow (data movement).
2. Re-clusters text/binary content by type.
3. Reclaims tombstones in the inode area (compaction).
4. Optionally re-encodes data with stronger codecs when bit-exactness
   is proven (text → PPMd, binaries → ZSTD+BCJ).

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

After a crash, the volume opens DIRTY and `invf-fsck` (or the next
`vol_open` call) replays the journal. If the replay succeeds, the
volume transitions to CLEAN automatically.

To **undo** the last sweep (e.g., a sweep that mis-clustered data):

```bash
invf-rollback /path/to/volume.img
```

Requires a live sweep checkpoint (the `--realize` flag in
`invf-sweep` makes one durable). Without a checkpoint, rollback
refuses — the volume's history is gone.

### 2.7 Capacity and the metadata reservation

For a Linux rootfs with ~50k+ small files, the default `INVFS_META_FRAC=64`
is too small. The comment in `src/cli/mkfs.c` says **"rootfs images want
16-24"** — use:

```bash
INVFS_META_FRAC=16 invf-mkfs /path/to/rootfs.img 30
```

Symptoms of an undersized metadata zone: ENOSPC on writes even though
`df` shows plenty free, and "inode compact: declined" spam in the
FUSE log.

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

- **"Why is my volume full after a few small writes?"** — sweep not
  running; tombstones are accumulating. Trigger `kill -USR1` or run
  `invf-sweep` offline.
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
