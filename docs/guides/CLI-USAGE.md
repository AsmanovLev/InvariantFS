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

# Dry-run mode (display what would be swept without modifying disk)
bin/invf-sweep volume.img --dry-run
```

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

# Read-only dry run
bin/invf-fsck -n volume.img
```
