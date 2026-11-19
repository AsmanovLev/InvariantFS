# CODE-MAP: карта кодовой базы по коду (слепой прогон)

**Снимок кода:** коммит **`f0f9924`** (2026-11-19 09:13 +0700), ветка `main`.
Дерево скопировано из этого состояния; `git diff f0f9924..HEAD -- src tools`
пуст, то есть карта описывает именно его. Если код ушёл вперёд — карту надо
перегенерировать или читать с поправкой на дифф.

**Метод.** Дерево `/var/tmp/blind/tree` — только `src/`, `tools/*.c|h`, `Makefile`:
- **вырезаны все комментарии** (`/* */`, `//`), 338 файлов;
- **нет документации**: ни `docs/`, ни `impl_docs/`, ни `*.md`, ни `CHANGELOG`;
- **нет `.git`** — то есть нет ни истории, ни сообщений коммитов.

Четыре агента (`opencode-go/hy4-flash-free`) работали **каждый в своей копии дерева**,
в чистых CLI-сессиях (`pi -p`), без проекции переписки — они не знали ни про ADR, ни
про наши замеры. Каждое утверждение обязано было иметь `file:line`; невидимое в коде
помечалось `НЕ-В-КОДЕ`.

Проверка слепоты: в выводах **ноль** упоминаний `README`/`docs/`/`ADR`/`impl_docs`/
«комментарий говорит».


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. КАРТА ФАЙЛОВ
- `src/core/vol_format.c` — приём/отказ формата; `invfs_format_accept()` `vol_format.c:7`; коды отказа `vol_format.h:10`, `vol_format.h:12`, `vol_format.h:14`.
- `src/core/vol_metabuf.c` — страницы BPG3, CRC, размещение в метазоне, два RT30-слота; `mbuf_page_crc()` `vol_metabuf.c:29`, `mbuf_write()` `vol_metabuf.c:102`, `mbuf_alloc()` `vol_metabuf.c:228`, `mbuf_rt30_load()` `vol_metabuf.c:309`, `mbuf_rt30_store()` `vol_metabuf.c:340`, `mbuf_root_publish()` `vol_metabuf.c:353`, `mbuf_root_read()` `vol_metabuf.c:391`.
- `src/core/vol_btree.c` — B-дерево базового дерева, bulk-сборка, xattr/inode-строки, orphan-рекламация; `btree_search()` `vol_btree.c:424`, `btree_upsert()` `vol_btree.c:616`, `btree_delete()` `vol_btree.c:916`, `btree_bulk_new()` `vol_btree.c:1105`, `btree_bulk_add()` `vol_btree.c:1151`, `btree_bulk_finish()` `vol_btree.c:1194`, `btree_check()` `vol_btree.c:1403`, `btree_check_tolerant()` `vol_btree.c:1624`, `btree_excise()` `vol_btree.c:1864`, `btree_reclaim()` `vol_btree.c:2070`, `btree_collect_orphans()` `vol_btree.c:2622`, `vol_bitmap_flush()` `vol_btree.c:2979`, `vol_base_root()` `vol_btree.c:3055`, `vol_xattr_delta_set()` `vol_btree.c:3462`.
- `src/core/vol_delta.c` — append-only сегменты дельты, индекс, staged-перезапись, retire; `vol_delta_rec_parse()` `vol_delta.c:181`, `vol_retire_recipe()` `vol_delta.c:789`, `vol_delta_pad_build()` `vol_delta.c:990`, `vol_delta_heal()` `vol_delta.c:1076`, `vol_delta_append()` `vol_delta.c:1169`, `vol_delta_drain()` `vol_delta.c:1356`, `vol_delta_lookup()` `vol_delta.c:1415`, `vol_delta_mount()` `vol_delta.c:2074`, `vol_delta_truncate()` `vol_delta.c:2254`, `vol_delta_close()` `vol_delta.c:2413`.
- `src/core/vol_fold.c` — слияние дельты в базу, сброс дельты и рекламация; `vol_fold_trigger()` `vol_fold.c:46`, `fold_reclaim_hook()` `vol_fold.c:216`, `fold_delta_reset()` `vol_fold.c:245`, `vol_fold()` `vol_fold.c:419`, `vol_fold_request()` `vol_fold.c:610`, `vol_reclaim_schedule()` `vol_fold.c:682`.
- `src/core/vol_records.c` — обёртки записи/чтения inode, dirent, xattr, class; `vol_create_file()` `vol_records.c:13`, `vol_create_file_with_meta()` `vol_records.c:25`, `vol_delete_inode()` `vol_records.c:58`, `vol_get_meta_rc()` `vol_records.c:482`, `vol_apply_meta()` `vol_records.c:524`, `vol_create_symlink()` `vol_records.c:533`, `vol_create_special()` `vol_records.c:553`, `vol_get_xattr()` `vol_records.c:577`, `vol_set_xattr()` `vol_records.c:586`, `vol_list_xattr()` `vol_records.c:643`, `vol_stamp_class()` `vol_records.c:698`, `inode_area_make_room()` `vol_records.c:743`.
- `src/core/vol_ast.c` — recipe-блоб AST, WINW/CHLD-хвосты, освобождение блоков; `vol_ast_recipe_serialize()` `vol_ast.c:13`, `..._serialize_win()` `vol_ast.c:23`, `..._serialize_tree()` `vol_ast.c:35`, `..._serialize_tail()` `vol_ast.c:47`, `vol_ast_recipe_tail()` `vol_ast.c:145`, `vol_ast_recipe_parse()` `vol_ast.c:234`, `vol_free_recipe_blocks()` `vol_ast.c:366`, `vol_release_superseded_blob()` `vol_ast.c:403`, `vol_zip_parse_children()` `vol_ast.c:443`.
- `src/core/vol_meta_merge.c` — MET0 и таблица экстентов; `meta_mapper_load()` `vol_meta_merge.c:11`, `meta_mapper_flush()` `vol_meta_merge.c:36`, `meta_mapper_get()` `vol_meta_merge.c:52`, `meta_mapper_set()` `vol_meta_merge.c:63`, `meta_met0_crc()` `vol_meta_merge.c:73`, `meta_met0_persist()` `vol_meta_merge.c:80`, `meta_get_append_pos()` `vol_meta_merge.c:98`, `meta_get_owner_append_pos()` `vol_meta_merge.c:247`.
- `src/core/vol_anchor.c` — хвостовой ANC0-якорь; `anchor_crc()` `vol_anchor.c:15`, `anchor_state_of()` `vol_anchor.c:39`, `anchor_pba()` `vol_anchor.c:59`, `anchor_build()` `vol_anchor.c:70`, `anchor_probe()` `vol_anchor.c:103`, `anchor_refresh()` `vol_anchor.c:163`, `anchor_invalidate_at()` `vol_anchor.c:198`, `anchor_block_protected()` `vol_anchor.c:231`.
- `src/core/vol_crash.c` — грязное состояние и лatch; `vol_mark_dirty()` `vol_crash.c:11`, `vol_pre_record()` `vol_crash.c:53`, `vol_needs_recovery()` `vol_crash.c:62`, `vol_io_error_latch()` `vol_crash.c:70`.

## 2. СТРУКТУРЫ НА ДИСКЕ
- Superblock: поля `magic/uuid/state/block_size/total_blocks/зоны/root_ast_hash/checksum/meta_mapper_*` `invarifs.h:146-201`; пишется `INVFS_STATE_DIRTY` `vol_crash.c:21` через `vol_write_sb()` `vol_crash.c:22`, CRC `volume.c:1874-1877`.
- RT30 по `0x9D0`: `magic,version,page_size,root_slot[2],delta_pba,seq,crc32c` `invarifs.h:456-471`; сериализация `vol_metabuf.c:344-346`.
- ANC0 в последнем блоке `vol_anchor.c:59-68`: `magic,version,geo,uuid,rt,spt0,crc32c` `invarifs.h:565-584`; сборка `vol_anchor.c:70-85`, запись `vol_anchor.c:178`, `blkio_flush()` `vol_anchor.c:99`.
- MET0 по `0x3A0`: `magic/version/active_extent/active_offset/extent_count/crc32c` `invarifs.h:386-399`; запись `vol_meta_merge.c:83-92`; таблицаmapper — 32 блока `vol_meta_merge.c:17-28`, запись `vol_meta_merge.c:39-46`;(entry = pba|class<<60) `invarifs.h:681-683`.
- Слоты метазаписей: абсолютная позиция `pba*512?` — нет: `pba*INVFS_BLOCK_SIZE + offset` `vol_meta_merge.c:239-240`, `vol_meta_merge.c:348-349`; `invfs_inode_rec` `invarifs.h:1047-1060`, `invfs_meta_ext_hdr` `invarifs.h:1089-1109`; сама сериализация записи — НЕ-В-КОДЕ области (лишь выделение слота `volume.c:2924-2953`).
- Страница BPG3: `magic,gen,level,nentries,checksum` `invarifs.h:593-604`; CRC по всей странице `vol_metabuf.c:29-35`; запись电话 entries `vol_btree.c:162-195`; парсинг с проверками границ `vol_btree.c:214-246`; размеры record `vol_btree.c:111-116`.
- `invfs_blkptr{pba,checksum,gen,flags}` `invarifs.h:609-618`; заполнение из заголовка страницы `vol_metabuf.c:116-124`.
- Дельта-сегмент: 32 блока `invarifs.h:630-633`, заголовок 44 байта BigEndian `vol_delta.c:299-321` (`DSG3/version/hdr_size/seg_blocks/seg_seq/prev_pba/next_pba/crc`), поля `invarifs.h:649-666`; record — 10-байтовый `klen/vlen/flags/crc` + key + value `vol_delta.c:1258-1267`, разбор `vol_delta.c:181-234`; pad-записи `vol_delta.c:990-1015`.
- Значение inode в B-дереве: `invfs_inode_row` `invarifs.h:1147-1161`, кодирование `vol_btree.c:2889-2907`; xattr-ключ `0x03|inode|nlen|name[|00|idx]` `vol_btree.c:3326-3348`.
- Recipe-блоб: V1/V2/V3 header `invarifs.h:859-888`, entry stride 32/40 `invarifs.h:868-876`; запись `vol_ast.c:55-140`, WINW/CHLD-хвост `vol_ast.c:127-137`, разбор `vol_ast.c:145-189`; физическая запись блоба — НЕ-В-КОДЕ области.

