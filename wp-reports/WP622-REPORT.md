WP: wp/622-ebpf-probe
Branch head: 6fd867c (worktree /tmp/invfs-wp622, 9 commits off merge-base, nothing pushed)
Files changed (all in scope; `git diff --name-only <merge-base>..HEAD` has
nothing outside these):
  tools/ebpf/Makefile              (new: vmlinux.h + CO-RE object + leg1 binary)
  tools/ebpf/leg1-rwpdf.bpf.c      (new: leg1 as libbpf CO-RE, no bpftrace dep)
  tools/ebpf/leg1-rwpdf.c          (new: loader/aggregator, -t/-o)
  tools/ebpf/leg1-rw.bt            (new: leg1 via bpftrace, same attach points)
  tools/ebpf/leg2-meta.bt          (new: metadata latency + dcache hit rate)
  tools/ebpf/leg3-sync.bt          (new: sync/fsync return vs block completion tail)
  tools/ebpf/leg4-pressure.bt      (new: shrinker activity under pressure 100/0)
  tools/ebpf/workloads.sh          (new: deterministic per-leg workloads, sourced)
  tools/ebpf/setup-mounts.sh       (new; +PATH fix; loop-leak fix in --down)
  tools/ebpf/run-leg.sh            (new: attach probe, run workload, save raw)
  tools/ebpf/test-probe.sh         (new: gate suite, runs through run-e2e.sh)
  tools/ebpf/README.md             (new: toolchain, layout, quick start, gate)
  tools/ebpf/.gitignore            (new: .build/)
  docs/benchmarks/VFS-PROBE.md     (new: four legs' numbers + pressure-0 verdict)
No `src/`, no mount default, no CI, no registry change.

Tests run:
  $ make test                     (worktree, full 4-shard suite, foreground)
  EXIT=0, 0 lines matching "RESULT: FAIL|FAIL ("
  ... repo hygiene: OK (704 tracked files, no binaries/oversize outside allowlist)
  ... test heredocs: OK (134 python block(s) compile)
  ... all shard binaries PASS, incl. rs_stability_test 24124 checks 0 failures
  Note: the previous (background) attempt of this same command died with
  "make: *** wait: No child processes" -- the bg wrapper was reaped, not a
  test failure. Re-run synchronously twice (before and after the tool edits),
  both EXIT=0.

  $ make -C tools/ebpf check
  OK   BTF present; leg1-rwpdf.bpf.o is BPF; leg1-rwpdf executable

  $ INVFS_E2E_AGENT=wp622-ebpf-probe bash tools/run-e2e.sh tools/ebpf/test-probe.sh
  (LOCKED suite: CAP_BPF + loop mounts; runner records agent attribution)
  == 1. toolchain ==   OK BTF, bpftrace, clang, cc, bc, python3, bpftool (/sbin)
  == 2. build ==       OK make -C tools/ebpf check
  == 3. attach ==      OK leg1-rw, leg2-meta, leg3-sync, leg4-pressure
                       (`bpftrace --dry-run` attaches every probe, then exits)
  == 4. mounts ==      OK mnt-ext4, mnt-tmpfs, mnt-invfs
  == 5. live leg ==    OK leg1/ext4, leg1/tmpfs, leg1/invfs emitted probe data
  == 6. CO-RE binary == OK leg1-rwpdf -t 3 emitted 5 counted lines
  == 7. teardown ==    OK nothing left mounted
  PROBE GATE: PASS (build + 4 attaches + leg1 on [ext4 tmpfs invfs] + CO-RE emit)

  $ WP622_GATE_LEG=leg2 ... test-probe.sh   -> PROBE GATE: PASS (leg2 x3 mounts)
  $ WP622_GATE_LEG=leg3 ... test-probe.sh   -> PROBE GATE: PASS (leg3 x3 mounts)
  leg4 has no live gate by design (its numbers need the operator-set
  vfs_cache_pressure / drop_caches lever); only its attach is asserted.

  $ bash tools/check-repo-hygiene.sh  -> repo hygiene: OK (704 tracked files)
  Loop/mount leak audit after all runs: 0 wp622 mounts, 0 wp622 loop devices.

Result: PASS

Two real defects were found in my own instrument code by running the gate;
both fixed and shipped in this branch:
  1. setup-mounts.sh called bare `mkfs.ext4`/`losetup`. Debian keeps them in
     /sbin, which is not on a non-root PATH -> "mkfs.ext4: command not found"
     (exit 127) the moment the gate ran as the invoking user instead of root.
     Now resolved PATH-or-/sbin, and mkfs absence is a named FAIL.
  2. setup-mounts.sh --down only detached the device recorded in $S/loopdev.
     Every setup that failed between attach and the `echo $LOOP` leaked a loop
     device; ten were leaked in one afternoon of runs. --down now detaches
     every loop device whose backing file lives under the scratch dir.

Measurement headlines now recorded in docs/benchmarks/VFS-PROBE.md (reported,
not acted on; no engine verdicts): invfs READ roundtrip ~2x the loop-device
read with ~49% pagecache hits in that mix; ~4x ext4 per 128K write; create
11.6x ext4 (1.023 vs 0.088 ms), unlink ~790x (15.8 vs 0.020 ms) -- the unlink
cost is one synchronous FUSE roundtrip per file; on this kernel ext4's fsync
return IS the durability point (last loop completion 15.3 ms BEFORE sync
returned, zero after), while invfs pays 453 us fsync / 1.65 ms fdatasync for
a daemon commit with no attributable bios; and the leg-4 source verdict holds
-- vfs_cache_pressure is applied by super_cache_count (mult_frac) and consumed
in do_shrink_slab BEFORE trace_mm_shrink_slab_start, fs/fuse registers no
shrinker at all (only shrink_dcache_parent), and ext4_es_scan /
jbd2_journal_shrink_scan register their own shrinkers and never read the knob,
so pressure=0 on invfs is a no-op for the FUSE cache path.

Remaining TODOs:
  - leg1 WRITE daemon spans never join (background-write requests evade the
    fuse_get_req kretprobe under burst; isolated joins work). Disclosed in the
    doc; no write-span claim is made. Fixing it needs a different join key.
  - The invfs volume file sat on tmpfs for every leg, so no block-layer
    durability number here applies to a real device; legs 1/3 should be
    re-run with the volume on a loop device before anyone quotes them.
  - leg4 has only an attach gate; a live gate needs the pressure/drop_caches
    lever to become an argument of run-leg.sh instead of an operator ritual.
  - Sighting, not a claim, needs its own WP: at 91% volume-full, `rm` returns
    0 but the files persist across sync+time -- consistent with the documented
    fence-failure doom restoring rows whose delete-fence cannot persist. It is
    an ENOSPC-unlink-semantics question (fail the syscall vs stage-and-doom).
