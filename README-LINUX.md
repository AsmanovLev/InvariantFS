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
    src-extracted/VFS/doc/   original design docs (17 files; aspirational
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
| `invf-sweep <img> [--dry-run]` | offline sweep driver |
| `invf-stats <img>` | full statistics walk incl per-zone compression ratios |
| `meta_probe <img> <name>` | developer probe (**mutates**: applies a test setattr) |

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

- Write path note: writes currently buffer per-handle and replace the
  file at flush/close; fsync commits pending data. Ranged/incremental
  segment writes (WP4b) are the active work item.

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

- Whole-file write buffering per open handle (WP4b ranged writes pending);
  fsync/close durability exists, per-op barriers/group-commit do not (WP3).
- No mmap support: gpg keyrings assert; stubs ship via
  `tools/configure-guest.sh` (fake-gpg-verify, getuto) until WP4a.
- External transcoders (JXL/APE/MP3) are Linux stubs; native codecs
  (LZ4/ZSTD/gzip-replica/PNG-replica/TAR/ZIP/FLAC) fully work (WP5).
- ~52 l2p_miss casualties from crash-killed merges remain on the demo
  volume; fsck reports them, data unrecoverable by design (no RAW backup
  after sweep).
- OpenRC shutdown stalls at its kill-all phase (guest-side quirk); the
  filesystem itself closes CLEAN when the daemon receives SIGTERM.
- Hot counters (`user.invfs.stats`) count unique names; `invf-stats`
  counts live record versions -- numbers converge after compaction.

## Workstreams

WP3 durability barriers/group-commit · WP4a mmap · WP4b ranged writes ·
WP4d recipe cache (drop per-read re-verification) · WP5 codec neutrality +
version pinning (see impl_docs/WP5-codec-seekability.md) · WP6 block
refcounts for hardlinks · compaction (online or fsck-time) · regression
suite.
