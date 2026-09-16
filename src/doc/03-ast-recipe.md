# AST Recipe — Byte-Range Remapping

The AST recipe is a **tree structure** that maps byte ranges of the original file to physical blocks in Shadow Space. It enables O(1) access to any byte range without decompressing unrelated data.

## Flat Recipe (simple files)

For a file with no container structure (e.g., a raw text file):

```json
{
  "version": 1,
  "file_size": 1523000,
  "blocks": [
    {"offset": 0,      "len": 10752,  "zone": "text",   "algo": "ppmd",   "block_id": 42,  "block_offset": 0},
    {"offset": 10752,  "len": 1400000, "zone": "binary", "algo": "ape",    "block_id": 17,  "block_offset": 0},
    {"offset": 1410752, "len": 112248, "zone": "binary", "algo": "jxl",    "block_id": 8,   "block_offset": 512}
  ]
}
```

On a `read(fd, buf, 4096, 0)`:
1. Find the block containing byte offset 0 → block 0 (text zone)
2. Read `block_id=42` from Text Zone
3. Decompress with PPMd
4. Return bytes 0-4095

Audio data is never touched.

## Nested Recipe (containers)

For archives and container formats, the recipe is a tree:

```json
{
  "version": 1,
  "file_size": 48200000,
  "type": "zip",
  "children": [
    {
      "name": "cover.jpg",
      "offset": 0,
      "len": 45000,
      "type": "jpeg",
      "blocks": [
        {"offset": 0, "len": 45000, "zone": "binary", "algo": "jxl", "block_id": 12, "block_offset": 0}
      ]
    },
    {
      "name": "track01.flac",
      "offset": 45000,
      "len": 32000000,
      "type": "flac",
      "children": [
        {
          "name": "streaminfo",
          "offset_within_parent": 0,
          "len": 42,
          "type": "raw",
          "blocks": [{"offset": 0, "len": 42, "zone": "raw", "algo": "none", "block_id": 0, "block_offset": 0}]
        },
        {
          "name": "tags",
          "offset_within_parent": 42,
          "len": 2048,
          "type": "vorbis_comment",
          "blocks": [{"offset": 0, "len": 2048, "zone": "text", "algo": "ppmd", "block_id": 55, "block_offset": 0}]
        },
        {
          "name": "cover_art",
          "offset_within_parent": 2090,
          "len": 45000,
          "type": "jpeg",
          "blocks": [{"offset": 0, "len": 45000, "zone": "binary", "algo": "jxl", "block_id": 13, "block_offset": 0}]
        },
        {
          "name": "audio",
          "offset_within_parent": 47090,
          "len": 31952910,
          "type": "pcm",
          "blocks": [{"offset": 0, "len": 31952910, "zone": "binary", "algo": "ape", "block_id": 18, "block_offset": 0}]
        }
      ]
    },
    {
      "name": "liner_notes.txt",
      "offset": 32045000,
      "len": 15500,
      "type": "text",
      "blocks": [
        {"offset": 0, "len": 15500, "zone": "text", "algo": "ppmd", "block_id": 56, "block_offset": 0}
      ]
    }
  ]
}
```

## Block Types

| `algo` | Zone | Description |
|--------|------|-------------|
| `none` | raw | Stored in RAW zone, not compressed |
| `ppmd` | text | PPMd -9 compressed text |
| `zstd` | binary | ZSTD -19 compressed binary |
| `ape` | binary | APE -c4000 compressed audio |
| `jxl` | binary | JPEG XL lossless recompression |
| `brotli` | text | (не реализовано; текст жмётся PPMd-батчами, WP10) |

## Read Algorithm

```
read(inode, buf, len, offset):
  recipe = load_ast(inode)
  block = find_block_containing(recipe, offset)
  
  if block.zone == "raw":
    raw_read(block, buf, len, offset - block.offset)
  else:
    compressed = zone_read(block.zone, block.block_id)
    decompressed = decompress(compressed, block.algo)
    copy(decompressed[offset - block.offset:], buf, len)
```

For nested recipes, the algorithm recurses into children, adjusting offsets relative to each container level.

## AST-children (контейнеры) + глубокая защита

**Формат сегмента** (начиная с v1.1): `[4B csize][4B crc32c(data)][data]` —
каждая физическая копия данных самопроверяема. При чтении CRC32C
проверяется ДО декомпрессии; несовпадение → отказ чтения (не мусор).

**AST children**: после block entries сериализуются члены контейнера:
`[u16 name_len][name][u16 method][u32 csize][u32 usize][u32 crc][u32 data_off]`
× num_children — это **окна** (windows) в данные контейнера.

**ИНВАРИАНТ (1:1)**: контейнер ХРАНИТ оригинальные байты архива
(num_blocks>0, file_size = полный размер). `cat container.zip` возвращает
байт-в-байт оригинал (проверяется b3sum) — git-безопасно, никакие
сторонние архиваторы/хэш-системы не ломаются. Члены — виртуальные:
`name!member` извлекается на лету из окна (stored = копия диапазона,
deflate = tinfl-inflate); вложенность `a.zip!inner.zip!x.txt` разбирается
рекурсивно без промежуточных inode.

