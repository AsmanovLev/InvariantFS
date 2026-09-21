# InvariantFS

A content-addressed filesystem built around one guarantee:

> what you write comes back byte-identical.

Every codec is verified by decompress-and-compare before it is trusted; when a
transcode cannot be proven reversible, the bytes are stored verbatim. Files are
stored as **content + a recipe**, not as a block range, which is what makes
deduplication, codec selection and partial reads fall out naturally.

FUSE-based, single-host, append-oriented. Written in C11.

**Status: experimental.** No frozen on-disk format, no power-loss durability
contract. Not a replacement for ext4/XFS on general workloads.

## Features

* **Bit-exactness as an invariant** — a transcode is applied only after
  decompress-and-compare proves it round-trips; otherwise the segment stays
  verbatim. `invf-verify [--deep]` proves stored content.
* **Content-addressed segments** — every stored segment is BLAKE3-hashed and
  deduplicated with refcounts; identical segments share one physical block.
* **Per-file recipe (AST)** — a tree mapping original byte ranges to stored
  segments, so partial reads touch only what is needed (reading a FLAC's tags
  never decompresses the audio).
* **Per-content codecs** — text and binary are classified and clustered, then
  batched across files (PPMd text batches, ZSTD+BCJ binary batches). In-tree:
  LZ4, ZSTD, PPMd, BLAKE3, FLAC, BCJ-x86, miniz, Reed–Solomon.
* **Verified transcode families** — FLAC (`FLACR`), TAR (`TARR`), gzip (`GZR`),
  PNG (`PNGR`), PE/EXE (`EXER`).
* **Containers kept original** — ZIP/TAR/7z/VDI/qcow2/... are stored
  byte-original with members exposed as on-demand windows.
* **Append-only zones + offline sweep** — writes land in RAW, then the sweep
  drains them into a type-clustered Shadow zone, re-encoding where proven.
* **Meta-v3 Metadata Architecture** — B+ tree base metadata + append-only Delta
  Log with background fold worker and lock-free reads.
* **Deduplication** — segment-level, BLAKE3, with an online pass in the sweep.
* **Checkpoints, rollback, time travel** — undo the last sweep, or mount a
  read-only view at a past checkpoint.
* **Recovery tooling** — append-only owner WAL with replay; `invf-fsck [-f]`
  walks, quarantines and repairs.
* **Two-device volumes** — dev0 (metadata + RAW) and dev1 (canonical Shadow),
  with metadata mirroring; volumes identified by UUID, not kernel name.
* **Codec packs** — codecs are external, versioned packs (`manifest` + helper)
  discovered at runtime; a volume can carry the packs it needs and self-host
  them across a maintenance boot.
