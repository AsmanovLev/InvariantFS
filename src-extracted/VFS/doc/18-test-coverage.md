# Спецификация покрытия тестами (Test Coverage Specification)

**InvariantFS** · актуально: 2026-08-31 (HEAD=14bb2d72) · платформа: **Linux**
(Windows-наборы `tests*.ps1` — EOL вместе с портом, см. §5).

---

## 1. Сводка

| Уровень | Команда | Что внутри | Окружение |
|---|---|---|---|
| unit | `make test` | invf-arctest (4467 проверок), invf-blkio_test (86), invf-codec_test (169) | без образов, секунды |
| e2e | `make e2e` | **21 набор** `tools/test-*.sh` (см. §2) | tmpfs-образы в `/dev/shm` |
| fuzz | `make fuzz` + `tools/fuzz/` | бинарь `invf-fuzz` (чистые/парсинговые слои, ручной прогон) + волна fuzz-инструментов (см. §3) | `/dev/shm` |
| flakey | `make flakey` | **WP22b** `tools/test-flakey.sh` + `tools/flakey/` (см. §3а) — потеря питания/нестабильное устройство через dm-flakey | loop+dm-устройство, root, минуты; **в `make e2e` не входит** |

`make e2e` собирает всё (`all`) и гоняет наборы подряд. Из комментария в
Makefile: test-jxl требует установленных `cjxl`/`djxl`; паки ФС-образов
пропускаются, если нет соответствующего mkfs/инструментария фикстур.

**Главный инвариант, который защищают ВСЕ тесты:**
> **1:1 — записанное возвращается байт-в-байт.** Контейнеры хранят
> оригинал; транскод применяется только там, где bit-exact доказано;
> иначе — оригинал.

Сборка перед запуском: `make` (корень репозитория, бинари в `bin/`).

---

## 2. e2e-наборы (`make e2e`, 21 штука, в порядке прогона)

| # | Набор | Что покрывает |
|---|-------|---------------|
| 1 | `test-textzone.sh` | WP10 Text Zone: cross-file PPMd-батчи, сортировка, GC мёртвых батчей, класс-флаги, сокрытие `\x01tzb` из листингов |
| 2 | `test-dedupe.sh` | WP12(h) офлайн-дедуп сегментов: merge дублей, выживший bit-exact, пропуск TEXT/целофайловых |
| 3 | `test-heat.sh` | WP19: heat-счётчики в L2P pad (rheat/wheat), распад, промоушн горячих членов, лестница профилей |
| 4 | `test-seal.sh` | WP20/20b: XOR-полосы чётности, переменный k, дескриптор RDP0, dirty-stripe reseal, RS layer-2, восстановление блока по полосе |
| 5 | `test-jxl.sh` | JPEG→JXL через jxl.codecpack (WP16e): bit-exact guard, retry-нога апгрейда (WP12(b), «6/6 bit-exact after upgrade») |
| 6 | `test-rawimg.sh` | WP13: codecpack exec-путь + raw_image (DICOM/PNM/BMP/TIFF → lossless JXL) |
| 7 | `test-binbatch.sh` | WP14a: бинарные батчи (zone=TEXT, algo=ZSTD/ZSTD_BCJ), семейная сортировка, отсутствие утечек владельца |
| 8 | `test-conbatch.sh` | WP14b M1: батчинг членов контейнеров (`!`-сиблинги) |
| 9 | `test-exercarve.sh` | WP14b M2: exe-as-container (EXER) — вырезание встроенных JPEG/PNG, rebuild-гард |
| 10 | `test-containerpack.sh` | WP16a ABI (enumerate/extract/strip/rebuild) + WP16b ABI v1.1 (seekable `!mbrmap`, DEFER_ENOSPC, профили) на фикстурном паке splt_test (algo 40) |
| 11 | `test-rawdisk.sh` | rawdisk пак (algo 16): MBR/EBR/GPT-разбиение образов дисков |
| 12 | `test-ext4fs.sh` | ext4fs пак (algo 17) |
| 13 | `test-fatfs.sh` | fatfs пак (algo 18): FAT12/16/32 + exFAT → per-file члены |
| 14 | `test-xfs.sh` | xfs пак (algo 19) |
| 15 | `test-ntfs.sh` | ntfs пак (algo 20) |
| 16 | `test-vdi.sh` | vdi пак (algo 21): динамические VDI; вложенная композиция (vdi→rawdisk→GPT) |
| 17 | `test-resize.sh` | WP18 invf-resize: grow/shrink, RSZ0 roll-forward, идемпотентный apply |
| 18 | `test-rollback.sh` | WP21: чекпоинт CKP0, реестр удержания `\x01reten`, invf-rollback, крах-ноги (INVFS_ROLLBACK_ABORT_AT) |
| 19 | `test-p7z.sh` | p7z пак (algo 23): 7z-архивы со stored-членами |
| 20 | `test-fuzz.sh` | редуцированный fuzz-прогон для e2e: bitflip×60, opseq 2×150, packfuzz×100/пак, manifest×400 (~25 с) |
| 21 | `test-writepath.sh` | WP4a mmap (все формы хранения) + WP4b streaming/ranged write, fsync-барьер, kill -9 крах-нога (legs A-F) |

