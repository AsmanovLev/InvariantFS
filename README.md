# InvariantFS

A content-addressed filesystem with one hard guarantee:

> **What is written comes back bit-for-bit identical. Always.**

Every codec application is verified by decompress-and-compare before it is
trusted. Containers (ZIP/TAR/7z) are stored byte-original with members exposed
as on-demand windows. Transcodes happen only where bit-exactness is proven per
file; where it is not proven, the bytes are stored verbatim.

**State: experimental.** Not tested on arbitrary real hardware. No power-loss
durability guarantee (see [Known limitations](#known-limitations)). Use backups.

Current format: **v0.3.x** (dynamic metadata extents; see
[Metadata model](#metadata-model-v03x)). Release string: `v0.2.1`.

## What it is / is not

**It is:** an append-oriented, single-host FUSE filesystem for archives,
read-mostly roots (container/VM bases, build roots, Gentoo stage3), and any
workload that values byte-preservation and deduplication over write throughput.

**It is not:** a drop-in ext4 replacement, a network/SAN filesystem, a
high-throughput write store, or a filesystem for millions of empty files
(metadata-space dominated). Writing directly to the backing image without going
through the volume is not supported.

## Feature tour

### Bit-exactness
- Per-segment codecs with a **decompress-and-compare** gate before a transcode
  is trusted; otherwise the segment stays verbatim.
- Bit-exact transcode families so far: **FLAC** (`FLACR`), **TAR** (`TARR`),
  **gzip** (`GZR`, deflate parameter brute-force), **PNG** (`PNGR`, lossless JXL
  + row-filter/deflate recipe), **PE/EXE** (`EXER`, x86 BCJ + ZSTD).
- `invf-verify [--deep]` proves content; `invf-cat` is the ground-truth reader.

### Tiered storage + codecs
- **Raw** landing zone (write path) → **Shadow** consolidated zone (sweep).
- Codecs: LZ4 (write path), ZSTD, PPMd (text zone), BLAKE3 (content hash),
  plus the bit-exact families above. Bundled: zstd, lz4, miniz, blake3, flacx,
  rs (Reed–Solomon), ppmd8, bcj_x86.
- Cross-file **batching** in the text/binary zones (PPMd text batches,
  ZSTD+BCJ binary batches) for better ratios than per-file coding.

### Content addressing & deduplication
- Segments are content-hashed (BLAKE3) and **deduplicated with refcounts**.
- Online dedupe pass (`vol_dedupe`) merges duplicate stored segments.

### Containers & partial reads
- **AST recipe** per file maps original byte ranges to `(zone, pba, offset)`
  tuples; the recipe is a tree, so containers nest (bounded depth).
- Partial reads touch only the needed segments: reading a FLAC's tags never
  decompresses the audio; ZIP/TAR members are extracted on demand.
- Container packs + `invf-zip` for ZIP inspection/extraction.

### Text/Binary classing & heat
- Segments are classified (text vs binary) and clustered by the sweep.
- **Read/write heat** (`rheat`/`wheat`, a per-inode TLV) drives promotion of hot
  text members into batches and decays exponentially per sweep run. Heat is
  session-accrued and **persisted at a sweep run's decay pass** (or an explicit
  `vol_heat_persist`), not on every close.

### Sweep worker
`invf-sweep` (offline) or the FUSE background sweep:
1. Drains **RAW → Shadow**, re-encoding per content class.
2. Re-clusters text/binary.
3. Runs **dedupe** and text-zone **GC**.
4. Optionally re-encodes with stronger codecs when bit-exactness is proven.
5. **Inode-area compaction** (tombstone reclamation), gated on a consistent cut.
6. Optional **parity seals** (`--seal`) for bit-rot recovery.

### Checkpoints, rollback & time-travel
- A sweep can arm a **checkpoint** (CKP0): `invf-rollback` undoes the last sweep;
  `invf-sweep --realize` accepts it and releases retained ranges.
- `vol_open_at` supports a read-only **time-travel** view at a checkpoint cut.

### Recovery & integrity
- Append-only **owner WAL** journal (MAP/UNMAP/SWEEP/CHECKPOINT), double-buffered
  slots; replay on mount; superblock state `CLEAN`/`DIRTY`/`RECOVERY`.
- `invf-fsck [-f]`: mapper-aware record walk, quarantine of torn versions,
  orphan reclamation, repair.
- Auto-recovery: a DIRTY volume with an anomaly-free scan returns to CLEAN
  read-write (`INVFS_AUTO_RECOVER=0` opts out).

### Multi-device
- Two-device volumes: **dev0** = metadata + RAW, **dev1** = Shadow/canonical
  (used for mirroring/acceleration and canonical placement).
- **Volume identity by UUID**: `invf-fuse --probe-uuid <dev>` prints the volume
  UUID from the superblock; used by the initramfs to pick devices by content
  instead of unstable kernel names.

### POSIX layer
- mode/uid/gid enforced daemon-side; **POSIX.1e ACLs** stored as
  `system.posix_acl_*` xattr blobs and honored in access checks.
- xattrs stored as opaque blobs (cap 4096 bytes/inode); no `security.*` /
  `trusted.*`; no NFSv4 ACLs.
- mtime truncated to seconds.

### Capacity policy
- ENOSPC ladder with a metadata reservation and a hard-min floor; breaching the
  floor flips the volume to READ-ONLY (`VOLF_RO_SPACE`), recoverable after a
  sweep/f`invf-resize`.
- `INVFS_META_FRAC=N` sizes the metadata zone at mkfs (rootfs images want `16`).

## Architecture

### Zones

| Zone | Role |
|---|---|
| **Metadata** | Superblock, block bitmap, metadata mapper (MET0), owner WAL journal, inode records |
| **RAW** | Linear landing area for new writes (LZ4 or verbatim) |
| **Shadow** | Consolidated storage (text/binary clustered), optional parity seals |

### Metadata model (v0.3.x)

Inode records no longer live in one contiguous area. They are stored in
**dynamic metadata extents** tracked by a **mapper table** (`[pba, size_class]`
pairs, 64 KiB … 2 GiB class) plus the **MET0** descriptor (`active_extent`,
`active_offset`, `extent_count`) in block 0. Appends go through
`meta_get_append_pos`; the mapper + MET0 are persisted on flush. The name and
id indexes are rebuilt at mount from a single extent-ordered record walk
(`vol_records_walk`).

Legacy `format_version=0` volumes keep the contiguous inode area; use
`invf-migrate-v2` to upgrade.

### AST recipe

Segments are `[4B csize][4B crc32c][payload]`; a file's recipe maps original
byte ranges to stored segments. Records carry their own segment `pba`s, so a
record is self-describing and needs no separate mapping to be read.

## Building

```sh
make                # all tools -> bin/
make bin/invf-fuse  # single target
make docs           # ctags indexes + doxygen HTML (impl_docs/doxygen/, graphviz)
```

Requires `gcc`, `libfuse3-dev`, `zlib1g-dev`, `libzstd-dev` (doxygen+graphviz
optional, for `make docs`). If the default `cc` in your environment is a broken
ccache symlink, use `make CC=gcc`.

## Install (host bootstrap)

`packaging/bootstrap.sh` installs the `invf-*` host tools from a GitHub Release
(or builds from source). It never installs runtime libraries and never silently
escalates to root: if it needs root it prints the `sudo sh` command and exits.

```sh
# release (picked automatically when assets exist); prompts unless --yes
curl -fsSL https://github.com/AsmanovLev/InvariantFS/releases/latest/download/bootstrap.sh \
  | sudo sh -s -- --yes

# pin a release / choose a prefix
curl -fsSL .../bootstrap.sh | sudo sh -s -- --version v0.3.0 --prefix /usr/local

# from a local checkout (no network); add --dry-run to just print the plan
sh packaging/bootstrap.sh --source --prefix /usr/local

# offline / inspect-before-run, then execute in two steps
sh packaging/bootstrap.sh --file invfs-v0.3.0-x86_64.tar.zst --download-only
sh packaging/bootstrap.sh --file invfs-v0.3.0-x86_64.tar.zst --run
```

The release tarball is verified against `SHA256SUMS` **before** it is unpacked
or executed, every installed path is recorded in
`$PREFIX/lib/invfs/installed.manifest`, and `--uninstall` removes exactly those
paths. The runtime deps (`fuse3`, `zstd`, `zlib`) come from your package
manager; the script prints the right command for Debian/Arch/Gentoo/Void/Fedora
via `/etc/os-release` but does not install them. `--list`, `--dry-run`,
`--no-systemd`, `--no-dracut`, `--no-mkinitcpio` and `--no-initramfs-tools` are
also supported. `make release` produces
`dist/invfs-<ver>-<arch>.tar.zst` + `dist/SHA256SUMS`; regression coverage is
`tools/test-bootstrap.sh`.

## Tools

| Tool | Purpose |
|---|---|
| `invf-mkfs <img> [gb] [dev1 [gb]]` | Format a volume (single- or two-device). `INVFS_META_FRAC=N` |
| `invf-fuse [-f] [-o opt] <vol> <mnt>` | FUSE daemon (below) |
| `invf-fuse --probe-uuid <dev>` | Print the volume UUID if `<dev>` is an InvFS volume |
| `invf-import <vol> <dir>` | Bulk tree import |
| `invf-ls <img>` | List live records |
| `invf-cat <img> <name> [out]` | Read a file byte-exact |
| `invf-cp <img> <file> [name]` | Copy one host file in |
| `invf-stat <img>` | Space/zone inspector |
| `invf-stats <img>` | Full statistics (population, per-class ratios, zones) |
| `invf-sweep <img> [--dry-run] [--seal] [--realize]` | Offline sweep / checkpoint accept |
| `invf-fsck [-f] <img>` | Check/repair |
| `invf-verify [--deep] <img>` | Content verification |
| `invf-rollback <img>` | Undo the last swept checkpoint |
| `invf-resize <img> ...` | Offline grow/shrink |
| `invf-migrate-v2 <img>` | v1 → dynamic-extent format |
| `invf-zip list\|get <img> ...` | ZIP container inspection/extraction |
| `meta_probe <img> <name>` | Developer probe (mutates) |
| `meta_probe <img> --heat <name>` | Read-only heat/class dump |

Tools take the second device via `INVFS_DEV1=<dev1>` when the volume is
two-device.

## FUSE daemon

```sh
invf-fuse [-f] [-o opt[,opt]] <volume> <mountpoint>
```

Mount options: `attr_timeout=0,ac_attr_timeout=0` (default),
`raw_watermark=<pct>` (background-sweep kick), `arc_limit=<MB>` (decoded-unit
cache, default 256), `dec_mem_limit=<MB>`, `dev1=<dev>` (or `INVFS_DEV1`).

Background sweep: opt in with `INVFS_SWEEP_INTERVAL=<sec>`; or trigger manually
with `kill -USR1 $(pidof invf-fuse)` / `setfattr -n user.invfs.sweep -v 1 <mnt>`.

Control xattrs on the mount root:
```sh
getfattr --only-values -n user.invfs /         # RAM summary
getfattr --only-values -n user.invfs.stats /   # hot counters + zone usage
```

## Booting a Gentoo rootfs (verified path)

The verified path is a **direct kernel + initramfs** boot (OVMF+UKI/GRUB is
known-blocked in the current firmware, see `INCIDENTS.md`). The initramfs
mounts the volume, waits for `InvariantFS mounted`, `rbind`s `/proc /sys /dev`,
loads the virtio-net module chain, and `chroot`s into the guest (`switch_root`
refuses a FUSE root).

1. **Format + populate**
   ```sh
   truncate -s 15G root.img
   INVFS_META_FRAC=16 bin/invf-mkfs root.img 15
   bin/invf-import root.img /path/to/stage3-root
   ```
2. **Provision the guest** (FUSE-portable): `tools/configure-guest.sh <mnt>` —
   sets `root` empty password, ttyS0 getty, sshd+dhcpcd runlevels, ssh host keys,
   binhost stub.
3. **Initramfs**: `tools/mkinitramfs.sh` installs `tools/initramfs-init.sh` as
   `/init`. Device selection is by **content**: it probes each block device with
   `invf-fuse --probe-uuid`; optional kernel cmdline `invfs.raw_uuid=` /
   `invfs.dev1_uuid=`.
4. **Run**
   ```sh
   qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 4G \
     -kernel vmlinuz -initrd initramfs.img -append console=ttyS0,115200 \
     -drive id=vol,file=root.img,format=raw,if=ide \
     -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
     -display none -serial stdio -no-reboot
   ```
   Expect `InvariantFS mounted: N files` → OpenRC runlevel 3 → serial login →
   `ssh -p 2222 root@localhost`.

Full guide and pitfalls: `docs/GENTOO-INSTALL.md`.

## Testing

```sh
make test     # 4722 unit checks (core + codecs + recipes + CLI)
make e2e      # serialized end-to-end FUSE suites (flock-protected)
make flakey   # chaos/soak (torn sweep, error storm, compact-flip)
bash tools/test-bootstrap.sh   # host bootstrap: release+sha256+manifest+uninstall
```

Mapper-era suites worth knowing: `tools/test-meta-extent-walk.sh`,
`tools/test-stats-mapper.sh`, `tools/test-sweep-mapper.sh`,
`tools/test-heat-mapper.sh`, `tools/test-fixture-bigvol.sh` (30k-object
multi-extent fixture), `tools/test-mapper-crash.sh` (kill-mid-import/sweep,
rollback, realize). Run any suite through `tools/run-e2e.sh` (it serializes via
`/tmp/invfs-e2e.lock`); use `--bg` + `--wait` when the lock is busy.

## Debug env vars

- `INVFS_DEBUG=1` — general trace in several paths.
- `INVFS_DEBUG_META_EXTENTS` (compile-time) — mapper/MET0/flush trace.
- `INVFS_AUTO_RECOVER=0` — disable DIRTY→CLEAN auto-recovery.
- `INVFS_META_FRAC`, `INVFS_DEV1`, `INVFS_TOOLS`, `INVFS_REQUIRE_HELPER_PATH`,
  `INVFS_SWEEP_INTERVAL`.

## Known limitations

- **No power-loss durability guarantee.** Crash recovery is good for normal
  shutdown/SIGKILL; arbitrary power loss is not covered (`make flakey` is a soak,
  not a contract).
- **Format is still evolving** (v0.3.x mapper work landed recently); no frozen
  on-disk compatibility promise yet.
- **Owner/batch paths were just repaired** (WP52); a fresh durability suite
  (`test-mapper-crash`) currently reports a full-sweep batch-commit failure and
  an `--realize` orphan leak being investigated.
- **Heat accounting** persists only at a sweep run (by default the FUSE
  background sweep is off), so read heat from short guest sessions is not
  accumulated.
- Transcode helpers (`cjxl`/`djxl`, `mac`+`ffmpeg`) absent ⇒ file stays RAW;
  helper lookup is restricted by default (root) — see `docs/SECURITY.md`.
- Bit-rot recovery only via explicit `invf-sweep --seal` parities.
- xattr cap 4096 B/inode; no `security.*`/`trusted.*`; no NFSv4 ACLs.
- OVMF/UKI/GRUB boot path blocked in current firmware; direct kernel+initramfs is
  the supported boot.

## Layout

```
src/         engine + CLI (C11): core/ codecs/ recipes/ cli/ legacy/
tools/       build scripts, guest config, test harnesses, import/sweep tools
docs/        Gentoo install guide, SECURITY.md
impl_docs/   architecture, FILEMAP, AUDIT, FUNCTIONS/TYPES (ctags), WP*.md
packaging/   install.sh, distro packaging, systemd units, man pages
Doxyfile     doxygen config for `make docs`
```

## License

GPL-2.0-only. See [LICENSE](LICENSE).