## 3. ПОРЯДОК ПУБЛИКАЦИИ
1. `vol_pre_record()` → `vol_mark_dirty()` → sb.state=DIRTY и `vol_write_sb()` `vol_crash.c:53-58`, `vol_crash.c:21-25`; при пороге 4/5 `vol_flush()` `vol_crash.c:36-46` (bitmap + barrier `volume.c:1901-1913`).
2. Новые страницы дерева пишутся до корня: `bt_write()` `mbuf_alloc()`+`mbuf_write()` `vol_btree.c:155-195`; `publish()` = `vol_bitmap_flush()` → barrier `" inode pages"` → `mbuf_root_publish()` `vol_btree.c:3048-3052`.
3. Корень: проверка страницы/gen `vol_metabuf.c:370-374`, bitmap flush `vol_metabuf.c:378`, слот `seq&1` и `seq++` `vol_metabuf.c:381-383`, `mbuf_rt30_store()` `vol_metabuf.c:386`, barrier `vol_metabuf.c:388`; store пишет RT30 `vol_metabuf.c:344-346`, затем `anchor_refresh()` `vol_metabuf.c:349` → ANC0 `vol_anchor.c:178` → barrier `vol_anchor.c:194`.
4. Дельта: 32-блочный сегмент + header `vol_delta.c:1146-1153`, barrier header `vol_delta.c:1153`, затем `rt.delta_pba=pba` и RT30 `vol_delta.c:1214-1225`, barrier publish `vol_delta.c:1233`; запись записи `vol_delta.c:1269-1270`, group-barrier `vol_delta.c:1293`, drain-barrier `vol_delta.c:1383`.
5. Fold: новые страницы → bitmap flush → barrier `"fold base pages"` → tolerant check → `mbuf_root_publish()` `vol_fold.c:372-399`/`539-576`; затем `fold_delta_reset()`: `vol_delta_close()`, `rt.delta_pba=0`, RT30, barrier, `vol_delta_mount()` `vol_fold.c:245-264`; после этого reclaim сегментов и старого корня `vol_fold.c:412-415`, `vol_fold.c:592-598`.
6. Truncate дельты: обнуление хвоста + перепись header `vol_delta.c:2214-2217`, затем `rt.delta_pba` и RT30 `vol_delta.c:2225-2227`, free новых сегментов `vol_delta.c:2231-2235`, barrier `vol_delta.c:2236`.
7. Инвалидация якоря: zero-блок `vol_anchor.c:211`, `vol_delta_heal()` `vol_anchor.c:213`, barrier `vol_anchor.c:215`, состояние `ABSENT` `vol_anchor.c:216`.

## 4. ИНВАРИАНТЫ В КОДЕ
- Отказ чужого формата при отсутствии `VOLF_META` `vol_format.c:20-29`; `format_version` не проверяется `vol_format.c:12`.
- CRC/magic страницы `vol_metabuf.c:29-59`; ptr дополнительно сверяет `checksum`+`gen`, bitmap и диапазон `vol_metabuf.c:126-142`; root_read выбирает только валидные слоты и отказывает при именованных, но битых `vol_metabuf.c:423-472`.
- Аллокатор: отказ readonly/не-4K `vol_metabuf.c:235-238`, boot-страницы `vol_metabuf.c:240-246`, meta-zona/shadow fallback `vol_metabuf.c:193-218`, `vol_metabuf.c:248-254`; rollback при неудачной записи `mbuf_free()` `vol_metabuf.c:258-260`.
- B-tree: лимит записей/размера `vol_btree.c:144-147`, child.pba!=0 `vol_btree.c:150-153`, rollback страницы `vol_btree.c:184-191`, границы парсинга `vol_btree.c:212-246`; отказы split `vol_btree.c:496`, `vol_btree.c:555`, `vol_btree.c:587`; аргументы upsert `vol_btree.c:622-625`; bulk `vol_btree.c:1165-1175`, `vol_btree.c:1198-1208`.
- Перед публикацией fold отказывает при `q.bad_pages>0 || q.qfull` `vol_fold.c:383-397`, `vol_fold.c:556-571`;/free old root только после публикации `vol_fold.c:415`, `vol_fold.c:598`.
- Delta-запись: `klen!=0`, unknown flags, delete с value, READONLY, предел размера `vol_delta.c:1183-1200`; формат record `vol_delta.c:207-228`; версия/geom/CRC header `vol_delta.c:272-285`; torn tail обрезается по последней валидной записи `vol_delta.c:2033-2062`.
- Delta rollback: ООМ при создании сегмента `vol_delta.c:1141-1157`; отказ RT30/barrier восстанавливает `delta_pba` и `dl_doom_locked()` `vol_delta.c:1225-1240`; ошибка записи → doom `vol_delta.c:1270-1277`; ошибка barrier откатывает счётчики `vol_delta.c:1293-1308`; drain → doom `vol_delta.c:1381-1394`.
- Fold guard’ы: `VOLF_META`/READONLY `vol_fold.c:430-434`, пустая дельта `vol_fold.c:435-436`, Place: `fold_space_ok()` `vol_fold.c:443-447`, ошибка сбора `vol_fold.c:451-458`, rollback пустой страницы `mbuf_free()` `vol_fold.c:520-533`; `fold_delta_reset()` ошибка → `-1` `vol_fold.c:408-416`, `vol_fold.c:586-600`.
- Метаэкстенты: wrlock `vol_meta_merge.c:106`, unlock на всех error-путях `vol_meta_merge.c:126-147`, `vol_meta_merge.c:167-189`, `vol_meta_merge.c:225-236`, `vol_meta_merge.c:279-336`; `-2` = ENOSPC `vol_meta_merge.c:189`, `vol_meta_merge.c:226`, `vol_meta_merge.c:281`, `vol_meta_merge.c:316`, `vol_meta_merge.c:336`; проверкаCRM размера `vol_meta_merge.c:158`, `vol_meta_merge.c:306`, `vol_meta_merge.c:334`.
- Якорь: magic/version/CRC `vol_anchor.c:39-45`, геометрия `vol_anchor.c:137-155`, отказ при IO/отсутствии `vol_anchor.c:111-135`,	failed-refresh latch `vol_anchor.c:181-190`, защита хвостового блока `vol_anchor.c:231-241`.
- Crash: write-enabled/refused `vol_crash.c:13-19`, `dirty` short-circuit `vol_crash.c:20`, sb не помечается при ошибке записи `vol_crash.c:22-25`, latch и откат `inode_area_pos` `vol_crash.c:75-84`.
- xattr: проверки имени/размера и fault `vol_btree.c:3469-3492`, превышение chunks → `-2` `vol_btree.c:3517-3518`, дочистка хвостовых chunk-записей `vol_btree.c:3524-3535`.

