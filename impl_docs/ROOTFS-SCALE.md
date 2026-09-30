# WP wp/rootfs-scale — ROOTFS SCALE: does a ~84k-file / 3.6 GB Linux rootfs import and sweep?

Worktree `/srv/bench/worktrees/wt-rootfs`, branch `wp/rootfs-scale`, off `main`
(49fdb78). No engine file was modified; everything below is measurement.
All images and scratch live under `/srv/bench/`.

> **Line numbers are at 49fdb78**, the base the prebuilt `bin/` was built from
> and the base that matches the citations in the task brief. `main` has since
> moved 11 commits (to 3f30cc9); see §4.5 — one of those commits changes the
> cost model this report measures, and **does not fix the thing that actually
> blocks rootfs scale**.

Corpus: a `cp -a` of the host's real `/usr` — **84,279 regular files,
3,563,904,931 bytes (3.32 GiB)** at `/srv/bench/rootfs/src`. The host `/usr`
was never touched.

---

## 1. Root cause of the import death — it is NOT an OOM

**`invf-import` issues an `fsync(2)` for every metadata record it appends, with
no group commit.** On this host that caps the whole tool at ~7 fsyncs and
0.78 s per file.

Call chain, backtrace symbolised with `addr2line` against `bin/invf-import`
(LD_PRELOAD interposer that `backtrace()`s on every `fsync`):

| frame | site |
|---|---|
| `fsync(io->fd)` in `blkio_flush` | `src/core/blkio.c:626` |
| `blkio_flush(&v->io)` in `vmux_barrier` | `src/core/volume.c:657` |
| `vmux_barrier(v, "delta append")` in `vol_delta_append` | `src/core/vol_delta.c:572` |
| `vol_v3_inode_delta_put` → `vol_v3_create_node` → `vol_v3_write_bulk` | |
| `import_dir` | `tools/invf-import.c:241` |

`vol_delta_append` barriers after **every single record**, unconditionally.
The same unconditional per-record barrier appears at `vol_delta.c:472`
("delta segment header"), `:537` ("delta segment publish"),
`vol_anchor.c:219` ("anchor refresh"), `vol_btree.c:2626`, and
`vol_fold.c:344`/`:460`.

### The measurement

`bin/invf-import` on **500 files of 12 bytes each** — zero payload, so every
second is metadata:

```
imported: 0 dirs, 500 files, 0 symlinks, 0 specials, 0 skipped in 390.0s
FSYNC_TOTAL=3509 busy=387.74s          <- the interposer, at exit
```

* **3,509 fsync calls for 500 files = 7.02 fsyncs/file**
* **387.74 s of the 390.0 s wall (99.4%) blocked inside `fsync`**
* 0.78 s/file, of which 0.775 s/file is fsync wait
* Total CPU time for the run: ~0.05 s. The process state is `D`, wchan
  `jbd2_log_wait_commit`, syscall 74 (`fsync`).

**The denominator.** Raw `fsync` latency on this filesystem, measured
directly (30 fsyncs of 200 KB each, no InvariantFS involved):
`/dev/sda1`, ext4, `rw,relatime`, `mq-deadline`, no swap —
**0.10–0.46 s, mean 0.12 s; 1.24 MB/s end to end.**

### The scale limit, with the number and the denominator

```
84,279 files x 7.02 fsyncs/file x 0.1105 s/fsync
  = 65,377 s  =  18.2 hours of fsync wait, for the metadata alone
```

That is before the sweep, and before a single byte of the 3.6 GB of payload
is considered.

### The data path is fine — the cost is per *record*, not per byte

| operation | size | wall | rate |
|---|---|---|---|
| one file, whole-tree import | 500 MB | 11.2 s | 45 MB/s |
| one file, whole-tree import | 3.58 GB | 124.6 s | 28.7 MB/s |
| 500 × 12-byte files (metadata only) | 6 KB | 390.0 s | 0.05 MB/s |

Three orders of magnitude apart. **A rootfs on InvariantFS is limited by its
inode count, not its byte count.**

