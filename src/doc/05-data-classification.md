# Data Classification — Magic Sniffer

> **⚠ ФОРМАТ В АКТИВНОЙ РАЗРАБОТКЕ.** Документ описывает текущее
> состояние Meta-v3, а не стабильную спецификацию; совместимость томов
> между сборками не гарантируется. Подробности: `02-on-disk-format.md`,
> `impl_docs/old_docs/AUDIT.md`, `INCIDENTS.md`.

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
| ZIP | `PK\x03\x04` | Контейнер verbatim: оригинал хранится целиком, члены — окна `name!member` (извлечение на лету); пережатие членов не делается (deflate необратим, 03-ast-recipe.md) |
| GZ | `\x1f\x8b` | GZR: deflate-реплика — перебор (level × memLevel) до бит-совпадения потока, иначе guard-оригинал (03-ast-recipe.md) |
| JPEG | `\xff\xd8\xff` | JXL lossless recompression |
| PNG | `\x89PNG` | JXL + delta (or store raw) |
| AVIF | `....ftypavif` | (не реализовано — сниффа нет, generic-путь) План: JXL recompression (if gain) / store raw |
| HEIC | `....ftypheic` | (не реализовано — сниффа нет, generic-путь) План: JXL recompression (if gain) / store raw |
| WebP | `RIFF....WEBP` | (не реализовано — сниффа нет, generic-путь) План: JXL recompression (if gain) / store raw |
| MP4/M4A | `ftyp` | **Контейнер (не реализовано, future work).** План: извлечь обложки (→JXL), субтитры SRT/ASS (→текст), главы (→текст); видеопоток → store raw |
| MKV | `\x1aE\xdf\xa3` | **Контейнер (не реализовано, future work).** То же: субтитры/обложки наружу, видеопоток → store raw |
| PDF | `%PDF` | Check for already-compressed streams |
| EXE (PE) | `MZ` | WP14a/WP14b: бинарный батчинг по семейству (x86 → BCJ-префильтр, algo=ZSTD_BCJ); встроенные JPEG/PNG ≥ 16 КиБ вырезаются (EXER, algo=15) |
| ELF | `\x7fELF` | То же: семейство по e_machine, батчинг, EXER-вырезание |
| WAV | `RIFF....WAVE` | Check PCM vs compressed |
| BZ2 | `BZ` | Store raw |
| XZ | `\xfd7zXZ` | Store raw |
| ZST | `\x28\xb5\x2f\xfd` | Store raw |

## Archive Detection (Nested AST)

**(Исторический контекст — первоначальный дизайн, не реализован.)**
План был «разжать члена → снифф → пережать»; shipped-поведение другое:
ZIP хранится verbatim с окнами членов, GZ — deflate-реплика GZR,
образы ФС/архивы — внешние контейнерные пакеты WP16. Эскизы ниже
оставлены как обоснование, почему от пережатия членов отказались
(deflate-поток необратим из несжатых данных — см. 03-ast-recipe.md):

### ZIP Detection (первоначальный дизайн)

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

### GZ Detection (первоначальный дизайн; shipped — GZR deflate-реплика)

```
GZ file → read header (magic + compression method + flags + mtime + extra)
  → check compression level from header heuristics
    - level 1-3 (fast) → decompress, sniff contents, recompress
    - level 7-9 (max) → store raw
  → reconstruct header + compressed data on read
```

## Compression Level Detection

**(Исторический контекст.)** Эвристика «уровень по заголовку» не
реализована; вместо неё GZR определяет параметры брутфорс-репликацией
(level 1-9 × memLevel 7-9 до бит-совпадения потока). Таблица —
первоначальный дизайн:

| Format | Low Compression (recompress) | High Compression (store raw) |
|--------|------------------------------|------------------------------|
| ZIP | Method 0 (store), Deflate level 1-3 | Deflate level 7-9 |
| GZ | Level 1-3 (fast) | Level 7-9 (best) |
| ZSTD | Level 1-3 | Level 19-22 |
| LZ4 | Level 0-3 (fast) | Level 9-16 (high) |

## Парсинг медиаконтейнеров (MP4/MKV)

**(Не реализовано — future work.)** Парсера MP4/MKV в коде нет; shipped
контейнерные пакеты WP16 покрывают образы ФС (rawdisk/ext4/fat/xfs/ntfs/
vdi/qcow2) и 7z. Ниже — план, для которого естественная посадка теперь
именно контейнерный пакет (манифест `type = container`, члены →
`name!mbrNNNN`):

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

**(Исторический контекст — не реализовано.)** Детектора Go в коде нет;
shipped-ответ WP14a — сортировка бинарного батча по семейству
(e_machine/PE/Mach-O), BCJ-префильтр применяется к x86-членам, а не
«всем, кроме Go». Обоснование (B9) остаётся в силе и стоит за этой
семейной группировкой:

Бенчмарк B9 показал: BCJ2 **ухудшает** сжатие Go-бинарников (32.77% vs 29.61% для ZSTD -19). Go-компилятор генерирует код без классических x86 branch/call паттернов.

**Признаки Go-бинарника:**

| PE | ELF |
|----|-----|
| Секция `.gopclntab` | Секция `.gopclntab` |
| Секция `.go.buildinfo` | Секция `.go.buildid` |
| Секция `.noptrdata` | Секция `.noptrdata` |
| Секция `.data.rel.ro` + `go:buildid` строка | `go.buildid` в `.note.go.buildid` |

**Если обнаружен Go → без BCJ-префильтра.** Для C/C++ (MSVC, GCC, clang) → BCJ + ZSTD (shipped: батч algo=ZSTD_BCJ).

## Unknown Data

Эвристика энтропии из первоначального дизайна не реализована (код её не
содержит). Как есть: неопознанные данные → generic ZSTD-19; sweep мерит
реальный выигрыш, и при выигрыше < `INVFS_MIN_GAIN_PCT` (0.5%) файл
получает класс UNCOMPRESSIBLE и больше не пересматривается без причины
(см. 06-sweep-worker.md, таблица классов). Ниже — первоначальный эскиз
(исторический контекст):

```
entropy = sum(-p(x) * log2(p(x)) for x in bytes)
if entropy > 7.5:  // near-maximum entropy
  store raw (likely encrypted or already compressed)
else:
  ZSTD -19
```