## 5. НАПРАВЛЕНИЕ ЗАВИСИМОСТЕЙ
- `vol_format.c:3` → только `vol_format.h` → `invarifs.h` `vol_format.h:6`; внешних вызовов нет.
- `vol_crash.c:4-5` → `volume_internal.h`, `vol_storage.h`; зовёт `vol_write_enabled()` `vol_crash.c:13`, `vol_write_sb()` `vol_crash.c:22`, `vol_flush()` `vol_crash.c:57`, `meta_mapper_get()` `vol_crash.c:37`.
- `vol_meta_merge.c:4` → `volume_internal.h`; зовёт `alloc_meta_extent()`/`extend_meta_extent()` `vol_meta_merge.c:125`, `vol_meta_merge.c:174`; вызывается из `volume.c:2929`, `volume.c:2967`, `vol_crash.c:37`.
- `vol_metabuf.c:4-6` → `volume_internal.h`, `vol_metabuf.h`, `vol_anchor.h`; зовёт `anchor_refresh()` `vol_metabuf.c:349`, `vol_bitmap_flush()` `vol_metabuf.c:378`, `alloc_blocks()` `vol_metabuf.c:252`; его зовут `vol_btree.c:7`, `vol_delta.c:6`, `vol_fold.c:13`, `vol_anchor.c:11`.
- `vol_btree.c:4-9` → `volume_internal.h`, `vol_fault.h`, `vol_btree.h`, `vol_metabuf.h`, `vol_delta.h`, `vol_reclaim.h`; зовёт `vol_delta_append()` `vol_btree.c:3189`, `vol_ast_recipe_parse()` `vol_btree.c:5999`, `vol_reclaim_reader_snapshot()` `vol_btree.c:3070`.
- `vol_delta.c:4-6` → `volume_internal.h`, `vol_delta.h`, `vol_metabuf.h`; зовёт `vol_mark_dirty()` `vol_delta.c:1195`, `mbuf_rt30_store()` `vol_delta.c:1225`, `vol_inode_row_key()` `vol_delta.c:801`, `vol_free_recipe_blocks()` `vol_delta.c:946`.
- `vol_fold.c:10-14` → `volume_internal.h`, `vol_btree.h`, `vol_delta.h`, `vol_metabuf.h`, `vol_reclaim.h`; зовёт `btree_bulk_finish()` `vol_fold.c:366`, `vol_delta_mount()` `vol_fold.c:263`, `vol_reclaim_delta_segments()` `vol_fold.c:412`.
- `vol_records.c:4-6` → `volume_internal.h`, `vol_fault.h`, `vol_walk.h`; зовёт `vol_xattr_delta_set()` `vol_records.c:586`, `meta_mapper_get()` `vol_records.c:748`, `vol_write_bulk()`/`vol_create_node()` `vol_records.c:32-33`.
- `vol_ast.c:4-5` → `volume_internal.h`, `vol_delta.h`; зовёт `vol_retire_recipe()` `vol_ast.c:418`, `vol_recipe_load()` `vol_ast.c:384`, `vol_free_blocks()` `vol_ast.c:366`; вызывается из `vol_btree.c:5999`.
- `vol_anchor.c:7-13` → `volume_internal.h`, `vol_anchor.h`, `vol_delta.h`, `vol_metabuf.h`, `vol_spt0.h`, `blkio.h`; зовёт `vol_delta_heal()` `vol_anchor.c:192`, `anchor_pba()` `vol_metabuf.c:291`.

## 6. ПОДОЗРИТЕЛЬНОЕ
- `INVFS_FMT_REFUSE_GENERATION/FORBIDDEN` объявлены, но не используются; `format_version` явно игнорируется `vol_format.h:12`, `vol_format.h:14`, `vol_format.c:12`.
- `mbuf_rt30_store()` не проверяет результат `anchor_refresh()` и возвращает 0 `vol_metabuf.c:349-350`, хотя отказ refresher поднимает `anchor_refresh_failed` `vol_anchor.c:181-190`.
- Слот корня выбирается как `seq&1` до инкремента `vol_metabuf.c:381-383`, но `mbuf_root_read()` считает `seq&1` «новым» слотом при равенстве gen после инкремента `vol_metabuf.c:435-437`, `vol_metabuf.c:463-468`.
- Fold публикует новый root раньше сброса дельты `vol_fold.c:398`, `vol_fold.c:572`; при отказе reset возвращает `-1` после reclaим `vol_fold.c:408-416`, `vol_fold.c:586-600`.
- Двойной `fold_reclaim_hook`: внутри `vol_fold()` `vol_fold.c:598` и повторно в `vol_fold_request()` с `fold_pre_root` `vol_fold.c:650-668`; поле очищается только в `vol_reclaim_schedule()` `vol_fold.c:693`.
- Truncate освобождает блоки новых сегментов до финального barrier `vol_delta.c:2225-2236`, `vol_delta.c:2385-2400`, хотя указатель RT30 меняется раньше barьера.


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. КАРТА ФАЙЛОВ
- `vol_write.c`: сессия записи `invfs_wsession` (`vol_write.c:12`); точки: `vol_write_begin:93`, `vol_write_range:720`, `vol_write_truncate:802`, `vol_write_read:914`, `vol_write_commit:1113`, `vol_write_abort:1138`, `vol_write_bulk:1172`, активные сессии `vol_write_active_{name,id}:59,71`.
- `vol_read.c`: чтение рецепта/сегментов; точки: `seg_read_checked:62`, `ast_frame_ok:260`, `vol_read_inode:1176`, `vol_read_file:1276`, `vol_read_named:1284`, `vol_stat{,_full}:1355,1364`, `vol_read_range:1786`.
- `vol_png.c`: обход сессии нет — прямая запись блоков `io_pwrite` (`vol_png.c:937,938,1023,1024`) + рецепт + inode; точки: `vol_create_blob_file:862`, `vol_publish_blob_inode:972`, `vol_publish_window_inode:1065`, `vol_create_png_file:500`, `vol_create_nbli_file:753`, `invfs_png_from_{jxl,nbli}:103,829`, `vol_create_{jxl,ape,pmp}_file:1159,1167,1175`.
- `vol_textzone.c`: пакетная переупаковка в `INVFS_ZONE_TEXT`; `tz_defer:111`, `bz_defer:121`, `tz_sniff_any:154`, `tz_reg_owned_blocks:380`, `vol_tz_flush:958`, `vol_acc_pending:973`, `vol_tz_gc:999`.
- `arc.c`: ARC-кэш с mutex, T1/T2/B1/B2 (`arc.c:13-50`); `arc_create:163`, `arc_get:195`, `arc_get_copy:223`, `arc_put:295`, `arc_invalidate:378`, `arc_clear:392`, `arc_stats:411`.
- `vol_seal.c`: чётность/герметизация и лечение; `vol_redun_config:521`, `vol_seal:937` → `vol_seal_ex:965`, `vol_seal_verify:1976`, `vol_seal_scrub:1986`, `vol_seal2_repair:2015`, `vol_seal_heal:3228`.
- `vol_repair.c`: пуст (`vol_repair.c:1-2`); функции восстановления объявлены в `volume.h:974,990`, реализованы в `vol_seal.c:2015,3228`; отдельной логики в файле НЕ-В-КОДЕ.
- `vol_cut.c`: усечение до корректного префикса; `vol_content_cut:457`, `vol_cut_span_ok:479`, декод `cut_decode_span:16`.
- `vol_tier.c`: таблицы西藏 `tier/rawm` в `v` (`vol_tier.c:92,103,275,289`), mirroring `wp25_rawm_write:333`, `wp25_on_free:397`, `wp25_fsck_prune:434`, `vol_tier_migrate:670`.
- `vol_heat.c`: сессионный счётчик + xattr-тепло; `heat_touch_read:139`, `heat_fold:371`, `vol_heat_persist:443`, `vol_heat_sweep_begin:525`, `vol_heat_promote:694`.
- Константы: `INVFS_SEGMENT_SIZE (64u*1024u)` `volume.h:650`, `SEGMENT_SIZE` алиас `volume_internal.h:149`, `SEAL_SYM_BYTES 65536u` `vol_seal.h:15`.

## 2. ПУТЬ ЗАПИСИ
1. `vol_write_begin` (`vol_write.c:93`): проверка имени/`vol_write_enabled` (`100`), `next_inode_id++` (`105`), `child_segs` из env (`109`, `82`), `vol_find_rc` (`113`), включение в `v->wsessions` (`125`).
2. `vol_write_range` (`720`): `EFBIG` (`725`), `wsession_stale` (`739`), `wsession_load_old` (`741`).
3. `wsession_load_old_impl` (`610`): старый рецепт (`627`), `old_pbas` (`653`), при `truncating` выход (`659`); простой случай — алиасинг (`676`), сложный — материализация через `vol_read_range` (`694`) и `wsession_write_seg` (`700`).
4. Сегмент: `wsession_write_seg:471` → `wsession_write_seg_n:356`; сменаchild `wsession_switch_child:275`/`wsession_seal_child:207`; рост `ents/touched` `382`; сжатие `387-417`; fallback RAW `418`; CRC+8-байтовый заголовок `421-431`; `phys_blocks` `429`.
5. Выделение: `alloc_raw_or_shadow` `437`; запись `write_segment_blocks` `439`; освобождение при ошибке `442` и прежнего PBA `449-452`; заполнение записи `455-464`; `mat_upto/dirty` `464-465`.
6. `vol_write_truncate` (`802`): без child — frees `839-846`; с child — `vol_free_recipe_blocks` `856`, frees `870-874`; добивка хвоста `880-890`;扩展 zero-fill `893-903`.
7. `vol_write_commit` (`1113`): stale/load `1125-1128`, только `VOLF_META` (`1131`) → `vol_write_commit_impl:948`: проверки `959`, обрезка лишних `974-980`, перепись хвоста `985-1014`, `wsession_seal_child` `1034`, `vol_ast_recipe_serialize{,_tree}` `1024,1036`, `vol_recipe_store` `1027,1039`, inode update/create `1041-1059`, `vol_retire_pbas` `1079`, `pba_ref_modify/validate` `1085-1094`, `vol_delete_siblings` `1098`, frees `1100-1107`, unlink/committed `1108-1109`.
8. `vol_write_abort` (`1138`): unlink `1143`, frees по `touched` `1147-1155`, `vol_free_recipe_blocks` child-рецептов `1157`, frees структуры `1160-1167`.
9. Ветви порядка: `truncate` пропускает материализацию (`659-663`); Turbo/`raw_nocompress` пропускает сжатие (`387`); `raw_effort_zlevel` меняет LZ4↔ZSTD (`341-350`, `388-417`); child-переключение сеалит текущий рецепт до записи в другой (`277-282`, `365-368`).

