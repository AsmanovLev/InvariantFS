# WP622 — eBPF probes for the VFS/cache/syscall layer (observability only)

## Scope

- `tools/ebpf/` (new: CO-RE probes + runner + doc; no engine changes)
- `docs/benchmarks/VFS-PROBE.md` (new: numbers ext4-vs-invfs-vs-tmpfs, one box)
- Build/install notes for the probe deps (this host has BTF
  (`/sys/kernel/btf/vmlinux`) but neither bpftool nor bpftrace)

Do not touch: `src/`, mount defaults, CI workflow, any engine behavior.

## Why

Review feedback (Artemy, 2026-10-13): our cache answers stop at sysctl
values; syscall-level and metadata-manipulation overheads are unmeasured;
wall-clock benches that ignore background writeback/fold/sweep work are
dishonest; the FUSE half of `vfs_cache_pressure` handling is unverified
against sources. Answer with instruments, not prose: eBPF probes injected
at the VFS boundary, reporting where time actually goes.

## Design

Probe set (all histogram-first, cheap enough to run beside a bench):

1. `vfs_read` / `vfs_write` latency split: pagecache hit vs a FUSE-daemon
   roundtrip (`fuse_dev_do_read`/`fuse_dev_do_write` span). Quantifies the
   "70% unexplored FUSE" as two numbers per op class.
2. Metadata ops (`vfs_lookup`, `vfs_create`, `vfs_unlink`, `vfs_setattr`):
   kernel-side vs daemon-side split; dentry-cache hit rate alongside.
3. Background-interference leg (the fsync critique, quantified): time a
   `sync()`/`fsync()` return against the tail of actual device writeback
   completion (block-layer completion trace) — report "returned at T, last
   byte at T+Δ" for ext4 and invfs alike.
4. `vfs_cache_pressure` behavior incl. `0`: dentry/inode reclaim counts
   under pressure settings `100` vs `0` on invfs, ext4, tmpfs; verify the
   ext4-special-handler claim from kernel sources and check what FUSE
   registers (shrinker? invalidation path?) from libfuse/kernel sources.

Method: same box, same corpus (photo + text + metadata-heavy tree), three
mounts (ext4 loop, tmpfs, invfs volume), each leg with cache-state control
(`posix_fadvise(DONTNEED)` + remount discipline documented per leg).
Corroborate probe numbers against the honest benches we already trust
(boot-leg kernel timestamps, userspace codec walls).

## Validation

- Probes build and run on this host; overhead leg (probe on vs off) shows
  the instrument does not move the numbers it reports.
- `docs/benchmarks/VFS-PROBE.md` with all four probe results + the
  pressure-0 source verdict; `bash tools/check-repo-hygiene.sh` green.
- Expected: no engine verdicts in this WP — findings that demand engine
  changes become new WPs with numbers.

## Out of scope

- Any `src/` change (this WP is a measuring instrument, not a fix).
- Production/CI deployment of the probes (local runs only).
- Decode of closed-source behavior (fNBLI and friends stay untouched).
- Re-litigation of settled benches (JXL-BATCH, NBLI-BENCH stay as-is).

## Coordination notes

- Subagent ID: `wp622-ebpf-probe`. Worktree: `/tmp/invfs-wp622`, branch
  `wp/622-ebpf-probe`, from main.
- No e2e gate (observability tooling; must not disturb shared `/dev/shm`
  or `/tmp` state — scratch under `/tmp/invfs-wp622-*`, ≤200M).
- Disk discipline + commit-per-leg (standing rules since the 2026-10-13
  ENOSPC deaths).
- Report format: WP / Files changed / Tests run / Result / Remaining TODOs.
