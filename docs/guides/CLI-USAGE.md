# InvariantFS CLI Usage Guide

This guide covers the primary command-line tools for formatting, inspecting, copying, verifying, and maintaining InvariantFS volumes.

---

## 1. Creating a Volume: `invf-mkfs`

Creates a new InvariantFS volume formatted with Meta-v3 by default:

```bash
# Create a 32 GiB volume
bin/invf-mkfs volume.img 32

# Create with custom metadata zone fraction (e.g., 1/16 for rootfs with many small files)
INVFS_META_FRAC=16 bin/invf-mkfs rootfs.img 30

# Multi-device setup (dev0 fast SSD, dev1 large HDD)
bin/invf-mkfs /dev/nvme0n1p3 100 /dev/sda1 1000
```

---

## 2. Copying Content: `invf-cp`

Imports files directly into an unmounted InvariantFS volume:

```bash
# Copy a local file into the root of the volume
bin/invf-cp volume.img local_file.tar.gz /archive/file.tar.gz
```

---

## 3. Directory Listing: `invf-ls`

Recursively lists all files and directories in the volume:

```bash
bin/invf-ls volume.img
```

Example output:
```
files in volume.img:
        33 bytes  inode 2  hello.txt
     65536 bytes  inode 3  rand.bin
     70001 bytes  inode 4  repeat.txt
3 file(s)
```

---

## 4. Reading Content: `invf-cat`

Streams the bit-exact content of a stored file to stdout:

```bash
bin/invf-cat volume.img /hello.txt
bin/invf-cat volume.img /rand.bin > restored_rand.bin
```

---

## 5. Offline Compression & Sweeping: `invf-sweep`

Drains data from the RAW landing zone into the Shadow zone, applies compression codecs (ZSTD, PPMd8), and deduplicates identical data blocks:

```bash
# Offline sweep
bin/invf-sweep volume.img

# Persistent combined stdout/stderr log (appended between run headers)
bin/invf-sweep volume.img --log /var/log/invfs/sweep.log
INVFS_SWEEP_LOG=/var/log/invfs/sweep.log bin/invf-sweep volume.img

# Force/disable ANSI colors (default: auto; NO_COLOR is honored)
bin/invf-sweep volume.img --color always
bin/invf-sweep volume.img --color never

# Dry-run mode (display what would be swept without modifying disk)
bin/invf-sweep volume.img --dry-run
```

Normal sweeps report numbered stages (`prepare`, `collect`, `transform`, `heat`, optional `tier`, `dedupe`, `batches`, `finalize`, optional `seal`). Countable stages show current/total, percentage, elapsed time, and an ETA when enough observations exist. Interactive terminals update one ANSI-colored status line in place; redirected output and persistent logs use clean newline-delimited lines. Dedupe progress separates `cross-file` and `intra-file` merges. Use `INVFS_SWEEP_PROGRESS_MS=<100..60000>` to change console/log update cadence.

---

## 6. Verification: `invf-verify`

Verifies structural integrity and bit-exact data recipes:

```bash
# Quick header check
bin/invf-verify volume.img

# Deep read verification (hashes every file and validates all recipes)
bin/invf-verify --deep volume.img
```

---

## 7. Filesystem Check & Repair: `invf-fsck`

Checks the B+ tree base, delta log, and page integrity:

```bash
# Check volume status
bin/invf-fsck volume.img
```

## 8. OS Integration: `mkfs.invfs` / `fsck.invfs` / `mount.invfs`

The fstype name is `invfs`. The three `/sbin` front-ends
(`tools/sbin/`, installed by `packaging/install.sh`) are what
`mkfs -t`, `fsck -A`, fstab and `mount -t invfs` dispatch on:

```bash
mkfs -t invfs /dev/sdb1 64          # 64 GB single-device volume
fsck.invfs -a /dev/sdb1             # preen: check only, never repairs
fsck.invfs -y /dev/sdb1             # repair via invf-fsck --fix
mount -t invfs /dev/sdb1 /data      # via mount.invfs -> invf-fuse
```

Rules: only `-y` repairs (and even it never passes
`--discard-reachable`); `-a`/`-p`/`-n` and the bare default only check.
Exit codes follow `fsck(8)`: 0 clean, 1 corrected (REPAIRED),
4 uncorrected (DAMAGED), 8 operational error, 16 usage.
`-L`/`-U` are refused: format v0 has a generated UUID and no label field.
`blkid` does not know the superblock magic yet, so fstab `UUID=` lookups
do not resolve -- use device paths (the initramfs identifies volumes with
`invf-fuse --probe-uuid`).