## 3. ПУТЬ ЧТЕНИЯ
- `vol_read_file:1276` → `vol_read_inode:1176`: raw-blob ветвь `1191-1201`; иначе `vol_recipe_load:1208`, `vol_ast_recipe_parse:1212`, сверка `ah.file_size == in.size` `1222`, `malloc(len)` `1226`, tail `1231`, `vol_decode_ast_entries:1243`, выдача `1268-1269`.
- `vol_decode_ast_entries:442`: проверки границ/перекрытий `454-471`; потоковая ветвь `490-536`; последовательная `541`; CHLD → `vol_decode_child_full:420` + `vol_read_range:432`; TEXT → `vol_read_text_slice:559` (`vol_read.c:119`); whole-file → `vol_read_inode:1694`; общий путь `pba`+heat `648-660`.
- `seg_read_checked:62`: 2-устройственный tier fallback `66-77`, dev0/rawm fallback `80-90`, `seal_recover_segment` `95-99`.
- `seg_read_once:25`: **длина хранения** `csize` из 8-байтового заголовка по `pba*INVFS_BLOCK_SIZE` `34`, проверка `plen`/CRC `37-51`, чтение `blob` `48`.
- Декод: `ast_frame_ok:260`; LZ4/ZSTD `674-693`; `ALGO_NONE` `1118`; codec/container `1122-1163`.
- Длина декодирования = `e->length` из записи рецепта (`vol_write.c:457`, чтение `vol_read.c:674,685,1118,1736,1742`), буфер `tmp[SEGMENT_SIZE]` `1525`/heap `1713-1715`, child `ctmp` `1585`/`1611-1613`; короткое чтение → отказ `1776-1782`.
- TEXT: `csize` заголовка `170`, `usize` из первых 4 байт блоба `177`, лимит `64u<<20` `177`, ARC `159,215`, BCJ-сдвиг `148,220-224`.

## 4. РАЗМЕР ЧАНКА
- `volume.h:650` + `volume_internal.h:149`: базовые 64 КиБ; смена меняет все смещения/индексы сегментов и совместимость записанных рецептов: `vol_write.c:172,212,244,248,251,455,473,482,499-515,555-570,589,681-695,745-746,763,774-793,822-824,853,860,880-887,897,932-935,974,985-1009`; `vol_read.c:1525,1585`; `vol_heat.c:748`.
- Стековые буферы 64 КиБ: `vol_write.c:784,883,935,989,1009`; `vol_read.c:1525,1585` — при увеличении макроса растёт стек каждого потока/вызова (`vol_read.c:279,311`).
- `SEAL_SYM_BYTES` (`vol_seal.h:15`): арифметика `vol_seal.c:163,166,841,868,1314,1400,2441,2544,3285`; заголовки/проверки `180,242,275,377,474,1689,1732,2193`; буферы `889,1020,1540,1588,1869,2106,2464,2501-2507,2854-2876,2975,3045-3089`; RS `703,732,2550,2923,3063,3193`; запись/чтение чётности `917-919,1479-1483,1558-1572,1878-1892,2545-2554,2904-2906,2937-2951,3205-3213`. Смена ломает существующую раскладку chётности и валидацию заголовков.
- Литерал `65536`: `vol_png.c:781` (только запас ёмкости энкодера; меньше → реаллокация/переполнение расчёта), `vol_textzone.c:29,31` (граница env `INVFS_TZ_BATCH_KB`; больше/меньше меняет размер пакета и число батчей).
- Не 64 КиБ, а 64 МиБ: `vol_read.c:177` (`64u<<20` лимит TEXT-батча), `vol_tier.c:613` (`64*1024*1024` лимит копируемого фрагмента).

## 5. СОСТОЯНИЕ
- Пер-сессия (`invfs_wsession`, `vol_write.c:12-58`): `v/name/new_id/old_id` (`99-113`); `gen/stale` (`111,152-156`); `loaded/truncating/have_old/owns_siblings/committed` (`121,647,659,711-712,1109`); `ents/n_ents/touched/touched_cap/aliased_n/mat_upto` (`382,455-464,488,604,676-689`); `logical_size` (`666,794,906`); `old_pbas/old_n_ents` (`651-656,1079`); `child_segs/cur_child/top_ents/chld` (`109-110,212-285,1034-1039`).
- Записаны, но не читаются: `wheat_carry` (`108,625`), `dirty` (`465,907`), `old_ext/old_ext_len` (только `free` `1162`).
- Пер-том: `v->wsessions` (`125,137`), `v->write_gen` (`111,152`), `v->next_inode_id` (`105`), `v->free_blocks/reserved_blocks/hard_min_blocks` (`765-767,899-901`), `v->raw_free` (`343`), `v->arc` (`vol_read.c:159,215`), `v->ndev/dev0_present` (`vol_read.c:66,81`), `v->tier/rawm + *_dirty/owner` (`vol_tier.c:92,103,289-299,364,625`), `v->heat_tab/heat_tab_n/heat_any_rhot/heat_any_whot` (`vol_heat.c:22-52,146-149,571-572`), `v->seal_dirty` (`vol_seal.c:17-66,1099,1426`), `v->rd/rd_present` (`vol_seal.c:539-550,1371`).
- Конфиг-наследование env: `INVFS_CHILD_SEGS` (`vol_write.c:82-90`), `INVFS_RAW_ADAPT` (`329-336`), `INVFS_TZ_BATCH_KB` (`vol_textzone.c:22-32`).

## 6. ИНВАРИАНТЫ В КОДЕ
- `vol_write.c:100` write-enabled; `725/805` `MAX_FILE_SIZE`; `747-755,823-831` лимит entry; `765-767,899-901` резерв блоков; `959-970` commit-границы; `1131` только `VOLF_META`; `151/152` ESTALE; `172-180` «простой» старый рецепт; `499-515,555-570` точное совпадение декомпрессии с `e->length`.
- `vol_read.c:37-40` `csize` против `plen`; `51` CRC; `135` срез в пределах `e->length`; `177` лимит `usize`; `269-276` `ast_frame_ok`; `454-471` границы/перекрытие записей; `426-431` глубина CHLD; `1453-1460,1474-1465` проверки range; `1748-1751` `hdr == e->length`; `1776` отказ при коротком покрытии.
- `vol_textzone.c:29` кламп env; `648-651` write-enabled; `660-672` совпадение имени/inode и класса; `465,494` отказ при отсутствии выигрыша/памяти; `563-575` обязательные `complete && !fallback && !committed && n_slices`.
- `arc.c:164` budget>0; `300-305` отказ записи > `c/2`; `257-284` вытеснение; `286-293` границы ghost-листов.
- `vol_seal.c:1088-1093` отказ при неполном обходе; `1099` условие инкремента; `2015-2021` detect-only отказ; `3240-3244` read-only отказ; `275,377,474,1689` сверка `SEAL_SYM_BYTES`; `841` сверка размера parity-файла.
- `vol_cut.c:25-40` запрет TEXT/WINDOW_SRC и нулевого PBA; `43-45` `ast_frame_ok` и лимит 128 МиБ; `462-466` read-only; `402-425` post-publish readback.
- `vol_tier.c:145` `n>65535`; `333-340` 24-битный ключ; `349-354` таблица полна; `598-613` лимиты промоушена; `671-675` `ndev==2 && !degraded && write-enabled`.
- `vol_heat.c:145-147,447` write-enabled; `542-562,712-729` отказ при незавершённом `vol_walk_commit`; `378-395` свёртка только живых inode.

