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
* **Dynamic metadata extents** — inode records grow with the tree (mapper table
  + `MET0`) instead of a pre-sized inode table.
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

A volume has three zones: **Metadata** (superblock, bitmap, mapper, owner WAL,
inode records), **RAW** (linear landing area for new writes), and **Shadow**
(consolidated, type-clustered storage).

```
write()  ->  RAW (LZ4 or verbatim)  ->  append inode record + CRC
```

A file is one record: a 36-byte prefix, a variable-length name, and an **AST
recipe** mapping original byte ranges to `(zone, pba, offset)` windows. Stored
segments are framed `[4B csize][4B crc32c][payload]`; the recipe is self-
describing, so a record can be read without a separate mapping table.

The **sweep** (`invf-sweep`, or the FUSE background sweep) drains RAW into
Shadow, re-clusters text/binary, runs dedupe and text-zone GC, re-encodes where
bit-exactness is proven, and optionally writes parity seals.

```
unlink()  ->  tombstone appended; blocks freed at the next sweep
```

Deletes do not free immediately: a volume that sees many writes-then-deletes
fills up until the sweep runs. This is expected, not a bug.

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

* **systemd as PID 1 on a FUSE root is degraded** — journald/udevd/dbus fail;
  use the busybox/OpenRC/runit fallback (documented per distro).
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
