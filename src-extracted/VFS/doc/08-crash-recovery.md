# Crash Recovery

## Journal-Based Recovery

InvariantFS uses an append-only journal in the Metadata Zone for crash-safe metadata updates.

### Journal Entry Types

Реально пишутся два типа (формат записи 36 B — см. 02-on-disk-format.md):

| Type byte | Entry | Description |
|-----------|-------|-------------|
| `0x01` | `MAP(inode, lba, pba, len)` | Assign logical-to-physical mapping |
| `0x02` | `UNMAP(inode, lba)` | Release mapping |

Константы `0x03` (SWEEP) и `0xFF` (CHECKPOINT) определены в invarifs.h,
но не пишутся; типов `0x04 DELTA` / `0x05 META` из раннего дизайна не
существовало никогда. Чекпоинт sweep'а — это CKP0-дескриптор в блоке 0
(WP21), а не запись журнала.

### Mount Recovery

Как реализовано (`vol_open`):

```
On mount:
  1. Read Superblock → check state
     └─ Clean (0xCA) → normal mount
     └─ Dirty (0xDA) → scan inode-области + replay журнала;
        если скан без аномалий — auto-recovery в CLEAN+rw
        (INVFS_AUTO_RECOVER=0 отключает); с аномалиями → read-only,
        чинит invf-fsck -f
     └─ Recovery (0x52) → armed-дескриптор RSZ0 (resize, WP18)
        применяется идемпотентно

  2. Scan inode-области: живые записи (INOD/INO2), position-kill
     DELT-tombstone'ы; битая запись переживается (skip по rec_len+4)

  3. Replay журнала (только MAP/UNMAP) → in-memory L2P;
     bitmap читается с диска (не пересобирается)

  4. Дескрипторы блока 0: RDP0 (живой seal), CKP0 (живой чекпоинт
     sweep'а), RSZ0 (незавершённый resize)

  5. Set state → Clean (0xCA) при корректном размонтировании
     (vol_close: flush + компакция журнала)
```

Проектные элементы «replay от последнего CHECKPOINT-снапшота» и
«выборочная BLAKE3-проверка свёрнутых файлов при монтировании» не
реализованы (в журнале нет снапшотов; проверка данных — CRC32C на
сегмент при чтении + verify --deep по требованию).

### Graceful Shutdown

```
On unmount:
  1. Flush L2P journal
  2. Flush Bitmap
  3. Create CHECKPOINT (compact L2P + Bitmap snapshot)
  4. Set state → Clean (0xCA)
  5. Flush superblock
```

## Consistency Guarantees

| Scenario | Outcome |
|----------|---------|
| Crash during RAW write | Порванный хвост виден по CRC записи (torn-write guard): скан останавливается на нём, данные за хвостом не появляются |
| Crash during journal flush | Replay берёт префикс до первой битой записи (на запись CRC32C); fsync-данные переживают kill -9 (нога WP21/test-writepath) |
| Crash during Sweep | Живой CKP0: ничего не освобождено (retention-реестр `\x01reten`); `invf-rollback` возвращает pre-sweep состояние, либо следующий sweep auto-realize'ит чекпоинт (точка невозврата) |
| Crash during resize | RSZ0: до arming — том нетронут; после — apply идемпотентен при следующем открытии |
| Bit error in Shadow data | CRC32C сегмента не сходится при чтении → отказ чтения (не мусор); при живом seal'е (WP20) блок прозрачно восстанавливается из XOR-полосы (layer-1, ровно один битый блок на полосу; layer-2 RS — только через fsck). «Re-sweep из RAW-бэкапа» не существует: RAW освобождается при sweep'е — redundancy даёт только seal |

## Superblock State Transitions

```
Clean ──mount──→ Dirty ──auto-recovery (anomaly-free scan)──→ Clean
  ↑                  │
  │                  └──аномалии──→ read-only → invf-fsck -f → Clean
  └──unmount (vol_close: flush + journal compact + sb CLEAN)──┘
  Recovery (0x52) — только под armed RSZ0 (resize), разрешается в vol_open
```

## fsck / repair (2025-08): invf-fsck

`invf-fsck <image> [-f|--fix]` проверяет и чинит том:

- **CRC inode-записей**: битые записи отмечены (bad_recs); vol_open и
  invf-ls теперь ПЕРЕЖИВАЮТ битую запись (skip по rec_len+4), так что
  файлы ПОСЛЕ неё остаются доступными (раньше зона обрезалась).
- **AST↔L2P**: каждый живой сегмент обязан иметь L2P-запись
  (l2p_miss = данные потеряны — формат хранит в AST только индекс
  сегмента, физический блок живёт в журнале, так что L2P из AST
  не пересобирается).
- **Битмап-ремонт**: used = metadata-зона + pba из живого L2P;
  осиротевшие блоки (заняты, без ссылок) освобождаются, потерянные
  (есть ссылка, но свободны) восстанавливаются.
- **Журнал-компакт**: при -f журнал переписывается только из живых
  L2P-записей (мусор от мёртвых inode удаляется), суперблок → CLEAN.