## 7. ПОДОЗРИТЕЛЬНОЕ
1. `vol_seal.c:77-83` `seal_recover_segment` всегда `-1`, хотя `vol_read.c:95-99` использует его как единственный путь восстановления повреждённого сегмента.
2. `vol_seal.c:2015-2021` `vol_seal2_repair` объявлен (`volume.h:990`), но только печатает отказ; `vol_repair.c:1-2` пуст — при actual-пути восстановление идёт через `vol_seal_heal:3228`.
3. `vol_read.c:1118` для `ALGO_NONE` копирует `hdr` байт без проверки `hdr == e->length`, тогда как range-путь проверяет `1748-1751` и `ast_frame_ok` `269-275`.
4. `vol_heat.c:44-51` повторный `heat_tab_touch` для уже существующего inode возвращает `1` без инкремента счётчика; `vol_heat.c:146-149` выставляет `heat_any_rhot` только при вставке — повторные чтения неUpgradeют тепло в этой сессии.
5. `vol_tier.c:8-19` `wp25_map`/`wp25_unmap` — пустые заглушки, хотя `wp25_owner_write_impl:145-170` и `wp25_index_load_impl:214-272` пишут/читают owner-блоб с `ord`.
6. `vol_png.c:949-953` при ошибке `vol_ast_recipe_serialize` освобождаются `pba`/`rblob`, но не `cblob` (сжатая копия из `897-905`).


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. КАРТА ФАЙЛОВ
- `src/core/vol_sweep.c`: драйвер свипа/приоритетных lane; входы `vol_sweep_file:132`, `vol_sweep_one_ex:1626`, `vol_sweep_pending:1728`, `vol_jxl_retry:286`, `vol_exer_retry:331`, `vol_collect_sweepables_ex:1876`, `vol_compute_stats:2239`.
- `src/core/vol_cpack.c`: внешние кодеки-паки, materialize, table/map, разборка контейнеров; входы `invfs_vol_pack_materialize:431`, `invfs_codec_pack_exec:658`, `vol_create_flac_file:1477`, `vol_create_tar_file:1761`, `vol_create_gz_file:1933`, `vol_containerpack_sweep:3515`, `vol_containerpack_sweep_stream:4203`, `vol_cpack_migrate:4218`, `pack_container_rebuild:4285`.
- `src/core/vol_exer.c`: EXER-карвинг встроенных JPEG/PNG из бинарника; входы `exer_payload_parse:145`, `exer_splice:183`, `vol_exer_carve:203`, `vol_exer_last_parts:445`.
- `src/core/vol_dedupe.c`: хэширование сегментов BLAKE3, дедуп по рецептам; входы `vol_sweep_dedupe_ex:343`, `vol_sweep_dedupe:586`.
- `src/core/vol_reclaim.c`: orphan/delta/reader-drain/pinned reclaim; входы `vol_reclaim_orphans:96`, `_full:103`, `_stats:108`, `vol_reclaim_reader_snapshot:162`, `vol_reclaim_reader_release:170`, `vol_reclaim_drain:177`, `vol_reclaim_mark_and_free:231`, `vol_reclaim_delta_segments:301`.
- `src/core/vol_spt0.c`: save point SPT0 + data pin SPN0; входы `spt0_load:57`, `spt0_store:127`, `spt0_capture:997`, `spt0_restore:1344`, `spt0_drop:1423`, `spt0_block_pinned:1461`, `spt0_verify_live:1518`, `spt0_reclaim_last:1553`.
- `src/core/vol_migrate.c`: дефраг/перемещение сегментов вниз; входы `vol_migrate_frag:18`, `vol_migrate_run:564`.
- `src/core/vol_walk.c`: квитирование полноты обхода (`vol_walk_t`); `vol_walk_init:18`, `vol_walk_result:31`, `vol_walk_commit:70`, `vol_walk_abandon:81`, `vol_walk_reap:100`.
- `src/core/vol_dirs.c`: namespace/обходы/удаление; `vol_walk:1062`, `vol_walk_strict:1071`, `vol_walk_strict_dir:1078`, `vol_unlink:724`, `vol_delete_file:1126`, `vol_replace_file:1103`, `vol_hardlink:1147`.

## 2. КОНВЕЙЕР СВИПА
1. `vol_collect_sweepables_grow:1898` → `vol_iter_live_inodes` (`vol_sweep.c:1884`) ↔ IDs live-инодов, не name[0]==0x01 (`1854-1861`); короткий обход → `-1` через `vol_walk_commit:1942`.
2. `vol_mark_pending:155` накапливает ID; `vol_sweep_pending:1728` → `vol_heat_sweep_begin:1729` → `vol_sweep_pending_cap:1701` → имя через `vol_sweep_name_of:1664` → `vol_sweep_one:1647` → `vol_sweep_one_ex:1626` пере-resolve имени в live id (`1630-1637`).
3. `vol_sweep_pending_finish:1679`: promote heat (`1683`), `vol_tier_migrate:1686`, `vol_tz_flush:1687`, затем `vol_fold_request`/`vol_reclaim_schedule` только если не READONLY (`1691-1695`).
4. `vol_sweep_one_impl:1004`: разбор/class-replay (`1035-1090`) → inode RAW (`1100-1109`) → чтение целого файла и `sweep_dispatch` (`1125-1133`) либо 8KiB-head + `vol_containerpack_sweep_stream` для больших файлов (`1141-1169`).
5. При declining dispatch: рецепт RAW (`1180-1210`) → по 128 сегментов (`SWEEP_BATCH_SIZE:892`, цикл `1252`) декомпрессия/BCJ/ZSTD в потоках (`941-990`) → запись в shadow (`1348-1359`) → новый рецепт + `vol_inode_delta_put` (`1412-1428`) → `vol_retire_pbas` старых PBA (`1456`) или откат новых блоков (`1470-1483`).
6. Для одного сегмента: generic floor `1510-1620`: `INVFS_ALGO_NONE`/LZ4/ZSTD из профиля (`1527-1536`, `1583-1608`), round-trip guard (`1548-1556`, `1596-1605`), публикация `vol_publish_blob_inode:1539/1573/1611`.
7. Независимые проходы: `vol_sweep_dedupe_ex:343` (hash→sort→scan→remap) и `vol_migrate_run:564` (frag→walk→per-file move).
8. Статистика: `wstats_cb:2078` потребляет live-иноды и производит `invfs_volume_stats` в `vol_compute_stats:2239`; `unclaimed_used_bytes` считается как разница используемого иClaimed (`2267-2277`).

## 3. МАРШРУТИЗАЦИЯ ФАЙЛА
- `vol_sweep.c:466` — нет имени/`full_len<4` → `-2`, пропускаются все lane.
- `vol_sweep.c:471-479` — малый не-sibling binary → `bz_defer` → `10`, пропускает anchored/hot/magic/паки/floor.
- `vol_sweep.c:481-486` — anchored → `-2`, пропускает остальные lane; `490-493` — hot → `-2`.
- `vol_sweep.c:495-535` — `fLaC` → `vol_create_flac_file` → `2`; пропускает tar/gz/photo/pack/generic.
- `vol_sweep.c:515-536` — `ustar` → `vol_create_tar_file` → `3` + `defer_container_parts:235`; пропускает gz и ниже.
- `vol_sweep.c:538-558` — gzip magic → `vol_create_gz_file` → `4`; пропускает photo/pack/generic.
- `vol_sweep.c:561-634` — PNG: если внешний `EXTERNAL`-sniff не заявил (`567-575`), builtin PNG/NBLI → `5`;claims → пропуск builtin и переход к общему pack-конвейеру.
- `vol_sweep.c:638-663` — JPEG: при отсутствии внешнего claims (`653`) builtin JXL → `12` (`386-446`); claims → пропуск builtin, дальше container/external pack.
- `vol_sweep.c:666-714` — ID3/MPEG → PMP с round-trip (`681-697`) → `8`; отказ ставит `GENERIC_GUARD` и продолжает (`698-713`).
- `vol_sweep.c:722-748` — `CAP_CONTAINER`-sniff → `vol_containerpack_sweep`; `prc>=100` возвращается, `prc==1` = deferral → `0`, остальныеcontinue; при `deferred` пропускаются external/generic.
- `vol_sweep.c:753-779` — `CAP_EXTERNAL`-sniff → `vol_pack_sweep`; `prc==1` → `0`, `>=100` → код кодека, `PACKONLY` без encode/decode → `0`.
- `vol_sweep.c:784-793` — `INVFS_SWEEP_NO_COMPRESS=1` → `0`, пропускает текст/EXER/бинарь/floor.
- `vol_sweep.c:796-812` — текст `invfs_text_family` → `tz_defer` → `9`; ошибка памяти — `GENERIC_MEMLIMIT`, но lane продолжается.
- `vol_sweep.c:814-826` — binary → `vol_exer_carve`; `1` → `11`, `2` (`exer_no_bz`) подавляет только финальный `bz_defer:877-884`.
- `vol_sweep.c:829-873` — «try-last»: только внешние кодеки без `sniff` claims, с `probe`, без `pack_claims`, с gain-баром из `sweep_lane_ref_size:354`; `declined_algo` исключён (`840`).
- `vol_sweep.c:877-885` — финальный `bz_defer` бинаря → `10`; иначе `-2` → generic floor `1500+`.
- `vol_cpack.c:2436-2469` — `cpack_member_is_container`防雷 magic/ sniffs; `2598-2661` — `cpack_size_guard`; `2522-2579` — `cpack_map_validate`; отказ → `out` без публикации.

