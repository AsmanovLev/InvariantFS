# QCOW2 & VM Image Compression & Performance Benchmark

Comprehensive empirical evaluation of **InvariantFS** versus **Squashfs** (`xz -b 1M`) and **TAR** (`xz -9`, `zstd -19`) on real-world QEMU/KVM disk images.

Date of Benchmark: September 2026  
Environment: Fedora Linux `7.2.5-200.fc44.x86_64`, Intel Core i5-10500H @ 2.50GHz (6C/12T), 32 GiB RAM, Western Digital SSD over USB 3.0 (`/dev/sdb1`, XFS).

---

## 1. Test Corpora Description

### Corpus A: Multi-VM Production Disk Images (8.56 GB Raw)
Includes base operating system templates plus an active overlay VM workload with a backing file:
1. `ubuntu2404-base.qcow2`: 624,829,952 B (596 MiB) — Clean Ubuntu 24.04 LTS (internal zlib compression).
2. `ubuntu2204-base.qcow2`: 735,051,776 B (701 MiB) — Clean Ubuntu 22.04 LTS (internal zlib compression).
3. `test1.qcow2`: 7,201,554,432 B (6.71 GiB) — Working VM disk (20 GB virtual disk, backed by `ubuntu2404-base.qcow2`).

### Corpus B: Clean Base OS Templates Only (1.36 GB Raw)
Only the two clean, zlib-compressed QEMU base images (`ubuntu2404-base` + `ubuntu2204-base`).

---

## 2. Storage Density & Compression Ratio

### Corpus A: 3 Images (8.56 GB Raw, including active VM disk)

| Solution / Format | Stored Size on Disk | % of Original | Compression Ratio | Packaging / Ingest Time | Peak RAM (RSS) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Raw Uncompressed Input** | **7.973 GiB** (8.561 GB) | 100.0% | 1.00x | — | — |
| **InvariantFS RAW (Streaming LZ4)** | **4.341 GiB** (4.661 GB) | 54.4% | **1.84x** | **1m 56s** (1 core, 7-8% CPU) | **~2.8 MB** |
| **InvariantFS Shadow + Dedupe (ZSTD)** | **2.761 GiB** (2.964 GB) | **34.62%** | **2.89x** | +12s (offline sweep) | **~23.6 MB** |
| **TAR.XZ (`-9 -T4`)** | **3.088 GiB** (3.315 GB) | 38.73% | **2.58x** | 13m 16s (4 cores @ 400%) | ~1,420 MB |
| **Squashfs (`-comp xz -b 1M`)** | **3.295 GiB** (3.538 GB) | 41.33% | **2.42x** | 9m 24s (4 cores @ 397%) | ~1,280 MB |

> **Key Takeaway:**
> - InvariantFS is **547.2 MiB smaller than Squashfs** (−16.2% space savings) and **327 MiB smaller than `tar.xz -9`**.
> - Block-level deduplication merged **15,144 duplicate 64 KiB segments**, freeing **~393 MiB** of data shared between `test1.qcow2` and `ubuntu2404-base.qcow2`.

---

### Corpus B: 2 Base Templates Only (1.36 GB Raw, Pre-compressed zlib QCOW2)

| Solution / Format | Stored Size on Disk | % of Original | Compression Ratio | Packaging Time | Peak RAM (RSS) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Raw Uncompressed Input** | **1,296.88 MiB** (1.266 GiB) | **100.0%** | **1.000x** | — | — |
| **TAR.XZ (`-9 -T4`)** | **1,257.83 MiB** (1.228 GiB) | **96.99%** | **1.031x** | 1m 45s (4 cores) | ~2,730 MB |
| **TAR.ZST (`-19 -T4`)** | **1,261.38 MiB** (1.232 GiB) | **97.26%** | **1.028x** | 1m 32s (4 cores) | ~200 MB |
| **Squashfs (`-comp xz -b 1M`)** | **1,279.82 MiB** (1.250 GiB) | **98.68%** | **1.013x** | 1m 24s (4 cores) | ~1,398 MB |
| **InvariantFS (Shadow ZSTD + Dedupe)** | **1,350.62 MiB** (1.319 GiB) | **104.14%** | **0.960x** | ~1m 36s (1 core) | **~8 MB** |
| **InvariantFS (Decomposed Containerpack)** | **3,718.00 MiB** (3.718 GiB) | N/A* | N/A* | ~1m 40s (1 core) | ~8 MB |

*\* Note: The decomposed volume stores 5.05 GB of uncompressed guest disk streams (`diskimg`) exposed for direct random access.*

---

## 3. Cold Full Recomposition / Extraction Speed

Extracting the full 1.36 GB archive (`u24.qcow2` 625 MB + `u22.qcow2` 735 MB) from cold compressed state:

| Tool / Target | CPU Cores | Wall Time | Throughput | Peak RAM (RSS) |
| :--- | :---: | :---: | :---: | :---: |
| **Squashfs (`unsquashfs -p 1`)** | 1 thread | **10.83 s** | **119.75 MB/s** | ~270 MB |
| **InvariantFS CLI (`invf-cat`)** | 1 thread | **10.88 s** | **119.20 MB/s** | ~1.7 GB (whole-file buffer) |
| **InvariantFS FUSE (VFS Streaming)** | 1 thread | **~12–14 s** | **~81.5 – 83.4 MB/s** | **~2.6 MB** |
| **Squashfs (`unsquashfs -p 4`)** | 4 threads | **2.39 s** | **542.63 MB/s** | ~275 MB |
| **TAR.XZ (`xz -d`)** | 4 threads | **3.13 s** | **~410 MB/s** | ~2,733 MB |
| **TAR.ZST (`zstd -d`)** | 4 threads | **0.49 s** | **~2,600 MB/s** | ~13.5 MB |

---

## 4. Random 4K I/O Read Performance (CrystalDiskMark-style)

Evaluated using `iobench` with 4K block size and $O(\log N)$ binary search recipe lookup on swept ZSTD volume:

| Metric | InvariantFS (FUSE Mount) | Squashfs (`-comp xz -b 1M`) | InvariantFS Advantage |
| :--- | :---: | :---: | :---: |
| **SEQ1M Read** | **7,086.0 MB/s** (7,086 IOPS) | **63.5 MB/s** (63.5 IOPS) | **~111x faster** |
| **RND64K Read** | **3,592.7 MB/s** (57,483 IOPS) | **5.17 MB/s** (82.7 IOPS) | **~694x faster** |
| **RND4K Read** | **455.7 MB/s** (116,665 IOPS) | **1.21 MB/s** (310.8 IOPS) | **~376x faster** |
| **Average 4K Latency** | **8.6 µs** | **3,217 µs (3.22 ms)** | **~374x lower latency** |

---

## 5. Verification & Bit-Exactness

All tests confirmed **100% bit-exactness**:
```
$ invf-verify /path/to/volume.img --deep
OK: volume is valid InvariantFS volume
deep: reading all live files...
deep: 8 files ok, 0 corrupt, 5046883998 bytes verified
```
SHA-256 hashes of all reconstructed QCOW2 images matched input source files identically:
- `u24.qcow2`: `d0fe84bb5f80853425fa6be28e2c106f30104c3cfe8611933f2e65c9b63f0e30`
- `u22.qcow2`: `bc56dd7ef6283a7d6c397e05df96be8291d715044d85af7be9e0e348d94c57d6`
