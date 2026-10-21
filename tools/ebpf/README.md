# tools/ebpf — WP622 eBPF CO-RE observability for the VFS/cache/syscall layer

Observability only: nothing here changes engine behaviour, mount
defaults, or CI. The probes attach at the VFS boundary and report
where time goes; the write-up with numbers is
`docs/benchmarks/VFS-PROBE.md`.

## Toolchain (this host)

BTF is present (`/sys/kernel/btf/vmlinux`), but the stock image had
neither bpftool nor bpftrace. Install once:

    sudo apt-get install -y bpftool bpftrace libbpf-dev clang llvm \
        bc python3 python3-numpy python3-pil

`bc` and the python3 workload deps (numpy, PIL) are what
`run-leg.sh`/`workloads.sh` need; the probe toolchain alone is the
first line.

`bpftool` generates the CO-RE header from host BTF, `clang` compiles
the BPF object, `libbpf-dev` provides userspace load/attach, and
`bpftrace` runs the four `.bt` legs with no build step (it resolves
BTF at attach time). Probes need privilege: run them under `sudo`
(CAP_BPF + tracefs; `unprivileged_bpf_disabled=2` on this box means
unprivileged loads are refused, privileged ones work).

debugfs/tracefs must be mounted (once per boot):

    sudo mount -t debugfs debugfs /sys/kernel/debug

## Layout

- `leg1-rwpdf.bpf.c` / `leg1-rwpdf.c` — leg 1 as a standalone
  libbpf CO-RE tool (no bpftrace dependency). Build with
  `make -C tools/ebpf`; outputs land in `.build/` (git-ignored).
  Run: `sudo .build/leg1-rwpdf -t 30 -o out.txt`.
- `leg1-rw.bt` — leg 1 as bpftrace (same attachment points).
- `leg2-meta.bt` — leg 2: metadata-op latency + dcache hit rate.
- `leg3-sync.bt` — leg 3: sync/fsync return vs block completion tail.
- `leg4-pressure.bt` — leg 4: shrinker activity under pressure 100/0.
- `workloads.sh` — deterministic per-leg workloads (sourced, not run).
- `setup-mounts.sh` — scratch ext4-loop + tmpfs + invfs mounts under
  one directory; `--down` tears down. Prints KEY=value lines.
- `run-leg.sh` — attach probe, run workload, save raw logs.

## Quick start (one leg, all three mounts)

    S=/tmp/invfs-wp622-leg1
    mkdir -p $S
    eval $(bash tools/ebpf/setup-mounts.sh $S)
    export MNT_EXT4 MNT_TMPFS MNT_INVFS   # run-leg.sh is a child process
    bash tools/ebpf/run-leg.sh leg1 $S        # ext4, tmpfs, invfs in turn
    grep -h leg1-wall $S/leg1-*.work.log
    bash tools/ebpf/setup-mounts.sh --down $S; rm -rf $S

The gate (build + attach + one live leg on all three mounts):

    INVFS_E2E_AGENT=wp622-ebpf-probe bash tools/run-e2e.sh tools/ebpf/test-probe.sh

It runs LOCKED on the e2e runner (it needs CAP_BPF and loop mounts), so a
concurrent subagent cannot collide with it. `WP622_GATE_LEG=leg2`,
`WP622_GATE_TAGS="tmpfs"` narrow it down.

Overhead check (the instrument must not move the numbers it reports):

    bash tools/ebpf/run-leg.sh overhead $S ext4

Leg 4 needs the pressure knob (saved/restored by hand, not the runner).
Passive observation during the workload measures NOTHING on a box with
free memory -- there is no reclaim to watch (zero shrinker invocations
in every window; that is what the first attempt found). The numbers in
`docs/benchmarks/VFS-PROBE.md` come from the standard reclaim lever,
drop_caches=2 (dentries+inodes only, no timed workload in flight),
run under the probe at each setting:

    sudo bpftrace tools/ebpf/leg4-pressure.bt &   # attach, then:
    echo 2 | sudo tee /proc/sys/vm/drop_caches    # at pressure 100
    echo 0 | sudo tee /proc/sys/vm/vfs_cache_pressure
    echo 2 | sudo tee /proc/sys/vm/drop_caches    # at pressure 0
    echo 100 | sudo tee /proc/sys/vm/vfs_cache_pressure   # restore
    sudo kill -INT %1

`bash tools/ebpf/run-leg.sh leg4 $S` still records the warm-tree
workload plus the knob before/after, which is the corroborating
slabinfo side of the same leg.

## Overhead and honesty notes

- bpftrace `hist()` aggregation is in-kernel; per-event cost is a
  map update plus, for the vfs kprobes, a BTF field chase
  (file->f_inode->i_sb->s_magic). The overhead leg quantifies it.
- The kprobe legs count every caller on the box: run one workload
  at a time and check the `slab-dentry` before/after lines in
  `*.quiet.txt` to confirm the box was otherwise idle.
- `wl_drop` uses per-file `posix_fadvise(DONTNEED)` (scoped to the
  corpus); only leg 4 keeps the cache warm on purpose. No global
  `drop_caches` during a timed run -- the one deliberate global
  drop is leg 4's reclaim lever, taken with no workload timing it.
- Loop-image files live on tmpfs scratch, but block-layer
  attribution still works: bios complete on the loop device (major 7)
  regardless of backing store, so leg 3 filters per-device rows.