### It is deliberate — and it was deferred "until measured"

`docs/adr/ADR-009-v3-durability-contract.md`, Decision 3:

> **The per-append delta barrier stays.** It is a durability ordering
> (design §9) ... Relaxing it is deferred until measured, and would require a
> `delta_durable` anchor and a redefined acknowledged-write contract.

It has now been measured, at rootfs scale, and the answer is "18 hours".
There is no knob: `INVFS_CLOSE_NOBARRIER` covers only the CLEAN superblock on
close (`src/core/volume.c:2232`), not the append path. **I did not change it**
— it is a durability contract, and relaxing it is the orchestrator's call,
not a measurement WP's.

### What the original 20:09–20:25 death actually was

`sudo -n dmesg`, mapped from kernel uptime to wall clock:

* `2026-09-29 20:25:35` — `Out of memory: Killed process 2336190 (invf-cat)
  total-vm:990228kB, anon-rss:560388kB`.
  In the same task table, `invf-import` (pid 2303530) is at **total_vm 3803
  pages (15 MB), rss 2407 pages (9.4 MB)**. The OOM victim was *not* the
  importer.
* `2026-09-29 03:51:13` — `invf-import invoked oom-killer` … victim
  `systemd` (2219278, oom_score_adj 100). The importer there was
  **total_vm 25810 pages (101 MB), rss 16537 (65 MB)**.
* The dominant consumer in both cases was **`shmem: 6,668,124 kB`** on a
  7,945,532 kB machine — i.e. tmpfs, not InvariantFS.

Independent confirmation that the importer is not memory-bound at all: my own
4,000-file run, at ~1,700 entries in, held **VmSize 10,272 kB, VmRSS 6,700 kB**.

And the 112-byte `import.log` is not a crash artefact at all:
`tools/invf-import.c:298` prints its summary only after the walk, so a killed
run leaves exactly the one 112-byte `vol_open` line.

**Conclusion: the importer was never the OOM victim and never close to being
one. It was still making progress when the host OOMed and its session was
torn down.** What the 15-minute run actually demonstrated is the rate above:
in 15 minutes it would have imported ~1,150 files.

---

## 2. Largest tree that imports and sweeps successfully

### **4,000 real `/usr` files — 542,462,714 bytes — fully verified**

| | |
|---|---|
| corpus | 4,000 files + 165 dirs, **542,462,714 bytes** of real `/usr` content |
| image | `INVFS_META_FRAC=16 invf-mkfs k4k.img 6` (6 GiB, v3) |
| `invf-import` | **6,660.3 s (1.11 h)** — `imported: 165 dirs, 4000 files, 0 symlinks, 0 specials, 0 skipped` |
| `invf-sweep` | **3,332 s (55.5 min)**, all 7 stages, **exit 0** |
| **total** | **9,992 s (2.78 h)** |
| volume after sweep | **540,450,816 bytes** real (`du -s`), 6 GiB apparent (sparse) |
| sweep detail | dedupe merged 403 segments, freed 897 blocks (3.5 MiB); reclaim collected 21,772 orphaned v3 base pages |
| bit-exactness | **4,000 / 4,000 identical, 0 mismatched** (§3) |
| `invf-fsck` | **OK**, exit 0 (§3) |

### The smaller fully-verified run, for comparison — 1,000 files

| | |
|---|---|
| corpus | 1,000 files + 12 dirs, **505,058,200 bytes** |
| `invf-import` | **1,482.7 s** — `imported: 12 dirs, 1000 files, 0 skipped` |
| `invf-sweep` | **1,805 s**, exit 0 |
| volume | 441,147,392 bytes real |
| bit-exactness | **1,000 / 1,000 identical** |

### The projection to rootfs scale

| files | import | note |
|---|---|---|
| 500 × 12 B | 390.0 s (0.78 s/file) | metadata only, uncontended |
| 1,000 | 1,482.7 s (1.48 s/file) | real content, contended |
| 4,000 | 6,660.3 s (1.67 s/file) | real content, contended |
| **84,279** | **65,377 s = 18.2 h** uncontended / **~39 h** contended | + a sweep of the same order |