## 4. ПУТИ ОСВОБОЖДЕНИЯ
- `vol_release_superseded_blob` (`vol_sweep.c:430,506,527,550,626,703`; `vol_cpack.c:3329-3335`): старый `recipe_addr` ненулевой и строка уже изменена (`vol_ast.c:403-420`).
- `vol_publish_blob_inode` при успешной замене рецепта вызывает `vol_retire_recipe` (`vol_png.c:1048-1056`); при ошибке записи освобождает новый pba (`vol_png.c:1004-1022,1030-1041`).
- `vol_retire_pbas`: после публикации svip’а (`vol_sweep.c:1440-1457`), дедупа (`vol_dedupe.c:229-239`), миграции (`vol_migrate.c:395-414`); `keep` защищает новые PBA (`vol_delta.c:820-855`).
- Прямые откаты: `vol_free_blocks` при неудачной pwrite/realloc (`vol_sweep.c:1356,1369`), при unpublished recipe (`1470-1476`), миграции (`vol_migrate.c:280,292,311,336,430`), SPT0 pin (`vol_spt0.c:946,958,963`).
- `vol_transcode_abort` → `vol_delete_siblings` (`vol_records.c:411-418`) после ошибок EXER (`vol_exer.c:395,413`) и cpack (`vol_cpack.c:3901,4002,4015,4024,4043,4061,4100`).
- `cpack_rollback_commit` (`vol_cpack.c:3339-3356`): `vol_delete_inode(newino)` только если `newino != inode_id`, плюс `vol_transcode_abort`.
- `vol_unlink_impl` (`vol_dirs.c:743-845`): `nlink<=1`, recipe загружается/парсится (`782-801`) → `vol_retire_recipe(...,1)` (`830`) и `vol_delete_siblings` (`834`); иначе только декремент nlink (`838-849`).
- `vol_reclaim_orphans[_full]` (`vol_reclaim.c:59-95`) — `btree_collect_orphans[_full]`, gate `INVFS_RECLAIM_ORPHANS` (`27-48`), по умолчанию включён.
- `fold_reclaim_hook` (`vol_fold.c:216-241`) — после drain: или только orphans (old_root в root_slot), или `vol_reclaim_mark_and_free` + orphans.
- `btree_reclaim_pinned` (`vol_btree.c:2097-2125`) помечает `keep_root` и `pinned_root`, затем `bt_free_rec(old_root)`.
- `vol_reclaim_delta_segments:301-330` — `mbuf_free` по страницам delta-сегментов при валидном magic.
- `spn_reclaim:721-779` → `spn_free_counted:706-719` → `vol_free_blocks`; `retain_release=1` (`757-773`) обходит pin-проверку.
- `spt0_pin_take:974` освобождает старый SPN0-файл после записи нового дескриптора; `spt0_drop:1423-1459` только снимает ARMED-флаг и не освобождает pin-блоки.

## 5. SAVE POINT / RECLAIM
- `spt0_capture:997`: reader snapshot (`1014`), `vol_base_root:1018`, `spt0_tree_ok:1022`, descriptor c `base_root`/`delta_end` (`1028-1059`), `spt0_pin_take:1067`, `pinned_root`+`savepoint_live` (`1069-1071`), `spt0_store:1074`, barrier (`1077`).
- `spt0_pin_take:784`: `vol_iter_inodes_at(...,spn_walk_ino)`:821 строит `c.map`/`c.dig`; старый map читается в `856`; `spn_reclaim` в `861`; отказ capture при root не аллоцирован в `886-894`.
- точные отказы `spn_reclaim:721`: нет old_map (`726`), неполный new_map (`729`), `tz_reg_owned_blocks` ошибка (`732`), `spn_meta_mark`/`btree_mark_reachable` ошибка (`737-748`); иначе освобождаются только биты `old_map & !new_map & bitmap & !tz_reg & !meta` (`755-765`).
- pin-файл: запись hdr/dig/map (`910-927`), `vmux_barrier:942`, `spn_desc_store:956`, повторный barrier (`960`), затем frees old pin (`974`); при ошибке — `vol_free_blocks(s.pba)` (`946,958,963`).
- отказ захвата: `root_pba` не в bitmap → `-1` без pin и без save point (`886-894`); нет места под pin → `-1`, но SPT0-дескриптор обнуляется (`902-913`).
- `fold_reclaim_hook:216`: `vol_reclaim_drain` (`221`), root-slot guard (`227-231`), `vol_reclaim_mark_and_free` (`237`), orphan pass (`240`).
- точный отказ reclaim: `vol_reclaim_drain:177-208` возвращает `-1`, если `g_readers_in_flight != 0` по истечении `reclaim_drain_timeout_ms` (`193-202`); fold тогда пропускает освобождение целиком (`vol_fold.c:221-226`).
- `vol_reclaim_mark_and_free:231-239` → `btree_reclaim_pinned`; отказ: `!v` (`vol_btree.c:2107`), `old_root.pba==0` (`2110`), сбой mark keep/pinned (`2117-2122`), сбой `bt_free_rec` (`2123-2126`).
- rollback-сторона: `spt0_restore:1344` отказывается при повреждённом `base_root` (`1367-1372`) или `!spt0_data_ok` (`1373-1380`); `spt0_data_ok:1301` требует целостности delta (`1273-1299`) и проверки сегментов (`spt0_seg_verify:1094`).
- `spt0_drop:1423` снимает ARMED в SPN0 (`1438-1448`) и пишет пустой SPT0 (`1450-1455`); `spt0_verify_live:1518` сверяет magic/version/CRC и `base_root/delta_end/flags` (`1542-1551`).

## 6. ИНВАРИАНТЫ В КОДЕ
- `vol_sweep.c:1028`, `1031` — активная запись по id/имени → no-op; `1100-1109` — inode/размер/тип/зона RAW обязательны.
- `vol_sweep.c:1037-1075` — class replay: generation guard для UNCOMPRESSIBLE/GUARD, переадресация EXER/JXL/контейнера.
- `vol_sweep.c:1412-1439` — публикация только после serialize+store+delta_put; при ошибке new blocks frees, файл остаётся RAW.
- `vol_sweep.c:946-990` — `ast_frame_ok`, `orig_len<=SEGMENT_SIZE`, точный размер декомпрессии; иначе задача invalid.
- `vol_sweep.c:1790-1815` — enospc frontier; `1816-1835` — точный decode+memcmp; `1844-1851` — memlimit/guard.
- `vol_exer.c:145-180` — EXER magic, `n<=EXE_MAX_MEDIA`, `8+17n<=pay_len`, непересекающиеся范围和 `member_sum+glue==file_size`.
- `vol_exer.c:227-241` — медиа ≥64KiB и ≥5% файла; `250-257` — decompose memlimit; `332-354` — rebuild guard; `360-367` — сумма blob < оригинала.
- `vol_cpack.c:3572-3576` — `is_container`, лимит имени; `3605-3638` — enumerate/table/memberlimit; `3663-3707` — arc/mem/ENOSPC.
- `vol_cpack.c:3780` — размер каждого извлечённого члена обязан совпасть с таблицей; `3845-3870` — rebuild-эквивалентность и size guard.
- `vol_cpack.c:4055-4079` — `cpack_map_guard`, rollback при отказе; `4060-4078` — обязательный `!mbrmap` inode.
- `vol_dedupe.c:312-327` — пропуск TEXT и JXL/APE/EXER, валидация `csize` и ≤16MiB; `147-181` —精致 remap только при совпадении `block_id` и `cur_pba`.
- `vol_dedupe.c:365,528` — только write-enabled volume и `vol_mark_dirty` перед remap.
- `vol_migrate.c:580-596` — walk-квитирование: незавершённый обход → `walk_refused`, ничего не двигается; `301-314` — проверка записанного кадра; `326-332` — только вниз (`pba_new < old_pba`).
- `vol_walk.h:48-51`, `vol_walk.c:59-67,70-98` — `rc==0 && found<=n`; без `vol_walk_result` commit/abandon → `-1`.
- `vol_spt0.c:57-100` — SPT0/ANCH0 magic+version+CRC; `145-230` — `spt0_tree_ok`; `1461-1487` — `spt0_block_pinned` требует `spn_armed && spn_bitmap && !spn_nopin`.

