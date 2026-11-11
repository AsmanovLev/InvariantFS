# HANDOFF — снос 32-байтного wire-формата AST (переход на 40 B)

Сессия длинная; это checkpoint, чтобы следующий агент стартовал с чистого
контекста и не переоткрывал уже найденное.

## Состояние git / CI

- remote: `origin git@github.com:AsmanovLev/InvariantFS.git`, ветка `main`.
- Уже ЗАПУШЕНО (3 коммита зачистки):
  - `7029f6f` format: единый гейт + зачистка маркеров версий
  - `019743a` chore: убраны упоминания v3 из прозы и имён
  - `b33663e` chore: переименованы *_v3 реализации в *_impl
- В рабочем дереве (НЕ коммитить): `tools/test-png.sh` (сломанный симлинк,
  git mode 120000 → файл), `src/codecs/ChangeLog.md` (CRLF-шум autocrlf).
- Окружение сборки: WSL2 + Void Linux, gcc 14.2.1, make, `ctags` (universal-ctags
  6.2.1, установлен). `make -j12` собирает `all`; тест-бинарники НЕ входят в
  `all` — их собирать явно: `make bin/invf-v2_open_test bin/invf-entry_v3_test`
  или `make test-bins`.

## Что уже сделано и не трогать

- Единый гейт формата: `src/core/vol_format.{h,c}` — `invfs_format_accept(sb,
  path, why, why_sz)` возвращает 0 / `-12` (нет VOLF_META) / `-13` (generation
  mismatch) / `-14`. Все 6 открывателей (vol_open primary+degraded, verify,
  resize, vnode) идут через него.
- `INVFS_FORMAT_VERSION = 1` (было 0); `VOLF_META2`/`VOLF_ASTV2` retire (значения
  0x02/0x08 не переиспользовать); декоративный `INVFS_VERSION` удалён.
- Проза: `metadata-v3/Meta-v3` → `metadata store`; идентификаторы `*_v3` →
  `*_impl` (реализации), обёртки/диспетчеры без суффикса остались public API.

## Осталось: ВАРИАНТ C — снос 32 B wire-формата (задача этого handoff)

Цель: сделать 40-байтный recipe entry единственным, удалить v1/v2 (32 B),
переименовать `*_v3` → базовое. Это **формат-breaking** (текущие v3-тома пишут
32 B — перестанут читаться), пользователь согласен (переустановка).

### Ключевые факты (уже выяснено, не переоткрывать)

1. `invfs_ast_block_entry` (32 B) — **текущий дефолт**, НЕ мёртвый. Поля:
   `file_offset, length, zone:2, algo:6, block_id:24, block_offset, pba`.
2. `invfs_ast_block_entry_v3` (40 B) — новый stride, поля:
   `file_offset, length, zone:6, algo:10, block_id:32, rsv:16, block_offset,
   crc32c, pba`. Включается только `INVFS_RECIPE_FORCE_V3=1` (тест-форс).
3. Выбор формата в `src/core/vol_ast.c:67-93`:
   ```c
   const char *f3 = getenv("INVFS_RECIPE_FORCE_V3");
   ...
   hlen = f3 ? invfs_ast_hdr_write_v3(...) : invfs_ast_hdr_write(...);
   ```
4. Заголовки: `invfs_ast_recipe_header_v1` (16 B, ver 1),
   `invfs_ast_recipe_header_v2` (24 B, ver 2), `invfs_ast_recipe_header`
   (24 B, ver 3). v2 и v3 **одинаковы по layout**, разница только `version`.
5. Версии/длины: `INVFS_AST_VERSION_V1=1, V2=2, V3=3`;
   `INVFS_AST_HDR_V1_LEN=16, INVFS_AST_HDR_V2_LEN=24, INVFS_AST_HDR_LEN=24`
   (HDR_LEN уже переименовано из бывшего V3_LEN).
6. `invfs_ast_entry_stride(version)`: V1/V2→32, V3→40, иначе 0.
7. 40 B on-disk собирается **арифметически** через
   `invfs_ast_entry_pack/unpack/verify` (invarifs.h ~1680-1760), НЕ memcpy
   (стабильность bitfield layout). `unpack` принимает `invfs_ast_block_entry*`
   (сейчас 32 B in-memory) и **отказывает** `zone>3 || algo>63 ||
   block_id>0xFFFFFF` — эти узкие проверки надо снять при расширении.
8. `invfs_ast_block_entry` (32 B) упоминается ~163 раза (in-memory тип по всему
   vol_write/vol_read/vol_dedupe/vol_records/...).
