WP: wp/623-sigterm-exit
Files changed (branch `wp/623-sigterm-exit`, 2 commits on base 376a55b -- main has since moved
to c4a9727, so this branch is behind main and the merge/rebase is the orchestrator's;
`git diff --stat main...HEAD` = 4 files, +268/-12):
  src/cli/fuse_fs.c          (61e074b) survive_term mounts set argv[0][0]='@' (systemd
                             root-storage-daemon mark), g_argv0 stashed at main() entry,
                             rationale written next to g_survive_term. Only argv[0][0] is
                             rewritten; comm stays "invf-fuse".
  tools/test-sigterm-exit.sh (61e074b, +a3fac23) NEW: leg A plain mount SIGTERM prompt
                             exit + fsck CLEAN; leg B TERM mid-sweep; leg C survive_term
                             unchanged + '@' mark + SIGKILL still kills. a3fac23 widens
                             leg B's pass-START wait 10s -> 120s (setup, not the assertion).
  Makefile                   (61e074b) register tools/test-sigterm-exit.sh in `make e2e`.
  tools/check-vol-find.sh    (61e074b) line-number ledger refresh for the 10 fuse_fs.c
                             call sites the new lines shifted (1420->1441 ... 3565->3586).

Root cause (the 80 s gap was NOT a stuck lock). The debian guest mounts its root from
tools/initramfs-init.sh with `-o survive_term,allow_other` (initramfs-init.sh:229-233), and
survive_term means SIGTERM is deliberately IGNORED (fuse_fs.c:4124-4135). systemd-shutdown
therefore applied its fixed ~90 s TERM wait to PID 98 (invf-fuse) and then SIGKILLed the
process serving PID 1's own root: serial 187 s TERM, 200 s "Waiting for process: 98
(invf-fuse)", 280 s SIGKILL, no "Power down" inside the 300 s budget. The missing piece was
systemd's documented protocol for exactly this process (https://systemd.io/ROOT_STORAGE_DAEMONS):
"Processes (run by the root user) whose first character of the zeroth command line argument is
@ are excluded from the killing spree ... your code should only modify argv[0][0] and leave the
comm name ... untouched". The mark is set only inside the `if (g_survive_term)` branch, so
non-survive_term mounts keep the plain SIGTERM-unmounts contract byte for byte, and it is set
daemon-side (after fuse_daemonize, or in-process under -f, which is how the initramfs runs it).
No change to survive_term semantics, no change to the sweep/USR1 path, and no harness change
(boot-debian-qemu.sh and POWEROFF_TIMEOUT=300 untouched — the fix is engine-only, as the WP
preferred).