## 7. ПОДОЗРИТЕЛЬНОЕ
- `vol_sweep.c:471` принимает малый binary до anchored/hot guard’ов (`481,490`).
- `vol_sweep.c:538-540` дублирует `full[0]!=0/1` перед проверкой `full[0]==0x1F`.
- `vol_spt0.c:861` может освободить старый pin до refusal’а в `886` и до ошибки размещения pin в `902-913`.
- `vol_spt0.c:808-814` при `spn_nopin` и сбое `calloc` продолжает обход с `map==NULL`; последствия для `c.map` НЕ-В-КОДЕ.
- `vol_cpack.c:3342-3349` при `newino==inode_id` явно оставляет superseded-сегменты не回收анными.
- `vol_cpack.c:4124-4132` `INVFS_SWEEP_NO_COMPRESS=1` пропускает member deferral, а не только сжатие.
- `vol_sweep.c:1440-1450` `UNCOMPRESSIBLE` решается по `new_total` только переписанных сегментов相对于 полному `in.size`.
- `vol_reclaim.c:318-325` вызывает `mbuf_free` без проверки `spt0_block_pinned`; проверка есть только в `volume.c:3152-3154`.


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. РЕЕСТР КОДЕКОВ
- Таблица встроенных: `static const invfs_codec registry[]` (`src/codecs/codec.c:1332`), 15 строк, `_Static_assert(REGISTRY_N < 64)` (`src/codecs/codec.c:1388`); рабочая таблица `g_all[REGISTRY_N + INVFS_PACK_MAX]` (`src/codecs/codec.c:1393`), пересобирается `packs_ensure()` (`src/codecs/codec.c:1779`).
- Поиск: `invfs_codec_by_algo()` — линейный проход по `g_all` (`src/codecs/codec.c:1918`), `invfs_codec_all()` (`src/codecs/codec.c:1927`), vol-вариант `invfs_codec_by_algo_vol()` с приоритетом паков этого тома (`src/codecs/codec.c:1694`).
- Регистрация внешних: `pack_register_inner()` (`src/codecs/codec.c:1499`), `invfs_codec_register_pack_mem()` (`src/codecs/codec.c:1638`), трамплины `pack_sniff_N` (`src/codecs/codec.c:1111`) и `pack_fns[]` (`src/codecs/codec.c:1195`); сброс `invfs_codec_probe_reset()` (`src/codecs/codec.c:1301`).
- Встроенные, sniff=NULL: `none` (`src/codecs/codec.c:1333`), `lz4` (`:1336`), `zstd` (`:1339`).
- Магические sniff (все `magic_at()` → 100): zip `PK\3\4` (`:250`), tarr `ustar`@257 (`:257`), gzr `1F 8B` (`:264`), pngr `\x89PNG\r\n\x1A\n` (`:271`), flacr `fLaC` (`:278`), ape `MAC ` (`:311`), wv `wvpk` (`:318`).
- Эвристические sniff: pmp `ID3`/`FF Exxx` (`:285`), jxl box/`FF 0A`/JPEG SOI (`:295`), exer = `invfs_binary_family()>0 ? 40 : 0` (`:327`), ppmd = `invfs_text_family()?50:0` (`:177`), nbli переиспользует `sniff_pngr` (`:1378`).
- Паковый sniff: `pack_sniff_impl()` — magic 100, расширение 50 (`src/codecs/codec.c:884`, `:910`).

## 2. СЛОЙ ПАКОВ
- ABI-описание пака: `ivpack_desc{api_version,name,version,pack_class,flags}` (`src/include/ivpack_api.h:22`), версии 2/мин 1 (`src/include/ivpack_api.h:8`), проверка `d->api_version == INVFS_CODEC_ABI_MAJOR` (=2, `src/core/vol_plugin_inproc.h:13`, `src/core/vol_plugin_inproc.c:282`); коды возврата `IVPACK_RC_OK/ERROR/USAGE/DECLINE` (`src/include/ivpack_impl.h:24`). Экспорты: `ivpack_get_desc`, `ivpack_container_cmd`, `ivpack_container_estimate` (`src/core/vol_plugin_inproc.c:272`).
- Возможности: биты `SEEK/BATCHED/WHOLEFILE/CONTAINER/EXTERNAL/PACKONLY` (`src/codecs/codec.h:11`), строки манифеста `caps=` разбираются `parse_caps()` (`src/codecs/codec.c:430`); манифест хранит name/algo/os/arch/caps/dec_mem/generation/magic/команды (`src/codecs/codec.c:346`), публичный контракт — `invfs_pack_def` (`src/codecs/codec.h:61`).
- Команды пака: `ENUMERATE=0 … EXTRACT_BATCH=6` (`src/codecs/codec.h:136`); map в ivpack-команды 1..5 (`src/core/vol_cpack.c:743`) и verify аргументов/dispatch (`src/include/ivpack_impl.h:139`); `EXTRACT_BATCH` идёт только через CLI-шаблон `def->batch` (`src/core/vol_cpack.c:757`).
- Загрузка: каталоги `.codecpack/manifest` и ZIP-запись `manifest` внутри `.ivpack` (`src/codecs/codec.c:1706`, `:1738`), `INVFS_CODECPACKS` / `/usr/lib/invfs/codecpacks` (`src/codecs/codec.c:1755`); ZIP-записи должны быть Stored (`src/core/ivpack_loader.c:125`), `.so` грузится из memfd (`src/core/ivpack_loader.c:279`), бинарник — `fexecve` (`:304`). Пак тома регистрируется из памяти (`src/codecs/codec.c:1638`) и материализуется (`src/codecs/codec.c:1854`).
- Песочница CLI-пака: `tool_exec_lim()` с `invfs_helper_sandbox{pack_dir,ro_path,rw_dir,requires}` (`src/core/vol_cpack.c:681`, `src/core/helper_exec.h:15`); в child — netns, RLIMIT_AS/CPU/NOFILE/NPROC/FSIZE, `no_new_privs`, `PDEATHSIG`, Landlock, очистка env, drop privs (`src/core/helper_exec.c:629`, `:230`, `:151`), fork (`src/core/helper_exec.c:738`).
- `.so` альтернативы: пул воркеров через `/tmp/invfs_plugin_pool.sock` + shm (`src/core/vol_plugin_client.c:66`, `:115`) либо in-process `dlopen` только для jxl после gating (`src/core/vol_plugin_inproc.c:197`, `:272`).

## 3. КАРТА ALGO ID
- Заняты встроенными: `0 none`, `1 zstd`, `2 ppmd`, `3 ape`, `4 jxl`, `5 lz4`, `6 wv`, `7 flacr`, `8 tarr`, `9 gzr`, `10 pngr`, `11 pmp`, `12 zip`, `15 exer`, `25 nbli` — строки таблицы `src/codecs/codec.c:1333`–`:1381`; константы `src/core/invarifs.h:51`–`:79`, `src/codecs/codec.h:22`.
- Без codec-записи в реестре: `13 rawimg` (`src/core/invarifs.h:69`), `14 zstd_bcj` (`:73`, обрабатывается в `src/core/vol_read.c:131`), `24 window_src` (`:82`), `27 child` (`:85`).
- Имена пак-фикстур по коду: `40 evil` (`src/cli/noautopack_test.c:388`), `41 evilc` (`:443`), `42 fakeimg` (`src/cli/codec_test.c:720`), `43 spltmini` (`:749`), `44 badcont` (`:763`), `45 spltmap` (`:775`) и `badpack` (`src/cli/noautopack_test.c:117`), `46 extbad` (`src/cli/codec_test.c:804`) и `vxp` (`src/cli/noautopack_test.c:181`), `61 scope` (`src/cli/codec_test.c:1074`). Реальные пак-манифесты в дереве НЕ-В-КОДЕ.

## 4. ВНЕШНЯЯ ПОВЕРХНОСТЬ
- **Жизненный цикл тома**:
- `invf-mkfs` — создать/форматировать образ или 2 устройства: `main()` `src/cli/mkfs.c:100`, usage `:150`.
- `invf-resize` — изменить размер тома: `main()` `src/cli/resize.c:275`, usage `:304`.
- `invf-rollback` — откат тома: `main()` `tools/invf-rollback.c:12`, usage `:21`.
- `invf-import` — массовый импорт каталога: `main()` `tools/invf-import.c:350`, usage `:339`, `vol_open` `:383`.
- `invf-cp` — добавить_host-файл: `main()` `src/cli/cp.c:29`, usage `:41`.
- `invf-cat` — извлечь named file: `main()` `src/cli/cat.c:17`, usage `:38`.
- `invf-ls` — список имён: `main()` `src/cli/ls.c:16`, usage `:25`.
- `invf-vnode` — put/get/del inode-рядов: `main()` `src/cli/vnode.c:268`, dispatch `:289`.
- `invf-verify` — проверка тома, deep/seal: `main()` `src/cli/verify.c:152`, usage `:170`.
- `invf-fsck` — проверка/починка: `main()` `src/cli/fsck.c:10`, usage `:25`.
- `invf-reconsolidate` — консолидация hot-экстентов: `main()` `tools/invf-reconsolidate.c:207`, usage `:238`.
- **Инспекция**:
- `invf-stats` / `invf-stat` — статистика тома; `main()` `tools/invf-stats.c:114`, usage `:133`; алиас `Makefile:1921`.
- `invf-verbatim` — отчёт по verbatim/листьям: `main()` `tools/invf-verbatim.c:191`, usage `:223`.
- `invf-why` — объяснение выбора кодека для path: `main()` `src/cli/why.c:84`, usage `:97`.
- `ivpack_probe` — проверка ABI .so (desc/est/cmd): `main()` `tools/ivpack_probe.c:39`, usage `:47` (не в `TOOLS`, `Makefile:189`).
- **Свип**:
- `invf-sweep` — свип/перекодирование с `--dry-run/--fast/--compact`: `main()` `tools/invf-sweep.c:1392`, usage `:1475`.
- **Паки**:
- `invf-pack` — list/info/install/keygen/sign/remove/replace/upgrade/verify/inproc: `main()` `src/cli/pack.c:4453`, dispatch `:4470`; подписи `src/cli/pack_sig.c:18`/`:44`.
- `invf-plugin-host` — demon пула воркеров (shm + Unix socket): `main()` `tools/invf-plugin-host.c:257`, пути `:260`.
- `invf-zip` — list zip внутри тома: `main()` `src/recipes/zip.c:147`, usage `:151`.
- **FUSE**:
- `invf-fuse` — монтирование образа, `--probe-uuid`: `main()` `src/cli/fuse_fs.c:3392`, usage `:3430`; сборка `Makefile:343`.