* **Pack registry** — `invfs-pack` installs/verifies packs from the
  [registry](https://github.com/AsmanovLev/InvariantFS-registry), with
  alternatives (one encoder per family) and static helpers.
* **POSIX layer** — mode/uid/gid enforced daemon-side, POSIX.1e ACLs stored as
  `system.posix_acl_*` xattr blobs, xattrs as opaque blobs.

## Why FUSE?

InvariantFS stores files as content segments + recipes. Reads traverse
an AST, decompress segments through codecs (ZSTD, PPMd, FLAC, ...), and
re-assemble byte ranges on demand. This is inherently user-space work:
you cannot put ZSTD seekable, FLAC decode, or container windowing into a
kernel module without pulling an entire codec ecosystem into ring-0.

FUSE gives this for free. The alternatives are worse:

* **In-kernel LKM** — you'd rewrite the FUSE daemon as a kernel module
  that still calls user-space for every compressed segment. Two code
  bases, kernel panics on AST parse bugs, and the same context switches
  with extra ioctl marshalling. Not simpler, just harder to debug.
* **ublk / block-device layer** — adds ext4/XFS on top, doubling the
  metadata overhead and the page cache, for no measurable gain on a
  content-addressed store that doesn't have fixed block semantics.
* **eBPF hot-path** — the kernel verifier limits instruction count and
  memory allocations; it cannot hold 512 MiB ZSTD dictionaries or run
  multi-millisecond FLAC decode loops. Good for stats, not data-plane.
* **Native library (libinvfs.so)** — forces every consumer (Jellyfin,
  compilers, package managers) to link against a proprietary SDK,
  defeating the point of a POSIX filesystem.

The real overhead is not the FUSE protocol (libfuse3 uses io_uring for
/dev/fuse I/O and is very fast). It is how the daemon handles requests
internally. The optimizations that matter:

* **FUSE writeback cache** (`FUSE_CAP_WRITEBACK_CACHE`) — kernel
  aggregates small writes into large aligned chunks; the daemon receives
  ideal 1 MiB+ sequential I/O for its append-only RAW layer.
* **In-memory AST cache** — recipes loaded once at open, resolved from
  RAM on every subsequent read. Zero disk metadata lookups in steady
  state.
* **Split thread pools** — fast-path pool for stat/lookup/readlink,
  separate codec pool for decompression. A slow PPMd batch never blocks
  an `ls -l`.
* **Pre-fetching** — io_uring readahead on the next AST segment while
  the current one is still decompressing.
* **Inline data** — files under ~2 KiB skip the recipe entirely; data
  lives in the inode record. Eliminates Shadow-zone reads for millions
  of small files.

With these, FUSE overhead becomes negligible compared to disk I/O and
codec CPU cost.

## Non-features

* Not a drop-in ext4/XFS/ZFS replacement for general workloads.
* No network/SAN support; single host only.
* No high write throughput: the write path is append-only, consolidation is
  offline.
* No power-loss durability guarantee (`make flakey` is a soak, not a contract).
* No snapshots/CoW clones in the btrfs/ZFS sense — checkpoint/rollback only.
* No frozen on-disk format yet; the v3 record layout is a deliberate break.
* Not for metadata-space-dominated sets (millions of empty files).
* No `security.*`/`trusted.*` xattrs, no NFSv4 ACLs; xattr cap 4096 B/inode,
  mtime truncated to seconds.

## How it works

A volume has three primary zones: **Metadata** (superblock, RT30 B+ tree base,
Delta Log, bitmap), **RAW** (linear landing area for new writes), and **Shadow**
(consolidated, type-clustered storage).

```
write()   ->  RAW (LZ4 or verbatim)  ->  append delta mutation (inode + dirent)
```

Reads check the in-memory delta index first; if absent, they read the immutable
B+ tree base without lock contention. Periodic background **fold** merges
accumulated delta records into the B+ tree and atomically updates the RT30
double-slot descriptor.

The **sweep** (`invf-sweep`, or the FUSE background sweep) drains RAW into
Shadow, re-clusters text/binary, runs dedupe and text-zone GC, re-encodes where
bit-exactness is proven, and optionally writes parity seals.

```
unlink()  ->  delta delete entry appended; space reclaimed at next fold + sweep
```

For complete architecture specifications, see:
* [Architecture Overview](docs/architecture/OVERVIEW.md)
* [Meta-v3 Specification](docs/architecture/META-V3.md)
* [Architecture Decision Records (ADRs)](docs/adr/README.md)
* [CLI Usage Guide](docs/guides/CLI-USAGE.md)

## Comparison

| | ext4 / XFS | btrfs / ZFS | InvariantFS |
|---|---|---|---|
| Unit of storage | fixed blocks | blocks + COW | content segments + recipe |
| Write model | in-place | COW | append-only zones, offline consolidation |
| Compression | no / opt | opt, not bit-exact-checked | per-segment, **proven** bit-exact |
| Deduplication | no | btrfs yes | built-in, segment-level, BLAKE3 |
| Metadata | fixed inode table / B-tree | B-tree | dynamic extents + mapper |
| Snapshots | no / LVM | yes (COW) | checkpoint + rollback + view |
| Containers | opaque | opaque | stored original, members on demand |
| Codecs | in-kernel | in-kernel | external versioned packs |
| Best at | general workloads | general + snapshots | archives, read-mostly roots, dedup |

## Install

```sh
# host tools (FUSE mode): release with sha256 verification, no silent sudo
curl -fsSL https://github.com/AsmanovLev/InvariantFS/releases/latest/download/bootstrap.sh \
  | sudo sh -s -- --yes

# or from a checkout
sh packaging/bootstrap.sh --source --prefix /usr/local
```

Boot a distribution root on InvFS (single- or two-device):
`docs/GENTOO-INSTALL.md`, `docs/ARCH-INSTALL.md`, `docs/VOID-INSTALL.md`.
Codec packs: `invfs-pack` — see the
[registry](https://github.com/AsmanovLev/InvariantFS-registry).

## Build & test

```sh
make                # all tools -> bin/
make test           # 4722 unit checks
make e2e            # serialized FUSE end-to-end suites
make flakey         # chaos/soak
make release        # dist/invfs-<ver>-<arch>.tar.zst + SHA256SUMS
```

Requires `gcc`, `libfuse3-dev`, `zlib1g-dev`, `libzstd-dev`. Run e2e suites
through `tools/run-e2e.sh` (serialized via `/tmp/invfs-e2e.lock`).

## Known issues

* **systemd as PID 1 on a FUSE root is partially working** — WP66 added
  `/run` tmpfs + cgroup2 pre-mount (H1) and fallocate/ioctl stubs (H3).
  Remaining: runtime lookup corruption on two-device volumes (H2) needs
  investigation; use the busybox/OpenRC/runit fallback meanwhile.
* **`invf-fsck -f` after a fresh import** can break runtime FUSE directory
  lookups; offline reads stay fine. Under investigation.
* **Two-device FUSE:** the first write can poison symlink-directory lookups
  (`EINVAL`/`ENOTDIR`). Engine-level multi-device tests are unaffected.
* Heat accounting persists only at a sweep run (background sweep is off by
  default), so short read sessions are not accumulated.

## Docs

```
docs/        per-distro install guides, SECURITY.md
src/doc/     on-disk format, AST recipe, sweep, crash recovery, dedup, benchmarks
impl_docs/   architecture, FILEMAP, AUDIT, WP*.md
```

## License

GPL-2.0-only. See [LICENSE](LICENSE).
