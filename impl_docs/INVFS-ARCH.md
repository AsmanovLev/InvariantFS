# InvariantFS Architecture — Quick Reference

## Two-Device WP25 Filesystem

InvariantFS is a **multi-tier FUSE filesystem** that spans two physical devices:

| Device | Role |
|--------|------|
| **dev0** (fast) | RAW zone (new writes land here first), tier arena (hot data cache) |
| **dev1** (capacity) | Shadow zone (canonical storage), metadata mirror, RAW mirror copy |

```
Global pba space = dev0_blocks + dev1_blocks
pba < dev0_blocks  → lives on dev0 at (local block = pba)
pba >= dev0_blocks → lives on dev1 at (local block = pba - dev0_blocks)
```

## Three Storage Zones

| Zone | Codec | Purpose |
|------|-------|---------|
| **RAW** | LZ4 | Linear landing area for new writes. Fast, cheap. |
| **Shadow/Text** | PPMd | Optimized text batches |
| **Shadow/Binary** | ZSTD, FLACR, TARR, GZR, PNGR | Optimized binary data |
| **Metadata** | — | Superblock, block bitmap, L2P journal, inode area. Mirrored on both devices. |

Background **sweep worker** moves data from RAW → Shadow under appropriate codec.

## FUSE Mount (`invf-fuse`)

```sh
# Single device (dev1_hint from volume descriptor)
invf-fuse /dev/sda3 /mnt/invfs -o allow_other

# Two devices (explicit dev1)
INVFS_DEV1=/dev/sdb invf-fuse /dev/sda3 /mnt/invfs -o allow_other
```

**Key FUSE options:**

| Option | Description | Default |
|--------|-------------|---------|
| `attr_t=<sec>` | Attribute/entry cache TTL | 1.0s |
| `ac_attr_t=<sec>` | Auto-cache attribute TTL | 1.0s |
| `arc_limit=<MB>` | Decoded-unit cache limit | 256 MB |
| `dec_mem_limit=<MB>` | Decompression memory limit | 512 MB |
| `raw_watermark=<pct>` | Kick early sweep when RAW exceeds % | 0 (disabled) |
| `at_checkpoint[=<seq>]` | Read-only mount at sweep checkpoint | — |

**Control xattrs on mount root:**

```sh
getfattr --only-values -n user.invfs /          # RAM summary
getfattr --only-values -n user.invfs.stats /    # hot counters + zone usage
```

**Sweep control:**

```sh
INVFS_SWEEP_INTERVAL=<sec>   # periodic background sweep
kill -USR1 $(pidof invf-fuse) # manual sweep trigger
```

## "Readonly Filesystem" Triggers

`vol_write_enabled()` returns false (EROFS) when `VOLF_READONLY` is set:

1. **Space latch** — free blocks below `hard_min_blocks`. Auto-releases when space recovers above `hard_min + 2%`.
2. **Operator hold** — `vol_set_readonly(v, 1)`. Never auto-releases.
3. **Time-travel mount** — `vol_open_at` with checkpoint. Sets `VOLF_READONLY | time_travel`.
4. **I/O error latch** — `vol_sync` fails (flush barrier error).
5. **Recovery pending** — previous session didn't close cleanly (`needs_recovery`).
6. **Degraded mount** — dev0 absent in two-device setup. Opens read-only from dev1 mirror.

## Volume Descriptor (Block 0, offset 0x2A0)

```c
typedef struct {
    char     magic[4];          // "DEVT"
    uint32_t dev_count;         // 1 or 2
    uint32_t flags;             // DEVTF_RAW_MIRROR
    uint64_t dev_blocks[2];     // per-device size in blocks
    uint64_t sync_seq;          // mirror sync sequence
    char     dev1_hint[128];    // path hint for device 1
} invfs_devt;
```

`DEVTF_RAW_MIRROR (0x01)` = RAW segments dual-written to both devices.

## Key Source Files

| File | Purpose |
|------|---------|
| `src/cli/fuse_fs.c` | Main FUSE daemon (`invf-fuse`) |
| `src/core/vol_tier.c` | Two-device tiering + RAW mirror |
| `src/core/volume.c` | Core volume engine (open, read, write, alloc) |
| `src/core/volume.h` | Public API |
| `src/core/invarifs.h` | On-disk format (superblock, AST, zones) |
