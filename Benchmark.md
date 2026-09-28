# InvariantFS Benchmark — Silesia corpus

> **SUPERSEDED — read this before quoting any number below.**
> This run is dated 2025-08-25 and predates Meta-v3 entirely (the `v0.5.0`
> default format), WP10/WP14 batching, and the v3 COW base-page accounting.
> It was taken without `INVFS_META_FRAC` set, and **no reclaim-gate
> condition was recorded**, so its `2.95x` is not a current figure and
> must not be compared against a gated run. Per AGENTS.md §1.7 a claim
> that cannot be pointed at in the tree is not restated, so this WP did
> **not** substitute a replacement ratio: the current Silesia numbers do
> not exist in the tree, and the live benchmark document is
> `docs/benchmarks/`. Two notes specific to the text below: the comment
> «RAW-оригиналы свип освобождает» describes v2 behaviour — on v3 the
> builtin container lanes do not release the superseded segments at lane
> time (see `AGENTS.md` §2.5) — and `bench-invfs.sh` was never written.
> The raw table is kept below as a historical record, otherwise unmodified.

Дата: 2025-08-25. Корпус: Silesia, 12 файлов, **211 938 580 B** (202.16 MiB).
Хост: i5-10500H (12 тредов), Fedora 44, том/образы в tmpfs. Все числа — только
полезные байты данных (метаданные ФС исключены; для INVFS — занятые блоки зон
данных по bitmap).

## Сжатие (ratio = logical / on-disk)

| Система | Настройки | On-disk | Ratio | Запись | Распаковка |
|---|---|---:|---:|---|---|
| SquashFS | xz, -b 1M, dict 100% (макс) | 52 916 224 B | **4.01x** | 7.9 s wall / 85.7 s CPU (MT) | n/a¹ |
| SquashFS | zstd lvl19, -b 1M | 57 077 760 B | 3.71x | 10.1 s wall / 108.9 s CPU (MT) | n/a¹ |
| **InvariantFS** | import LZ4 + sweep ZSTD-19 (по файлу) | 71 884 800 B | **2.95x** | 0.7 s + 61.7 s sweep (1 поток) | **0.46 s** cold² |
| EROFS | lz4hc lvl9 (из ранней серии, синт. корпус) | — | ~1.73x³ | — | — |

¹ squashfs — read-only образ, «распаковка» = монтирование.
² FUSE-маунт + tar всего корпуса после drop_caches (sudo). Тёплый ≈ 0.2 s.
³ на другом корпусе, прямой сравнительной цифры нет.

## Эталоны архиваторов (тот же корпус)

| Инструмент | Размер | Ratio | Время |
|---|---:|---:|---|
| zpaq m5 (официальный silesia.zpaq) | 47 753 698 B | 4.44x | — |
| xz -9 (tar stream) | ~52 MB | ~4.0x | минуты, MT |
| zstd -19 (tar stream) | ~57 MB | ~3.7x | ~10 s MT |

## Комментарии

- **Разрыв с xz (4.01x vs 2.95x)** объясняется гранулярностью: INVFS жмёт
  каждый файл ЦЕЛИКОМ одним ZSTD-19 потоком (окно ограничено файлом), тогда
  как xz на tar-стриме видит весь корпус как один 212MB поток и словарь
  100% от блока 1M работает поверх общего контекста. Главный резерв —
  cross-file контекст (общий словарь/сессия свипа) и более сильный уровень
  на крупных файлах.
- **Время записи**: у INVFS это две фазы — import 0.7s (LZ4, мгновенный
  доступ) и фоновый sweep 62s (ZSTD-19, однопоточно). Sweep легко
  распараллеливается по файлам (сейчас 1 поток) → реалистично ~8-10s MT,
  что сравнимо с mksquashfs.
- **Чтение**: 0.46s cold на весь корпус через FUSE (~440 MB/s) — узкое место
  не декомпрессия, а round-trip'ы; recipe-cache (WP4d) должен поднять выше.
- Метрика INVFS честная: used-блоки bitmap'а обеих зон данных после свипа;
  RAW-оригиналы свип освобождает.

Повтор: `/var/tmp/bench/harness/fs-bench.sh` (btrfs/erofs), INVFS-легa —
команды в истории сессии. Скрипт `bench-invfs.sh` **не написан**: ни
`tools/`, ни `tests/` в дереве нет, так что этот прогон невоспроизводим из
репозитория.

> Этот прогон (2025-08-25, 2.95x) — до WP10/WP14. Актуальные числа на том
> же корпусе и чтение vs btrfs жили в отдельном документе серии B30+
> (Silesia end-to-end, батчинг, B34–B36) под `src/doc/`. Он удалён вместе
> со всем `src/doc/` в d559a8b: 18 документов v2-эпохи (L2P-маппер,
> CKP0/`\x01reten`), 15 из которых вообще не упоминали Meta-v3 — см.
> AGENTS.md §1.7. Читать оттуда: `git show d559a8b^:src/doc/16-benchmarks.md`
> (1137 строк). Живой бенчмарк-док — `docs/benchmarks/`.