**The `/usr` rootfs-scale target has never been imported, and with the code
as it stands it cannot be imported in a session, a day, or a working day.**

### The sweep is fsync-bound too

Confirmed directly: mid-sweep, `invf-sweep`'s process state is `D`, wchan
`jbd2_log_wait_commit`, syscall 74 (`fsync`) — the same
`vol_delta_append` → `vmux_barrier` → `blkio_flush` chain, reached through
the batch-flush path in stage 6. Stage 6 alone was ~27 min of the 4,000-file
sweep's 55 min. **Any lane that writes many metadata records — the sweep's
text/binary batching included — pays the same per-record price.**

---

## 3. Bit-exactness

Every regular file re-read from the volume with `bin/invf-cat` and `cmp`'d
against the original — `invf-cat` streams straight into `cmp`, so there are
no intermediate copies.

| volume | files checked | identical | mismatched |
|---|---|---|---|
| **`k4k.img`** — 4,000 real `/usr` files, after import **and** sweep | **4,000** | **4,000** | **0** |
| `r1k.img` — 1,000 real `/usr` files, after import **and** sweep | **1,000** | **1,000** | **0** |
| `t500.img` — 500 files, after import **and** sweep | **500** | **500** | **0** |

```
VERIFY k4k.img: checked=4000 identical=4000 mismatched=0
VERIFY r1k.img: checked=1000 identical=1000 mismatched=0
VERIFY t500.img: checked=500 identical=500 mismatched=0
```

**5,500 files re-read and `cmp`'d byte-for-byte after a full import + sweep
cycle. Zero mismatches.**

### `invf-fsck`

`k4k.img` (4,000 real files, post-sweep) — **exit 0, verdict `OK`**:

```
  state:        CLEAN
  torn slots:   0
  bad pages:    0
  names/inodes:  4165 name(s) over 4001 live inode(s)
  orphan rows:   1 live inode(s) that no directory entry names (reported; not damage, not repairable)
  nlink/fan-in:  ok (4001 live inode(s), each with exactly as many names as its nlink)
  live recipes: ok (4001 live inode(s) with content, every recipe blob they name loads and parses)
OK
```

`r1k.img` (1,000 real files, post-sweep) — **exit 0, verdict `OK`**:

```
  state:        CLEAN
  format:       v3 (metadata-v3 base tree)
  root seq:     2011
  pages walked: 203
  base keys:    2122
  torn slots:   0
  bad pages:    0
  cycles/shared: 0
  free blocks:  1432373
  names/inodes:  1012 name(s) over 1001 live inode(s)
  nlink/fan-in:  ok (1001 live inode(s), each with exactly as many names as its nlink)
  live recipes: ok (1001 live inode(s) with content, every recipe blob they name loads and parses)
  save point:   live
OK
```

`t500.img` (500 files, post-sweep) — **exit 0, verdict `OK`**, same shape
(31 pages walked, 0 bad pages, 0 torn slots, 501/501 recipes load).

The single orphan row in all three is the root inode; `fsck` states it is
"not damage, not repairable" and by design (the v3 write order is
row-then-dirent).

**Bit-exactness is intact through import and sweep. Throughput is what does
not scale.**

---

## 4. Containerpack at 84k members: **DECLINED**

The brief's premise needs one correction: **the decline does not come from
`CPACK_MEMBER_COST`.** It comes from a hard ABI cap that fires much earlier.

### What I actually ran

A real Copy-mode 7z of the **entire** corpus — **84,279 members,
3,579,191,296 bytes** — built with
`7za a -t7z -m0=Copy -mhc=off -mx=0`, imported into a v3 image
(`IMPORT84_WALL=125s`), and swept with the p7z codecpack
(`INVFS_CODECPACKS` pointed at a pack copy carrying a `bin/7zz` shim,
`INVFS_TOOL_SCRATCH` on real disk). **The log line:**

