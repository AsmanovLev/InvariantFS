# ADR-008: FUSE Userspace Daemon vs. In-Kernel Filesystem (VFS)

## Status
Accepted

## Context

As InvariantFS approached feature-parity with general-purpose filesystems, the
question arose whether it must move from a FUSE userspace daemon to an
in-kernel filesystem (registering with the kernel VFS) — especially to be a
"drop-in replacement" for ext4/XFS/btrfs/ZFS.

The pressure comes from a list of gaps. Classifying them by whether they are
actually *caused by FUSE* matters:

| Blocker | FUSE-specific? | Fixable in userspace? |
|---|---|---|
| Durability contract (`fsync` → on-disk) | no | yes (barriers/flush policy) |
| `security.*` / `trusted.*` xattr, file capabilities | no | yes (daemon forwards any xattr; engine is namespace-agnostic. VFS requires root/CAP_SYS_ADMIN for `trusted.*`/`security.*`, and mount without `nosuid` for `security.capability`) |
| systemd as PID 1 on a FUSE root | no | yes (mount propagation, `/run`, cgroup2; WP66 started) |
| `invf-fsck` reliability after fresh import | no | yes |
| metadata scaling / package-manager churn | no | yes |
| **daemon death = system death** | **yes** | mitigations only (`auto_unmount`, systemd `Restart`) |
| **swap** | **yes** | no — needs a separate swap partition |
| cold-read latency (context switches) | partly | io_uring/splice, ARC, heat, tiering |

Four of the five "hard" blockers are implementation gaps in the daemon, not
properties of FUSE. A kernel module would not remove them; it would relocate
them into a second codebase.

## Decision

1. **Stay with FUSE.** Do not build an in-kernel filesystem now.
2. **Close the userspace gaps first**, in priority order: durability contract,
   xattr namespace passthrough (verify empirically), systemd/PID-1 fixes,
   fsck reliability.
3. **Revisit the kernel route only if a kernel-only constraint becomes the
   binding limit** — i.e. after the userspace gaps are closed and measurement
   shows the residual is kernel-attributable (swap, daemon-fate, or a read
   latency floor that io_uring/ARC cannot close).
4. If a kernel path is ever warranted, prefer a **thin read-only in-kernel read
   path (EROFS-like) plus a userspace/overlay write path** over a full LKM.

### Why not a full LKM

- The hot path is AST resolution + codec decode (ZSTD/PPMd/FLAC/miniz). An LKM
  either pulls the whole codec ecosystem into ring 0 — where a malformed recipe
  becomes a kernel panic — or still calls userspace per segment, so the context
  switches remain.
- Two codebases, two bug surfaces, kernel-debugging cost.
- The gain would be metadata-side only (lookup/getattr), which is not the
  bottleneck for the read-mostly workload this filesystem targets.

### Why not ublk

A userspace block device + ext4/XFS on top imposes block semantics and doubles
metadata/page-cache overhead, discarding content-addressing — i.e. it builds a
different filesystem.

## Consequences

### Positive
- One codebase; codecs stay in userspace where bugs are recoverable.
- The userspace gaps are tractable and independently testable.
- Keeps the door open for a narrow kernel read path without committing to an LKM.

### Negative / Risks
- `swap` on the root volume remains impossible; a separate swap partition is
  required.
- A daemon crash can still take the system down; this is mitigated, not
  eliminated.
- If read latency proves kernel-bound, a future read-path project is still owed.

## Revisit triggers

- Durability + systemd + xattr gaps closed, yet measured boot/app-launch latency
  or throughput is unacceptable.
- A hard requirement for swap-on-root or for the daemon-fate guarantee appears.
