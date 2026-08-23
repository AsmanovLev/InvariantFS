# InvariantFS как RootFS в Linux

## Архитектура декомпрессоров

Декомпрессоры разделены на два уровня, чтобы ядро оставалось легковесным, а загрузка — надёжной.

### Уровень 1: Built-in (в VFS-драйвер)

Эти алгоритмы вкомпилированы в `invarifs.ko` и доступны всегда, в том числе на этапе initramfs:

| Алгоритм / Фильтр | Назначение | Размер кода |
|------------------|-----------|-------------|
| **ZSTD** | Исполняемые файлы, .so, конфиги rootfs | ~30 KB |
| **BCJ2** | x86-фильтр для ELF (branch/call conversion) | ~2 KB |
| **NONE** | Сырые блоки (без сжатия) | 0 |

BCJ2 — это не алгоритм сжатия, а простой байт-трансформ. Он сканирует x86 опкоды, конвертирует относительные branch/call смещения в абсолютные и разделяет цели переходов во вторичный поток. Кода там ~200 строк — тривиально включить в VFS-драйвер. Ядро уже имеет аналогичную реализацию для XZ (`lib/xz/xz_dec_bcj.c`).

**BCJ2 + ZSTD** — основной pipeline для системных ELF-файлов в rootfs.

### Уровень 2: Динамические модули ядра (.ko)

Загружаются через `request_module()` при первом обращении к файлу соответствующего типа:

```
/lib/modules/$(uname -r)/invarifs/
├── invarifs_core.ko        (VFS-драйвер + ZSTD + BCJ2 built-in)
├── invarifs_ppmd.ko        (декомпрессия PPMd — текст)
├── invarifs_ape.ko         (декомпрессия APE — аудио)
└── invarifs_jxl.ko         (декомпрессия JXL — изображения)
```

Когда VFS-драйвер видит в AST-рецепте `algo="ape"`:
1. Ищет зарегистрированный обработчик в ядре
2. Если не найден → `request_module("invarifs_ape")`
3. Ядро загружает `.ko` из `/lib/modules/`
4. Декомпрессия выполняется

**Критические системные файлы (ELF, .so) никогда не требуют PPMd/APE/JXL — ZSTD + BCJ2 достаточно.**

## AST-рецепты в ядре

AST-рецепт — это **не** "просто метаданные". Это основная структура данных, без которой VFS-драйвер не может выполнить ни одного `read()`. Он обязан быть в ядре.

### Почему не userspace

Если AST обрабатывается в userspace (через FUSE/WinFsp):

```
read()
  → VFS → context switch → userspace парсит AST
  → находит block_id → context switch → VFS
  → читает диск → декомпрессия → ответ
```

Каждый `read()` — минимум **два context switch**. Для `execve()` (читать ELF, парсить заголовки, читать сегменты, перемещать) — десятки переключений. Это убивает производительность.

### В ядре

VFS-драйвер хранит AST-рецепты в своём адресном пространстве:

```
read(inode, buf, len, offset):
  1. Драйвер загружает AST-рецепт из Metadata Zone (кэш в RAM)
  2. Парсит: какой block_id покрывает offset
  3. Читает блок из Shadow/RAW Zone
  4. Декомпрессия через built-in или request_module
  5. Копирует данные в buf пользователя
  6. Возврат (один context switch — стандартный syscall)
```

**Ни одного лишнего context switch.** Всё в ядре, как в ext4 или btrfs.

### AST Cache в ядре

AST-рецепты кэшируются в RAM ядра:

- **AST Cache**: LRU, до 10-50 MB
- **Data Block Cache**: LRU с приоритетами, до 1 GB 
- Всё в ядре, без копирования в userspace и обратно

### Формат AST для ядра

Для эффективного парсинга в ядре AST-рецепт хранится не как JSON, а как **бинарный flat buffer**:

```
struct ast_recipe_header {
    uint32_t version;
    uint32_t file_size;
    uint16_t num_blocks;
    uint16_t num_children;  // для вложенных (ZIP, FLAC, MP3)
    uint32_t checksum;      // CRC32C рецепта
};

struct ast_block_entry {
    uint64_t file_offset;   // смещение в оригинальном файле
    uint64_t length;        // длина диапазона
    uint32_t zone:2;        // 0=raw, 1=text, 2=binary
    uint32_t algo:6;        // 0=none, 1=zstd, 2=ppmd, 3=ape, 4=jxl
    uint32_t block_id:24;   // номер блока в зоне
    uint32_t block_offset;  // смещение внутри блока
};
```

Никакого JSON-парсера в ядре. Просто загрузить бинарную структуру и читать поля.

## Загрузка через initramfs