```
  sweeping usr84k.7z (3579184377 bytes)...
[sweep-v3] checking inode 2 (usr84k.7z)...
[cpack] enter usr84k.7z (decomp_gen=1 gen=1)
[cpack] enumerate failed
```

It bails at **step 1 (`enumerate`)**, long before the size guard is reached.

### Why: a 65,536-member ABI cap, not the cost model

`bin/p7z enumerate` on the 84,279-member archive returns **rc=3 in 0 s** — a
clean instant refusal, not a timeout.

* `src/core/vol_cpack.c:1826` — `#define CPACK_MAX_MEMBERS 65536u`
  ("the WP10 §12 total-member sanity bound"); the table parser stops there at
  `vol_cpack.c:1927` (`if (n == CPACK_MAX_MEMBERS)`).
* `tools/codecpacks/p7z.codecpack/p7z.c:138` — `#define MAX_FILES 65536u`
  ("FS member bound (table rows)"), enforced at `p7z.c:655`
  (`if (fi->num_files == 0 || fi->num_files > MAX_FILES) return -1;`) and at
  `p7z.c:1357` on the recipe side (`CPACK_MAX_IDX_PLUS1`).
* The identical bound is mirrored by **every** container pack:

  | pack | constant | site |
  |---|---|---|
  | ext4fs | `ABI_MAX_MEMBERS 65536u` | `ext4fs.c:195` |
  | fatfs | `MAX_MEMBERS 65536u` | `fatfs.c:116`, enforced `:280` |
  | xfs | `MAX_MEMBERS 65536u` | `xfs.c:125`, enforced `:594` |
  | ntfs | `MAX_MEMBERS 65536u` | `ntfs.c:175` |
  | rawdisk | `MAX_MEMBERS 65535u` | `rawdisk.c:125`, enforced `:244` |
  | qcow2 / vdi | `MAP_MAX_ENTS 262148u` = 4·65536+4 | `qcow2.c:185`, `vdi.c:144` |

**So: no container on InvariantFS can hold more than 65,536 members.** An
84,279-file rootfs cannot be packed into a single container, full stop.

### The size guard would have ACCEPTED it

I replicated `cpack_project` + `cpack_size_guard` exactly, in a standalone
tool (`/srv/bench/cpack/guardcalc.c`, engine untouched — `ZSTD_compress` at
`CPACK_ZSTD_LANE = 19`, `min(compressed, usize)` per member, 8-way sharded
which is bit-identical to the engine's serial loop because `ZSTD_compress`
is a pure function of `(src, len, level)`):

```
nmem         = 84,279
sum_usize    = 3,567,834,051 B
content      = 1,087,009,467 B    (zstd-19 shrinks /usr by 69.53%)
               23,171 members stored verbatim, 61,108 shrank
member_cost  = 84,279 x 16,384    = 1,380,827,136 B
fixed        = ~13.7 MB           (recipe + member table)
projected    = ~2,481,553,660 B   vs  orig_len 3,579,191,296 B
projected/orig_len = 0.693        ->  ACCEPT
```

Against the actual refusal test at `vol_cpack.c:2285-2287`:
`projected*1000 = 2,481,553,660,000` vs the required
`orig_len*995 = 3,561,295,339,520`. **Passes with 28% margin.**

Break-even, where the `CPACK_MEMBER_COST` term exactly consumes the whole
compression gain:

```
N* x 16384 == gain   ->   N* = 2,480,824,584 / 16,384 = 151,418 members
```

84,279 is well under 151,418. **The brief's "at 50,000 members that is
781 MB and the decomposition is DECLINED" does not hold for this corpus** —
50,000 × 16,384 = 819 MB against a 2.48 GB gain is still a clear win. The
binding constraint is the member cap, and it binds at **65,536**, ~28% below
where the cost model would have said no.

**Control (the lane does work below the cap):** a 20-member 7z sweeps to