## 5. ТЕСТОВАЯ ПОВЕРХНОСТЬ
- Реестр/sniff/профили: `codec_test.c:157` — ppmd-text-heuristic последний в `invfs_codec_all()`.
- Регистрация паков, caps, override: `codec_test.c:966` — пак с `algo=4` перекрывает builtin EXTERNAL.
- Парсинг манифеста и лимиты: `codec_test.c:833` — имя/алго приходят из manifest.
- Autopack guard: `noautopack_test.c:271` — при guard'е `pack_exec` обязан отказать.
- Подписи паков/ed25519: `pack_sign_test.c:216` — подмененный helper даёт `PACK_SIG_BAD`.
- in-process доверие: `dlopen_trust_test.c:119` — `invfs_inproc_allowed("jxl")` по умолчанию 0.
- ivpack ABI `.so`: `ivpack_packs_test.c:159` — три обязательных экспорта присутствуют.
- Песочница/лимиты helper'а: `helper_exec_test.c:109` — `RLIMIT_AS` виден как 256 MiB.
- Scratch-политика: `scratch_policy_test.c:236` — выбранный tmpdir укладывается в разрешённый root.
- Пул плагинов: `plugin_mt_test.c:119` — потоковые `container_cmd_mem` не ломают ответ.
- Container map: `cpack_map_san_test.c:335` — параллельные чтения возвращают только свои байты.
- Габариты декомпозиции: `cpack_guard_test.c:77` — превышение лимита даёт отказ с причиной.
- Свип: `sweep_orig_len_test.c:123` — переполненный orig_len не роняет процесс.
- Рецепты/контейнеры: `recipe_tree_test.c:64` — сериализация recipe с CHILD-записями; `tar_cap_test.c:181` — 4000 members читаются байт-в-байт.
- FUSE-путь: `walk_status_fuse_test.c:369` — после svc table снова консистентна.
- Кодеки данных: `deflate_repro_test.c:81`, `crc32c_test.c:152`, `gz_header_test.c:187`.

## 6. ИНВАРИАНТЫ В КОДЕ
- Пак не может быть helper'ом (`src/codecs/codec.c:1525`), не может быть без пары encode/decode (`:1530`) или container без enumerate/extract/strip/rebuild (`:1527`).
- Algo пака обязан быть `<64` (`:1533`) и не захватывать встроенный codec без `CAP_EXTERNAL` (`:1539`); дубли algo/имени внутри одного `origin_vol` запрещены (`:1549`).
- Контейнеру принудительно выставляются `CONTAINER|EXTERNAL|WHOLEFILE`, `SEEK` только при наличии `map`, `encode/decode` обнуляются (`src/codecs/codec.c:1602`–`:1608`); `map`/`batch` принимаются только от container (`:1573`).
- Суффиксы blob/raw проходят фильтр `.`+alnum (`src/codecs/codec.c:558`); `sniff.magic` ограничен 8 правилами (`:337`).
- Хост-платформа пака сверяется с `uname()` (`:1466`); таблица паков ограничена `INVFS_PACK_MAX=64` (`:811`).
- Всё выполнение пак-кода блокируется `INVFS_NO_AUTOPACK`/сеттером: probe (`src/codecs/codec.c:817`), материализация (`:1854`), exec (`src/core/vol_cpack.c:668`), cmd (`:714`).
- Пак-команды доступны только контейнерам, шаблон команды обязан существовать, argv ≤24 (`src/core/vol_cpack.c:726`, `:762`, `:711`).
- in-process `.so`: только `jxl`, только opt-in, только закреплённый локальный путь, только с подписью (`src/core/vol_plugin_inproc.c:202`, `:210`, `:220`, `:230`); volume-паки в inproc не допускаются (`:152`). Проверка ABI/имени в отдельном child (`src/core/vol_plugin_inproc.c:282`, `:284`, `:397`).
- Установка пака проходит `pack_sig_gate_install()` (`src/cli/pack.c:3563`), а `EXTRACT_BATCH` не маршрутизируется в ivpack .so (`src/core/vol_cpack.c:747`).

## 7. ПОДОЗРИТЕЛЬНОЕ
- `ivpack_dlmopen()` при неудаче `dlmopen(LM_ID_NEWLM)` молча падает до обычного `dlopen()` — изоляция namespace теряется (`src/core/ivpack_loader.c:291`, `:295`).
- Границы ZIP-записи: проверка `local_off + 30` и `payload_off + usize` без проверки переполнения `uint32_t`/`size_t` (`src/core/ivpack_loader.c:129`, `:135`).
- Регистрация пака из env-каталогов и `.ivpack` не вызывает проверку подписи; подпись гейтится только при `invf-pack install` (`src/codecs/codec.c:1748`, `src/cli/pack.c:3563`).
- Для `.ivpack` `manifest_tool_ok()` при отсутствии записи возвращается к поиску по хостовому `PATH` (`src/codecs/codec.c:663`, `:688`).
- `INVFS_PACK_SANDBOX=0` полностью отключает режим песочницы; без Landlock остаётся режим 1 без FS-ограничений (`src/core/helper_exec.c:391`, `:414`).
- Готовность пула плагинов определяется только фактом существования `/tmp/invfs_plugin_pool.sock` (`src/core/vol_client.c:68`), сам путь и shm фиксированы (`src/core/vol_plugin_client.c:123`).

最后一处 file path 修正: `src/core/vol_client.c:68` → `src/core/vol_plugin_client.c:68`.

- Готовность пула плагинов определяется только фактом существования `/tmp/invfs_plugin_pool.sock` (`src/core/vol_plugin_client.c:68`), сам путь фиксирован (`:123`).


---

## Кросс-проверка: слепые находки ↔ падающие тесты

Самое ценное здесь — совпадения. Слепые агенты не знали ни про тесты, ни про наши
ADR, и всё же указали на те же узлы, что краснеют в e2e:

| находка (слепая) | падающий тест | совпадение |
|---|---|---|
| `vol_heat.c:44-51` повторный `heat_tab_touch` не инкрементит счётчик; `:146-149` тепло поднимается только при вставке | `test-heat`: `INVFS_HEAT_INIT did not seed (rheat=0)` | **прямое** |
| `vol_spt0.c:861` может освободить старый pin ДО отказа в `:886` и до ошибки размещения pin `:902-913` | `test-rollback`, `test-watermark` (SPT0 refuse), unit-шард 4 | **прямое** |
| `vol_reclaim.c:318-325` зовёт `mbuf_free` без проверки `spt0_block_pinned` (проверка есть только в `volume.c:3152-3154`) | `test-resize` rc=139 (SIGSEGV) — кандидат | вероятно |
| `vol_cpack.c:3342-3349` при `newino==inode_id` оставляет superseded-сегменты неосвобождёнными | `test-dedupe` «free blocks did not stabilize» | вероятно |
| `vol_delta.c:2225-2236, 2385-2400` truncate освобождает блоки новых сегментов ДО финального barrier, а указатель RT30 меняется раньше | — (окно потери при крэше) | не покрыто тестом |
| `vol_metabuf.c:381-383` слот выбирается как `seq&1` до инкремента, а `mbuf_root_read()` `:435-437,463-468` трактует `seq&1` как «новый» после | — (риск чтения не того слота) | не покрыто тестом |
| `ivpack_loader.c:291,295` при сбое `dlmopen` молча падает до `dlopen` (теряется изоляция namespace) | `test-helper-isolation` — **проходит**, т.е. дыра не проверяется | не покрыто тестом |
| `codec.c:1748` регистрация пака из env-каталогов/`.ivpack` не проверяет подпись (только `invf-pack install`) | `test-pack-sign` — **проходит** (проверяет только install-путь) | не покрыто тестом |
| `vol_seal.c:77-83` `seal_recover_segment` всегда `-1`, но `vol_read.c:95-99` — единственный путь восстановления; `vol_seal2_repair` `:2015-2021` только печатает отказ, `vol_repair.c` пуст | — (восстановление фактически отсутствует) | не покрыто тестом |
| `vol_png.c:949-953` на ошибке освобождает `pba`/`rblob`, но не `cblob` | — (утечка памяти) | не покрыто тестом |
| `vol_tier.c:8-19` `wp25_map`/`wp25_unmap` — пустые заглушки | `test-multidev` «no demotion under pressure» — кандидат | вероятно |

Вывод: три из шести «группы E» в `OPEN-ISSUES.md` (heat, multidev, resize) имеют
названного слепым агентом подозреваемого с точностью до строки. Это и есть польза
упражнения — не «документация», а список мест, где код расходится с ожиданием.
