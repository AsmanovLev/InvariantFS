# InvariantFS

Semantic content-addressed filesystem with a bit-exactness invariant:
every compression is verified by decompress-and-compare before it is
trusted; containers (ZIP/TAR) are stored byte-original with members
exposed as on-demand windows; transcodes happen only where bit-exactness
is proven per file.

> **What is written comes back bit-for-bit identical. Always.**

## State: experimental

**Not yet tested on real hardware. Use at your own risk. Do backups.**

A Gentoo/OpenRC system runs with its entire root on an InvariantFS volume
(see `tools/mkdisk.sh`, `vm/`). Current release: **v0.2.0**.

## Architecture

### Zones

| Zone | Role |
|---|---|
| **RAW** | Linear landing area. New writes go here immediately, LZ4-compressed, cheap and fast. |
| **Shadow** | Optimized storage, split into **Text** and **Binary** zones so similar data is consolidated. |
| **Metadata** | Superblock, block bitmap, L2P journal, inode area. |

Writes are decoupled from compression. Data lands in RAW at write speed; the
**sweep worker** later moves it to Shadow under the appropriate codec. Expensive
analysis therefore never sits on the write path.

### AST recipe

Each file carries a recipe that maps byte ranges of the *original* file onto
`(zone, block, offset)` tuples. Segments are stored as
`[4B csize][4B crc32c][data]`.

- **Partial reads are cheap.** Reading a FLAC file's tags touches only the text
  segments; the audio blocks are never decompressed.
- **Containers nest.** The recipe is a tree, so a zip-in-a-tar mirrors its real
  structure (bounded at depth 16).

### Journal

An append-only L2P journal (`MAP` / `UNMAP` / `SWEEP` / `CHECKPOINT`) persists the
logical-to-physical mapping. Mount replays from the last checkpoint. The
superblock carries a state byte (`CLEAN` / `DIRTY` / `RECOVERY`) driving recovery.

### Deduplication

Content addressing via BLAKE3. Identical blocks are stored once and refcounted.

## Building

```sh
make                # all tools -> bin/
make bin/invf-fuse  # single target
```

Requires `gcc`, `libfuse3-dev`, `zlib1g-dev`, `libzstd-dev`. Bundled codecs:
zstd, lz4, miniz, blake3, flacx, rs, ppmd, bcj_x86.

## Tools

| Tool | Purpose |
|---|---|
| `invf-mkfs <img> [gb]` | Format a volume. Env `INVFS_META_FRAC=N` scales metadata zone |
| `invf-fsck [-f] <img>` | Check and repair. `-f` forces repair; auto-recovery on DIRTY volumes |
| `invf-fuse [-o opts] <vol> <mnt>` | FUSE daemon (see below) |
| `invf-ls <img>` | List files in a volume |
| `invf-cat <img> <name> [out]` | Extract a file |
| `invf-cp <img> <file> [name]` | Copy a host file into a volume |
| `invf-stat <img> [--files]` | Space inspector with zone bars |
| `invf-import <vol> <dir>` | Bulk tree import (~50k files/sec) |
| `invf-sweep <img> [--dry-run] [--seal]` | Offline sweep (RAW -> Shadow, transcodes) |
| `invf-stats <img>` | Full statistics walk with per-class ratios |
| `invf-resize <img> ...` | Offline volume grow/shrink |
| `invf-rollback <img>` | Undo last sweep from checkpoint |
| `invf-verify [--deep] <img>` | Content verification |
| `invf-migrate-v2 <img>` | Format v1 -> v2 conversion |
| `invf-zip list\|get <img> <zip>` | ZIP container inspection |
| `invf-zip get <img> <zip> <member> <out>` | Extract ZIP member |
| `meta_probe <img> <name>` | Developer probe (mutates) |
| `meta_probe <img> --heat <name>` | Read-only heat/class dump |

## FUSE daemon

```sh
invf-fuse [-f] [-o opt[,opt]] <volume> <mountpoint>
```