```
[cpack] enter tiny20.7z (decomp_gen=1 gen=1)
[packdbg] p7z on tiny20.7z -> prc=123
  tiny20.7z: p7z (codecpack)
```

— `prc >= 100` means committed (`src/core/vol_cpack.c:2830`).

### 4.5 A cost-model fix landed on `main` during this WP — and it does not fix this

`f42434e` ("cpack: price a member by its size, not by a flat 16 KiB",
03:16 today) is already on `main`. It retires `CPACK_MEMBER_COST = 16384ull`
(`src/core/volume_internal.h:1851` at 49fdb78) in favour of a size-aware
`CPACK_MEMBER_META 1024 + CPACK_MEMBER_UNBATCHED 256`
(`src/core/volume_internal.h:1923-1924` at HEAD), charged at
`src/core/vol_cpack.c:2351-2352`.

Re-running the guard's own arithmetic over the same 84,279-member corpus under
the new price:

| price per member | member_cost | projected | vs orig 3,579,191,296 | verdict |
|---|---|---|---|---|
| 16,384 (old, 49fdb78 — what I measured) | 1,380,827,136 | 2,481,553,660 | 0.693 | **ACCEPT** |
| 1,280 (new, f42434e) | 108,214,236 | 1,208,940,760 | 0.338 | **ACCEPT** |

**The cost model accepts at 84,279 members under both prices.** So:

* f42434e's stated motivation — *"At 50,000 members that is 781 MB of charged
  overhead and the decomposition is declined — which is exactly the shape of a
  Linux rootfs, the workload this filesystem is for"* — rests on a premise this
  measurement contradicts. For this corpus the flat 16 KiB term was already
  affordable at 84,279 members, with 28% margin under the old price. The
  change is a reasonable refinement on its own terms; it just is not what was
  blocking rootfs ingest.
* `CPACK_MAX_MEMBERS 65536u` is **still `src/core/vol_cpack.c:1826` at HEAD**,
  still enforced at `:1927`, still mirrored by all nine container packs.
  f42434e does not touch it. **The refusal at 84,279 members is unchanged by
  that commit** — it still fails at `enumerate`, exactly as measured here.

If the intent of that work is "make rootfs-scale containers ingestible",
the cap is the thing to raise.

### Three defects found on the way

1. **p7z cannot run on this host out of the box.** `tools/codecpacks/p7z.codecpack/manifest`
   declares `requires = 7zz`; the host has `7z`/`7za` but no `7zz`.
   `pack_probe_impl` (`src/codecs/codec.c:792-832`) checks `requires`
   correctly, so the pack reports
   `cpack: p7z: tools absent, waiting for RAW (no stamp set)`
   (`vol_cpack.c:2865`) — **indistinguishable in the log from a genuine size
   refusal**, and it costs a 3.5 GB pin + extract to discover. Worked around
   with a `bin/7zz` → `7za` shim in a copied pack dir.
2. **rawdisk and splt_test have no `bin/` helper built at all**
   (`tools/codecpacks/rawdisk.codecpack/bin/` is empty), so they are
   permanently "tools absent". rawdisk's manifest also uses a **bare** argv0
   (`enumerate = rawdisk enumerate {in} {out}` — no `/`), which is precisely
   the trap p7z's own manifest documents at
   `tools/codecpacks/p7z.codecpack/manifest:34-38`: the probe resolves
   `<pack>/bin/rawdisk`, but the exec layer resolves
   `$INVFS_TOOLS -> /usr/lib/invfs/tools -> PATH`. Latent today; build the
   binary and the probe will pass while the exec still cannot find it.
