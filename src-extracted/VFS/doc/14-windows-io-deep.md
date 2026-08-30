# Windows I/O — Детали реализации

> **Статус: EOL / stale (исторический контекст).** Windows-порт заморожен
> (см. 09-windows-port.md): вся работа WP10–WP21 — только Linux.
> Документ оставлен как справка по Windows I/O (OVERLAPPED/IOCP,
> WRITE_THROUGH, TRIM); единственный платформо-независимый живой раздел —
> про ZNS-совпадение архитектуры (ниже).

## Асинхронный I/O через OVERLAPPED

**Проблема:** `SetFilePointerEx + ReadFile` не thread-safe — указатель смещения глобален для хэндла.

**Решение:** Использовать `ReadFile`/`WriteFile` со структурой `OVERLAPPED`.

```c
// Правильный способ чтения с диска в Windows
HANDLE hDisk = CreateFile(
    L"\\\\.\\PhysicalDrive0",
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,  // <-- OVERLAPPED!
    NULL
);

// Каждая операция I/O имеет свою OVERLAPPED структуру
// со своим смещением — thread-safe!
OVERLAPPED ov = {0};
ov.Offset = (block_address * 4096) & 0xFFFFFFFF;
ov.OffsetHigh = (block_address * 4096) >> 32;
ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

void* buffer = _aligned_malloc(4096, 4096);
ReadFile(hDisk, buffer, 4096, NULL, &ov);

// Ждём completion через IOCP или событие
WaitForSingleObject(ov.hEvent, INFINITE);
GetOverlappedResult(hDisk, &ov, &bytesRead, FALSE);
```

## FILE_FLAG_WRITE_THROUGH для журнала

Журнал L2P требует гарантированной записи на диск (минуя кэш):

```c
HANDLE hJournal = CreateFile(
    journalPath,
    GENERIC_WRITE,
    FILE_SHARE_READ,
    NULL,
    OPEN_ALWAYS,
    FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
    NULL
);
```

- `FILE_FLAG_NO_BUFFERING` — обход системного кэша
- `FILE_FLAG_WRITE_THROUGH` — обход кэша контроллера (force unit access)

Для HDD это снижает производительность, но для журнала целостность важнее скорости.

## TRIM / Unmap

При освобождении блоков (Sweep, удаление файлов) — сообщаем SSD, что блоки больше не нужны:

```c
// Освобождение диапазона блоков
FILE_STORAGE_TIER_MEDIA_TYPE mediaType = FILE_STORAGE_TIER_MEDIA_TYPE_SSD;
DEVICE_MANAGE_DATA_SET_ATTRIBUTES dsa = {0};
dsa.Size = sizeof(dsa);
dsa.Action = DeviceDsmAction_Trim;

DEVICE_DATA_SET_RANGE range;
range.StartingOffset = block_address * 4096;
range.LengthInBytes = block_count * 4096;

DEVICE_MANAGE_DATA_SET_ATTRIBUTES_OUTPUT output;
DWORD bytesReturned;

DeviceIoControl(
    hDisk,
    IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES,
    &dsa, sizeof(dsa),
    &output, sizeof(output),
    &bytesReturned,
    NULL
);
```

TRIM выравнивает срок службы SSD и предотвращает деградацию скорости записи на заполненных накопителях.

## Формат адресации диска

| Тип | Формат пути | Описание |
|-----|------------|----------|
| Физический диск | `\\.\PhysicalDriveN` | Прямой доступ к диску N |
| Том | `\\.\X:` | Доступ к тому X (только если не смонтирован) |
| Раздел | `\\.\GLOBALROOT\Device\Harddisk0\Partition4` | По GUID или номеру раздела |

Для InvariantFS используем `\\.\PhysicalDrive0` + смещение, вычисленное из GPT.

## ZNS SSD — аппаратный матч

InvariantFS архитектура концептуально повторяет парадигму ZNS SSD:

| InvariantFS | ZNS SSD |
|----------|---------|
| RAW Zone (линейная запись) | Зона с последовательной записью |
| Sweep переезд RAW → Shadow | Zone Reset после освобождения |
| Батчи Shadow (послед. запись) | Последовательные зоны |

**Правила для ZNS-режима:**
1. RAW Zone и батчи Shadow Space размещаются в терминах аппаратных зон
2. Sweep-воркер сбрасывает (Zone Reset) зону RAW после переезда данных в Shadow
3. Нулевой Write Amplification на уровне контроллера
4. TRIM не нужен — Zone Reset атомарный

**Итог:** Идеальный матч — исключает GC накопителя, увеличивает срок жизни SSD.

## Аппаратное ускорение (опционально)

- **Intel QAT** — асинхронные движки ZSTD/LZ4, поддержка в ядре (`crypto/qat`). Sweep и Read-путь могут выгружаться на QAT.
- **NVIDIA NVJPEG** — для ML-датасетов декомпрессия JPEG/JXL прямо в GPU-память (минуя RAM).
- **SPDK** — для NVMe с задержкой ~10-50мкс: zero-copy чтение из userspace (Linux). В Windows аналог — **DirectStorage API**.

## Планировщик I/O (IOCP)

Для многопоточного доступа:

```c
// Создаём Completion Port
HANDLE hIOCP = CreateIoCompletionPort(hDisk, NULL, 0, numThreads);

// Пул потоков обрабатывает завершённые I/O
while (GetQueuedCompletionStatus(hIOCP, &bytes, &key, &ov, INFINITE)) {
    // Обработка завершённой операции
    ProcessCompletedIO(key, ov, bytes);
}
```

IOCP позволяет сотням потоков читать/писать диск без блокировок.

### Важно: OVERLAPPED в куче

Структура `OVERLAPPED` **обязательно** должна быть выделена в куче (heap), а не на стеке:

```c
// ❌ НЕПРАВИЛЬНО — на стеке
OVERLAPPED ov = {0};
ReadFile(hDisk, buffer, 4096, NULL, &ov);
// Поток может завершиться до завершения I/O → UB / AV

// ✅ ПРАВИЛЬНО — в куче
OVERLAPPED* ov = malloc(sizeof(OVERLAPPED));
memset(ov, 0, sizeof(*ov));
ov->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

ReadFile(hDisk, buffer, 4096, NULL, ov);
// В callback: free(ov) после обработки
```

Поток, инициировавший I/O, может завершиться до того, как операция физически выполнится. Стековая структура будет перезаписана или освобождена. Для концептуального прототипа это не критично, но в production-коде — обязательно heap-аллокация.