**Почему не «взрыв в члены»**: deflate-поток необратим из несжатых
данных (LZ77+Хаффман разных зипперов невоспроизводимы), поэтому
единственный способ гарантировать инвариант — хранить оригинал целиком
и индексировать окна. Сжатые члены между разными архивами частично
дедуплицируются на уровне сегментов 64KB (детерминированные deflate-
потоки идентичных членов совпадают).

**Идемпотентность**: sweep индексирует контейнер один раз
(num_children>0 — пропуск); окна не пересоздаются, данные не трогаются.

## FLAC-recipe (`algo=flacr`) — bit-exact транскод FLAC→APE

FLAC-файл (магик `fLaC`) транскодируется при sweep в **два inode**:

1. `song.flac` — `algo=flacr`, file_size = **размер оригинального FLAC**,
   данные = APE(-c4000)-блоб PCM (16/24-bit, каналы/частота как в оригинале)
2. `song.flac!recipe` — бинарный рецепт фреймов (**IVFR v2**, ~2–3% от FLAC
   на реальной музыке)
3. `song.flac!coverN` — обложки (PICTURE-блоки метаданных), вынесенные
   отдельными NONE-сегментами

`cat song.flac` реконструирует оригинал **байт-в-байт**:
`APE → WAV (MAC.exe -d) → flacx_rebuild(WAV + recipe + covers) → FLAC`.
Остатки предсказания пересчитываются из PCM детерминированно — рецепт
хранит только заголовки фреймов: предиктор/порядок/коэффициенты LPC
(точные биты), сдвиг, Rice-метод/порядок/k-параметры партиций (читаются
после КАЖДОЙ партиции, не все подряд), channel-assignment, wasted-биты,
block-size/sample-rate коды, sample-number. Rebuild воспроизводит
оригинальный битстрим: CRС-8/CRC-16 пересчитываются, байты совпадают
(проверено b3sum на 16-bit LPC/VERBATIM/моно и 24-bit 96k).

**Cover-дедуп**: в FLAC обложки живут в метаданных (PICTURE-блок). Рецепт
v2 выносит их из заголовка: заголовок хранится без PICTURE-блоков
(слоты `[offset,len]` в рецепте), обложки — в `name!coverN`. last-флаг
метаданных нормализован (0) и восстанавливается на rebuild, поэтому
ОДИНАКОВЫЕ обложки разных треков дают идентичные сегменты и
схлопываются блочным дедупом (проверено: 2 трека с одной обложкой →
dedupe merged 1; обе реконструкции bit-exact).

**ИНВАРИАНТ**: транскод выполняется ТОЛЬКО если `ape_len + recipe_len <
flac_len` (иначе хранится оригинал — git-safe). На реальной 16-bit музыке
APE -c4000 выигрывает у FLAC -8 ~2–6%; на синтетике/24-bit hi-res —
проигрывает, поэтому guard оставляет FLAC как есть (env `INVFS_FORCE_FLACR`
форсирует транскод для тестов инварианта).

**Чтение**: `vol_read_inode` для `algo=flacr` находит сегмент-рецепт через
`vol_find(name + "!recipe")`, декодирует APE→WAV, вызывает `flacx_rebuild`;
длина результата сверяется с `e->length` (защита от повреждения).
Сегменты flacr — BINARY-зона: sweep их не пересжимает (идемпотентность).

**Глубокая защита (deep protection)**:
## TAR-recipe (`algo=tarr`) — bit-exact разбор tar на члены

Tar — поток 512-байтных записей `[заголовок][данные][паддинг]` + хвост из
нулевых блоков. InvariantFS **не интерпретирует** члены (никаких longname/PAX
расширений, пересчёта чексумм): точные байты заголовка каждого члена
остаются в рецепте, payload — в `name!partN` (каждый сжат по своему
контенту: текст → ZSTD-19, уже сжатое → NONE), межчленный паддинг — в
zero-slot, когда он из нулей.

**IVFT v1**: `[4B "IVFT"][1B ver=1][8B total][2B nparts]`
`[за член: 512B header_orig][8B data_len][1B pad_kind][2B pad_len]`
`[8B trailer_len][1B trailer_kind][trailer байты если kind=0]`.

Хранение: `name` = blob-рецепт (`[0x01][zstd]` или `[0x00][raw]`,
orig_size = размер tar), `name!partN` = члены. **Rebuild** = байт-в-байт
сборка: заголовки из рецепта, данные из partN, паддинг/хвост — memset.
Длина сверяется с `e->length`. Члены — обычные inode: видны в `ls`,
доступны через `name!partN`, дедупятся блочным дедупом (одинаковые
файлы в разных архивах — один сегмент).

**Guard**: транскод только если `Σ parts + recipe < tar` (иначе оригинал).
**Идемпотентность**: маркер `name!part0` + защита от флаг-байтов
рецепта (`full[0]∈{0x00,0x01}` не считается tar). Проверено: текст+флак+
рандом+gz в одном архиве → bit-exact, повторный sweep ничего не трогает.

