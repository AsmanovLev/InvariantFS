# WP623 — invf-fuse must exit promptly on SIGTERM (debian shutdown hang)

## Scope

- `src/cli/fuse_fs.c` (SIGTERM disposition / graceful-shutdown path only)
- `tools/test-sigterm-exit.sh` (new: local repro + gate, no guest needed)
- Maybe `tools/boot-debian-qemu.sh` (only if the harness must cooperate;
  prefer engine-only fix)

Do not touch: `survive_term` semantics (stays: SIGTERM ignored when set),
sweep/USR1 path, JXL/NBLI lanes, registry, CI workflow (WP621 already did
its part; POWEROFF_TIMEOUT stays 300 regardless).

## Why

engine-ci run 38416052796, debian serial (`boot-serial-logs-debian`):

```
[  187s] systemd-shutdown[1]: Syncing filesystems and block devices.
[  187s] systemd-shutdown[1]: Sending SIGTERM to remaining processes...
[  200s] systemd-shutdown[1]: Waiting for process: 98 (invf-fuse)
[  280s] systemd-shutdown[1]: Sending SIGKILL to remaining processes...
```

The guest reaches `poweroff.target` cleanly, then waits 80s for the FUSE
daemon serving its own root, which never exits on SIGTERM and dies by
SIGKILL. The harness 300s budget (WP621) then expires before QEMU prints
`Power down`. WP616 measured the graceful unmount+close at 4.4 ms — but
that is the cooperative path, not the SIGTERM-during-shutdown path. The
daemon's contract per `src/cli/fuse_fs.c:94-97` is that SIGTERM unmounts
it (except `survive_term`); an 80s SIGTERM→SIGKILL gap breaks that
contract and keeps the debian leg red.

## Design

1. Repro locally (no guest): `invf-mkfs` a small volume, mount via
   `invf-fuse`, SIGTERM the daemon pid, measure exit latency. Assert prompt
   exit (seconds, not tens of seconds). This is `tools/test-sigterm-exit.sh`.
2. Root-cause along the disposition chain (`src/cli/fuse_fs.c:4035-4130`):
   libfuse `fuse_set_signal_handlers` vs the `survive_term` SIGTERM-takeback
   vs the sweep-thread SIGUSR1 setup — find what the daemon is stuck IN for
   80s (graceful-shutdown path blocked on `g_io_lock`? fold/sweep thread
   joining? signal blocked in a worker?). Fix the block, not the symptom:
   SIGTERM on a non-`survive_term` mount must unwind promptly; `survive_term`
   behavior unchanged.
3. Validate the repro goes from red (>10s or SIGKILL-needed) to green
   (<5s exit, mount released), then `make test` + hygiene + a local KVM
   void-boot poweroff leg (WP616 pattern) as the e2e gate.

## Validation

- `tools/test-sigterm-exit.sh` red-before/green-after (record both).
- `make test`, `check-repo-hygiene.sh` green.
- Local `boot-void-qemu.sh` poweroff leg (KVM): PASS with daemon exit
  visible in the fuse log (no 80s gap class of behavior).
- Expected: debian CI leg can then complete inside its 300s budget; the
  standing CI watch confirms.

## Out of scope

- `survive_term` redesign (out of scope by contract; test both dispositions).
- Guest systemd tuning (rejected in WP621: would weaken the gate).
- The void SSH-banner flake (same run, different phase — separate harness
  item, not this WP).
- TCG host-variance budgets beyond the existing knobs.

## Coordination notes

- Subagent ID: `wp623-sigterm-exit`. Worktree: `/tmp/invfs-wp623`, branch
  `wp/623-sigterm-exit`, from main.
- E2E gate: local `boot-void-qemu.sh` poweroff leg only (KVM here, fast);
  debian TCG pass is CI's job after merge.
- Disk discipline + commit-per-leg. Report format:
  WP / Files changed / Tests run / Result / Remaining TODOs.
