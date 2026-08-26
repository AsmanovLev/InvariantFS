# Sweep Worker

The Sweep worker is a background thread/process that moves data from the RAW Zone to Shadow Space, applying optimal compression based on data type.

## Design

- **Idle-triggered**: Runs when the filesystem is idle (like ReFS scrubber)
- **On-demand**: Can be manually triggered for bulk processing
- **Cursor-based**: Tracks last swept position via `sweep_cursor` in superblock
- **Crash-safe**: Every sweep batch is journaled

## Pipeline

```
For each file in RAW Zone (from sweep_cursor):
  1. Read file from RAW Zone
  2. Magic Sniff → determine type(s)
  3. For each semantic component:
     a. Compress with optimal algorithm
     b. Write to Shadow Space (batched by type)
     c. Update in-memory L2P
     d. Log journal entry: SWEEP_COMMIT
  4. Build/update AST recipe
  5. Update Primary Index (inode → AST recipe location)
  6. Mark RAW blocks as free in Bitmap
  7. Advance sweep_cursor
  8. On fsync: flush journal + bitmap
```

## Per-Type Processing

### FLAC

```
1. Parse FLAC metadata blocks
   └─ METADATA_BLOCK_STREAMINFO → store raw (tiny, needed for header)
   └─ METADATA_BLOCK_PICTURE → magic sniff → JPEG → JXL
   └─ METADATA_BLOCK_VORBIS_COMMENT → text → ZSTD-19 + dict
   └─ METADATA_BLOCK_PADDING → strip (not needed)
2. Extract PCM frames → APE -c4000
3. Build AST recipe (nested: FLAC container → components)
4. Invariant check: decompress → compare BLAKE3 hash
```

### MP3 / ID3

Реализовано (2026-08): файл целиком идёт в packMP3 → PMP-блоб, bit-exact,
без рецепта и без разбора на компоненты.

```
1. Магия: ID3v2-тег ('ID3') или голый frame sync (0xFF 0xEx)
2. packMP3 -np -o  → PMP-блоб
   └─ MPEG-1 Layer III  → блоб (−9…−13%)
   └─ MPEG-2/2.5 L3     → отказ (exit 0!) → generic ZSTD-19
   └─ тяжёлый ID3v2     → отказ («no mpeg audio data recognized») → ZSTD-19
3. Guard: принять только если pmp_len < mp3_len
4. vol_create_pmp_file (algo=pmp) + tombstone старого inode
```

Разбор ID3v2 на компоненты — **не реализован**, план:

```
   └─ APIC (cover art) → magic sniff → JPEG → JXL
   └─ T* frames (text: title, artist, album, etc.) → PPMd -9
   └─ USLT (lyrics) → PPMd -9
   └─ COMM (comments) → PPMd -9
   └─ ID3v1 tag в конце → PPMd -9
```

Считалось, что он окупится только вместе с text-суб-секцией (тег в 4 KB
PPMd жмёт хуже ZSTD, B7). **B28.1 это опровергает**: трек с тегом 1.59 MB
packMP3 отвергает целиком — не по формату, файл нормальный MPEG-1, просто
frame sync за пределами окна поиска. Отрезали тег — тот же файл дал
−17.47% bit-exact, лучший результат в корпусе.

То есть выигрыш от разбора — не сжатие обложки, а **сам факт запуска
аудио-кодека** на файлах с тяжёлым тегом. Причём минимальная версия не
требует ни PPMd, ни text-зоны: достаточно отделить тег от аудио, аудио →
PMP, тег → как есть (или ZSTD). Декомпозиция тега на APIC/T*/USLT — уже
следующий слой.

### ZIP / Archives

```
1. Parse ZIP structure (local headers, central directory)
2. For each entry:
   └─ If low compression: decompress → sniff → recurse into Sweep
   └─ If max compression: store raw as single block
3. Build AST recipe (nested: ZIP container → entries)
4. Reconstruct central directory on read
```

### Executables

```
1. Magic sniff → PE/ELF
2. Apply BCJ2 filter (x86 branch/call conversion)
3. ZSTD -19
4. Invariant check
```

### Unknown Files

```
1. Entropy test
   └─ Low entropy → ZSTD -19
   └─ High entropy → store raw
2. Invariant check
```

## Compactor (дефрагментация Shadow Space)

**Проблема:** В TextZone лежит PPMd-батч 1MB (100 тегов). Удаляется 50 файлов → внутри блока "дыры" (refcount упал). Нельзя удалить половину PPMd-блока.

**Решение:** Фоновый Compactor (как в базах данных):

```
1. Мониторинг: если фрагментация зоны > 30% (живые байты < 70%)
2. Выбрать батчи с упавшим refcount
3. Прочитать живые записи (refcount > 0)
4. Переписать их в НОВЫЙ батч в конце зоны
5. Обновить AST-рецепты (новые offset'ы)
6. Пометить старый блок свободным (refcount=0)
```

Запускается как часть Sweep-воркера (тот же фоновый процесс), приоритет ниже чем у основного Sweep. Compactor не трогает блоки с полным refcount — только фрагментированные.