**Mount options:**

- `attr_timeout=0,ac_attr_timeout=0` — no kernel attr caching (default)
- `raw_watermark=<pct>` — kick background sweep when RAW fill exceeds mark (WP26)
- `arc_limit=<MB>` — decoded-unit cache limit (default 256)
- `dec_mem_limit=<MB>` — decompression memory limit

**Background sweep:**

- Opt-in: `INVFS_SWEEP_INTERVAL=<sec>`
- Manual: `kill -USR1 $(pidof invf-fuse)` or `setfattr -n user.invfs.sweep -v 1 /`

**Control namespace on mount root:**

```sh
getfattr --only-values -n user.invfs /        # RAM summary
getfattr --only-values -n user.invfs.stats /  # hot counters + zone usage
```

**Permissions (WP-A):** mode/uid/gid enforced daemon-side. POSIX ACLs stored as
`system.posix_acl_*` xattr blobs and honored in access checks.

## Bootable VM

```sh
tools/mkdisk.sh <volume.img>     # GPT: ESP + volume as p2
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -nographic \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vm/OVMF_VARS.fd \
  -drive file=vm/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0
```

Chain: OVMF -> GRUB (ESP) -> initramfs (busybox + fuse.ko + invf-fuse)
-> switch_root into Gentoo. See `docs/GENTOO-INSTALL.md` for full guide.

## Testing

```sh
make test    # 4722 unit checks (core + codecs + recipes + CLI)
make e2e     # 38 end-to-end FUSE tests
make flakey  # chaos/soak tests (torn sweep, error storm, compact-flip)
```

Gate tests in `tools/`:

| Script | Coverage |
|---|---|
| `bench-gate-b.sh` | v2 vs v1 read benchmark |
| `test-gate-c.sh` | ENOSPC stress (floor breach, race, sweep interlock) |
| `test-gate-d1.sh` | ACL/xattr edge cases (4096 boundary, listxattr, full-volume, concurrent create) |
| `test-gate-d2.sh` | Parallel writes (same-file, multi-file, sweep-during-writes) |

## Codecs

**General purpose:** LZ4 (write path), ZSTD, PPMd (text zone), BLAKE3 (dedup).

**Bit-exact transcode families:**

| Tag | Family | Reproduction method |
|---|---|---|
| `FLACR` | FLAC | PCM as APE + frame recipe rebuilding exact FLAC bitstream |
| `TARR` | TAR | Members split; `IVFT` recipe restores headers, padding, alignment |
| `GZR` | gzip | Deflate replicated by brute-forcing zlib parameters |
| `PNGR` | PNG | Lossless JXL + `IVPN` recipe for row filters and deflate params |
| `EXER` | PE/EXE | x86 BCJ filter + ZSTD, recipe reproduces original |

Where replication fails, the file stays in its original form. That is the
invariant doing its job.

## Known limitations

- Per-op write barriers/group-commit still open (WP3); durability contract is
  fsync/close (`vol_sync`).
- mmap read/write supported via FUSE writeback cache (WP4a).
- Codecs: JPEG->JXL needs `cjxl`/`djxl`; FLAC needs `mac` + `ffmpeg`;
  absent tools = file stays RAW.
- Bit-rot recovery only via explicit seal (`invf-sweep --seal`, WP20).
- xattr storage capped at 4096 bytes per inode (`INVFS_META_XATTR_MAX`).
- No NFSv4 ACLs (POSIX.1e only).
- No `security.*`/`trusted.*` xattr namespaces.

## Layout

```
src-extracted/VFS/src/   engine + CLI (C11): core/ codecs/ recipes/ cli/
tools/                   build scripts, guest config, test harnesses
packaging/               install.sh, debian/, RPM, Arch, systemd, man pages
docs/                    Gentoo install guide
vm/                      initramfs tree, disk builder inputs
```

## License

GPL-2.0-only. See [LICENSE](LICENSE).
