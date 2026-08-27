# On-Disk Format

## Partition Layout

The physical storage is divided into four zones:

```
┌─────────────────────────────────────────────────┐
│  Superblock (Block 0)                           │  ~4 KB
├─────────────────────────────────────────────────┤
│  Metadata Zone                                  │  ~64 MB
│  ├── L2P Journal (append-only log)              │
│  ├── Block Bitmap (1 bit per block)             │
│  ├── Tag Index (B-tree: artist/album/title)     │
│  └── Primary Index (B-tree: inode → metadata)   │
├─────────────────────────────────────────────────┤
│  RAW Zone                                       │  ~20% of volume
│  (linear write area for incoming data)          │
├─────────────────────────────────────────────────┤
│  Shadow Space                                   │  ~80% of volume
│                                                   │
│  Blocks are type-consolidated across all files:  │
│  ┌─────────────────────────────────────────┐    │
│  │ TextZone: PPMd-compressed tag blocks   │    │
│  │ All text data from all files batched   │    │
│  │ into large blocks for better ratio     │    │
│  └─────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────┐    │
│  │ BinaryZone: WavPack/JXL/ZSTD blocks    │    │
│  │ Audio, images, executables — each      │    │
│  │ stored in type-homogeneous extents     │    │
│  └─────────────────────────────────────────┘    │
├─────────────────────────────────────────────────┤
│  Template Zone                                  │  (часть Shadow)
│  ├── Эталонные сэмплы (audio LZ77)             │
│  ├── Эталонные секции ELF/PE (.rodata и т.д.)  │
│  └── Базовые слои ОС (Docker, rootfs)          │
│  refcount + пинятся в ARC-кэше                  │
└─────────────────────────────────────────────────┘
```

### Суб-секции Shadow Space

Shadow — не однородная зона, а контейнер **суб-секций**. Сейчас их две:

| Суб-секция | Что лежит | Как сжимаются блоки |
|---|---|---|
| **text** | несжатый текст (теги, JSON, исходники, субтитры) | **PPMd** поверх блока |
| **binary** | всё остальное: APE/WavPack/JXL-блобы, ZSTD-сегменты, EXE | по типу (см. 04-compression-matrix.md) |

Ключевой момент про text: **в суб-секции лежит именно несжатый текст**, а
сжатие применяется к **блоку** суб-секции, а не к отдельному файлу. Смысл
ровно в этом: PPMd на 4 KB даёт 38.75%, на батче 80 KB — 8.06% (B7), то есть
выигрыш появляется только от укрупнения. Кладя текст в общую суб-секцию, мы
получаем большой блок «бесплатно», из текста всех файлов сразу.

Суб-секции — это **разделение внутри `shadow_zone_start/blocks`**, а не новые
поля суперблока: место под них уже выделено, границы выводятся внутри зоны.
Поэтому добавление text не ломает формат.

#### Порядок компонентов внутри блока

L2P отвязывает логическое от физического, поэтому sweep волен класть
компоненты в батч в любом порядке — читатель разницы не заметит. Порядок
поэтому свободный параметр, и **он стоит +26.6% ratio** (B29): на блоке 1M
PPMd даёт 2.484× при случайном порядке и 3.146× при сортировке.

Правило: **группировать по типу, внутри группы — по размеру**
(`bytypesize`). Сортировка только по размеру заметно хуже — она разрывает
соседство однотипных файлов.

Два следствия, которые важнее самих процентов:

- **Обход дерева уже даёт почти весь выигрыш.** Порядок по пути (`natural`)
  равен сортировке по типу с точностью до третьего знака: дерево само
  группирует однотипное. Настоящую прибавку даёт только вторичный ключ по
  размеру — то есть sweep не обязан копить и сортировать всё, достаточно не
  перемешивать.
- **Сортировка есть условие читаемости крупных блоков.** PPMd не имеет
  произвольного доступа: чтобы достать компонент, декодер проходит весь блок
  перед ним (4M — 335 мс, B27.3). В отсортированном батче чтение одного тега
  прогревает ARC-кэш для соседних однотипных; в перемешанном — декодируешь
  4 MB ради 4 KB и выбрасываешь.