Отдельно от `make e2e` живёт `tools/test-qcow2.sh` (qcow2 пак, algo 22):
не wired в цель — нужен внешний инструментарий (qemu-img/qemu-io;
в packfuzz-ноге test-fuzz qcow2 участвует при их наличии).

---

## 3. Fuzz-волна (`tools/fuzz/`, детерминированная, `--seed`)

| Инструмент | Что делает |
|---|---|
| `bitflip.py` | mkfs→import→sweep (каждый 8-й образ с `--seal`)→ одна мутация (flip/zero-4K/truncate/поддельные RDP0/RSZ0/CKP0 с валидными CRC) взвешенно по регионам → fsck + verify --deep + побайтовая сверка всех файлов |
| `opseq.py` + `ophelper.c` | случайные программы create/rewrite/delete/sweep/fsck/verify/stat/cat через настоящие CLI; теневое эталонное дерево, после каждого sweep: fsck rc==0, verify rc==0, `invf-ls` == эталону (без утечек `\x01`-владельцев) |
| `packfuzz.py` | мутации фикстур 7 паков (rawdisk…qcow2); контракт rc∈{0,1,3}, без сигналов и зависаний; map сверяется с локальным MRMP-оракулом |
| `fuzz_manifest.c` | 400+ порченых манифестов через настоящий путь регистрации паков (exec-хуки заглушены) |
| `test-fuzz.sh` | редуцированный срез всего этого для `make e2e` (набор 20) |

Полные прогоны (вне e2e): bitflip 1100 итераций, opseq 5400 оп,
packfuzz 16100 мутантов, manifest ~11800 кейсов — чисто
(детали и найденные баги: `tools/fuzz/FINDINGS.md`).

---

## 3а. Soak-уровень: потеря питания через dm-flakey (WP22b)

`make flakey` (= `bash tools/test-flakey.sh`) — отдельный уровень, в
`make e2e` **не входит**: нужны passwordless sudo (dmsetup/losetup),
модуль ядра `dm-flakey` (иначе — честный STOP на preflight) и ~4 ГБ
свободного tmpfs. Прогон — минуты, а не секунды. Скретч —
`/tmp/invfs-flakey` (на стенде `/dev/shm` забит квотой прошлых прогонов).

Том живёт прямо на блочном устройстве: sparse-файл 2 ГБ → loop →
dm-flakey `/dev/mapper/invfs_flakey` → `invf-mkfs` на самом dm-устройстве
(blkio трактует `/dev/*` как raw device — образ IS устройство, а не файл
внутри ФС). dm-flakey даёт то, чего kill -9 дать не может:

* `drop_writes` — записи **подтверждаются**, но не доходят до устройства
  (врущий кэш записи / потеря питания в случайной точке writeback);
* `error` — цельная ошибка устройства: любой bio → EIO (шина отвалилась);
* смена таблиц = `dmsetup suspend --noflush + reload + resume`, так что
  грязные страницы bdev-кэша переживают смену режима и попадают под
  **новый** — это и есть torn-write окно; переупорядочивание при
  потере питания эмулируется drop-окнами поверх буферизованной записи
  без барьеров.

Ноги (инварианты после каждого хаоса: fsck структурно чист, verify
--deep rc==0, каждый читаемый файл bit-exact, нечитаемый — громко
CORRUPT/EIO, штормовые записи complete-or-absent, никогда «третье
состояние»):

1. **baseline** — mkfs на dm-устройстве, ~200 МБ смешанного корпуса
   (тексты → PPMd-батчи, фабрикованные ELF/PE → бинарные батчи, tar →
   TARR, несжимаемый файл), sweep, sha256-манифест; детерминизм
   генератора доказывается двойной генерацией с тем же сидом.
2. **error-шторм** — error-target на 6 с посреди живых записей через
   FUSE-маунт; писатели обязаны получать EIO; после восстановления
   доштормовые файлы (fsync на здоровом устройстве) bit-exact, штормовые
   — complete-or-absent; демон обязан пережить шторм и завершиться
   самостоятельно.