**Статус (WP10, ff84bf8):** рефкаунтов нет и не нужно — вместо них GC
mark-and-sweep (`vol_tz_gc`: метка по живым member-записям, освобождаются
только **полностью мёртвые** батчи). Перепаковка дырявых, но живых батчей
(триггер >30% фрагментации) — следующая фаза, не v1.

**Триггеры:**
- Фрагментация зоны > 30%
- Refcount батча упал > 50% от исходного
- Ручной вызов (ioctl)

## Конкурентность: L2P Redirect-on-Write (Вариант B)

Запись в файл **во время** работы Sweep-воркера не блокирует inode (никаких `EBUSY`, как в CoW-деревьях btrfs):

```
1. Sweep фиксирует текущие PBA блока (физические адреса)
2. Приложение делает write() во время Sweep:
   └─ данные пишутся в НОВЫЕ блоки RAW-зоны
   └─ L2P-журнал обновляет маппинг (MAP entry)
3. Sweep заканчивает сжатие "старых" PBA:
   └─ кладёт их в Shadow/Template Zone
   └─ AST собирается по АКТУАЛЬНОЙ L2P-таблице:
      └─ старые байты → ссылка на Shadow (сжатый)
      └─ новые байты  → ссылка на свежие RAW (LZ4)
4. Старые блоки, если refcount=0 → мусор, освобождаются
5. Приложение не висит. Целостность гарантируется журналом.
```

**Отличия от CoW-деревьев (btrfs):**
- Нет деревьев с double-indirection
- Нет блокировок inode на время сжатия
- Всё через существующий L2P-журнал

## Invariant Verification

Every Sweep operation ends with:

```
compress(data) → compressed
decompress(compressed) → reconstructed
blake3(reconstructed) == blake3(data) ? PASS : FAIL
```