Размер блока: **4M**, не больше. 8M и 16M дают +1.5% и +1.8% плотности за
×2.9 и ×7.0 латентности декода (PPMd на чистом корпусе: 327 мс → 967 мс →
2206 мс, B29.4/B29.5). Латентность растёт суперлинейно, плотность —
сублинейно, так что 8M платит почти всю цену 16M, не получая его плотности.

**Кодек — PPMd для всей суб-секции.** Раннее правило «выбирать кодек по
составу батча» отменено: оно опиралось на сравнение разных корпусов, причём
загрязнённого (B29.5). На честном контрфактуале — одни и те же байты,
разная группировка — PPMd впереди и на однородном батче (+12.1% над ZSTD-19),
и на смешанном (+12.5%). Смесь стоит всем кодекам примерно одинаково
(−5.8% PPMd, −6.2% ZSTD), так что состав батча кодек не выбирает.

Смесь, однако, **удваивает латентность декода PPMd** (340 → 770 мс на 4M),
не трогая ZSTD и xz. Это ещё один довод за сортировку батча — по латентности,
а не только по ratio.

Разбиение задаётся размером компонента: `size > block` — компонент режется
на блоки; `size < block` — мелкие копятся в один блок, **отсортированные**
перед сжатием.

**Статус: реализовано (WP10, ff84bf8; спека — impl_docs/WP10-textzone-codec-registry.md).**
`INVFS_ZONE_TEXT` и `INVFS_ALGO_PPMD` (`invarifs.h:29,35`) больше не мёртвые
константы. Устройство хранилища:

- **Владелец `\x01tzb`** — один скрытый служебный inode (имена с первым
  байтом 0x01 фильтруются из `vol_list_dir`/readdir). В его AST — по записи
  на батч: `{length=usize, zone=TEXT, algo=PPMD, block_id=batch_seq}`, а его
  L2P отображает `batch_seq → pba` — это и держит батчи живыми для fsck.
  Id владельца неизменен (перезапись через position-kill tombstone), так
  что его L2P-ключи валидны всю жизнь тома.
- **Сегмент батча** — обычный фрейминг `[4B csize][4B crc32c]` поверх
  payload'а `[4B usize LE][2B props][PPMd-поток]` (o=8, 64 MB, CUT_OFF).
  `usize` — размер декодированного батча: член знает лишь свой срез, а
  re-sweep по политике памяти узнаёт размер юнита без декода (pread 12 B).
- **Член (member)** — по AST-записи на срез: `{file_offset, length,
  zone=TEXT, algo=PPMD, block_id=batch_seq, block_offset=смещение в
  декодированном батче}` плюс **дубликат L2P** `(member_id, batch_seq) →
  pba` (тот же приём, что у офлайн-дедупа). Файл крупнее батча режется на
  срезы по соседним батчам.
- **Чтение**: членский L2P-дубль → pba → сегмент → CRC → ARC по
  **тегированному** ключу `pba | 1<<63`, не по inode_id (батч общий, а
  pba переиспользуется аллокатором); промах = PPMd-декод всего батча в
  heap → копия среза `[block_offset, +length)`.
- **Удаление члена НЕ освобождает TEXT-блоки** (anti-PB7 по дизайну): член
  держит только L2P-дубль, блоки принадлежат владельцу. Мусор собирает
  **GC**: mark-and-sweep в конце sweep-прогона (после дедупа, до seal
  новых батчей) — обход живых записей метит `batch_seq`; батчи владельца
  вне множества меток → `arc_invalidate(pba|тег)` → free → перезапись
  записи владельца. Дедуп записи `zone==TEXT` пропускает: их pba
  принадлежат владельцу и не являются кандидатами.

Пределы v1: суммарный объём батчей ≤ 4 GB (`file_size` владельца — u32);
перезапись AST владельца — O(число батчей) на каждый seal.