3. **drop во время sweep** — `invf-sweep` поперёк seeded-окон
   `drop_writes` (хаос стартует после арминга WP21-чекпоинта — та
   механика и призвана стоять до обхода); лестница восстановления
   (auto-recover → invf-rollback → fsck -f); sweep байты не меняет,
   поэтому инвариант — каждый файл bit-exact против оригинала.
4. **крах посреди seal** — `sweep --seal` + drop_writes + kill -9 в
   середине прогона; после лестницы seal либо complete (паритет
   сходится), либо absent (rollback/--free-redundant), файлы bit-exact,
   verify --deep отчитывается честно (порванный паритет чинится одним
   re-seal).
5. **soak** (`tools/flakey/soak.py`, `FLAKEY_SOAK_S`, по умолчанию 210 с) —
   seeded программа create/write/rename/delete/readback через FUSE +
   CLI-фазы sweep/seal/unseal/realize/fsck/verify под переключающимся
   хаосом (up/drop/error); шлюзы каждые 6 раундов и финальный: fsck +
   verify --deep + сверка с теневой моделью: **присутствует ⇒ хэш
   содержимого ∈ истории записанных версий этого имени** (rollback
   законно воскрешает старую версию), неизвестных имён нет, демон не
   умирал.
6. **повторяемость** — сиды фиксированы (`FLAKEY_SEED=20260831` по
   умолчанию); при FAIL образ `backing.img` (sparse) + все логи +
   модель/oplog сохраняются в `tools/flakey/artifacts/<нога>-<ts>/`
   вместе с `replay.env` (сид, git HEAD, версия dm-flakey).

Запуск: `sudo -v && make flakey`. Отладка одной ноги:
`FLAKEY_ONLY=3 bash tools/test-flakey.sh`.

---

## 4. Покрываемые подсистемы

Движок (после сплита `volume.c` → `vol_*` × 18 + `volume_internal.h`):
формат и суперблок-дескрипторы (RDP0/RSZ0/CKP0), L2P/битмап, записи
INOD/INO2, sweep (батчи, дедуп, GC, heat, классы), контейнерные формы и
паки, seal, resize, rollback, путь записи/mmap, ARC-кэш (unit-инварианты
|T1|+|T2| ≤ c и односкановая защита рабочего набора — invf-arctest).

---

## 5. Legacy: Windows-наборы (EOL, исторический контекст)

С Windows-портом заморожены и его наборы: `tests.ps1` (ядро/транскоды,
131 проверка, T1–T23), `tests_dokan.ps1` (26), `tests_fuse.ps1` (не
содержал Assert — FUSE под WSL), `tests_fsck.ps1` (13). Они не
поддерживаются, не запускаются и ничего не говорят о текущем коде
(WP10–WP21 — только Linux). Описания сняты; если порт воскреснет,
нумерацию продолжать с T24 / T-D11.

---

## 6. Известные пробелы покрытия (не покрыто)

- **Quota/EDQUOT**, **throttling (EAGAIN)** — политика описана в
  `doc/12-enospc-strategy.md`, кодом не реализована (только READONLY).
- **Права/ACL** сверх хранения (`doc/11`) — у xattr-уровня тестов нет.
- **Параллельные записи одного файла** (несколько потоков на один inode) —
  не тестируется.
- Закрытые ранее пробелы, о которых говорил старый список: чтение после
  порчи данных сегментов покрыто fuzz/bitflip (+ seal-паритет), крах
  демона посреди flush — ногой kill -9 в test-writepath; потеря питания
  на уровне блочного устройства — уровнем flakey (§3а), который уже
  нашёл настоящий дефект (дыра в append-only inode area после
  error-окна — потеря fsync-подтверждённых файлов; см. AUDIT, WP22b).

---

## 7. Как добавить тест

1. e2e-набор — это `tools/test-<имя>.sh`: bash, образы в `/dev/shm`,
   понятные `PASS`/`FAIL` на выходе, детерминизм (фикстуры либо
   hand-built inline, либо пропуск при отсутствии внешнего инструмента).
   Включается в `make e2e` строкой в Makefile (число наборов в шапке
   этого документа и в §1 не забудьте подвинуть).
2. Unit-проверки — в `src/*_test.c` (счётчик checks, ненулевой rc при
   падении), подключаются в цель `make test`.
3. Fuzz-инструменты живут в `tools/fuzz/`, берут `--seed`; в e2e попадает
   только редуцированный срез через `tools/test-fuzz.sh`.
