# ADR-007: Modular Pack Architecture, On-Volume `.invariantfs`, and `.ivpack`

## Context

InvariantFS utilizes external codecs and container unpackers (e.g. `qcow2`, `ext4fs`, `vdi`, `jxl`, `flac`) to decompose and transcode heterogeneous storage formats while guaranteeing the bit-exactness invariant.

Historically, codecpacks in InvariantFS were invoked as out-of-process CLI helpers via `fork()` + `execve()` under Landlock/RLIMIT sandboxing (`helper_exec.c`, WP61). While this provided crash isolation, it introduced fundamental performance bottlenecks:
1. **Per-call process spawn overhead:** In container formats like `qcow2` containing 10,000 to 110,000 compressed 64 KiB clusters, launching an external binary for each cluster costs 1–2 minutes of CPU time solely in kernel process spawn overhead.
2. **Duplicated primitive logic:** Deflate reproduction ("bruteforce" parameter search) and stream verification were fragmented across `vol_cpack.c`, `gzrepro.c`, and `pngx.c`, with no reusable primitive for `qcow2` or `zip`.
3. **Volume Portability (Self-Containment):** When an InvariantFS volume is detached and moved to another machine, host-installed tools under `/usr/lib/invfs/` or `/.invariantfs/` may be absent, rendering specialized containers unreadable or non-decomposable.
4. **Long-Term Backward Compatibility & Migration:** Codec and containerpack formats evolve. To ensure old volumes remain readable and can be safely upgraded during offline sweeps, the filesystem requires explicit retention of legacy pack versions.

## Decision

InvariantFS adopts a modular, dynamic library-based plugin architecture with on-volume self-containment, structured into three tiers:

### 1. Three Pack Classes

Packs are formally classified into three distinct categories:

* **`codecpack` (Data Transformations):**
  - Implements `encode` and `decode` operations on raw data segments or whole files.
  - Examples: `flac`, `jxl`, `ppmd`, `bcj_x86`, `zstd-heavy`.
  - Stateless with respect to disk volume structure.

* **`containerpack` (Structural Decomposers):**
  - Implements container lifecycle operations: `enumerate`, `extract`, `strip`, `rebuild`, and `map` (MRMP).
  - Decomposes container formats into metadata recipes and raw payload streams (`diskimg` or `name!partN`).
  - Examples: `qcow2`, `ext4fs`, `fatfs`, `xfs`, `ntfs`, `vdi`, `zip`.

* **`helperpack` (Universal Accelerators & Primitives):**
  - Shared algorithmic and mathematical primitives called across multiple containerpacks and codecpacks.
  - Examples:
    - `deflate_repro`: Fast in-memory parameter search (levels, memLevels, strategies, windowBits) for bit-exact zlib/deflate reproduction. Used by `qcow2`, `zip`, `gzr`, and `pngx`.
    - `delta_diff`: Binary micro-patching generator for ADR-006 delta-lossless compression.

### 2. On-Volume Self-Containment (`.invariantfs/`)

To ensure a volume is 100% self-contained and portable across hosts, all required packs and their historical versions are stored directly on the volume filesystem root under `.invariantfs/`:

```
<volume_root>/.invariantfs/
├── packs/                     # Active, current pack versions
│   ├── codecs/                # e.g., flac-2.0.0.ivpack
│   ├── containers/            # e.g., qcow2-2.0.0.ivpack
│   └── helpers/               # e.g., deflate_repro-1.0.0.ivpack
│
└── old_packs/                 # Historical pack versions retained for read & sweep migration
    ├── containers/
    │   └── qcow2-1.0.0.ivpack  # Retained so legacy Q2R1 recipes remain readable
    └── codecs/
        └── flac-1.0.0.ivpack
```

* **Resolution Order:** On-volume `/.invariantfs/packs/` takes precedence, followed by host administrator overrides (`$INVFS_PACK_ROOT` / host `/.invariantfs/packs/`).
* **Sweep Migration:** `invf-sweep` detects older on-disk recipe versions, uses `old_packs/` to read the legacy structures, re-encodes them via `packs/` using modern codecs/deduplication, verifies bit-exactness, and commits the updated recipes. Once no live references point to an old version, `old_packs` can be pruned (`invfs-pack gc`).

