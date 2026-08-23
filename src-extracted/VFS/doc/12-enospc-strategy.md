# ENOSPC — Обработка нехватки места

## Проблема

RAW Zone имеет фиксированный размер. Если скорость записи превышает скорость Sweep-воркера, RAW переполняется — новые `write()` получат `ENOSPC`, хотя в Shadow Space ещё есть место.

## Решение: Watermarks + Throttling

### Watermarks

```
RAW Zone заполнение:
  0%  ░░░░░░░░░░░░░░░░░░░░░░░░░░░  100%
      │           │           │
      │         80%          95%     98%
      │           │           │       │
      ▼           ▼           ▼       ▼
   Normal    High Priority  Throttle  Sync Sweep
```

| Порог | Режим | Действие |
|-------|-------|----------|
| 0-79% | **Normal** | Sweep в фоне / on-demand |
| 80-94% | **High Priority** | Sweep получает приоритет CPU + IO |
| 95-97% | **Throttle** | write() замедляется (потоки ставятся на wait) |
| 98%+ | **Sync Sweep** | write() блокируется до освобождения блока |

### Emergency: Read-Only Mode

Если RAW переполнен (99%+), и Sync Sweep не справляется:

```
write() → RAW full (99%)
  → Emergency: переключение в Read-Only режим
  → Все write() возвращают EROFS (Read-only file system)
  → Sweep Worker продолжает работать на максимальном приоритете
  → Как только освобождается место → возврат в RW
```

**Read-Only — единственный безопасный сценарий.**

Запись напрямую в Shadow Space (algo=none) создаёт проблему: такой блок не будет "дожат" Sweep-воркером, и потребуется дополнительный трекинг (needs_sweep флаг), который усложняет архитектуру и расходует место в AST/L2P. Read-Only режим чище: система явно сообщает о проблеме, не создавая "мусорных" данных.

### Резервирование для root (5%)

Как в ext4: 5% объёма зарезервировано для `uid=0`. Это гарантирует, что:
- Sweep Worker (запущенный от root) сможет писать метаданные и журнал
- Системный администратор сможет войти и почистить диск
- Emergency Fallback не заблокируется из-за нехватки блоков

## Размер RAW Zone при форматировании

`mkfs.invarifs --raw-zone-size=<N>`

| Сценарий | Рекомендуемый размер RAW |
|----------|-------------------------|
| Медиа-архив (редкая запись) | 10% |
| Общего назначения | 20% |
| Разработка (CI/CD, частые коммиты) | 35-40% |

## Поведение приложения

Приложение получает:
- **ENOSPC** — только когда диск физически заполнен (RAW + Shadow + Metadata).
- **EAGAIN** / **EWOULDBLOCK** — при throttling (write() может быть повторён).
- **EDQUOT** — если превышена квота пользователя.

Пользователь никогда не видит "No space left" при наличии свободного места в Shadow Space — система использует Emergency Fallback.

## Реализация (2025-08): резерв + hard-min + READONLY

- Суперблок v0.9 (вне checksum, 0 на старых образах): `reserved_blocks`
  (1/128+64), `hard_min_blocks` (1/1024+16), `vol_flags` (bit0=READONLY).
- `alloc_blocks(zone, n, use_reserve)`: обычные записи не трогают
  резерв+флор; транскоды (sweep) — могут опустошить резерв, но не флор.
  free-кэш в `invfs_volume.free_blocks` (vol_count_free при open, +/−
  при alloc/free).
- **Атомарный ENOSPC**: vol_create_file отказывает ДО записи, если
  весь файл (worst-case несжатый) не влезает выше резерва+флора;
  частично записанные сегменты освобождаются (никаких осиротевших
  блоков до fsck).
- **RAW→SHADOW spill**: при исчерпании RAW-зоны свежие записи пишутся
  в SHADOW (zone=BINARY, тот же LZ4-сегмент) — записи не блокируются
  при живом SHADOW.
- **READONLY**: при free <= hard_min флаг пишется в суперблок
  (vol_flush теперь пишет sb), все write-пути (delete/mkdir/rmdir/raw/
  create/sweep) возвращают EROFS/ENOSPC. Dokan: STATUS_DISK_FULL в
  WriteFile/Close-flush (проверка по free-кэшу до буферизации — .NET
  видит ошибку на WriteFile, а не молча теряет запись). FUSE: -ENOSPC
  в write, -EROFS в create/mkdir/unlink.
