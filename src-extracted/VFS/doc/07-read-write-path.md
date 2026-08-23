# Read/Write Path

## Write Path

```
write(fd, buf, len, offset):
  1. Allocate blocks in RAW Zone (via Bitmap + free-list cursor)
  2. Write data to RAW Zone (linear append, fast)
  3. Update in-memory L2P: inode → RAW blocks
  4. Update file size in Primary Index
  5. Return written bytes

  Note: No compression on write. The file stays in RAW Zone
  until the Sweep worker processes it.
```

### fsync

```
fsync(fd):
  1. Flush L2P journal to Metadata Zone
  2. Flush Bitmap to Metadata Zone
  3. Issue FlushFileBuffers (or fsync equivalent)
  4. Return

  Journal contains only the delta (pending mappings), not the
  full L2P table. This makes fsync O(pending writes), not O(total files).
```

## Read Path

```
read(fd, buf, len, offset):
  1. Load AST recipe for inode
  
  2. If file is still in RAW Zone (not yet swept):
     └─ Direct read from RAW blocks via L2P
     └─ Return data

  3. If file is in Shadow Space (swept):
     a. Find the AST block containing offset
     b. If block.zone == "raw":
          └─ Direct read from RAW blocks
        Else:
          └─ Read compressed block from Shadow Space
          └─ Decompress with block.algo
          └─ Extract requested byte range
     c. Return data

  4. For nested AST (containers):
     └─ Recurse into children, adjusting offsets
     └─ Reconstruct container framing bytes as needed
     └─ Return reconstructed bytes
```

### Ranged read: два пути, и почему второй нужен кэш

Всё выше описывает чтение, у которого есть промежуточная единица декодирования —
сегмент. У свёрнутого файла её нет: остаётся блоб плюс рецепт-сиблинг, и
единственная операция, которая превращает это в оригинал, — реконструкция
целиком. Отсюда развилка в `vol_read_range`:

| Форма записи | Как отвечаем на окно |
|---|---|
| сегменты LZ4/ZSTD | декодируем сегмент, покрывающий окно (≤64 КБ работы) |
| контейнер (`num_children > 0`) | реконструируем целиком |
| один сегмент == весь файл (FLACR/TARR/GZR/PNGR/PMP/APE/JXL) | реконструируем целиком |

Вторые две формы обслуживаются через ARC-кэш реконструированного содержимого
(doc/15). Без него монтирование, читающее окнами по 64 КБ, выполняло бы полную
реконструкцию на каждое окно: для контейнера это O(N²) на последовательное
чтение, для FLAC — по запуску `MAC.exe` на каждые 64 КБ.

До появления этой ветки FLACR/TARR/GZR/PNGR не попадали ни в сегментную ветку,
ни в контейнерную, а проваливались в raw-ветку с проверкой
`if (hdr != e->length) return -1`, где `hdr` — длина хранимого блоба, а
`e->length` — длина реконструкции; равными они не бывают. Через монтирование
свёрнутый файл не читался вообще, тогда как `invf-cat` отдавал его байт-в-байт,
потому что идёт через `vol_read_file`. Проверяет обе стороны T22 (`tests.ps1`,
инструмент `invf-rangechk`): окна против целого файла на каждом кодеке.

### Metadata-Only Read (Header / Tags)

```
read(fd, buf, 4096, 0):   // Reading file header
  → AST says: bytes 0-4096 → Text Zone, block_id=55, algo=ppmd
  → Read single block from Text Zone
  → PPMd decompress (~8 MB/s → ~0.5 ms for 4 KB)
  → Return bytes

  Audio data is NEVER touched.
```

This is the key performance win: reading file metadata is O(1) and near-instant.

### Sequential Audio Read (Playback)

```
read(fd, buf, 65536, offset):   // Reading audio data
  → AST says: bytes N to N+65536 → Binary Zone, block_id=18, algo=ape
  → Read APE block from Binary Zone
  → APE decompress (frame-seekable)
  → Return PCM data

  APE supports seeking to frame boundaries, enabling
  time-based seeking without decompressing from the start.
```

## mmap() поддержка

СУБД (PostgreSQL), LLM-инференс, numpy используют `mmap`. Ядро при этом само инициирует `readpage()` — VFS-драйвер обязан интегрировать AST-движок с `address_space_operations`:

```c
static const struct address_space_operations invarifs_aops = {
    .readpage = invarifs_readpage,    // AST → блок → декомпрессия → page
    .writepage = invarifs_writepage,  // page → LZ4 → RAW zone
    .dirty_folio = invarifs_dirty_folio,
    .release_folio = invarifs_release_folio,
};

// invarifs_readpage:
//   AST recipe → block_id → декомпрессия → заполнить page
//   page-fault обрабатывается без userspace (всё в ядре)
```

Это критично для корректной работы mmap-приложений.

## Metadata Operations

### stat

```
stat(path):
  → Look up inode in Primary Index
  → Return file_size, permissions, timestamps from index
  → No I/O to Shadow Space needed
```

### readdir

```
readdir(dir):
  → Look up directory inode
  → Read directory listing from Primary Index or dedicated directory blocks
  → Return entries
```

### chmod / chown / utimens

```
chmod(path, mode):
  → Update permissions in Primary Index (in-memory)
  → Log change in L2P journal
  → On fsync: flush journal
```

### Tag Edit (rename artist)

```
rename_metadata(path, "artist", "Beyoncé"):
  → Load current tag block from Text Zone
  → Append delta {artist: "Beyoncé"} to Text Zone
  → Update AST recipe (add delta reference)
  → No re-compression of audio needed
  → On fsync: flush journal

  Cost: ~50 bytes written, ~15 ms total
```