9. `invfs_ast_hdr_parse` (invarifs.h:1233) — единый читатель, уже умеет все три
   версии (V1→16 B, V2→24 B, V3→24 B). V2 и V3 проверяют одинаковые cap'ы
   (`MAX_FILE_SIZE`, `MAX_SEGMENTS_V2`). При сносе v1/v2 остаётся одна ветка
   (version==3), а `hdr_len` всегда `INVFS_AST_HDR_LEN`.

### План (одним заходом)

1. In-memory struct: тело `invfs_ast_block_entry_v3` (40 B) переименовать в
   `invfs_ast_block_entry`; старый 32 B struct удалить. Проверить, что все 163
   места компилируются (поля `zone/algo/block_id` стали шире — имена те же).
2. `invfs_ast_entry_unpack`: убрать проверки `zone>3/algo>63/block_id>0xFFFFFF`
   (теперь in-memory широкий).
3. `invfs_ast_hdr_write_v3` → `invfs_ast_hdr_write`; старую v1/v2 версию
   удалить. В `vol_ast.c` убрать `getenv(FORCE_V3)` и ветку, всегда 40 B.
4. `invfs_ast_entry_stride` → всегда 40 (убрать диспатч), либо убрать функцию.
5. Чтение в `vol_ast.c` (~164, ~262, ~268): убрать диспатч по
   `hdr->version` / `version == INVFS_AST_VERSION_V3` — всегда 40 B.
6. Удалить: `invfs_ast_recipe_header_v1/v2`, `INVFS_AST_VERSION_V1/V2`,
   `INVFS_AST_HDR_V1_LEN/V2_LEN`, `MAX_SEGMENTS` (0xFFFF, v1-граница u16),
   `INVFS_RECIPE_FORCE_V3`.
7. `INVFS_AST_VERSION_V3` → `INVFS_AST_VERSION` (значение 3 остаётся).

### ВАЖНО: `MAX_SEGMENTS_V2` НЕ удалять (правка к ранней версии заметок)

`MAX_SEGMENTS_V2 = (1u << 24)` (invarifs.h:1162) — это **текущий действующий
cap** на число записей рецепта, а не legacy. Используется в ~24 местах:
`vol_write.c` (834, 913, 1058), `vol_ast.c` (83, 98, 102, 180, 190),
`fuse_fs.c:2442`, `recipe_tree_test.c:3`, `invarifs.h` (137, 1153, 1159, 1231,
1270, 1291, 1307, 1351, 1584) — включая `INVFS_RECIPE_BLOB_MAX`.
Суффикс `_V2` исторический (block_id стал 24-битным ещё в v2).
Действие: **переименовать** в `MAX_RECIPE_ENTRIES` (или `MAX_SEGMENTS_BLOCKID`),
НЕ удалять. А удалять надо `MAX_SEGMENTS` (0xFFFF) — он остался только в
2 местах (invarifs.h:1161 определение и :1596, где уже сомнительно используется
вместе с 40 B stride).
8. Обновить `src/cli/entry_v3_test.c` (и `tools/test-entry-v3.sh`): убрать
   FORCE_V3/32B-ноги, оставить 40 B; переименовать файл `entry_v3_test.c` →
   `entry_test.c` (Makefile `CLI_MAINS` + `tools/test-entry-v3.sh`) — можно
   отложить переименование файла, если оно тянет лишний git mv.

### Проверка после захода

```
make -j12
make bin/invf-v2_open_test bin/invf-entry_v3_test
bash tools/test-meta.sh
bash tools/test-meta-dirent.sh
./bin/invf-v2_open_test
./bin/invf-entry_v3_test
```
Ожидание: всё зелёное; `invf-mkfs` + `invf-cp` пишут 40 B recipe.

### Смежное, НЕ в этой задаче

- `TZV3` (магия 0x33565a54 в batch_owner_test.c) — on-disk магия, не трогать.
- Roadmap: пункт «Metadata Sharding & Partitioning (XFS-style AG & dcache
  hierarchy, v0.8 target)» **удалён из `README.md`** — фича выходит в v0.7,
  так что в отдалённом roadmap ей не место. Формулировка про AG осталась в
  `impl_docs/design-meta-v3.md` §9 и §14 (там она точная).
- Граница для v0.7 (чтобы не переоткрывать спор `design-meta-v3.md:187`):
  шардируем **RAW/delta append-арены (AG)** и **FUSE-зеркало имён + локи**
  (per-directory бакеты вместо плоского `g_entries` и `sync_parents_locked`),
  а **on-disk namespace НЕ партиционируем** — иначе rename/link/fold
  становятся кросс-доменными.
- Стенд Gentoo `192.168.0.2`: rsync портеджа застрял (~58/184 категорий,
  invf-fuse выжимает 1 ядро). НЕ трогать — будет переустановка на новую
  архитектуру. SSH: `ssh -i ~/.ssh/id_ed25519_invfs root@192.168.0.2`.