On FAIL:
1. Log the failure with file metadata
2. Keep the RAW data (don't free it)
3. Report via health status
4. Optionally retry with different algorithm

## Batched Compression

**Реализовано (WP10, ff84bf8)** — Text Zone: cross-file PPMd-батчи мелких
текстов (устройство хранения — 02-on-disk-format.md). Первоначальный план
«64 KB или 100 файлов» и отдельных буферов под аудио/картинки заменён
измеренным оптимумом **4 MB** (B27/B29) — и батчится только текст.

### Аккумулятор и seal

Текст в `vol_sweep_one` не жмётся по одному файлу, а **откладывается в
аккумулятор** тома (`tz_defer`; код результата 9 = «text → PPMd batch»).
Классификатор гибридный (`invfs_text_family`, codec.c): сначала семейство
по расширению (`.c/.h/.cpp→1`, `.py→2`, `.js/.ts→3`, `.java→4`, `.rs→5`,
`.go→6`, `.json/.xml/.yaml/.csv→7`, `.txt/.md/.log→8`, `.html/.css→9`,
`.sh→10`), при неизвестном расширении — снифф контента (выборка ≤8 KB:
текст = ноль NUL-байтов и ≥85% печатных/whitespace). Не текст → обычный
generic-путь нетронут.

В конце прогона `vol_tz_flush` (CLI — после дедупа и GC, где они есть;
демон — drain pending-очереди) запечатывает накопленное:

1. **Стабильная сортировка по (семейство, размер)** — языковая группировка
   B29; qsort нестабилен, исходный индекс служит тай-брейком.
2. Батч наполняется до цели **min(4 MB, arc_limit/2)**, минимум 4096 (ARC
   отказывает записям больше половины бюджета — слишком крупный батч было
   бы некуда кэшировать). Файл крупнее цели режется на срезы по соседним
   батчам.
3. **Seal**: PPMd encode (o=8, 64 MB, CUT_OFF) → **обязательный
   decode+memcmp** (инвариант, в процессе, бесплатно) → size-guard
   `ppmd < raw` (без сравнения с ZSTD — 3 с/4 MB слишком дорого; проигравший
   батч не пишется, его члены уходят в generic-путь) → сегмент в
   shadow-зоне под владельцем `\x01tzb` → записи членов со штампом TEXT.
4. **Крах-безопасность**: сегмент батча и L2P владельца журналируются ДО
   записей членов; крах между ними — сирота-батч заберёт GC, члены остаются
   читаемыми из RAW. Точка инъекции для теста: `INVFS_TZ_SKIP_COMMIT`.

Аккумулятор живёт только в RAM: прерванный sweep ничего не портит — файлы
просто переклассифицируются следующим прогоном.

### Флаг класса хранения (`invfs.class`)

Каждый обработанный файл получает внутренний xattr `invfs.class` (4 байта
`{cls u8, algo u8, gen u16}` в INO2-ext записи — паттерн VOLF_META2, формат
тома не меняется). Флаг отвечает на вопрос «почему файл хранится так», и
следующий sweep не пересматривает контент без причины:

| cls | класс | что делает следующий sweep |
|---|---|---|
| — | нет флага | свежая запись: полный путь (снифф → политика → транскод → guard → штамп) |
| 1 | UNCOMPRESSIBLE | выигрыш < `INVFS_MIN_GAIN_PCT` (по умолч. 0.5%); повтор, только если поколение реестра выросло **и** снифф головы что-то признал; иначе снапшот gen подвигается (полного чтения нет — только 8 KB снифф) |
| 2 | CODEC | кодек-специфичное (PMP/JXL/APE/WV); проверка соответствия политике |
| 3 | CONTAINER | контейнер (ZIPR/TARR/GZR/PNGR/FLACR); то же |
| 4 | GENERIC | generic ZSTD-19; «пол» всегда соответствует — ниже некуда |
| 5 | GENERIC_MEMLIMIT{algo,gen} | кодек отклонён лимитом памяти; повтор, когда `dec_mem` кодека ≤ текущего лимита |
| 6 | GENERIC_GUARD{algo,gen} | guard кодека отказал; повтор первым кандидатом, когда `generation` кодека выросло (новый субэнкодер) |
| 7 | TEXT | член PPMd-батча; соответствие = размер батча против arc_limit/2 |

Для 5/6 поля `algo`/`gen` — **отклонивший** кодек и его поколение на момент
отказа; для 1 — 0 + снапшот `invfs_registry_generation()`; для принятых —
победивший кодек. Штамп пишется только при **смене** класса
(check-then-write), поэтому re-sweep по живому тому почти ничего не
переписывает.

### Политика памяти (только на sweep)

Допуск считается на sweep; **путь чтения хранимое не отвергает никогда**
(«гарантированные условия»: всё записанное декодируется в рамках политики,
действовавшей на последнем sweep'е). Ручки: mount-опции `arc_limit=` /
`dec_mem_limit=` (fuse_fs.c; дефолты 256 MB / 512 MB), env
`INVFS_ARC_BYTES` / `INVFS_DEC_MEM_LIMIT` для CLI (invf-sweep).

Рабочее множество **декодера** выводится из дешёвых заголовков, без
пробного декода: PNG — из IHDR (≈ h·(1 + w·ch·bd/8)), JPEG — из SOF0/1/2
(≈ w·h·3), PPMd — константа реестра (68 MiB = модель 64 MB + юнит 4 MB),
whole-file кодек — `file_size` против arc_limit. Смена политики работает
**в обе стороны**: ужесточение → compliance-даунгрейд в generic (со штампом
MEMLIMIT, чтобы поднятый лимит нашёл файл снова); послабление → повтор по
таблице выше. TEXT-члены при даунгрейде расшиваются (L2P-дубли снимаются,
дыры в батчах — территория GC); перепаковка под новый размер случается сама
на следующем текстовом проходе.

### Порядок внутри батча

Батч перед сжатием **сортируется**: ключ — тип компонента, вторичный —
размер (`bytypesize`). Это стоит +26.6% ratio на блоке 1M (B29), и
одинаково для PPMd, ZSTD и LZMA2 — то есть работает локальность, а не
прогрев модели.

Дёшево это потому, что L2P уже отвязывает логическое от физического:
порядок в батче читателя не касается. Причём **обход дерева сам по себе
даёт почти весь выигрыш** — порядок по пути равен сортировке по типу с
точностью до третьего знака, так как дерево группирует однотипное. Копить
и сортировать весь том не нужно; нужно **не перемешивать**, а
доупорядочивать в пределах уже накопленного батча.

Пересборка батчей по мере роста тома — работа Compactor'а (выше): он и так
переписывает фрагментированные батчи, сортировка добавляется туда же.

## On-demand sweep (встроенный в демон, 2025-08)

Полный CLI-проход (`invf-sweep`) — офлайн. Для «горячо смонтированной»
ФС (Dokan/FUSE) sweep живёт **внутри демона** (одна L2P в памяти — два
процесса не открывают том: `io_open` держит `FILE_SHARE_READ` без
WRITE).

- **pending-список в RAM**: `vol_mark_pending` при записи
  (DkFlushFileBuffers/DkCloseFile, FUSE-flush — после `vol_create_file`).
  Крах просто оставляет файлы необработанными — инвариант: ничего не
  потеряно, офлайн-sweep их найдёт.
- **Фоновый поток**: `vol_sweep_pending` дренит очередь, когда
  `open_handles == 0` (никто не читает — tombstone/free безопасны).
  Интервал — `INVFS_SWEEP_INTERVAL` (сек, по умолчанию 30; тесты — 2).
- **Единое ядро**: `vol_sweep_one(inode)` — ZIP-explode / FLAC / TAR /
  GZ / PNG / JPEG / MP3 / generic-shadow — используется и CLI, и демоном
  (sweep.c main стал тонким циклом поверх ядра, коды 1-8 = тип транскода).
- **Горячий путь честный**: транскод никогда не происходит при чтении;
  но чтение PNGR вызывает djxl (секунды) — принято на этой фазе,
  оптимизация (кэш пикселей / dual-store) — отдельной задачей.
