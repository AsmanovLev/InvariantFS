# WP626 — FUSE writeback stall at shutdown sync (journald D-state)

## Scope

- `src/cli/fuse_fs.c` + `src/core/vol_write.c` (write/flush path under
  writeback burst only)
- `tools/test-writeback-stall.sh` (new: local red-before/green-after gate)
- Maybe `tools/boot-debian-qemu.sh` (diagnostics only, no budget changes)

Do not touch: `@` mark / survive_term (WP623, working as designed),
POWEROFF_TIMEOUT (stays 300), guest systemd tuning, lanes, registry.

## Why

engine-ci run 38424325516, debian serial (`boot-serial-logs-debian`,
WITH the WP623 `@` fix deployed — the mark is set in-guest and the old
"Waiting for process: 98 (invf-fuse)" is GONE):

```
[ 179s] systemd-shutdown[1]: Syncing filesystems and block devices.
[ 179s] systemd-shutdown[1]: Sending SIGTERM to remaining processes...
[ 369s] INFO: task systemd-journal:160 blocked for more than 122 seconds.
         task:systemd-journal state:D ... fuse_wait_on_page_writeback
         fuse_page_mkwrite <- do_page_mkwrite <- do_wp_page (page fault)
```

Shutdown proceeds past the kill phase, then wedges in final sync:
journald is stuck in uninterruptible sleep waiting for a FUSE page
writeback that never completes — an mmap'd write fault on a journal file
on the FUSE root, pending against a daemon that is alive (excluded from
kills by design) but never answers. No `Power down` inside 300s. This is
a NEW layer below WP623, not a regression of it: the kill-phase hang is
fixed, the sync-phase write path now stalls.

Prime suspect (not a verdict): WRITEBACK_CACHE mode + shutdown-sync
burst — the kernel pushes all dirty pages at once and the daemon's
write/flush path wedges (lock inversion with fold/flush? allocation stall?
a request the daemon never replies to?). The D-state (not an error return)
means the request is still pending, so the daemon is alive-but-stuck or
the reply path is broken — distinguish by instrumenting which FUSE
opcode/unique never completes.

## Design

1. Local repro first (no guest): mount a volume (survive_term AND plain —
   find which dispositions stall), dirty a large mmap'd file to build a
   writeback backlog, then force the sync-phase shape (`sync` + a
   writeback storm while the daemon is mid-sweep/fold). Red = a writeback
   request pending >N s with the daemon alive (read from
   `/sys/kernel/debug/fuse/...` or daemon request tracing, NOT from wall
   time alone).
2. Root-cause the pending request: which opcode, what the daemon thread
   holding it is blocked on (lock? allocator? fold fence?). Fix the stall,
   not the timeout — no budget changes anywhere.
3. Gate `tools/test-writeback-stall.sh` (red-before/green-after, wired
   into `make e2e` next to `test-sigterm-exit.sh`), `make test`, hygiene,
   local KVM void-boot poweroff leg.

## Validation

- Repro red-before/green-after (record both latencies).
- `make test`, `check-repo-hygiene.sh` green; void-boot poweroff PASS.
- Expected: debian CI leg completes inside 300s; standing CI watch
  confirms (a stall fix must move the serial past final sync to
  `Power down`, not merely past the kill phase).

## Out of scope

- `@`/survive_term rework (proven working by this very serial).
- Timeout/budget changes (the gate must prove a real fix).
- The void SSH-banner variance (separate harness item).
- Write-path refactors beyond the stalled request (minimal fix; broader
  work becomes its own WP with numbers).

## Coordination notes

- Subagent ID: `wp626-writeback-stall`. Worktree: `/tmp/invfs-wp626`,
  branch `wp/626-writeback-stall`, from main.
- E2E gate: new script + local void-boot poweroff leg (KVM here); debian
  TCG proof is CI's job after merge.
- Disk discipline + commit-per-leg. Report format:
  WP / Files changed / Tests run / Result / Remaining TODOs.
