WP: wp/626-writeback-stall
STEP 0 (verify-first): /home/user/InvariantFS-main is at c4a9727 — the SAME
commit this worktree was cut from — and it has no `g_fg_write_ns`, no drain
deferral, and no gate; `grep INVFS_SWEEP_INTERVAL` finds only the opt-in
periodic mode. Main does NOT solve the stall: path (b), no merge needed
(`git merge-base HEAD main` == c4a9727, so `git merge main` is a no-op).
Pre-existing note: AGENTS 2.5's fuse_fs.c line citations were ALREADY stale
on main (:2183 is not the USR1 handler, :2512/:2556-2566 are not the tick and
the drain). Left alone — separate doc bug — and the paragraph I added cites
lines verified in this tree.

Files changed:
  src/cli/fuse_fs.c               (WP626: arrival clock + drain deferral gate,
                                   yield/hold log lines, INVFS_SWEEP_DRAIN_DEFER)
  Makefile                        (wire tools/test-writeback-stall.sh into `make e2e`)
  tools/test-writeback-stall.sh   (new gate: 2 legs, A/B, plus a liveness leg)
  tools/check-vol-find.sh         (re-point the line-keyed vol_find ledger, no
                                   verdict changed: 29 ACTS / 81 SKIP / 109 sites)
  AGENTS.md                       (2.5: what the drain's lock hold costs, the gate,
                                   the knob and why it exists)
  INCIDENTS.md                    (layer 3: suspect -> measured evidence, stays OPEN)

Why: every FUSE_WRITE/.flush/.fsync needs g_io_lock; the 1 Hz on-demand drain
re-encodes the WHOLE pending set inside one hold of it (vol_sweep_pending()
loops to empty). The write path waits that hold out — the INCIDENTS "debian
shutdown saga" layer 3 D-state shape (fuse_wait_on_page_writeback against a
live daemon that never answers). The drain now skips a tick while the write
path is arriving (arrival stamped BEFORE the lock: a request queued on the lock
is still a request the drain would be making wait) and runs on the first tick
after arrivals stop. Same work, same codecs — only when it may start.

Tests run:
  $ make -j2 test                                     -> MAKE_EXIT=0 (/tmp/wp626-maketest2.log)
  $ INVFS_E2E_AGENT=wp626-writeback-stall bash tools/run-e2e.sh tools/test-writeback-stall.sh
      == leg gate-on (INVFS_SWEEP_DRAIN_DEFER=default) ==
        max foreground write+fsync 0 ms; drains during storm 0; yields 1
        drain resumed 3s after the write path went quiet; drains 1   (LIVENESS)
      == leg control-off (INVFS_SWEEP_DRAIN_DEFER=0) ==
        max foreground write+fsync 726 ms; drains during storm 2; yields 0
        [sweep] on-demand: processed 12 pending (held g_io_lock 1000 ms, deferred 0 tick(s))
      CONTROL: pre-gate shape peaked at 726 ms vs 0 ms with the gate
      PASS  (exit 0; re-run for flake load: PASS, control 717 ms)
  $ bash tools/run-e2e.sh tools/test-usr1-savepoint.sh   -> PASS (exit 0)
  $ bash tools/run-e2e.sh tools/test-watermark.sh        -> PASS (exit 0; first
      attempt exited 127 because bin/meta_probe is an e2e-only build target —
      built it, then green)
  $ bash tools/run-e2e.sh tools/test-fuse-sweep-thread.sh -> PASS (exit 0)
  $ bash tools/run-e2e.sh tools/test-writepath.sh        -> run 1 FAIL
      ("invf-fuse daemon did not exit after unmount"), immediate re-run PASS.
      Investigated, not dismissed: a 4-cell probe (300 MB write, unmount, time
      to daemon exit, defer on/off interleaved) measured 1 ms post-unmount exit
      in every cell, so the deferral does not lengthen the exit — the suite's
      60 s wait uses `pgrep -f "invf-fuse <img>"`, which also matches daemons in
      OTHER agents' private mount namespaces. Environmental flake, pre-existing.
  $ bash tools/check-repo-hygiene.sh                     -> OK (714 files; ledger ok)

Measured effect (12-file / 72 MiB text pending set, 8 s foreground storm, one
write+fsync probe per tick): control 724 / 726 / 717 / 1156 ms peak against
0 ms with the gate, with the drain's own line reporting a 1000–1459 ms hold of
g_io_lock over the whole pending set. Both halves of the claim are asserted by
the gate: the yield line must be present (a green leg cannot mean "the drain
had nothing to do") and the drain must resume by itself after quiet (a
deferral that never ends is a leak, not a yield).

Result: PASS
Remaining TODOs: the guest leg (tools/test-writeback-guest.sh, QEMU/KVM void
boot, USR1 pass + 1 GiB dirty + poweroff) is left UNCOMMITTED in the worktree:
it needs /dev/kvm + sshpass + a provisioned stage tree and is not in any
target, so committing it would repeat the "coverage that runs nowhere" mistake.
INCIDENTS layer 3 stays OPEN until the debian serial is re-run against this
fix; what lands here is local FUSE evidence, not the guest proof.