- **l2p_miss не блокирует ремонт** (WP22a/H6): раньше `fsck -f`
  отказывался переписывать журнал/битмап, когда единственной проблемой
  был l2p_miss, — том оставался DIRTY и монтировался read-only навсегда.
  Теперь ремонт проходит: пострадавшие файлы перечисляются громко
  (имя + потерянные байтовые диапазоны сегментов, stderr — даже при -q:
  потеря данных никогда не молчит), журнал компактируется из живых
  маппингов, суперблок → CLEAN, том снова монтируется RW. Потерянные
  сегменты при этом НЕ восстанавливаются (физический адрес жил только в
  журнале, восстанавливать нечего): пострадавший файл продолжает честно
  отдавать EIO на каждом потерянном сегменте — данные мертвы, том жив.
  Код выхода fsck после такого ремонта остаётся 3 (issues found): потеря
  зафиксирована, не скрыта.

## WP22c: ошибка записи между commit и барьером (dm-flakey tier)

Linux-устройство открывается буферизованным (без O_DIRECT): pwrite
признаётся страничным кэшем, и ошибка всплывает только на барьере
(`fsync` в `blkio_flush` <- `vol_sync`). Окна dm-flakey (error-target
шторм + drop_writes) поймали два реальных дефекта этого класса
(механизмы и репро: `tools/flakey/FINDINGS.md`, F1/F2).

**F1 — append-курсор уезжал за несохранённые байты.** `vol_write_commit`
двигал `inode_area_pos` безусловно; storm-страницы умирали в writeback ->
нулевая дыра в append-only inode-области; записи ПОСЛЕ дыры валидны по
CRC, но недостижимы при следующем открытии (скан останавливается на
дыре), а fsck забирал их блоки как orphans. 124 fsync-подтверждённых
файла исчезали после ЧИСТОГО размонтирования. Теперь: `vol_open`
запоминает `inode_area_durable` (конец скана), каждый успешный
`vol_sync` двигает якорь на текущий хвост, а ЛЮБАЯ ошибка
`vol_flush`/`vol_sync` вызывает `vol_io_error_latch` (vol_crash.c):
курсор откатывается на последний подтверждённый барьер (без rescan --
чтение mid-шторма ненадёжно, а якорь и есть консервативная нижняя
граница; точный конец найдёт обычный скан при remount), том
защёлкивается в `needs_recovery` (любая дальнейшая мутация -- громкий
отказ до remount+fsck), и `vol_close` НЕ пишет CLEAN, так что следующее
монтирование идёт через recovery. Ошибка остаётся громкой ровно в том
вызове, который отказал. Журнал и битмап покрыты той же защёлкой: с
остановленными мутациями ни одна структура не расширяется за
несохранённые байты, а первый flush на ожившем устройстве переписывает
их целиком из таблиц в RAM. Детерминированный репро без dm:
`INVFS_SYNC_FAIL_AT=N` зануляет незабарьеренный хвост области и валит
N-й `vol_sync` (tools/test-flushfail.sh, leg A).

**F2 -- present-but-unreadable, которого fsck не видел.** Цепочка по
артефакту leg 5: fast-path rename (`vol_hardlink` + `vol_unlink_name`
под ОБЩИМ id) писал position-kill tombstone на позицию, которую
возвращал id-index, -- а после hardlink это позиция НОВОЙ записи: старое
имя не умирало ни в RAM, ни при replay. Sweep видел зомби-имя,
транскодировал его, и retire общего id выкидывал L2P-маппинги из-под
ЖИВОГО переименованного файла; fsck при этом резал записи по голому
id/pos (vol_open -- по (name,id)/(name,pos)), поэтому структурный чекер
сообщал «OK, l2p misses: 0» там, где verify --deep говорил CORRUPT, и
--repair никогда не включался. Теперь: `vol_unlink_name` бьёт по позиции
самого ИМЕНИ (`idx_get(name)->pos`); `vol_retire_inode` при живом
совпадении id (`idx_id_live`) -- tombstone-only, маппинги уходят с
ПОСЛЕДНЕЙ живой записью id; а fsck повторяет семантику name-index в
порядке записи (fsck_nameset: INOD перезаписывает имя, legacy DELT
убивает только при совпадении id, v2 DELT -- только при совпадении
позиции), так что l2p_miss считается ровно по тем записям, которые видит
read path (артефакт leg5: 0 -> 129 misses, файл назван, rc=3 -- честно
и слышно). Медленный путь rename (`rename_one`) тоже получил
crash-rule 2: маппинги копируются под новый id и сбрасываются
`vol_pre_record` ДО append'а записи (старый in-place re-key сохранялся
только к следующему flush -- запись могла стать durable без маппингов).

  Кроме того, soak довсказал ещё два разрыва того же класса: (1) два
  «сырых» обходчика записей вне ядра -- ls.c и сборочный обход в
  invf-sweep.c -- игнорировали v2 position-kill tombstone'ы совсем
  (комментарий там утверждал, что замещающий INOD всегда следует в том же
  combo; для голого kill от vol_unlink_name это неверно), так что
  переименованное-прочь имя оставалось «живым» в листинге и могло быть
  подметено sweep'ем -- теперь оба обхода повторяют семантику движка
  (kill только при совпадении позиции текущей версии имени); (2) rename,
  подтверждённый, но не забарьеренный, мог бесследно пропасть в окне
  ошибок (пара записей умирала в writeback после ACK) или быть порванным
  откатом WP21 (decapitation отбрасывает пост-контрольные записи
  wholesale -- копия умирает, tombstone умирает, источник воскресает).
  Поэтому invf_rename теперь барьерирует пару записей через vol_sync до
  ответа OK (окно ошибок валит rename громко, а не молча), а vol_rename
  отказывает (-4 -> EBUSY), пока жив чекпоинт sweep'а -- то же «сначала
  разрешите чекпоинт», что уже стоит на fsck -f.