### 3. Pack Archive Format: `.ivpack` (Uncompressed ZIP-0)

Packs are packaged as `.ivpack` files using standard uncompressed ZIP format (`zip -0` / stored):

```
my_pack.ivpack (zip -0)
├── manifest                 # Key-value pack metadata, class, ABI version, dependencies
├── sha256                   # Cryptographic verification checksums
├── lib/
│   └── my_pack.so           # Shared object built for target platform
└── include/                 # (Optional) Public C headers for helperpacks
```

**Rationale for `zip -0`:**
1. **$O(1)$ Central Directory Access:** Metadata and manifest can be inspected in microseconds by reading the trailing ZIP directory without sequential streaming.
2. **Direct Shared Memory & Zero-Copy Loading:** Files stored without compression can be read or mapped directly without temporary disk extraction.
3. **Block-Level Deduplication:** Multiple `.ivpack` archives or versions containing identical code pages or static runtime blocks are naturally deduplicated by InvariantFS 64 KiB deduplication.

### 4. Concurrency, Execution & Memory Model

To guarantee both high performance and crash resilience:

* **Linker Namespace Isolation:**
  - Shared objects are loaded using `dlmopen(LM_ID_NEWLM, ...)`.
  - Each pack operates in its own isolated dynamic linker namespace, preventing symbol collisions across conflicting versions (e.g. differing `zlib` or library builds).

* **Worker Pool with Partitioned Memory** *(name corrected by WP101: the pool is partitioned, not sandboxed)*
  - `invf-fuse` and background sweepers spawn a fixed pool of $N = \min(nproc, 16)$ worker processes (`invf-plugin-host`).
  - **WP101 — the workers are NOT sandboxed.** `tools/invf-plugin-host.c` contains no `clone`/`unshare` (so no `CLONE_NEWNET`), no `setuid`/`setgid`/`seteuid` (so no drop to `nobody`) and no `landlock_*` call; `accept()` (`:374`) does no `SO_PEERCRED` check. The control socket is a fixed `/tmp` path (`:253`) that the code never `chmod`s (its mode is whatever the launcher's umask leaves) and the shm object is `shm_open(..., 0666)` (`:308`), so the pool is reachable by any local user that can reach `/tmp` and `/dev/shm`. What the design actually buys is the `dlmopen` namespace above, per-slot memory partitioning, and the fault isolation below. Hardening this host is tracked in `impl_docs/AUDIT.md`; do not cite this ADR as a containment claim.
  - **Shared Memory Pool:** Sized at $64\text{ MiB} \times nproc$ in `/dev/shm/invfs_plugin_pool` (`src/core/invf_plugin_ipc.h:16`).
  - **Dedicated SPSC Slots:** Memory is strictly partitioned into independent $64\text{ MiB}$ slots (one per worker). Communication uses lock-free Single-Producer Single-Consumer ring buffers with `eventfd`/`futex` notification.
  - **Fault Isolation:** If a shared object in worker $i$ encounters a `SIGSEGV` or memory corruption, only worker $i$ terminates. The main daemon and remaining $N-1$ workers continue uninterrupted. The failed slot is assigned a freshly spawned worker in $\approx 1\text{ ms}$, mirroring the recovery speed of legacy CLI helpers while achieving in-memory transfer speeds.

## Consequences

- **Extreme I/O Throughput:** Container parsing and cluster decompression run at C function call speed in RAM, eliminating hundreds of seconds of `fork/exec` overhead in large disk images.
- **Portability:** Volumes carrying `.invariantfs/packs/` are self-hosting and mountable on any compatible Linux host without prior external tool installations.
- **Deduplication Enabled on Guest Disk Content:** Containers like `qcow2` can utilize `helper:deflate_repro` to extract uncompressed guest clusters, allowing cross-VM deduplication and compact recipes.
- **Graceful Lifecycle & Migration:** `old_packs/` guarantees seamless read backward-compatibility while allowing offline sweeps to advance recipe formats.