Кроме сегментных форм в Shadow живут и **целофайловые контейнерные формы**
(`num_blocks==1`, один блоб на файл): TARR/GZR/PNGR/FLACR (рецепт +
сиблинги `name!…`) и **EXER** (`algo=15`, WP14b M2) — exe-as-container:
из ELF/PE/Mach-O вырезаются встроенные JPEG/PNG ≥ 16 КиБ (кап 64), члены
уходят в сиблинги `name!exrN` (JPEG → lossless JXL, PNG — просто ZSTD:
djxl перекодирует PNG, так что бит-экзактный транскод невозможен, пока
PNGR живёт только на Windows). Главная запись — ZSTD-19 блоб
`[4B "EXER"][u32 n][n × {u64 off, u64 len, u8 codec}][склеенные остатки]`;
чтение = распаковка блоба + splice членов по смещениям, бит-в-бит
(гардиан — полная пересборка в памяти с memcmp до коммита).

### Superblock (Block 0, 4096 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x0000 | 8 | `magic` | "InvariantFS\0" |
| 0x0008 | 16 | `uuid` | Volume UUID |
| 0x0018 | 4 | `state` | Clean (0xCA), Dirty (0xDA), Recovery (0xRE) |
| 0x001C | 4 | `block_size` | Logical block size (default: 4096) |
| 0x0020 | 8 | `total_blocks` | Total blocks in volume |
| 0x0028 | 8 | `metadata_zone_start` | Block offset of Metadata Zone |
| 0x0030 | 8 | `metadata_zone_blocks` | Size of Metadata Zone in blocks |
| 0x0038 | 8 | `raw_zone_start` | Block offset of RAW Zone |
| 0x0040 | 8 | `raw_zone_blocks` | Size of RAW Zone in blocks |
| 0x0048 | 8 | `shadow_zone_start` | Block offset of Shadow Space |
| 0x0050 | 8 | `shadow_zone_blocks` | Size of Shadow Space in blocks |
| 0x0058 | 32 | `root_ast_hash` | BLAKE3 hash of root AST directory |
| 0x0078 | 4 | `sweep_cursor` | Last swept RAW block position |
| 0x007C | 4 | `checksum` | CRC32C of superblock (bytes 0-0x7B) |

Total: 128 bytes used, rest reserved for future use.

### Block Bitmap

Simple bit array: 1 bit per block in the volume.
- 0 = free
- 1 = allocated

For a 43 GB partition with 4 KB blocks (~11M blocks), the bitmap is ~1.4 MB.

Maintained with a **free-list cursor** (pointer to last known free block) to avoid O(n) scans on allocation.

### L2P Journal

Append-only log recording logical-to-physical mappings:

```
Entry format:
┌─────────┬──────────┬──────────┬──────────┐
│  inode  │  lba     │  pba     │  length  │
│  8 bytes │ 8 bytes │ 8 bytes  │  4 bytes │
└─────────┴──────────┴──────────┴──────────┘
Type byte prefix:
  0x01 = MAP    (assign mapping)
  0x02 = UNMAP  (release mapping)
  0x03 = SWEEP  (sweep commit marker)
  0xFF = CHECKPOINT (full L2P snapshot follows)
```

On mount: replay journal to rebuild in-memory L2P.
On fsync: flush journal to disk (O(pending writes), not O(total mappings)).
Periodically: compact journal + L2P into a CHECKPOINT.

### Primary Index (B-tree by inode)

```
inode → {
  ast_recipe_loc:  (zone, block, offset)  // where the AST recipe lives
  file_size:       u64
  blake3_hash:     [u8; 32]
  ctime:           u64
  mtime:           u64
  permissions:     u16
}
```

~72 bytes per file. For 100K files: ~7 MB.

### Tag Index (B-tree by BLAKE3 hash)

```
(artist_hash, album_hash, title_hash) → [inode]
```

Enables O(log n) lookup: "find all tracks by artist X".
~32 bytes per tag triple + 8 bytes per inode reference.

## Виртуальные каталоги (2025-08)

Пространство имён плоское (имя = путь `proj/src/main.c`), каталоги —
**виртуальные**: `vol_is_dir`/`vol_list_dir` выводят их из имён файлов.
`mkdir` создаёт **якорь** — пустой файл с именем `dir/` (пустой каталог
живёт как якорь). Авто-якоря: `vol_ensure_path` создаёт родителей при
записи `a/b.txt`. Удаление каталога: `vol_rmdir` (якорь + отказ при
непустом). List-пропуск tombstone: vol_list_dir делает второй проход
и вычёркивает имена, убитые более поздним DELT-рекордом.