```
1. BIOS/UEFI → GRUB → ядро + initramfs (сжатый tmpfs в RAM)
2. Ядро распаковывает initramfs в tmpfs и выполняет /init
3. /init внутри initramfs:
     a. Загружает invarifs_core.ko (ZSTD + BCJ2 + AST-парсер)
     b. mount -t invarifs /dev/sda2 /mnt/root
        └─ VFS-драйвер читает Superblock, Metadata Zone
        └─ AST-рецепты /sbin/init, /lib/systemd/...
           ─ Если algo=zstd+bcj2 → декомпрессия через built-in
           ─ Если algo=none → прямое чтение из RAW/Shadow
     c. switch_root /mnt/root /sbin/init
        └─ Ядро читает /sbin/init через VFS-драйвер (AST в ядре)
        └─ init распакован (BCJ2+ZSTD) → выполняется как PID 1
4. systemd стартует, загружает остальные модули (.ko):
     modprobe invarifs_ppmd
     modprobe invarifs_ape
     modprobe invarifs_jxl
5. Sweep Worker запускается как systemd-сервис
```

**Sweep Worker до switch_root не запускается** — это нормально. VFS-драйвер с ZSTD + BCJ2 + AST в ядре достаточен для полной загрузки системы.

## NoSweep (Out-of-Policy) флаг

Флаг `INVARIFS_NOSWEEP` (аналог `chattr +C` для Copy-on-Write в ext4) запрещает Sweep-воркеру сжимать файл.

```
chattr +S /boot/vmlinuz   # ядро не сжимать
chattr +S /sbin/init      # альтернатива, если не хотим ZSTD
chattr +S /etc/fstab      # критические конфиги
```

**Когда использовать:**

| Сценарий | Пример |
|----------|--------|
| Загрузочные файлы (альтернатива ZSTD) | `/boot/vmlinuz`, `/boot/initramfs` |
| Файлы с гарантией нулевой задержки | QEMU-образы, контейнеры |
| Уже сжатые данные (чтобы не тратить CPU) | `/usr/share/fonts/*.gz` |
| Отладка / форензика | Логи, core dumps |

**Файлы с флагом NoSweep:**
- Навсегда остаются в RAW Zone
- Не дублируются в Shadow Space
- Игнорируются Sweep-воркером
- AST-рецепт ссылается на RAW blocks с `algo=none`

**Важно:** NoSweep не обязателен для boot. VFS-драйвер с ZSTD + BCJ2 built-in и AST в ядре справляется с rootfs без него. Флаг — для явного контроля над сжатием конкретных файлов.

## BCJ2-фильтр в корне

BCJ2 вкомпилирован в `invarifs_core.ko`:

- **Объём кода**: ~200 строк C, ~2 KB в бинарнике
- **Логика**: сканирование x86 опкодов (E8/E9/0F8x...), сложение/вычитание смещений
- **Прецедент**: ядро Linux уже включает BCJ-фильтр в `lib/xz/xz_dec_bcj.c`
- **Зависимости**: не требует внешних библиотек

Pipeline для rootfs ELF: `RAW/Shadow → BCJ2 → ZSTD → ELF загрузчику`

BCJ2 не используется с PPMd, APE или JXL — он имеет смысл только для x86 машинного кода перед ZSTD/LZMA.

## VFS-драйвер ядра, не FUSE

FUSE для корневой ФС приведёт к:
- Проблемам с правами (root не сможет читать файлы пользователя без `allow_other`)
- Огромным задержкам (каждый syscall — context switch в userspace)
- Невозможности использовать `execve()` из FIFO/PIPE на FUSE

**Решение:** VFS-драйвер уровня ядра, как ext4 / btrfs. С AST-парсером и кэшем прямо в ядре.

## Типы файлов для rootfs

AST-рецепт поддерживает специальные типы. В бинарном формате:

```
struct ast_inode {
    uint8_t type;        // 0=regular, 1=symlink, 2=blockdev, 3=chardev, 4=fifo, 5=socket
    uint32_t uid, gid;
    uint16_t mode;
    uint64_t size;
    
    union {
        char target[256];     // symlink target
        struct { uint32_t major, minor; } dev;  // device numbers
        struct ast_recipe regular;  // regular file blocks
    };
};
```

- **symlink** — target хранится в inode, 0 байт в зонах
- **blockdev/chardev** — dev_t в inode
- **fifo/socket** — только inode
- **regular** — AST recipe с блоками

## initramfs сборка

```
INITRAMFS_FILES := \
    init \
    mount.invarifs \
    invarifs_core.ko       # core: AST + ZSTD + BCJ2
    busybox

gen_initramfs:
    find $(INITRAMFS_DIR) -print0 | cpio -0 -o -H newc | gzip > initramfs.img
```

Модули PPMd/APE/JXL в initramfs не нужны — они загрузятся после старта systemd.

## Атрибуты для корневой ФС

| Атрибут | Описание |
|---------|----------|
| Read-only режим | Для fsck и восстановления |
| Device-mapper | Для dm-crypt/LUKS поверх InvariantFS |
| Quotas | Для multiuser систем |
| **NoSweep** | `chattr +S` — запрет сжатия для конкретных файлов |

## Совместимость с systemd

systemd требует:
1. **Fanotify** — мониторинг изменений (поддержка в VFS-драйвере)
2. **Inotify** — стандартный VFS
3. **Cgroups** — не зависят от ФС
4. **Mount namespace** — стандартный VFS
5. **Request Module** — `request_module("invarifs_ape")` для автозагрузки декомпрессоров