Tests run:
  $ make -j2 all
  BUILD_RC=0   (canonical paths: OBJ=build/obj, OUT=bin)

  $ bash tools/run-e2e.sh tools/test-sigterm-exit.sh          [GREEN, after]
  == [A] plain mount: SIGTERM exits promptly, volume CLEAN ==
    leg A idle mount: SIGTERM exit latency 0s
    leg A: PASS (prompt exit, mount released, fsck CLEAN)
  == [B] SIGTERM mid-sweep: same prompt exit ==
    leg B mid-sweep mount: SIGTERM exit latency 0s
    leg B: PASS (TERM landed mid-pass: yes; prompt exit, fsck CLEAN)
  == [C] survive_term: TERM ignored (unchanged) + '@' mark, KILL kills ==
    leg C: '@' mark present: @tmp/invfs-wp623/bin/invf-fuse -o survive_term -f wp623-c.im
    leg C: SIGTERM ignored, mount still serving (semantics unchanged)
    leg C: SIGKILL exit latency 0s
    leg C: PASS
  PASS: test-sigterm-exit (WP623)

  RED-BEFORE (probe: `if (0 && g_argv0 …)` in fuse_fs.c, rebuild invf-fuse only, then reverted)
  $ bash tools/run-e2e.sh tools/test-sigterm-exit.sh
  == [A] ... leg A: PASS (prompt exit, mount released, fsck CLEAN)
  == [B] ... leg B: PASS (TERM landed mid-pass: yes; prompt exit, fsck CLEAN)
  == [C] ... FAIL: leg C: survive_term mount lacks the '@' root-storage mark
  i.e. the mark IS the delta; legs A/B pass before and after and are regression guards for the
  plain-mount contract, not part of the fix. src/cli/fuse_fs.c reverted with `git checkout`;
  the rebuilt binary was byte-identical to the pre-probe build (`cmp` clean).

  $ make -j2 test
  MAKE_TEST_RC=0   (4 shards + test-shard-check; 0 "Error N" lines, 0 ENOSPC lines;
                   21 "PASS:" verdict lines, e.g. "PASS: unlink rollback (WP604)",
                   "PASS: pack_sign_test"; "check-codecpack-sync: SKIPPED layers B and C --
                   no registry checkout" is the documented host-state skip, RC still 0)

  $ bash tools/check-repo-hygiene.sh
  citation check: OK (line numbers in range across 35 doc(s))
  check-vol-find: ok (29 ACTS, 80 SKIP, 108 call sites on the ledger)
  repo hygiene: OK (691 tracked files, no binaries/oversize outside allowlist)
  HYGIENE_RC=0

  $ INVFS_E2E_AGENT=wp623 INVFS_VOID_INITRD=$PWD/vm/initramfs.cpio.gz \
      INVFS_VOID_WORK=/var/tmp/wp623-void/work INVFS_VOID_LOGDIR=/var/tmp/wp623-void/logs2 \
      INVFS_VOID_TARBALL=/var/tmp/wp623-void/void-x86_64-ROOTFS-20250202.tar.xz \
      INVFS_VOID_MEM=2048 bash tools/run-e2e.sh tools/boot-void-qemu.sh
  (initramfs from `bash tools/mkinitramfs.sh`: static=[invf-fuse invf-sweep invf-rollback
   invf-verify invf-fsck invf-cat invf-ls invf-stat] dynamic=[])
  == waiting for runit stage 2 + autologin ==   stage 2 + autologin OK
  == asserting runit services + invfs root over SSH ==
  invfs[vda] / fuse rw,nosuid,nodev,relatime,user_id=0,group_id=0,allow_other,max_read=1048576 0 0
  == poweroff over SSH ==   poweroff OK (QEMU exited)
  == offline bit-exact invf-cat + fsck ==   11/11 "OK <path>", fsck CLEAN: OK
  PASS: Void boot (single) — stage2, autologin, sshd+dhcpcd, SSHD/mount invfs, poweroff,
        bit-exact, fsck CLEAN
  Guest serial evidence for this WP (logs2/serial-single.log):
    [survive_term] ignoring SIGTERM: this mount is the boot root
    [survive_term] root-storage mark set (argv[0] now starts with '@')
    => Sending TERM signal to processes...   => Sending KILL signal to processes...
    - runit: power off...   [15.610891] reboot: Power down
  i.e. the boot-root daemon now carries the mark in-guest, and the shutdown TERM broadcast is
  followed by poweroff in the same breath instead of a ~90 s wait for a process that ignores it.

Result: PASS

Remaining TODOs:
  - Debian TCG poweroff leg is CI's job per the WP ("debian TCG pass is CI's job after
    merge"); not run here because this box has no staged debian root. Command for CI / a
    host with one: `bash tools/run-e2e.sh tools/boot-debian-qemu.sh` — the standing CI watch
    confirms the leg now finishes inside INVFS_DEB_POWEROFF_TIMEOUT=300 (WP621's budget is
    unchanged, as the WP requires).
  - The void gate above ran with INVFS_VOID_MEM=2048 (not the 3072 default) because this host
    has 7.7 GB RAM and 6 parallel subagent worktrees were building: 3072 MB of guest plus host
    load risked OOM (no swap). Re-run at the default with
    `INVFS_VOID_MEM=3072 … bash tools/run-e2e.sh tools/boot-void-qemu.sh` on a quiet box if
    the exact default shape matters.
  - Observation, NOT fixed (out of scope, different subsystem — worth its own WP):
    `tools/mkinitramfs.sh` silently produces an initramfs whose tools cannot start when the
    static relink is unavailable. The relink needs `$ROOT/build/core_objs.txt` to hold
    worktree-relative-to-absolute object paths; if the tree is built with OBJ/OUT redirected
    (e.g. because /tmp was full), the file holds foreign absolute paths, the relink fails, and
    the "dynamic fallback" copies the libs into /lib64 — which ld.so on a Debian-multiarch
    host does not search (default path is /lib/x86_64-linux-gnu). Symptom measured here:
    guest boots to "INVFS probe: []" / "no InvariantFS volume found on any block device" and
    drops to the rescue shell, with no error from mkinitramfs (its `ldd` check runs against
    host paths, so it passes). A canonical `make` (default OBJ/OUT) makes it static and green
    again, which is how the gate above was finally run.
  - Host note (disk discipline): /tmp on this box was at 100% (sibling worktrees + a 2.1 GB
    sibling scratch dir) for most of this leg, so intermediate build artifacts were built
    under /var/tmp/wp623-art and the gate ran with its own WORK/LOGDIR under
    /var/tmp/wp623-void. All of that scratch is deleted again (verdict logs kept at
    /var/tmp/wp623-void/logs2, the Void rootfs tarball kept at /var/tmp/wp623-void/); the
    final `make all` / `make test` / suite run are the canonical in-worktree build.
  - Nothing else open in the tree: `git status -sb` is clean on wp/623-sigterm-exit (build
    artifacts are gitignored), no merge to main, no push.
