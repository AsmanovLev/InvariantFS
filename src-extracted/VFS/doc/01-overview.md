# InvariantFS — Overview

**InvariantFS** (from "data invariant") is a semantic, content-aware storage system that understands the *meaning* of the data it stores, not just its bytes. It decomposes files into typed components, stores each with the mathematically optimal compression algorithm, and reconstructs them on demand — all transparently to applications.

## Core Philosophy

1. **Semantic decomposition** — Files are not opaque byte streams. A FLAC file is [PCM audio] + [cover art] + [text tags]. Each component is stored independently.
2. **Optimal compression per type** — Audio gets APE, text gets PPMd, JPEG cover art gets JXL lossless recompression. No one-size-fits-all.
3. **Background optimization** — The Sweep worker processes files asynchronously. Writes go to a fast RAW zone; compression happens offline.
4. **Byte-range remapping** — The AST recipe maps any byte range of the original file to a physical block. Reading the file header touches only the text zone — not the audio.
5. **Invariant verification** — Every compression operation is followed by decompression + BLAKE3 hash comparison. Bit-perfect reconstruction is guaranteed.
6. **Nested container handling** — Archives (ZIP, GZ, TAR) are recursively decomposed. Low-compression entries are recompressed; the AST tree mirrors the container hierarchy.

## Key Concepts

| Concept | Description |
|---------|-------------|
| **RAW Zone** | Linear write area. New files land here immediately, uncompressed. |
| **Shadow Space** | Optimized storage for compressed data, split into Text Zone and Binary Zone (type-consolidated). |
| **Metadata Zone** | Superblock, L2P journal, block bitmap, tag index, content index (BLAKE3). |
| **AST Recipe** | A tree structure mapping original byte ranges to (zone, block, offset) tuples. Supports nesting for containers. |
| **Sweep Worker** | Background process that moves data from RAW → Shadow, applying optimal compression per type. On-demand / offline. |
| **L2P Table** | In-memory Logical-to-Physical address mapping, persisted via journal. |
| **Magic Sniffer** | Classifies data by type (FLAC, MP3, ZIP, EXE, JPEG, etc.) using magic bytes and heuristics. |
| **BLAKE3 Dedup** | Content-addressed storage via BLAKE3 hashing. Same block stored once, referenced many times. |
| **Cache Layer** | In-RAM LRU cache (up to 1 GB) for decompressed blocks. |
| **Trimming (TRIM)** | SSD-friendly block deallocation via IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES. |

## Design Goals

- **Transparent** — Applications see normal files. No special APIs needed.
- **Lossless** — Every byte is verified. Reconstruction is bit-perfect.
- **Efficient** — Metadata access is O(1) and near-instant. Audio streaming is sequential.
- **Space-optimal** — Each data type gets the best compression algorithm for its characteristics.
- **Crash-safe** — Journal-based metadata ensures recovery after power loss.
- **Cross-platform** — POSIX + Windows permissions via hybrid metadata model.
- **Developer-friendly** — Semantic dedup for node_modules, containers, CI/CD artifacts.
- **Container-aware** — MP4/MKV/FLAC/MP3 разбираются на компоненты (обложки→JXL, субтитры→PPMd).
- **ZNS-ready** — RAW/Shadow мапятся на аппаратные зоны ZNS SSD (нулевой WA).

## Document Index

| # | Document | Description |
|---|----------|-------------|
| 01 | `01-overview.md` | Philosophy, concepts, design goals |
| 02 | `02-on-disk-format.md` | Superblock, zones, bitmap, indexes |
| 03 | `03-ast-recipe.md` | AST recipe format, flat & nested, read algorithm |
| 04 | `04-compression-matrix.md` | Algorithm selection, benchmark results |
| 05 | `05-data-classification.md` | Magic Sniffer, magic bytes, container detection |
| 06 | `06-sweep-worker.md` | Sweep pipeline, per-type processing, batching |
| 07 | `07-read-write-path.md` | Read/write I/O flow, fast metadata path |
| 08 | `08-crash-recovery.md` | Journal recovery, superblock state machine |
| 09 | `09-windows-port.md` | WinFsp vs raw, paths, tools table |
| 10 | `10-deduplication.md` | BLAKE3 content addressing, semantic dedup, refcounting |
| 11 | `11-security-and-permissions.md` | POSIX + Windows hybrid permissions, xattr |
| 12 | `12-enospc-strategy.md` | Watermarks, throttling, emergency fallback |
| 13 | `13-linux-rootfs.md` | Initramfs, VFS driver, special file types |
| 14 | `14-windows-io-deep.md` | OVERLAPPED I/O, WRITE_THROUGH, IOCP, TRIM |
| 15 | `15-caching.md` | 1 GB LRU cache, dedup-aware |
| 16 | `16-benchmarks.md` | 20 бенчмарков (B1-B20), обоснование алгоритмов |
| 17 | `17-template-zone.md` | Семантическая декомпозиция: эталоны, residuals, audio LZ77 |

## Инструменты (обновление: ivfs-stat)

`ivfs-stat <image> [--files]` — консольный инспектор места (Windows + Linux):

- **Одна отсортированная полоса** (110 ячеек, масштаб ко всему тому):
  `[зелёный dedup][бирюзовый both][синий semantic][серый As-IS] → ░ free`
  — слева дедуп, справа семантика, сырые приклеены к правому концу перед свободными;
  метаданные (битмап/журнал/inode) в полосу не входят
- Refcount дедупа считается только по **живым** inode (last-wins, без stale-записей журнала)
- Стата: used/free по зонам, файлы (last-wins по именам), журнал L2P, inode area, доля semantic/dedup блоков
- `--files`: список файлов с меткой политики `[A]/[S]/[D]/[B]`

Тесты в ОЗУ: `build_linux.sh` собирает весь CLI под Linux (gcc, системный zstd);
`build_linux_ram_test.sh` — полный цикл cp→sweep→verify→stat в `/dev/shm` (tmpfs, RAM).
Windows-сборка: `build.bat` (invf-stat добавлен).
