# InvariantFS

Семантическая файловая система с инвариантом точного совпадения битов:
каждое сжатие проверяется декомпрессией и сравнением через BLAKE3
до подтверждения; контейнеры (ZIP/TAR) хранятся в оригинальном виде
с доступом к компонентам по запросу; транскодирование применяется только
там, где доказана точность для данного файла.

> **Записанное возвращается бит-в-бит идентично. Всегда.**

Статус: **экспериментальная, загружаемая**. Gentoo/OpenRC работает
с корневой файловой системой на InvariantFS (см. `tools/mkdisk.sh`, `vm/`).
Текущий релиз: **v0.2.0**.

## Состояние: экспериментальное

**Не тестировалась на реальном оборудовании. Используйте на свой риск. Делайте резервные копии.**

## Архитектура

### Зоны

| Зона | Назначение |
|---|---|
| **RAW** | Линейная область для записи. Новые данные попадают сюда немедленно, LZ4-сжатие, быстро и дёшево. |
| **Shadow** | Оптимизированное хранилище, разделено на **текстовую** и **бинарную** зоны для консолидации однородных данных. |
| **Metadata** | Суперблок, битмап блоков, L2P-журнал, область inode. |

Запись отделена от сжатия. Данные попадают в RAW на скорости записи;
**sweep-воркер** затем перемещает их в Shadow с нужным кодеком.
Анализ никогда не попадает на путь записи.

### AST-рецепт

Каждый файл несёт рецепт, отображающий диапазоны байтов *оригинала*
на тройки `(зона, блок, смещение)`. Сегменты хранятся как
`[4B csize][4B crc32c][данные]`.

- **Частичные чтения дешёвы.** Чтение тегов FLAC затрагивает только
  текстовые сегменты; аудиоблоки не декомпрессируются.
- **Контейнеры вкладываются.** Рецепт — дерево, поэтому zip-в-tar
  отражает реальную структуру (глубина до 16).

### Журнал

Append-only L2P-журнал (`MAP` / `UNMAP` / `SWEEP` / `CHECKPOINT`) сохраняет
маппинг логико-физических адресов. Монтирование восстанавливается от
последнего чекпоинта. Суперблок содержит байт состояния (`CLEAN` / `DIRTY` /
`RECOVERY`).

### Дедупликация

Контент-адресация через BLAKE3. Идентичные блоки хранятся один раз
с подсчётом ссылок.

## Сборка

```sh
make                # все инструменты -> bin/
make bin/invf-fuse  # один файл
```

Требуется `gcc`, `libfuse3-dev`, `zlib1g-dev`, `libzstd-dev`.
Бандлированные кодеки: zstd, lz4, miniz, blake3, flacx, rs, ppmd, bcj_x86.

## Инструменты

| Инструмент | Назначение |
|---|---|
| `invf-mkfs <img> [gb]` | Форматирование тома. Переменная `INVFS_META_FRAC=N` |
| `invf-fsck [-f] <img>` | Проверка и восстановление |
| `invf-fuse [-o opts] <vol> <mnt>` | FUSE-демон (см. ниже) |
| `invf-ls <img>` | Список файлов |
| `invf-cat <img> <name> [out]` | Извлечение файла |
| `invf-cp <img> <file> [name]` | Копирование файла в том |
| `invf-stat <img> [--files]` | Инспектор пространства с зонами |
| `invf-import <vol> <dir>` | Массовый импорт дерева (~50k файлов/сек) |
| `invf-sweep <img> [--dry-run] [--seal]` | Офлайн-sweep (RAW -> Shadow, транскоды) |
| `invf-stats <img>` | Полная статистика с коэффициентами по классам |
| `invf-resize <img> ...` | Офлайн-изменение размера |
| `invf-rollback <img>` | Откат последнего sweep по чекпоинту |
| `invf-verify [--deep] <img>` | Проверка целостности |
| `invf-migrate-v2 <img>` | Конвертация формата v1 -> v2 |
| `invf-zip list\|get <img> <zip>` | Инспекция ZIP-контейнера |
| `invf-zip get <img> <zip> <member> <out>` | Извлечение компонента ZIP |
| `meta_probe <img> <name>` | Разработка (мутирует) |
| `meta_probe <img> --heat <name>` | Read-only дамп тепла/класса |