3. **The containerpack scratch defaults to tmpfs.**
   `tool_tmpdir` (`src/core/vol_cpack.c:197-200` at 49fdb78) tries
   `/dev/shm` then `/tmp`. The pipeline pins the whole container *and*
   extracts every member there — **~2× the container size in RAM-backed
   tmpfs**, i.e. ~7 GB for a 3.5 GB rootfs container on this 7.6 GB swapless
   host. That is the same pressure that OOMed this box on 2026-09-29
   (dmesg: `shmem: 6,668,124 kB`). `INVFS_TOOL_SCRATCH` is the documented
   escape hatch; I used it for the 84k run.

   > **Already fixed on `main` while this WP was running.**
   > `src/core/tool_scratch.c` (new file, 05:29 today) replaces the
   > tmpfs-first default with a capacity-checked search
   > (`INVFS_SCRATCH_ROOTS`, default `/dev/shm:/tmp:/var/tmp`), an
   > allocation-ceiling cap on tmpfs offers
   > (`INVFS_SCRATCH_TMPFS_MAX_FRAC`, default 50%), and mid-job migration
   > when the member total only becomes known after `enumerate`. AGENTS.md
   > §2.8b now documents it. My measurements used `INVFS_TOOL_SCRATCH`
   > pointed at real disk, which is exactly the remedy it automates — so
   > finding 3 needs no further work; the first two do.

---

## 5. Recommended next steps (none taken — this WP is measurement only)

1. **The one decision that matters: what to do about the per-append fsync.**
   ADR-009 deferred it "until measured". It is now measured. Group commit —
   barrier once per batch of appends, with a `delta_durable` anchor so the
   acknowledged-write contract stays honest — is the obvious shape, and
   `docs/adr/ADR-009-v3-durability-contract.md` already names the
   prerequisites. Without it, rootfs-scale ingest is a multi-day operation.
2. **Raise `CPACK_MAX_MEMBERS` past 65,536.** This is the single thing that
   makes an 84,279-file container ingestible, and it is untouched by the
   cost-model work that landed on `main` during this WP (§4.5). If it stays,
   document it as a hard ingest ceiling in
   `docs/architecture/META-V3.md` §2.7 alongside the `INVFS_META_FRAC`
   guidance — right now it is discoverable only by reading
   `vol_cpack.c:1826`. And correct f42434e's rationale, which claims the
   flat 16 KiB price was what declined a rootfs-shaped container; the
   measurement says the price accepted it at 84,279 members with 28% margin
   and the member cap is what refused it.
3. Install `7zz` (or drop the `requires` when `-mhc=off`), and build the
   missing `rawdisk` / `splt_test` helpers.
4. ~~tmpfs scratch~~ — **done**, see §4 defect 3: `src/core/tool_scratch.c`
   landed on `main` today with a capacity-checked root list and mid-job
   migration.

---

## 6. Reproduce

```bash
# corpus (never modify the host /usr)
mkdir -p /srv/bench/rootfs/src && cp -a /usr /srv/bench/rootfs/src

# per-record fsync: the interposer and the run
#   /srv/bench/rf-scale/fsspy.c   (backtrace on every fsync)
#   /srv/bench/rf-scale/fscount.c  (count + busy time, printed at exit)
LD_PRELOAD=/srv/bench/rf-scale/fscount.so bin/invf-import t500.img tiny500
#   -> imported: 0 dirs, 500 files, ... in 390.0s
#   -> FSYNC_TOTAL=3509 busy=387.74s

# containerpack at 84k members
cd /srv/bench/rootfs/src
7za a -t7z -m0=Copy -mhc=off -mx=0 /srv/bench/cpack/usr84k.7z "@rel.txt"   # 84,279 members
INVFS_CODECPACKS=/srv/bench/cpack/packs \
INVFS_TOOL_SCRATCH=/srv/bench/cpack/scratch \
INVFS_DEBUG_PACKS=1 bin/invf-sweep b84.img
#   -> [cpack] enumerate failed

# the size guard's own arithmetic, with the engine untouched
/srv/bench/cpack/guardcalc list.txt <shard> 8      # 8 shards, summed
```

### Files changed

**None — measurement only.** No engine file was modified; `make` was never
needed. The worktree `/srv/bench/worktrees/wt-rootfs` on branch
`wp/rootfs-scale` is clean at `49fdb78`. Nothing was committed to `main` and
nothing was merged. All tooling lives under `/srv/bench/` and is not part of
the repo.