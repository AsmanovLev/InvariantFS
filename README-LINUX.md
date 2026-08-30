# InvariantFS on Linux

Semantic content-addressed filesystem with a bit-exactness invariant:
every compression is verified by decompress-and-compare before it is
trusted; containers (ZIP/TAR) are stored byte-original with members
exposed as on-demand windows; transcodes happen only where bit-exactness
is proven per file.

Status: **experimental, bootable**. A Gentoo/OpenRC system runs with its
entire root on an InvariantFS volume (see `tools/mkdisk.sh`, `vm/`).

## Layout

    src-extracted/VFS/src/   engine + FUSE daemon + CLI tools (C11)
    src-extracted/VFS/doc/   original design docs (18 files; aspirational
                             in parts -- see impl_docs/AUDIT.md for the
                             code-vs-docs reconciliation)
    impl_docs/               generated navigation (functions/types/maps)
                             + AUDIT.md (doc-vs-code audit, findings PB1..)
    tools/                   build scripts, guest config, disk/initramfs
                             builders, invf-import, invf-sweep, invf-stats,
                             meta_probe
    vm/                      initramfs tree, disk builder inputs
    var/tmp/                 working volumes (not in git)

## Build

    make                # all tools -> bin/
    make bin/invf-fuse  # single target

Flags mirror `tools/build_native.sh`: `-std=gnu11 -O2`, bundled
zstd/lz4/miniz/blake3/flacx, libfuse3 for the daemon.

## Tools

| tool | purpose |
|---|---|
| `invf-mkfs <img> [gb]` | format; env `INVFS_META_FRAC=N` scales metadata zone (default 64 → N=1/N of volume; 16 recommended for churny workloads) |
| `invf-fsck [-f] <img>` | check; `-f` repairs orphans/bitmap/journal. Auto-recovery: a DIRTY volume with an anomaly-free scan self-heals to CLEAN+rw at mount (`INVFS_AUTO_RECOVER=0` disables) |
| `invf-ls / cat / cp / stat` | read-side CLI (position-kill aware) |
| `invf-import <vol> <dir>` | direct engine-level tree import (~50k files in seconds). `INVFS_IMPORT_PREFIX=a/b` imports under an existing dir; uid/gid default to 0 (`INVFS_IMPORT_KEEP_OWNER=1` keeps) |
| `invf-sweep <img> [--dry-run] [--seal\|--unseal] [--realize]` | offline sweep driver; `--seal` re-seals shadow parity after the run (WP20), `--realize` accepts the previous sweep's checkpoint (WP21) |
| `invf-stats <img>` | full statistics walk incl per-zone compression ratios |
| `invf-resize <img> ...` | WP18 offline volume grow/shrink (RSZ0 roll-forward, idempotent apply at next open) |
| `invf-rollback <img>` | WP21 undo the last sweep from its CKP0 checkpoint (bit-exact to pre-sweep state) |
| `meta_probe <img> <name>` | developer probe (**mutates**: applies a test setattr) |
| `meta_probe <img> --heat <name>` | WP19 read-only dump: storage class, per-segment AST (zone/algo), per-entry heat counters (rheat/wheat from the L2P pad) |

## FUSE daemon

    invf-fuse [-f] [-o opt[,opt]] <volume> <mountpoint>

- Default mount options include `attr_timeout=0,ac_attr_timeout=0`
  (kernel attr caching served stale sizes across clients) and
  `fsname=invfs[<image>]`.
- SIGTERM/SIGINT exit cleanly: volume closed with CLEAN superblock.
- Background sweep is **opt-in**: `INVFS_SWEEP_INTERVAL=<sec>`.
  Manual sweep: `kill -USR1 $(pidof invf-fuse)` or, inside the mount,
  `setfattr -n user.invfs.sweep -v 1 /` (root only).
- Control namespace on the mount root:

      getfattr --only-values -n user.invfs /        # RAM summary
      getfattr --only-values -n user.invfs.stats /  # hot counters +
                                                    # zone usage (instant)

- Write path: `write()` streams through engine write sessions
  (`vol_write_begin/range/commit`) — per-handle memory is O(1) (one staged
  64K tail segment), complete segments compress+store immediately, ranged
  pwrite RMWs only edge segments, beyond-EOF gaps zero-fill. fsync commits
  + barriers the volume (`vol_sync`). mmap read/write work (writeback
  cache); `gpg` over the mount is a covered acceptance leg.

## Volume format v2 (de-facto spec = invarifs.h + volume.c)

Append-only inode area: `[INOD rec_len ... CRC][DELT tombstone CRC]`
pairs; v2 tombstones kill by record position (`file_size` field),
legacy ones by inode id. Metadata lives in an `INO2` ext block appended
after the AST recipe inside the record: type, mode, uid/gid, mtime/atime,
nlink, symlink target, xattr TLVs. Directory anchors are records named
`path/`. Hard links clone the record under a new name sharing the inode
(no block refcounts yet -- see WP6 caveat in volume.h).

Superblock flag `VOLF_META2`; old readers skip unknown ext via rec_len.
Endianness: host LE (x86_64).

## Bootable VM (Gentoo/OpenRC root on InvariantFS)

    tools/mkdisk.sh <volume.img>     # GPT: ESP(fat32,grub,kernel,initrd) + volume as p2
    qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -nographic \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file=vm/OVMF_VARS.fd \
      -drive file=vm/disk.img,format=raw,if=virtio \
      -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0

Chain: OVMF → GRUB (ESP) → initramfs (busybox + fuse.ko + failover.ko +
virtio_net.ko + invf-fuse) mounts `/dev/vda2` → switch_root into Gentoo.
Guest config (binhost, sshd, serial console, empty-root-password labs):
`tools/configure-guest.sh <mountpoint>`; run it against a mounted volume
before building the disk.

## Current limitations (post-audit, honest)

- Per-op write barriers/group-commit still open (WP3); the durability
  contract is fsync/close (`vol_sync` = journal+bitmap+data past a real
  storage barrier), which the kill -9 crash leg verifies.
- mmap is supported (read of all storage forms; write via FUSE
  writeback cache, WP4a). If a kernel declines FUSE_CAP_WRITEBACK_CACHE,
  shared-writable mmap fails ENODEV rather than faking it.
- Codecs on Linux: JPEG→JXL is fully wired (jxl.codecpack owns the lane,
  WP16e; needs cjxl/djxl installed); PMP/APE/WavPack are probe-gated
  external tools (packMP3 / mac / wavpack resolved via `$INVFS_TOOLS`,
  `/usr/lib/invfs/tools`, PATH — absent tool = codec deferred, file waits
  in RAW). The PNG/FLAC **transcode** bodies are still `#ifdef _WIN32`
  (WP12(c)): on Linux those files take the generic path; reading existing
  FLACR records works when `mac` is installed.
- Bit-rot recovery exists only via an explicit seal (`invf-sweep --seal`,
  WP20: XOR stripes + optional RS layer-2); after a sweep the RAW
  originals are gone, so an unsealed volume has no second copy.
- OpenRC shutdown stalls at its kill-all phase (guest-side quirk); the
  filesystem itself closes CLEAN when the daemon receives SIGTERM.
- Hot counters (`user.invfs.stats`) count unique names; `invf-stats`
  counts live record versions -- numbers converge after compaction.

## Workstreams

WP3 durability barriers/group-commit · WP4d recipe cache (drop per-read
re-verification) · WP5 codec neutrality +
version pinning (see impl_docs/WP5-codec-seekability.md) · WP6 block
refcounts for hardlinks · compaction (online or fsck-time) · regression
suite.