## FUSE-демон

```sh
invf-fuse [-f] [-o opt[,opt]] <volume> <mountpoint>
```

**Опции монтирования:**

- `attr_timeout=0,ac_attr_timeout=0` — без кеширования атрибутов (по умолчанию)
- `raw_watermark=<pct>` — запуск фонового sweep при заполнении RAW (WP26)
- `arc_limit=<MB>` — лимит кеша декодированных юнитов (по умолчанию 256)
- `dec_mem_limit=<mpl>` — лимит памяти декомпрессии

**Фоновый sweep:**

- По умолчанию выкл.: `INVFS_SWEEP_INTERVAL=<сек>`
- Вручную: `kill -USR1 $(pidof invf-fuse)` или
  `setfattr -n user.invfs.sweep -v 1 /`

**Контрольное пространство в корне примонтирования:**

```sh
getfattr --only-values -n user.invfs /        # Сводка по RAM
getfattr --only-values -n user.invfs.stats /  # Счётчики + зоны
```

**Права (WP-A):** mode/uid/gid проверяются демоном. POSIX ACL хранятся как
`system.posix_acl_*` xattr-блобы и учитываются при проверке доступа.

## Загружаемая VM

```sh
tools/mkdisk.sh <volume.img>     # GPT: ESP + том как p2
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -nographic \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vm/OVMF_VARS.fd \
  -drive file=vm/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0
```

Цепочка: OVMF -> GRUB (ESP) -> initramfs (busybox + fuse.ko + invf-fuse)
-> switch_root в Gentoo. Полное руководство: `docs/GENTOO-INSTALL.md`.

## Тестирование

```sh
make test    # 4722 юнит-проверки (ядро + кодеки + рецепты + CLI)
make e2e     # 38 end-to-end FUSE-тестов
make flakey  # хаос/стресс (torn sweep, error storm, compact-flip)
```

Gate-тесты в `tools/`:

| Скрипт | Покрытие |
|---|---|
| `bench-gate-b.sh` | Бенчмарк v2 vs v1 |
| `test-gate-c.sh` | ENOSPC-стресс (porог, гонка, sweep-interlock) |
| `test-gate-d1.sh` | ACL/xattr (4096 граница, listxattr, полный объём, параллельное создание) |
| `test-gate-d2.sh` | Параллельная запись (один файл, много файлов, sweep при записи) |

## Кодеки

**Универсальные:** LZ4 (путь записи), ZSTD, PPMd (текстовая зона), BLAKE3 (дедуп).

**Семейства бит-точных транскодов:**

| Тег | Семейство | Метод воспроизведения |
|---|---|---|
| `FLACR` | FLAC | PCM как APE + рецепт восстановления FLAC-битстрима |
| `TARR` | TAR | Компоненты разделены; `IVFT`-рецепт восстанавливает заголовки, выравнивание |
| `GZR` | gzip | Deflate реплицируется перебором параметров zlib |
| `PNGR` | PNG | Lossless JXL + `IVPN`-рецепт для фильтров строк и параметров deflate |
| `EXER` | PE/EXE | x86 BCJ + ZSTD, рецепт воспроизводит оригинал |

Где репликация не удаётся, файл остаётся в исходном виде.

## Известные ограничения

- Барьеры записи на операцию (WP3) ещё не реализованы; контракт — fsync/close.
- mmap чтение/запись через FUSE writeback cache (WP4a).
- Кодеки: JPEG->JXL требует `cjxl`/`djxl`; FLAC требует `mac` + `ffmpeg`;
  отсутствующие инструменты = файл остаётся RAW.
- Восстановление от бит-рота только через `invf-sweep --seal` (WP20).
- Лимит xattr: 4096 байт на inode (`INVFS_META_XATTR_MAX`).
- Только POSIX ACL (NFSv4 не поддерживаются).
- Пространства `security.*`/`trusted.*` не поддерживаются.

## Структура

```
src/   движок + CLI (C11): core/ codecs/ recipes/ cli/
tools/                   скрипты сборки, конфигурация гостя, тесты
packaging/               install.sh, debian/, RPM, Arch, systemd, man pages
docs/                    руководство по установке Gentoo
vm/                      initramfs, компоненты диска
```

## Лицензия

GPL-2.0-only. См. [LICENSE](LICENSE).
