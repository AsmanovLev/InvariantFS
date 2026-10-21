# VFS probe results: ext4 vs invfs vs tmpfs on one box (WP622)

eBPF CO-RE observability at the VFS boundary, answering the review
questions with instruments instead of prose: where does a read/write
go (pagecache hit vs daemon roundtrip vs block device), what do
metadata ops cost kernel-side vs daemon-side, does
sync/fsync-return mean the bytes are down, and what does
vfs_cache_pressure=0 actually change.

Instrument: `tools/ebpf/` (bpftrace legs + a standalone libbpf CO-RE
tool for leg 1). No engine changes, no mount-default changes, local
runs only. Method and overhead notes are in `tools/ebpf/README.md`;
the runner is `tools/ebpf/run-leg.sh`, workloads
`tools/ebpf/workloads.sh`, mounts `tools/ebpf/setup-mounts.sh`.

## Box and method (all legs)

- Debian trixie, kernel 6.12.111+deb13-amd64, 8 CPUs, 7.7 GB RAM.
  BTF at /sys/kernel/btf/vmlinux; bpftool 7.5.0, bpftrace 0.23.2,
  clang 19, libbpf 1.5 (distro packages, installed for this WP).
- Three mounts from one scratch dir: ext4 on a 48M loop image,
  128M tmpfs, invfs 0.1G volume + FUSE daemon (binaries built from
  this tree). Loop/image files live on tmpfs scratch; block-layer
  attribution still works because bios complete on the loop device.
- Same corpus everywhere: 20 JPEG photos (deterministic PRNG, seed
  622) + 40 text files (~1.4M total) + a 16M deterministic write file
  once the first leg has run. Seeded once per mount, before the probe
  attaches; per-file sha verified identical across mounts.
- Cache control: per-file posix_fadvise(DONTNEED) before timed reads
  (scoped to the corpus, no global drop_caches mid-run); leg 4 keeps
  the cache warm on purpose. Discipline per leg below.
- Quiet-proof: every run records slab-dentry before/after, uptime,
  and the pressure knob. The box is shared with other work; ext4
  (private loop sb) and invfs (private FUSE mount) attribute exactly
  by superblock/device. tmpfs is the box's /tmp, so tmpfs-class
  counts include background readers -- disclosed per leg.
- Overhead leg: leg-1 workload 3x with probe off (102.5-107.5 ms)
  vs 3x with probe on (103.4-104.5 ms). The instrument does not move
  the numbers it reports.
- Reproduce / re-gate: `INVFS_E2E_AGENT=wp622-ebpf-probe bash
  tools/run-e2e.sh tools/ebpf/test-probe.sh` runs the whole contract
  (toolchain, `make -C tools/ebpf check`, `bpftrace --dry-run` attach
  on all four legs, the three mounts, one live leg on each, the CO-RE
  binary, teardown) as a LOCKED e2e suite. leg1, leg2 and leg3 pass
  through it on this box; leg4 has no live gate because its numbers
  need the operator-set pressure/`drop_caches` knob, so only its
  attach is asserted.

## Instrument findings (read these before the numbers)

1. The fuse_request_send trace event is unjoinable on this kernel.
   fuse_send_one() traces BEFORE send_req assigns the request unique
   (fs/fuse/dev.c: fuse_send_one traces, then
   fuse_dev_queue_req assigns req->in.h.unique), so the send event's
   unique field is always 0 while the end event carries the real id.
   Both a bpftrace field read and a raw BPF ctx+16 read agree.
   Legs 1-2 therefore join on the request pointer instead: kretprobe
   fuse_get_req (retval = new req) to kprobe fuse_request_end (single
   arg = req), opcode via BTF through fuse_req in.h.opcode.
   Span semantic: alloc-to-completion (includes the pending-queue
   wait -- part of the roundtrip, disclosed where it matters).
2. block_bio_complete never fires for tracepoint-only users here:
   bio_endio() emits it only for bios carrying BIO_TRACE_COMPLETION,
   which the tracepoint path never sets (only blktrace does; measured
   137 queued / 0 completed). Leg 3 uses block_rq_complete, which
   fires unconditionally and is the better durability signal anyway.
3. 6.12 has no vfs_lookup/vfs_setattr symbols (verified in
   /proc/kallsyms). Leg 2 probes filename_lookup (exported lookup
   entry) and notify_change (the setattr entry chmod/chown/truncate
   funnel through); dcache hit rate comes from lookup_fast
   (static, probeable; ret 0 = usable dentry, no slow path).

