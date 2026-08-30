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
