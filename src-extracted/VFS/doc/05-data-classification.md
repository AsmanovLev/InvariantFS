# Data Classification — Magic Sniffer

The Magic Sniffer identifies file types by examining magic bytes and structure. It determines:

1. Whether the file is a **container** (FLAC, MP3, ZIP, TAR) → recurse into children
2. Whether the data is **already compressed** (Opus, MP4, JPG) → store raw
3. The **semantic components** (audio, text, image) → route to optimal algorithm

## Magic Byte Detection

| Type | Magic Bytes | Action |
|------|-------------|--------|
| FLAC | `fLaC` | Container: parse METADATA_BLOCK_*, extract PCM, cover, tags |
| MP3 | `ID3` / `\xff\xfb` / `\xff\xf3` | **packMP3 → PMP** (bit-exact, MPEG-1 Layer III; −9…−12%). MPEG-2/2.5 отвергается → ZSTD-19. Разбор ID3v2 на компоненты (APIC→JXL, теги→text) — будущий этап, сейчас файл жмётся целиком |
| Opus | `OpusHead` (within Ogg) | Store raw (already lossy-compressed) |
| ZIP | `PK\x03\x04` | Container: check compression method per entry, recurse |
| GZ | `\x1f\x8b` | Check compression level, recompress if low |
| JPEG | `\xff\xd8\xff` | JXL lossless recompression |
| PNG | `\x89PNG` | JXL + delta (or store raw) |
| AVIF | `....ftypavif` | JXL recompression (if gain) / store raw |
| HEIC | `....ftypheic` | JXL recompression (if gain) / store raw |
| WebP | `RIFF....WEBP` | JXL recompression (if gain) / store raw |
| MP4/M4A | `ftyp` | **Контейнер!** Извлечь обложки (→JXL), субтитры SRT/ASS (→ZSTD-19 + dict (горячий путь)), главы (→text). Видеопоток → store raw |
| MKV | `\x1aE\xdf\xa3` | **Контейнер!** То же: субтитры → ZSTD-19, обложки → JXL, видеопоток → store raw |
| PDF | `%PDF` | Check for already-compressed streams |
| EXE (PE) | `MZ` | Проверить компилятор: Go → ZSTD -19 без BCJ2; C/C++ → BCJ2 + ZSTD -19 |
| ELF | `\x7fELF` | Проверить компилятор: Go → ZSTD -19; C/C++ → BCJ2 + ZSTD -19 |
| WAV | `RIFF....WAVE` | Check PCM vs compressed |
| BZ2 | `BZ` | Store raw |
| XZ | `\xfd7zXZ` | Store raw |
| ZST | `\x28\xb5\x2f\xfd` | Store raw |

## Archive Detection (Nested AST)

For archives with low compression, the Sniffer decomposes the container:

### ZIP Detection

```
ZIP file → read End of Central Directory → enumerate local file headers
  → for each entry:
    - compression method = 0 (store) → decompress entry, sniff contents
    - compression method = 8 (deflate) → check deflate level
      - level 1-3 → decompress, sniff, recompress with ZSTD -19
      - level 7-9 → store raw (already optimal)
    - sniff entry contents → route to appropriate zone/algo
```

The AST recipe is built as a tree mirroring the ZIP structure, including local file headers, central directory, and end-of-central-directory record for byte-perfect reconstruction.

### GZ Detection

```
GZ file → read header (magic + compression method + flags + mtime + extra)
  → check compression level from header heuristics
    - level 1-3 (fast) → decompress, sniff contents, recompress
    - level 7-9 (max) → store raw
  → reconstruct header + compressed data on read
```

## Compression Level Detection

For archives, detecting the compression level helps decide whether to recompress:

| Format | Low Compression (recompress) | High Compression (store raw) |
|--------|------------------------------|------------------------------|
| ZIP | Method 0 (store), Deflate level 1-3 | Deflate level 7-9 |
| GZ | Level 1-3 (fast) | Level 7-9 (best) |
| ZSTD | Level 1-3 | Level 19-22 |
| LZ4 | Level 0-3 (fast) | Level 9-16 (high) |

## Парсинг медиаконтейнеров (MP4/MKV)

MP4/MKV — не "просто store raw". Это контейнеры, внутри которых есть ценные семантические компоненты:

```
MP4:
  ├── Video stream (H.264/HEVC/AV1) → store raw (уже сжат)
  ├── Audio stream (AAC/Opus)        → store raw (уже сжат)
  ├── Cover art (covr atom)          → JXL lossless
  ├── Subtitles (SRT/ASS/tx3g)       → ZSTD-19 (горячий путь)
  ├── Chapters (XML)                 → ZSTD-19 (горячий путь)
  └── Metadata (title, artist)       → ZSTD-19 (горячий путь)

MKV:
  ├── Video stream                   → store raw
  ├── Audio stream                   → store raw
  ├── Attachments (JPEG/PNG)         → JXL
  ├── Subtitles (SRT/ASS/SSA)        → ZSTD-19 (горячий путь)
  └── Tags (XML)                     → ZSTD-19 (горячий путь)
```

Sweep-воркер извлекает субтитры и обложки (семантическая природа ФС), оставляя видеопоток нетронутым. AST-рецепт сохраняет структуру контейнера для bit-perfect реконструкции.

## Детекция Go-бинарников (BCJ2 не применять!)

Бенчмарк B9 показал: BCJ2 **ухудшает** сжатие Go-бинарников (32.77% vs 29.61% для ZSTD -19). Go-компилятор генерирует код без классических x86 branch/call паттернов.

**Признаки Go-бинарника:**

| PE | ELF |
|----|-----|
| Секция `.gopclntab` | Секция `.gopclntab` |
| Секция `.go.buildinfo` | Секция `.go.buildid` |
| Секция `.noptrdata` | Секция `.noptrdata` |
| Секция `.data.rel.ro` + `go:buildid` строка | `go.buildid` в `.note.go.buildid` |

**Если обнаружен Go → ZSTD -19 без BCJ2.** Для C/C++ (MSVC, GCC, clang) → BCJ2 + ZSTD -19.

## Unknown Data

If the Sniffer cannot identify the data:
- Bytes pass entropy test → binary → **ZSTD -19**
- Bytes fail entropy test (high entropy / random) → **store raw**

## Entropy Heuristic

```
entropy = sum(-p(x) * log2(p(x)) for x in bytes)
if entropy > 7.5:  // near-maximum entropy
  store raw (likely encrypted or already compressed)
else:
  ZSTD -19
```