## Leg 1: read/write latency -- pagecache hit vs daemon roundtrip

Workload (`tools/ebpf/workloads.sh` wl_leg1_rw): cold sequential
read of the mount (~18M incl. the previous run's write file),
400-shuffled 4K random reads, 16M sequential write in 128K chunks +
fsync. Probe `tools/ebpf/leg1-rw.bt` (+ the `.bpf.c`/`.c` CO-RE twin,
both reporting; numbers below are the bpftrace leg, the CO-RE tool
agrees within bucket).

Wall (same workload, three mounts): ext4 105 ms, tmpfs 115 ms,
invfs 466 ms. The invfs 4.4x is write-dominated (see below); reads
are the interesting half.

vfs_read, per superblock class (us, log2 hist):

- ext4 (n=370, all ours): bimodal. 181 calls at 0-1 us (pagecache
  hits) + 184 calls at 8-64 us peaking 16-32 us (device reads).
  No daemon exists; the slow half IS the block layer on the loop.
- invfs/FUSE (n=370, all ours): bimodal with the same shape.
  180 calls at 0-1 us (pagecache hits, no request fired) + 187
  calls at 16-128 us. Daemon spans joined by request pointer:
  FUSE_READ n=188, bulk 32-64 us, tail to 256 us. The slow
  population IS the daemon roundtrip: 16-64 us vs ext4's 8-32 us
  device half. FUSE READ roundtrip costs ~2x the loop-device read
  on this box, and pagecache hits cost the same ~0-1 us on both.
  Hit rate in this mix: (370-188)/370 ~= 49%.
- tmpfs (class shared with box readers, disclosed): hits at 0-1 us
  dominate (139K of 190K); no slow half anywhere -- there is no
  device and no daemon. Our 370 workload reads are inside that
  population, inseparable by class; the shape (no tail past 64 us)
  is the point, not our exact mean.

vfs_write (128 x 128K + fsync, us):

- ext4: 118/128 at 32-64 us, tail 9 at 64-128 us. Pagecache insert
  + journal-bound dirtying, no daemon.
- invfs: 106/128 at 128-256 us, 11 at 256-512 us, 9 scattered to
  256 ms (the fsync tail). ~4x ext4 per 128K write through the
  FUSE writeback path on this volume. The 9 slow ones are the
  fsync draining prior writes, not the writes themselves -- leg 3
  separates return-vs-durable properly.
- FUSE_WRITE end events (n=35) fired but none joined an alloc in
  this run (background-write reqs evade the get_req kretprobe under
  burst; isolated write+fsync joins fine). Reported as unmatched,
  not as spans -- no write-span claim from leg 1. Instrument TODO,
  not an engine claim.

Ends-by-opcode on invfs (joined + unjoined, n=1132): READ 188,
WRITE 35, OPEN/RELEASE/FLUSH 184 each, GETATTR 40, LOOKUP 18,
SETATTR 7, CREATE 1, FSYNC 1, OPENDIR 9, READDIR 12, plus
IOCTL 123 + GETXATTR 129 + RELEASEDIR 9 + READDIRPLUS 9 unjoined
(no alloc observed; likely box background -- an ACL/flags sweep
walks every file twice -- disclosed, not attributed to the
workload). Daemon idle chatter exists; the workload's own
READ/LOOKUP/CREATE/SETATTR spans are exact-count matched.

CO-RE twin (`tools/ebpf/.build/leg1-rwpdf`, `make -C tools/ebpf`):
same classes, same bimodal read split under traffic (fuse/read
n=4, fuse/write n=1 in a 12 s spot check incl. CREATE 159 us,
LOOKUP 82 us, OPEN 48 us, FSYNC 47 us, GETATTR 27 us, SETATTR 71
us). Cheap enough to sit beside a bench: the overhead leg above
covers both (bpftrace numbers shown; CO-RE adds no per-event
formatting, only map updates).

## Leg 2: metadata ops -- kernel-side vs daemon-side + dcache hits

Workload (`tools/ebpf/workloads.sh` wl_leg2_meta): 2000 files across
50 dirs (create), 2000 stats, 2000 chmods, 2000 unlinks, timed per
phase. Probe `tools/ebpf/leg2-meta.bt`. One ENOSPC false start on
the 0.1G invfs volume (half-built tree, recovered by offline sweep +
remake at 0.2G) is excluded from the numbers; all runs below
completed clean.

Wall per phase, same workload (seconds for 2000 ops):

- create:  ext4 0.088 / tmpfs 0.065 / invfs 1.023  (invfs 11.6x ext4)
- stat:    ext4 0.008 / tmpfs 0.008 / invfs 0.033  (invfs 4.1x)
- chmod:   ext4 0.011 / tmpfs 0.008 / invfs 0.063  (invfs 5.7x)
- unlink:  ext4 0.020 / tmpfs 0.011 / invfs 15.8   (invfs ~790x)

Kernel-side per-call latency (us, log2 hist):

- create: ext4_create 4-8 (1948/2000) / shmem_mknod 1-2 bulk /
  fuse_create_open 32-64 (1984/2000). FUSE create costs ~8x the
  ext4 create; the daemon CREATE span (16-64, same shape) is
  essentially the whole cost -- the VFS layer above it is thin.
- unlink: ext4 2-4 tight / tmpfs 0-1 tight. invfs unlink wall
  (7.9 ms/file mean) == daemon UNLINK span (bulk 4-8 ms,
  1064/2000, range 0.5-16 ms): the daemon spends ~1000x ext4's
  unlink cost inside one synchronous roundtrip per file (delta
  append + retire bookkeeping per unlink, by construction of the
  write path; a future WP with these numbers, not a fix here).
- stat/chmod kernel-side hists are box-mixed (12-46K filename_lookup
  calls per window vs ~10K ours) and not quoted per-op; the walls
  above carry the comparison. unlink/setattr counts attribute
  exactly (ext4: unlink 2076 vs 2000 ours; invfs: unlink 2245,
  setattr 2239 -- noise near zero for mutating ops).

Daemon-side (invfs, joined by request pointer, exact counts):
CREATE 2000, UNLINK 2000, LOOKUP 6646, SETATTR 6000, GETATTR 1899,
plus background IOCTL/GETXATTR/RELEASEDIR/READDIRPLUS (box chatter
on the shared box, disclosed, unattributed). No FUSE traffic during
the ext4/tmpfs runs except 308 READs from this session's own
debugging reads -- disclosed.

dcache: lookup_fast ret==0 (usable dentry, no slow path) counts
2048-4889 per window against 2000 warm stats issued per workload:
consistent with warm stat == dcache hit on all three filesystems
including FUSE. The miss columns (95-415K) are box-wide noise from
co-tenants, not the workload -- quoted as context so nobody
mistakes them for workload misses.

Instrument note (2nd of the WP): vfs_create is a dead letter for
the common path on 6.12 -- every local fs implements atomic_open,
so open(O_CREAT) never reaches it (attached, zero hits on
touch/redirect). Create latency must be read at the fs ops
(ext4_create / shmem_mknod / fuse_create_open); universal create
COUNT comes from security_inode_create. The probe does all three.

## Leg 3: does sync/fsync-return mean the bytes are down?

Workload (`tools/ebpf/workloads.sh` wl_leg3_sync): 4M write + fsync,
2M write + fdatasync, global sync, with CLOCK_MONOTONIC markers on
the same boot clock the probe uses (exact correlation; a last-wins
revision lost our events to box fsync noise, so exits are now
sequence-keyed). Probe `tools/ebpf/leg3-sync.bt`. Completions via
block_rq_complete per raw dev_t (loop = 7:3, sda = 8:0).

- ext4 on loop: dd-fsync (4M) 3.7 ms, fdatasync (2M) 2.3 ms.
  sync() 207 ms; last loop-device completion 15.3 ms BEFORE sync
  returned, zero after: returned at T, last byte at T-15ms. The
  fsync critique does not land here -- on this kernel the return
  IS the durability point within what the block layer can see.
  (Per-fsync tails are unresolvable with last-wins on a shared
  loop: the next completion 6.3 ms after our fsync return belongs
  to the workload's own next write phase. Disclosed, not claimed.)
- tmpfs: fsync 4 us, fdatasync 4 us -- pure pagecache, zero bios
  attributable by construction (2 stray loop reqs in the window).
  sync 113 ms is the system flushing everyone else's dirt.
- invfs (volume file on tmpfs): fsync 453 us, fdatasync 1.65 ms
  (daemon commit + pagecache; no bios by construction). The
  daemon's OWN volume-file fsyncs are visible beside ours
  (fuse_worker, 0-3 us). sync 62 ms -- fastest of the three,
  because there is nothing of ours for it to flush.
  Cross-check the surprise (invfs fsync BEATS ext4 3.7 ms here):
  ext4 pushed 4M through a real loop device + journal; the daemon
  wrote into a tmpfs-backed volume file. Backing store, not
  filesystem design, decides this one -- put the volume on a real
  disk and the ranking can flip. Stated so nobody misquotes it.

Instrument note (3rd): block_bio_complete is a dead letter for
tracepoint users on 6.12 -- bio_endio() gates it on
BIO_TRACE_COMPLETION, which only blktrace sets (137 queued / 0
completed measured). Request completion is the working signal.

## Leg 4: vfs_cache_pressure, including 0 -- behavior + source verdict

Workload (`tools/ebpf/workloads.sh` wl_leg4_pressure): 3000-file
tree per mount, two warm stat walks, 1500 renames; cache kept warm
on purpose. Probe `tools/ebpf/leg4-pressure.bt` (shrinker counts by
ksym). First attempt (passive observation during the workload)
measured nothing: with free memory on the box, no reclaim runs at
all -- zero shrinker invocations in every window. The real
measurement below uses the standard reclaim lever (drop_caches=2,
dentries+inodes only) under the probe, at 100 then at 0, knob
restored to 100 immediately after (verified).

- Pressure 100: drop fires super_cache_scan x81 (51,035 scanned,
  28,937 freed). Slab deltas: dentry active 71,128 -> 53,180,
  fuse_inode 3,672 -> 543, ext4_inode_cache 17,837 -> 11,249.
  Extras: ext4_es_scan x5 (5,202/5,316), jbd2 x4 (1,696/779).
- Pressure 0: super_cache_scan ZERO invocations -- no trace, no
  scan, no free. Slab deltas ~nil (dentry active drifts +1.4K by
  box noise). The knob gates the generic dentry/inode reclaim
  completely through the count path. Meanwhile ext4_es_scan still
  runs x5 (1,938 scanned, 103 freed) and jbd2 scans: ext4's
  private caches do NOT consult the knob.

Mechanism (6.12.y sources, fetched for this WP): do_shrink_slab
returns early when count_objects reports 0, BEFORE the
shrink-start trace -- hence zero invocations, not just zero
frees. The count is super_cache_count scaled by
vfs_pressure_ratio (fs/super.c), i.e. x pressure/100; at 0 the
count is 0 and the sb shrinker is skipped wholesale.

Source verdict on the two review claims:

- "ext4-special handler": NO. ext4 registers the generic per-sb
  shrinker like everyone else, plus ext4_es_scan and the jbd2
  checkpoint shrinker -- none of which reads the knob (measured:
  es still scans at 0). ext4's dentry/inode reclaim obeys the same
  generic path as tmpfs and FUSE.
- "what FUSE registers": NOTHING. No fs/fuse file is among the
  43 in-tree shrinker_register callers. FUSE dentries/inodes die
  through the generic super_cache_scan -> prune_dcache_sb /
  prune_icache_sb -> fuse_evict_inode -> FORGET queue path, and
  the fuse_inode slab drops (-3.1K at 100) prove it reclaims like
  any other fs. Pressure 0 therefore pins FUSE dentries exactly
  as it pins ext4/tmpfs ones -- verified by the same
  zero-invocation result covering all mounts' shrinkers.

Per-mount split caveat: super_cache_scan is per-sb but the probe
keys by ksym, not sb, so the invocation counts are system-wide;
the slab deltas (fuse_inode vs ext4_inode_cache) are the
per-mount corroboration. The workload trees (3000 files each)
were warm on all three mounts for both drops.

## What this WP does NOT claim

- No engine verdicts: the 790x unlink, 11.6x create, 4x write
  numbers are measurements for future WPs to cite, not fixes made
  here. Nothing under src/ was touched.
- The invfs volume file sat on tmpfs throughout, so every
  block-layer comparison flatters invfs persistence costs and
  hides its real-device writeback. Re-run legs 1/3 with the
  volume on a loop device before quoting durability numbers.
- WRITE daemon spans never joined in leg 1 (background-write reqs
  evade the get_req kretprobe under burst; isolated joins work).
  No write-span claim is made from leg 1.
- Tmpfs-class hists include co-tenant readers (the box's /tmp is
  tmpfs); ext4 (private loop sb) and invfs (private FUSE mount)
  attribute exactly.
- Sighting while cleaning up (not a claim, needs its own WP to
  confirm): at 91% volume-full, rm returns 0 but the files persist
  across sync+time -- consistent with the documented fence-failure
  doom restoring rows whose delete-fence cannot persist for lack
  of space. Striking to watch; candidate for a follow-up WP on
  ENOSPC-unlink semantics (fail the syscall vs stage-and-doom).