## GZIP-recipe (`algo=gzr`) — bit-exact tar.gz через deflate-реплику

gzip нельзя пережать без нарушения 1:1 — deflate-поток воспроизводится
ТОЛЬКО тем же энкодером. Решение: **настоящий zlib 1.3.1 (src/zlib,
deflate-сторона)** как репликатор. При транскоде поток пережимается
всеми (level 1-9) × (memLevel 7-9) и побитово сравнивается с
оригиналом; совпавшие параметры идут в рецепт, иначе — guard
(хранить оригинал, git-safe).

**IVGZ-рецепт**: `[4B "IVGZ"][1B ver][1B level][1B memLevel][4B crc32]`
`[4B isize][2B hlen][gzip header hlen байт] + [IVFT]` (tar-структура
переиспользована). Rebuild: члены → tar (tarx_rebuild) → `deflate(level,
mem, raw)` → deflate-поток + оригинальный header + crc32 + isize.

Проверено: GNU gzip 1/6/9, bsdtar 6, zlib 1.2.x — реплицируются
(1.2.13 ⇄ 1.3.1 потоки идентичны); 7-Zip — нет (guard). Экономия на
слабых gzip -1/-2 (defalte -1: −12%), на -6/-9 guard отклоняет
(дефлейт уже сжал; изоляция членов теряет межфайловые повторы окна) —
честное поведение. Идемпотентность: маркер `name!part0`.

## PNG-recipe (`algo=pngr`) — JXL-lossless + IVPN (bit-exact repack)

PNG = чанки + IDAT (единый zlib-поток). IDAT распаковывается
(фильтры строк — в данных!), пиксели сжимаются **JXL modular lossless**
(`cjxl -d 0 -e 7`, `name!jxl`), а рецепт IVPN хранит:

- non-IDAT чанки (pre/post, как есть, включая их CRC) + **длины IDAT-чанков**
  (разбиение — часть файла!) + **CRC IDAT/IEND** (могут быть битые — часть файла!);
- **фильтры строк — 1 байт/строку** (из RGB невосстановимы — выбор энкодера);
- параметры deflate-реплики: `[enc=zlib|miniz][level][memLevel]`.

**Транскод** (офлайн): спул.png (stored IDAT) → cjxl → djxl → round-trip
пиксели == оригинал → refilter (детерминированный: `filt = px − pred(px)`)
→ brute-force (zlib 1.3.1 уровни 1-9 × memLevel 7-9, miniz 1-10) →
совпавший поток = реплика. **Rebuild**: djxl → пиксели → refilter →
deflate(level,mem) → IDAT + чанки (CRC как в оригинале) → PNG 1:1.

Guard v1: только 8-bit non-interlaced (Adam7/16-бит/палитра-2бит →
оригинал); энкодер не распознан (zopfli/ffmpeg-zlib-ng) → guard;
JXL+рецепт ≥ PNG → guard. Вскрытые баги: refilter считал pred из
filtered (взаимно-обратная математика!) — исправлен; CRC-чанков —
часть файла (у pnggen CRC=0000!) — теперь хранятся, не пересчитываются.

## MP3 (`algo=pmp`) — packMP3, bit-exact, БЕЗ рецепта

Единственный транскод-путь без sibling-рецепта: весь файл — один PMP-блоб,
из которого packMP3 восстанавливает исходный MP3 байт-в-байт. Рецепт не
нужен, потому что восстановление полное, а не собранное из частей.

- **Инструмент**: packMP3 v1.0g (Matthias Stirner, LGPL v3). Тип определяет
  **по содержимому, а не по расширению**; один и тот же бинарник жмёт и
  разжимает. Флаг `-p` запрещён: он ослабляет warning'и, а readme прямо
  говорит, что bit-exact тогда не гарантирован — это ровно тот инвариант,
  ради которого существует ФС.
- **Ограничение**: только **MPEG-1 Layer III**. MPEG-2/2.5 отвергается — но
  **exit code при этом 0**, поэтому успех определяется по наличию выходного
  файла, а не по статусу. Проверено: файл печатает «fatal error: file is
  MPEG-2 LAYER III, not supported» и выходит с 0.
- **Guard**: транскод принимается только при `pmp_len < mp3_len`. Отказ или
  разбухание → файл падает в generic-путь (ZSTD-19), ничего не теряется.
- **Идемпотентность**: у PMP нет sibling'а `name!recipe`, а `vol_read_file`
  отдаёт **декодированные** байты — то есть повторный sweep видит обычный
  MP3 и пережал бы его снова. Поэтому `vol_sweep_one` до чтения проверяет
  зону первого AST-сегмента (`vol_inode_first_zone`): всё, что не RAW, уже
  обработано. Побочный эффект — проверка стоит до `vol_read_file`, и
  повторный проход по свежесмётанному тому упал с 6.5 с до 0.25 с.
- **Цена чтения**: ranged-read декодирует весь блоб на каждый вызов
  (~1.9 MB/s). Для `invf-cat` это нормально, для проигрывания прямо с
  монтирования нужен кэш — отдельная задача.
