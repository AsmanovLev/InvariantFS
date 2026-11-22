# CODE-MAP: code-based map of the codebase (blind run)

**Code snapshot:** commit **`f0f9924`** (2026-11-19 09:13 +0700), branch `main`.
The tree was copied from this state; `git diff f0f9924..HEAD -- src tools`
was empty AT THE TIME OF THE SNAPSHOT, so the map described exactly it.

**DRIFT (checked at `8e6ea9c`, 2026-11-22 05:39):** that diff is now 15 files,
838 insertions, 45 deletions. Line numbers below are still in snapshot
coordinates and must not be trusted as-is. Parts KNOWN superseded:

- `§177` (sweep pending path) — candidates no longer resolve their name with a
  reverse walk per file (`vol_sweep_name_of`); one id→path snapshot is built per
  run and used as a verified hint. See `OPEN-ISSUES.md` §1a rows 11–12.
- the `pba_ref_ensure` call in the sweep's remap path — removed; the drain
  rebuilds once per pass, and pba-form retire entries are replayed instead of
  rebuilt. See `OPEN-ISSUES.md` §1a rows 14–15.
- `src/core/vol_delta.c` (`vol_delta_range` ordering), `src/core/volume.h`
  (name-index API), `src/cli/fuse_fs.c` (sweep driver), and the new
  `PERF=1` counters (`src/core/perf_counters.{c,h}`) are not in this map at all.

Regeneration means redoing the blind run (see Method), not editing this file.

**Method.** Tree `/var/tmp/blind/tree` — only `src/`, `tools/*.c|h`, `Makefile`:
- **all comments stripped** (`/* */`, `//`), 338 files;
- **no documentation**: neither `docs/`, nor `impl_docs/`, nor `*.md`, nor `CHANGELOG`;
- **no `.git`** — that is, no history and no commit messages.

Four agents (`opencode-go/hy4-flash-free`) worked **each in its own copy of the tree**,
in clean CLI sessions (`pi -p`), without projecting the correspondence — they knew nothing
either about ADRs or about our measurements. Every claim had to have a `file:line`;
what was not visible in the code was marked `NOT-IN-CODE`.

Blindness check: the reports contain **zero** mentions of `README`/`docs/`/`ADR`/`impl_docs`/
"the comment says".


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. FILE MAP
- `src/core/vol_format.c` — format accept/refuse; `invfs_format_accept()` `vol_format.c:7`; refusal codes `vol_format.h:10`, `vol_format.h:12`, `vol_format.h:14`.
- `src/core/vol_metabuf.c` — BPG3 pages, CRC, placement in the metazone, two RT30 slots; `mbuf_page_crc()` `vol_metabuf.c:29`, `mbuf_write()` `vol_metabuf.c:102`, `mbuf_alloc()` `vol_metabuf.c:228`, `mbuf_rt30_load()` `vol_metabuf.c:309`, `mbuf_rt30_store()` `vol_metabuf.c:340`, `mbuf_root_publish()` `vol_metabuf.c:353`, `mbuf_root_read()` `vol_metabuf.c:391`.
- `src/core/vol_btree.c` — B-tree of the base tree, bulk build, xattr/inode rows, orphan reclaim; `btree_search()` `vol_btree.c:424`, `btree_upsert()` `vol_btree.c:616`, `btree_delete()` `vol_btree.c:916`, `btree_bulk_new()` `vol_btree.c:1105`, `btree_bulk_add()` `vol_btree.c:1151`, `btree_bulk_finish()` `vol_btree.c:1194`, `btree_check()` `vol_btree.c:1403`, `btree_check_tolerant()` `vol_btree.c:1624`, `btree_excise()` `vol_btree.c:1864`, `btree_reclaim()` `vol_btree.c:2070`, `btree_collect_orphans()` `vol_btree.c:2622`, `vol_bitmap_flush()` `vol_btree.c:2979`, `vol_base_root()` `vol_btree.c:3055`, `vol_xattr_delta_set()` `vol_btree.c:3462`.
- `src/core/vol_delta.c` — append-only delta segments, index, staged overwrite, retire; `vol_delta_rec_parse()` `vol_delta.c:181`, `vol_retire_recipe()` `vol_delta.c:789`, `vol_delta_pad_build()` `vol_delta.c:990`, `vol_delta_heal()` `vol_delta.c:1076`, `vol_delta_append()` `vol_delta.c:1169`, `vol_delta_drain()` `vol_delta.c:1356`, `vol_delta_lookup()` `vol_delta.c:1415`, `vol_delta_mount()` `vol_delta.c:2074`, `vol_delta_truncate()` `vol_delta.c:2254`, `vol_delta_close()` `vol_delta.c:2413`.
- `src/core/vol_fold.c` — fold of the delta into the base, delta reset and reclaim; `vol_fold_trigger()` `vol_fold.c:46`, `fold_reclaim_hook()` `vol_fold.c:216`, `fold_delta_reset()` `vol_fold.c:245`, `vol_fold()` `vol_fold.c:419`, `vol_fold_request()` `vol_fold.c:610`, `vol_reclaim_schedule()` `vol_fold.c:682`.
- `src/core/vol_records.c` — wrappers for writing/reading inode, dirent, xattr, class; `vol_create_file()` `vol_records.c:13`, `vol_create_file_with_meta()` `vol_records.c:25`, `vol_delete_inode()` `vol_records.c:58`, `vol_get_meta_rc()` `vol_records.c:482`, `vol_apply_meta()` `vol_records.c:524`, `vol_create_symlink()` `vol_records.c:533`, `vol_create_special()` `vol_records.c:553`, `vol_get_xattr()` `vol_records.c:577`, `vol_set_xattr()` `vol_records.c:586`, `vol_list_xattr()` `vol_records.c:643`, `vol_stamp_class()` `vol_records.c:698`, `inode_area_make_room()` `vol_records.c:743`.
- `src/core/vol_ast.c` — recipe-blob AST, WINW/CHLD tails, block release; `vol_ast_recipe_serialize()` `vol_ast.c:13`, `..._serialize_win()` `vol_ast.c:23`, `..._serialize_tree()` `vol_ast.c:35`, `..._serialize_tail()` `vol_ast.c:47`, `vol_ast_recipe_tail()` `vol_ast.c:145`, `vol_ast_recipe_parse()` `vol_ast.c:234`, `vol_free_recipe_blocks()` `vol_ast.c:366`, `vol_release_superseded_blob()` `vol_ast.c:403`, `vol_zip_parse_children()` `vol_ast.c:443`.
- `src/core/vol_meta_merge.c` — MET0 and the extent table; `meta_mapper_load()` `vol_meta_merge.c:11`, `meta_mapper_flush()` `vol_meta_merge.c:36`, `meta_mapper_get()` `vol_meta_merge.c:52`, `meta_mapper_set()` `vol_meta_merge.c:63`, `meta_met0_crc()` `vol_meta_merge.c:73`, `meta_met0_persist()` `vol_meta_merge.c:80`, `meta_get_append_pos()` `vol_meta_merge.c:98`, `meta_get_owner_append_pos()` `vol_meta_merge.c:247`.
- `src/core/vol_anchor.c` — tail ANC0 anchor; `anchor_crc()` `vol_anchor.c:15`, `anchor_state_of()` `vol_anchor.c:39`, `anchor_pba()` `vol_anchor.c:59`, `anchor_build()` `vol_anchor.c:70`, `anchor_probe()` `vol_anchor.c:103`, `anchor_refresh()` `vol_anchor.c:163`, `anchor_invalidate_at()` `vol_anchor.c:198`, `anchor_block_protected()` `vol_anchor.c:231`.
- `src/core/vol_crash.c` — dirty state and latch; `vol_mark_dirty()` `vol_crash.c:11`, `vol_pre_record()` `vol_crash.c:53`, `vol_needs_recovery()` `vol_crash.c:62`, `vol_io_error_latch()` `vol_crash.c:70`.

## 2. ON-DISK STRUCTURES
- Superblock: fields `magic/uuid/state/block_size/total_blocks/zones/root_ast_hash/checksum/meta_mapper_*` `invarifs.h:146-201`; written `INVFS_STATE_DIRTY` `vol_crash.c:21` via `vol_write_sb()` `vol_crash.c:22`, CRC `volume.c:1874-1877`.
- RT30 at `0x9D0`: `magic,version,page_size,root_slot[2],delta_pba,seq,crc32c` `invarifs.h:456-471`; serialization `vol_metabuf.c:344-346`.
- ANC0 in the last block `vol_anchor.c:59-68`: `magic,version,geo,uuid,rt,spt0,crc32c` `invarifs.h:565-584`; assembly `vol_anchor.c:70-85`, write `vol_anchor.c:178`, `blkio_flush()` `vol_anchor.c:99`.
- MET0 at `0x3A0`: `magic/version/active_extent/active_offset/extent_count/crc32c` `invarifs.h:386-399`; write `vol_meta_merge.c:83-92`; mapper table — 32 blocks `vol_meta_merge.c:17-28`, write `vol_meta_merge.c:39-46`;(entry = pba|class<<60) `invarifs.h:681-683`.
- Metarecord slots: absolute position `pba*512?` — no: `pba*INVFS_BLOCK_SIZE + offset` `vol_meta_merge.c:239-240`, `vol_meta_merge.c:348-349`; `invfs_inode_rec` `invarifs.h:1047-1060`, `invfs_meta_ext_hdr` `invarifs.h:1089-1109`; the record serialization itself is NOT-IN-CODE in this area (only slot allocation `volume.c:2924-2953`).
- BPG3 page: `magic,gen,level,nentries,checksum` `invarifs.h:593-604`; CRC over the whole page `vol_metabuf.c:29-35`; writing entries `vol_btree.c:162-195`; parsing with bounds checks `vol_btree.c:214-246`; record sizes `vol_btree.c:111-116`.
- `invfs_blkptr{pba,checksum,gen,flags}` `invarifs.h:609-618`; filled from the page header `vol_metabuf.c:116-124`.
- Delta segment: 32 blocks `invarifs.h:630-633`, 44-byte BigEndian header `vol_delta.c:299-321` (`DSG3/version/hdr_size/seg_blocks/seg_seq/prev_pba/next_pba/crc`), fields `invarifs.h:649-666`; record — 10-byte `klen/vlen/flags/crc` + key + value `vol_delta.c:1258-1267`, parsing `vol_delta.c:181-234`; pad records `vol_delta.c:990-1015`.
- inode value in the B-tree: `invfs_inode_row` `invarifs.h:1147-1161`, encoding `vol_btree.c:2889-2907`; xattr key `0x03|inode|nlen|name[|00|idx]` `vol_btree.c:3326-3348`.
- Recipe blob: V1/V2/V3 header `invarifs.h:859-888`, entry stride 32/40 `invarifs.h:868-876`; writing `vol_ast.c:55-140`, WINW/CHLD tail `vol_ast.c:127-137`, parsing `vol_ast.c:145-189`; the physical writing of the blob is NOT-IN-CODE in this area.

## 3. PUBLICATION ORDER
1. `vol_pre_record()` → `vol_mark_dirty()` → sb.state=DIRTY and `vol_write_sb()` `vol_crash.c:53-58`, `vol_crash.c:21-25`; at the 4/5 threshold `vol_flush()` `vol_crash.c:36-46` (bitmap + barrier `volume.c:1901-1913`).
2. New tree pages are written before the root: `bt_write()` `mbuf_alloc()`+`mbuf_write()` `vol_btree.c:155-195`; `publish()` = `vol_bitmap_flush()` → barrier `" inode pages"` → `mbuf_root_publish()` `vol_btree.c:3048-3052`.
3. Root: page/gen check `vol_metabuf.c:370-374`, bitmap flush `vol_metabuf.c:378`, slot `seq&1` and `seq++` `vol_metabuf.c:381-383`, `mbuf_rt30_store()` `vol_metabuf.c:386`, barrier `vol_metabuf.c:388`; store writes RT30 `vol_metabuf.c:344-346`, then `anchor_refresh()` `vol_metabuf.c:349` → ANC0 `vol_anchor.c:178` → barrier `vol_anchor.c:194`.
4. Delta: 32-block segment + header `vol_delta.c:1146-1153`, header barrier `vol_delta.c:1153`, then `rt.delta_pba=pba` and RT30 `vol_delta.c:1214-1225`, publish barrier `vol_delta.c:1233`; record write `vol_delta.c:1269-1270`, group barrier `vol_delta.c:1293`, drain barrier `vol_delta.c:1383`.
5. Fold: new pages → bitmap flush → barrier `"fold base pages"` → tolerant check → `mbuf_root_publish()` `vol_fold.c:372-399`/`539-576`; then `fold_delta_reset()`: `vol_delta_close()`, `rt.delta_pba=0`, RT30, barrier, `vol_delta_mount()` `vol_fold.c:245-264`; after that reclaim of segments and the old root `vol_fold.c:412-415`, `vol_fold.c:592-598`.
6. Delta truncate: zeroing the tail + rewriting the header `vol_delta.c:2214-2217`, then `rt.delta_pba` and RT30 `vol_delta.c:2225-2227`, free of new segments `vol_delta.c:2231-2235`, barrier `vol_delta.c:2236`.
7. Anchor invalidation: zero block `vol_anchor.c:211`, `vol_delta_heal()` `vol_anchor.c:213`, barrier `vol_anchor.c:215`, `ABSENT` state `vol_anchor.c:216`.

## 4. INVARIANTS IN CODE
- Foreign format refused when `VOLF_META` is absent `vol_format.c:20-29`; `format_version` is not checked `vol_format.c:12`.
- Page CRC/magic `vol_metabuf.c:29-59`; ptr additionally checks `checksum`+`gen`, bitmap and range `vol_metabuf.c:126-142`; root_read selects only valid slots and refuses named but broken ones `vol_metabuf.c:423-472`.
- Allocator: refusal for readonly/non-4K `vol_metabuf.c:235-238`, boot pages `vol_metabuf.c:240-246`, meta-zone/shadow fallback `vol_metabuf.c:193-218`, `vol_metabuf.c:248-254`; rollback on failed write `mbuf_free()` `vol_metabuf.c:258-260`.
- B-tree: entry/size limit `vol_btree.c:144-147`, child.pba!=0 `vol_btree.c:150-153`, page rollback `vol_btree.c:184-191`, parsing bounds `vol_btree.c:212-246`; split refusals `vol_btree.c:496`, `vol_btree.c:555`, `vol_btree.c:587`; upsert arguments `vol_btree.c:622-625`; bulk `vol_btree.c:1165-1175`, `vol_btree.c:1198-1208`.
- Before publication fold refuses when `q.bad_pages>0 || q.qfull` `vol_fold.c:383-397`, `vol_fold.c:556-571`;/free old root only after publication `vol_fold.c:415`, `vol_fold.c:598`.
- Delta record: `klen!=0`, unknown flags, delete with value, READONLY, size limit `vol_delta.c:1183-1200`; record format `vol_delta.c:207-228`; header version/geom/CRC `vol_delta.c:272-285`; torn tail trimmed to the last valid record `vol_delta.c:2033-2062`.
- Delta rollback: OOM when creating a segment `vol_delta.c:1141-1157`; RT30/barrier refusal restores `delta_pba` and `dl_doom_locked()` `vol_delta.c:1225-1240`; write error → doom `vol_delta.c:1270-1277`; barrier error rolls back counters `vol_delta.c:1293-1308`; drain → doom `vol_delta.c:1381-1394`.
- Fold guards: `VOLF_META`/READONLY `vol_fold.c:430-434`, empty delta `vol_fold.c:435-436`, space: `fold_space_ok()` `vol_fold.c:443-447`, collection error `vol_fold.c:451-458`, empty-page rollback `mbuf_free()` `vol_fold.c:520-533`; `fold_delta_reset()` error → `-1` `vol_fold.c:408-416`, `vol_fold.c:586-600`.
- Meta extents: wrlock `vol_meta_merge.c:106`, unlock on all error paths `vol_meta_merge.c:126-147`, `vol_meta_merge.c:167-189`, `vol_meta_merge.c:225-236`, `vol_meta_merge.c:279-336`; `-2` = ENOSPC `vol_meta_merge.c:189`, `vol_meta_merge.c:226`, `vol_meta_merge.c:281`, `vol_meta_merge.c:316`, `vol_meta_merge.c:336`; size check `vol_meta_merge.c:158`, `vol_meta_merge.c:306`, `vol_meta_merge.c:334`.
- Anchor: magic/version/CRC `vol_anchor.c:39-45`, geometry `vol_anchor.c:137-155`, refusal on IO/absence `vol_anchor.c:111-135`,	failed-refresh latch `vol_anchor.c:181-190`, tail block protection `vol_anchor.c:231-241`.
- Crash: write-enabled/refused `vol_crash.c:13-19`, `dirty` short-circuit `vol_crash.c:20`, sb not marked on write error `vol_crash.c:22-25`, latch and rollback of `inode_area_pos` `vol_crash.c:75-84`.
- xattr: name/size checks and fault `vol_btree.c:3469-3492`, chunks exceeded → `-2` `vol_btree.c:3517-3518`, clean-up of trailing chunk records `vol_btree.c:3524-3535`.

## 5. DEPENDENCY DIRECTION
- `vol_format.c:3` → only `vol_format.h` → `invarifs.h` `vol_format.h:6`; no external calls.
- `vol_crash.c:4-5` → `volume_internal.h`, `vol_storage.h`; calls `vol_write_enabled()` `vol_crash.c:13`, `vol_write_sb()` `vol_crash.c:22`, `vol_flush()` `vol_crash.c:57`, `meta_mapper_get()` `vol_crash.c:37`.
- `vol_meta_merge.c:4` → `volume_internal.h`; calls `alloc_meta_extent()`/`extend_meta_extent()` `vol_meta_merge.c:125`, `vol_meta_merge.c:174`; called from `volume.c:2929`, `volume.c:2967`, `vol_crash.c:37`.
- `vol_metabuf.c:4-6` → `volume_internal.h`, `vol_metabuf.h`, `vol_anchor.h`; calls `anchor_refresh()` `vol_metabuf.c:349`, `vol_bitmap_flush()` `vol_metabuf.c:378`, `alloc_blocks()` `vol_metabuf.c:252`; called by `vol_btree.c:7`, `vol_delta.c:6`, `vol_fold.c:13`, `vol_anchor.c:11`.
- `vol_btree.c:4-9` → `volume_internal.h`, `vol_fault.h`, `vol_btree.h`, `vol_metabuf.h`, `vol_delta.h`, `vol_reclaim.h`; calls `vol_delta_append()` `vol_btree.c:3189`, `vol_ast_recipe_parse()` `vol_btree.c:5999`, `vol_reclaim_reader_snapshot()` `vol_btree.c:3070`.
- `vol_delta.c:4-6` → `volume_internal.h`, `vol_delta.h`, `vol_metabuf.h`; calls `vol_mark_dirty()` `vol_delta.c:1195`, `mbuf_rt30_store()` `vol_delta.c:1225`, `vol_inode_row_key()` `vol_delta.c:801`, `vol_free_recipe_blocks()` `vol_delta.c:946`.
- `vol_fold.c:10-14` → `volume_internal.h`, `vol_btree.h`, `vol_delta.h`, `vol_metabuf.h`, `vol_reclaim.h`; calls `btree_bulk_finish()` `vol_fold.c:366`, `vol_delta_mount()` `vol_fold.c:263`, `vol_reclaim_delta_segments()` `vol_fold.c:412`.
- `vol_records.c:4-6` → `volume_internal.h`, `vol_fault.h`, `vol_walk.h`; calls `vol_xattr_delta_set()` `vol_records.c:586`, `meta_mapper_get()` `vol_records.c:748`, `vol_write_bulk()`/`vol_create_node()` `vol_records.c:32-33`.
- `vol_ast.c:4-5` → `volume_internal.h`, `vol_delta.h`; calls `vol_retire_recipe()` `vol_ast.c:418`, `vol_recipe_load()` `vol_ast.c:384`, `vol_free_blocks()` `vol_ast.c:366`; called from `vol_btree.c:5999`.
- `vol_anchor.c:7-13` → `volume_internal.h`, `vol_anchor.h`, `vol_delta.h`, `vol_metabuf.h`, `vol_spt0.h`, `blkio.h`; calls `vol_delta_heal()` `vol_anchor.c:192`, `anchor_pba()` `vol_metabuf.c:291`.

## 6. SUSPICIOUS
- `INVFS_FMT_REFUSE_GENERATION/FORBIDDEN` are declared but unused; `format_version` is explicitly ignored `vol_format.h:12`, `vol_format.h:14`, `vol_format.c:12`.
- `mbuf_rt30_store()` does not check the result of `anchor_refresh()` and returns 0 `vol_metabuf.c:349-350`, although a refresher refusal raises `anchor_refresh_failed` `vol_anchor.c:181-190`.
- The root slot is selected as `seq&1` before the increment `vol_metabuf.c:381-383`, but `mbuf_root_read()` treats `seq&1` as the "new" slot when gen is equal after the increment `vol_metabuf.c:435-437`, `vol_metabuf.c:463-468`.
- Fold publishes the new root before the delta reset `vol_fold.c:398`, `vol_fold.c:572`; on reset refusal it returns `-1` after reclaim `vol_fold.c:408-416`, `vol_fold.c:586-600`.
- Double `fold_reclaim_hook`: inside `vol_fold()` `vol_fold.c:598` and again in `vol_fold_request()` with `fold_pre_root` `vol_fold.c:650-668`; the field is cleared only in `vol_reclaim_schedule()` `vol_fold.c:693`.
- Truncate frees blocks of new segments before the final barrier `vol_delta.c:2225-2236`, `vol_delta.c:2385-2400`, although the RT30 pointer changes before the barrier.


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. FILE MAP
- `vol_write.c`: write session `invfs_wsession` (`vol_write.c:12`); entry points: `vol_write_begin:93`, `vol_write_range:720`, `vol_write_truncate:802`, `vol_write_read:914`, `vol_write_commit:1113`, `vol_write_abort:1138`, `vol_write_bulk:1172`, active sessions `vol_write_active_{name,id}:59,71`.
- `vol_read.c`: reading recipe/segments; entry points: `seg_read_checked:62`, `ast_frame_ok:260`, `vol_read_inode:1176`, `vol_read_file:1276`, `vol_read_named:1284`, `vol_stat{,_full}:1355,1364`, `vol_read_range:1786`.
- `vol_png.c`: no session walk — direct block writes `io_pwrite` (`vol_png.c:937,938,1023,1024`) + recipe + inode; entry points: `vol_create_blob_file:862`, `vol_publish_blob_inode:972`, `vol_publish_window_inode:1065`, `vol_create_png_file:500`, `vol_create_nbli_file:753`, `invfs_png_from_{jxl,nbli}:103,829`, `vol_create_{jxl,ape,pmp}_file:1159,1167,1175`.
- `vol_textzone.c`: batch repacking into `INVFS_ZONE_TEXT`; `tz_defer:111`, `bz_defer:121`, `tz_sniff_any:154`, `tz_reg_owned_blocks:380`, `vol_tz_flush:958`, `vol_acc_pending:973`, `vol_tz_gc:999`.
- `arc.c`: ARC cache with mutex, T1/T2/B1/B2 (`arc.c:13-50`); `arc_create:163`, `arc_get:195`, `arc_get_copy:223`, `arc_put:295`, `arc_invalidate:378`, `arc_clear:392`, `arc_stats:411`.
- `vol_seal.c`: parity/sealing and healing; `vol_redun_config:521`, `vol_seal:937` → `vol_seal_ex:965`, `vol_seal_verify:1976`, `vol_seal_scrub:1986`, `vol_seal2_repair:2015`, `vol_seal_heal:3228`.
- `vol_repair.c`: empty (`vol_repair.c:1-2`); recovery functions declared in `volume.h:974,990`, implemented in `vol_seal.c:2015,3228`; separate logic in the file is NOT-IN-CODE.
- `vol_cut.c`: truncation to a valid prefix; `vol_content_cut:457`, `vol_cut_span_ok:479`, decode `cut_decode_span:16`.
- `vol_tier.c`: `tier/rawm` tables in `v` (`vol_tier.c:92,103,275,289`), mirroring `wp25_rawm_write:333`, `wp25_on_free:397`, `wp25_fsck_prune:434`, `vol_tier_migrate:670`.
- `vol_heat.c`: per-session counter + xattr heat; `heat_touch_read:139`, `heat_fold:371`, `vol_heat_persist:443`, `vol_heat_sweep_begin:525`, `vol_heat_promote:694`.
- Constants: `INVFS_SEGMENT_SIZE (64u*1024u)` `volume.h:650`, `SEGMENT_SIZE` alias `volume_internal.h:149`, `SEAL_SYM_BYTES 65536u` `vol_seal.h:15`.

## 2. WRITE PATH
1. `vol_write_begin` (`vol_write.c:93`): name/`vol_write_enabled` check (`100`), `next_inode_id++` (`105`), `child_segs` from env (`109`, `82`), `vol_find_rc` (`113`), inclusion in `v->wsessions` (`125`).
2. `vol_write_range` (`720`): `EFBIG` (`725`), `wsession_stale` (`739`), `wsession_load_old` (`741`).
3. `wsession_load_old_impl` (`610`): old recipe (`627`), `old_pbas` (`653`), exit on `truncating` (`659`); simple case — aliasing (`676`), complex case — materialization via `vol_read_range` (`694`) and `wsession_write_seg` (`700`).
4. Segment: `wsession_write_seg:471` → `wsession_write_seg_n:356`; child switch `wsession_switch_child:275`/`wsession_seal_child:207`; growth of `ents/touched` `382`; compression `387-417`; RAW fallback `418`; CRC+8-byte header `421-431`; `phys_blocks` `429`.
5. Allocation: `alloc_raw_or_shadow` `437`; write `write_segment_blocks` `439`; free on error `442` and of the former PBA `449-452`; record fill `455-464`; `mat_upto/dirty` `464-465`.
6. `vol_write_truncate` (`802`): without child — frees `839-846`; with child — `vol_free_recipe_blocks` `856`, frees `870-874`; tail padding `880-890`; extended zero-fill `893-903`.
7. `vol_write_commit` (`1113`): stale/load `1125-1128`, only `VOLF_META` (`1131`) → `vol_write_commit_impl:948`: checks `959`, trimming of excess `974-980`, rewriting the tail `985-1014`, `wsession_seal_child` `1034`, `vol_ast_recipe_serialize{,_tree}` `1024,1036`, `vol_recipe_store` `1027,1039`, inode update/create `1041-1059`, `vol_retire_pbas` `1079`, `pba_ref_modify/validate` `1085-1094`, `vol_delete_siblings` `1098`, frees `1100-1107`, unlink/committed `1108-1109`.
8. `vol_write_abort` (`1138`): unlink `1143`, frees by `touched` `1147-1155`, `vol_free_recipe_blocks` of child recipes `1157`, structure frees `1160-1167`.
9. Ordering branches: `truncate` skips materialization (`659-663`); Turbo/`raw_nocompress` skips compression (`387`); `raw_effort_zlevel` switches LZ4↔ZSTD (`341-350`, `388-417`); child switch seals the current recipe before writing to another (`277-282`, `365-368`).

## 3. READ PATH
- `vol_read_file:1276` → `vol_read_inode:1176`: raw-blob branch `1191-1201`; otherwise `vol_recipe_load:1208`, `vol_ast_recipe_parse:1212`, check `ah.file_size == in.size` `1222`, `malloc(len)` `1226`, tail `1231`, `vol_decode_ast_entries:1243`, output `1268-1269`.
- `vol_decode_ast_entries:442`: bounds/overlap checks `454-471`; streaming branch `490-536`; sequential `541`; CHLD → `vol_decode_child_full:420` + `vol_read_range:432`; TEXT → `vol_read_text_slice:559` (`vol_read.c:119`); whole-file → `vol_read_inode:1694`; common `pba`+heat path `648-660`.
- `seg_read_checked:62`: 2-device tier fallback `66-77`, dev0/rawm fallback `80-90`, `seal_recover_segment` `95-99`.
- `seg_read_once:25`: **stored length** `csize` from the 8-byte header at `pba*INVFS_BLOCK_SIZE` `34`, `plen`/CRC check `37-51`, reading `blob` `48`.
- Decode: `ast_frame_ok:260`; LZ4/ZSTD `674-693`; `ALGO_NONE` `1118`; codec/container `1122-1163`.
- Decode length = `e->length` from the recipe record (`vol_write.c:457`, read at `vol_read.c:674,685,1118,1736,1742`), buffer `tmp[SEGMENT_SIZE]` `1525`/heap `1713-1715`, child `ctmp` `1585`/`1611-1613`; short read → refusal `1776-1782`.
- TEXT: header `csize` `170`, `usize` from the first 4 bytes of the blob `177`, limit `64u<<20` `177`, ARC `159,215`, BCJ shift `148,220-224`.

## 4. CHUNK SIZE
- `volume.h:650` + `volume_internal.h:149`: base 64 KiB; changing it alters all segment offsets/indices and the compatibility of written recipes: `vol_write.c:172,212,244,248,251,455,473,482,499-515,555-570,589,681-695,745-746,763,774-793,822-824,853,860,880-887,897,932-935,974,985-1009`; `vol_read.c:1525,1585`; `vol_heat.c:748`.
- 64 KiB stack buffers: `vol_write.c:784,883,935,989,1009`; `vol_read.c:1525,1585` — enlarging the macro grows the stack of every thread/call (`vol_read.c:279,311`).
- `SEAL_SYM_BYTES` (`vol_seal.h:15`): arithmetic `vol_seal.c:163,166,841,868,1314,1400,2441,2544,3285`; headers/checks `180,242,275,377,474,1689,1732,2193`; buffers `889,1020,1540,1588,1869,2106,2464,2501-2507,2854-2876,2975,3045-3089`; RS `703,732,2550,2923,3063,3193`; parity writing/reading `917-919,1479-1483,1558-1572,1878-1892,2545-2554,2904-2906,2937-2951,3205-3213`. Changing it breaks the existing parity layout and header validation.
- Literal `65536`: `vol_png.c:781` (only an encoder capacity margin; smaller → reallocation/calculation overflow), `vol_textzone.c:29,31` (limit of env `INVFS_TZ_BATCH_KB`; larger/smaller changes the batch size and number of batches).
- Not 64 KiB but 64 MiB: `vol_read.c:177` (`64u<<20` TEXT-batch limit), `vol_tier.c:613` (`64*1024*1024` copied-fragment limit).

## 5. STATE
- Per-session (`invfs_wsession`, `vol_write.c:12-58`): `v/name/new_id/old_id` (`99-113`); `gen/stale` (`111,152-156`); `loaded/truncating/have_old/owns_siblings/committed` (`121,647,659,711-712,1109`); `ents/n_ents/touched/touched_cap/aliased_n/mat_upto` (`382,455-464,488,604,676-689`); `logical_size` (`666,794,906`); `old_pbas/old_n_ents` (`651-656,1079`); `child_segs/cur_child/top_ents/chld` (`109-110,212-285,1034-1039`).
- Written but not read: `wheat_carry` (`108,625`), `dirty` (`465,907`), `old_ext/old_ext_len` (`free` only `1162`).
- Per-volume: `v->wsessions` (`125,137`), `v->write_gen` (`111,152`), `v->next_inode_id` (`105`), `v->free_blocks/reserved_blocks/hard_min_blocks` (`765-767,899-901`), `v->raw_free` (`343`), `v->arc` (`vol_read.c:159,215`), `v->ndev/dev0_present` (`vol_read.c:66,81`), `v->tier/rawm + *_dirty/owner` (`vol_tier.c:92,103,289-299,364,625`), `v->heat_tab/heat_tab_n/heat_any_rhot/heat_any_whot` (`vol_heat.c:22-52,146-149,571-572`), `v->seal_dirty` (`vol_seal.c:17-66,1099,1426`), `v->rd/rd_present` (`vol_seal.c:539-550,1371`).
- env config inheritance: `INVFS_CHILD_SEGS` (`vol_write.c:82-90`), `INVFS_RAW_ADAPT` (`329-336`), `INVFS_TZ_BATCH_KB` (`vol_textzone.c:22-32`).

## 6. INVARIANTS IN CODE
- `vol_write.c:100` write-enabled; `725/805` `MAX_FILE_SIZE`; `747-755,823-831` entry limit; `765-767,899-901` block reserve; `959-970` commit bounds; `1131` only `VOLF_META`; `151/152` ESTALE; `172-180` "simple" old recipe; `499-515,555-570` exact match of decompression with `e->length`.
- `vol_read.c:37-40` `csize` against `plen`; `51` CRC; `135` slice within `e->length`; `177` `usize` limit; `269-276` `ast_frame_ok`; `454-471` entry bounds/overlap; `426-431` CHLD depth; `1453-1460,1474-1465` range checks; `1748-1751` `hdr == e->length`; `1776` refusal on short coverage.
- `vol_textzone.c:29` env clamp; `648-651` write-enabled; `660-672` name/inode and class match; `465,494` refusal when there is no gain/memory; `563-575` mandatory `complete && !fallback && !committed && n_slices`.
- `arc.c:164` budget>0; `300-305` refusal to write > `c/2`; `257-284` eviction; `286-293` ghost-list bounds.
- `vol_seal.c:1088-1093` refusal on incomplete walk; `1099` increment condition; `2015-2021` detect-only refusal; `3240-3244` read-only refusal; `275,377,474,1689` `SEAL_SYM_BYTES` check; `841` parity-file size check.
- `vol_cut.c:25-40` ban on TEXT/WINDOW_SRC and zero PBA; `43-45` `ast_frame_ok` and 128 MiB limit; `462-466` read-only; `402-425` post-publish readback.
- `vol_tier.c:145` `n>65535`; `333-340` 24-bit key; `349-354` table full; `598-613` promotion limits; `671-675` `ndev==2 && !degraded && write-enabled`.
- `vol_heat.c:145-147,447` write-enabled; `542-562,712-729` refusal on unfinished `vol_walk_commit`; `378-395` fold of live inodes only.

## 7. SUSPICIOUS
1. `vol_seal.c:77-83` `seal_recover_segment` always returns `-1`, although `vol_read.c:95-99` uses it as the only recovery path for a corrupted segment.
2. `vol_seal.c:2015-2021` `vol_seal2_repair` is declared (`volume.h:990`) but only prints a refusal; `vol_repair.c:1-2` is empty — on the actual path recovery goes through `vol_seal_heal:3228`.
3. `vol_read.c:1118` for `ALGO_NONE` copies `hdr` bytes without checking `hdr == e->length`, whereas the range path checks `1748-1751` and `ast_frame_ok` `269-275`.
4. `vol_heat.c:44-51` a repeated `heat_tab_touch` for an already existing inode returns `1` without incrementing the counter; `vol_heat.c:146-149` sets `heat_any_rhot` only on insertion — repeated reads do not upgrade heat in this session.
5. `vol_tier.c:8-19` `wp25_map`/`wp25_unmap` are empty stubs, although `wp25_owner_write_impl:145-170` and `wp25_index_load_impl:214-272` write/read the owner blob with `ord`.
6. `vol_png.c:949-953` on an error in `vol_ast_recipe_serialize` frees `pba`/`rblob`, but not `cblob` (the compressed copy from `897-905`).


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. FILE MAP
- `src/core/vol_sweep.c`: sweep/priority-lane driver; entry points `vol_sweep_file:132`, `vol_sweep_one_ex:1626`, `vol_sweep_pending:1728`, `vol_jxl_retry:286`, `vol_exer_retry:331`, `vol_collect_sweepables_ex:1876`, `vol_compute_stats:2239`.
- `src/core/vol_cpack.c`: external codec packs, materialize, table/map, container disassembly; entry points `invfs_vol_pack_materialize:431`, `invfs_codec_pack_exec:658`, `vol_create_flac_file:1477`, `vol_create_tar_file:1761`, `vol_create_gz_file:1933`, `vol_containerpack_sweep:3515`, `vol_containerpack_sweep_stream:4203`, `vol_cpack_migrate:4218`, `pack_container_rebuild:4285`.
- `src/core/vol_exer.c`: EXER carving of embedded JPEG/PNG from binaries; entry points `exer_payload_parse:145`, `exer_splice:183`, `vol_exer_carve:203`, `vol_exer_last_parts:445`.
- `src/core/vol_dedupe.c`: BLAKE3 hashing of segments, dedupe by recipes; entry points `vol_sweep_dedupe_ex:343`, `vol_sweep_dedupe:586`.
- `src/core/vol_reclaim.c`: orphan/delta/reader-drain/pinned reclaim; entry points `vol_reclaim_orphans:96`, `_full:103`, `_stats:108`, `vol_reclaim_reader_snapshot:162`, `vol_reclaim_reader_release:170`, `vol_reclaim_drain:177`, `vol_reclaim_mark_and_free:231`, `vol_reclaim_delta_segments:301`.
- `src/core/vol_spt0.c`: SPT0 save point + SPN0 data pin; entry points `spt0_load:57`, `spt0_store:127`, `spt0_capture:997`, `spt0_restore:1344`, `spt0_drop:1423`, `spt0_block_pinned:1461`, `spt0_verify_live:1518`, `spt0_reclaim_last:1553`.
- `src/core/vol_migrate.c`: defragmentation/moving segments downward; entry points `vol_migrate_frag:18`, `vol_migrate_run:564`.
- `src/core/vol_walk.c`: walk-completeness acknowledgement (`vol_walk_t`); `vol_walk_init:18`, `vol_walk_result:31`, `vol_walk_commit:70`, `vol_walk_abandon:81`, `vol_walk_reap:100`.
- `src/core/vol_dirs.c`: namespace/walks/deletion; `vol_walk:1062`, `vol_walk_strict:1071`, `vol_walk_strict_dir:1078`, `vol_unlink:724`, `vol_delete_file:1126`, `vol_replace_file:1103`, `vol_hardlink:1147`.

## 2. SWEEP PIPELINE
1. `vol_collect_sweepables_grow:1898` → `vol_iter_live_inodes` (`vol_sweep.c:1884`) ↔ IDs of live inodes, not name[0]==0x01 (`1854-1861`); short walk → `-1` via `vol_walk_commit:1942`.
2. `vol_mark_pending:155` accumulates IDs; `vol_sweep_pending:1728` → `vol_heat_sweep_begin:1729` → `vol_sweep_pending_cap:1701` → name via `vol_sweep_name_of:1664` → `vol_sweep_one:1647` → `vol_sweep_one_ex:1626` re-resolves the name into a live id (`1630-1637`).
3. `vol_sweep_pending_finish:1679`: promote heat (`1683`), `vol_tier_migrate:1686`, `vol_tz_flush:1687`, then `vol_fold_request`/`vol_reclaim_schedule` only if not READONLY (`1691-1695`).
4. `vol_sweep_one_impl:1004`: parse/class-replay (`1035-1090`) → RAW inode (`1100-1109`) → whole-file read and `sweep_dispatch` (`1125-1133`) or an 8KiB head + `vol_containerpack_sweep_stream` for large files (`1141-1169`).
5. On declining dispatch: RAW recipe (`1180-1210`) → in batches of 128 segments (`SWEEP_BATCH_SIZE:892`, loop `1252`) decompression/BCJ/ZSTD in threads (`941-990`) → writing to shadow (`1348-1359`) → new recipe + `vol_inode_delta_put` (`1412-1428`) → `vol_retire_pbas` of old PBAs (`1456`) or rollback of new blocks (`1470-1483`).
6. For a single segment: generic floor `1510-1620`: `INVFS_ALGO_NONE`/LZ4/ZSTD from the profile (`1527-1536`, `1583-1608`), round-trip guard (`1548-1556`, `1596-1605`), publication `vol_publish_blob_inode:1539/1573/1611`.
7. Independent passes: `vol_sweep_dedupe_ex:343` (hash→sort→scan→remap) and `vol_migrate_run:564` (frag→walk→per-file move).
8. Statistics: `wstats_cb:2078` consumes live inodes and produces `invfs_volume_stats` in `vol_compute_stats:2239`; `unclaimed_used_bytes` is computed as the difference between used and claimed (`2267-2277`).

## 3. FILE ROUTING
- `vol_sweep.c:466` — no name/`full_len<4` → `-2`, all lanes skipped.
- `vol_sweep.c:471-479` — small non-sibling binary → `bz_defer` → `10`, skips anchored/hot/magic/packs/floor.
- `vol_sweep.c:481-486` — anchored → `-2`, skips the remaining lanes; `490-493` — hot → `-2`.
- `vol_sweep.c:495-535` — `fLaC` → `vol_create_flac_file` → `2`; skips tar/gz/photo/pack/generic.
- `vol_sweep.c:515-536` — `ustar` → `vol_create_tar_file` → `3` + `defer_container_parts:235`; skips gz and below.
- `vol_sweep.c:538-558` — gzip magic → `vol_create_gz_file` → `4`; skips photo/pack/generic.
- `vol_sweep.c:561-634` — PNG: if the external `EXTERNAL`-sniff did not claim it (`567-575`), builtin PNG/NBLI → `5`; claims → skip builtin and move to the common pack pipeline.
- `vol_sweep.c:638-663` — JPEG: with no external claim (`653`) builtin JXL → `12` (`386-446`); claim → skip builtin, then container/external pack.
- `vol_sweep.c:666-714` — ID3/MPEG → PMP with round-trip (`681-697`) → `8`; refusal sets `GENERIC_GUARD` and continues (`698-713`).
- `vol_sweep.c:722-748` — `CAP_CONTAINER`-sniff → `vol_containerpack_sweep`; `prc>=100` returns, `prc==1` = deferral → `0`, the rest continue; on `deferred` external/generic are skipped.
- `vol_sweep.c:753-779` — `CAP_EXTERNAL`-sniff → `vol_pack_sweep`; `prc==1` → `0`, `>=100` → codec code, `PACKONLY` without encode/decode → `0`.
- `vol_sweep.c:784-793` — `INVFS_SWEEP_NO_COMPRESS=1` → `0`, skips text/EXER/binary/floor.
- `vol_sweep.c:796-812` — text `invfs_text_family` → `tz_defer` → `9`; memory error — `GENERIC_MEMLIMIT`, but the lane continues.
- `vol_sweep.c:814-826` — binary → `vol_exer_carve`; `1` → `11`, `2` (`exer_no_bz`) suppresses only the final `bz_defer:877-884`.
- `vol_sweep.c:829-873` — "try-last": only external codecs without `sniff` claims, with `probe`, without `pack_claims`, with a gain bar from `sweep_lane_ref_size:354`; `declined_algo` is excluded (`840`).
- `vol_sweep.c:877-885` — final `bz_defer` of binary → `10`; otherwise `-2` → generic floor `1500+`.
- `vol_cpack.c:2436-2469` — `cpack_member_is_container` guards magic/sniffs; `2598-2661` — `cpack_size_guard`; `2522-2579` — `cpack_map_validate`; refusal → `out` without publication.

## 4. RELEASE PATHS
- `vol_release_superseded_blob` (`vol_sweep.c:430,506,527,550,626,703`; `vol_cpack.c:3329-3335`): old `recipe_addr` nonzero and the row has already been changed (`vol_ast.c:403-420`).
- `vol_publish_blob_inode` calls `vol_retire_recipe` on successful recipe replacement (`vol_png.c:1048-1056`); on a write error it frees the new pba (`vol_png.c:1004-1022,1030-1041`).
- `vol_retire_pbas`: after sweep publication (`vol_sweep.c:1440-1457`), dedupe (`vol_dedupe.c:229-239`), migration (`vol_migrate.c:395-414`); `keep` protects the new PBAs (`vol_delta.c:820-855`).
- Direct rollbacks: `vol_free_blocks` on failed pwrite/realloc (`vol_sweep.c:1356,1369`), for an unpublished recipe (`1470-1476`), migration (`vol_migrate.c:280,292,311,336,430`), SPT0 pin (`vol_spt0.c:946,958,963`).
- `vol_transcode_abort` → `vol_delete_siblings` (`vol_records.c:411-418`) after EXER errors (`vol_exer.c:395,413`) and cpack errors (`vol_cpack.c:3901,4002,4015,4024,4043,4061,4100`).
- `cpack_rollback_commit` (`vol_cpack.c:3339-3356`): `vol_delete_inode(newino)` only if `newino != inode_id`, plus `vol_transcode_abort`.
- `vol_unlink_impl` (`vol_dirs.c:743-845`): `nlink<=1`, recipe loaded/parsed (`782-801`) → `vol_retire_recipe(...,1)` (`830`) and `vol_delete_siblings` (`834`); otherwise only decrements nlink (`838-849`).
- `vol_reclaim_orphans[_full]` (`vol_reclaim.c:59-95`) — `btree_collect_orphans[_full]`, gate `INVFS_RECLAIM_ORPHANS` (`27-48`), enabled by default.
- `fold_reclaim_hook` (`vol_fold.c:216-241`) — after drain: either only orphans (old_root in root_slot), or `vol_reclaim_mark_and_free` + orphans.
- `btree_reclaim_pinned` (`vol_btree.c:2097-2125`) marks `keep_root` and `pinned_root`, then `bt_free_rec(old_root)`.
- `vol_reclaim_delta_segments:301-330` — `mbuf_free` over the pages of delta segments with valid magic.
- `spn_reclaim:721-779` → `spn_free_counted:706-719` → `vol_free_blocks`; `retain_release=1` (`757-773`) bypasses the pin check.
- `spt0_pin_take:974` frees the old SPN0 file after writing the new descriptor; `spt0_drop:1423-1459` only clears the ARMED flag and does not free pin blocks.

## 5. SAVE POINT / RECLAIM
- `spt0_capture:997`: reader snapshot (`1014`), `vol_base_root:1018`, `spt0_tree_ok:1022`, descriptor with `base_root`/`delta_end` (`1028-1059`), `spt0_pin_take:1067`, `pinned_root`+`savepoint_live` (`1069-1071`), `spt0_store:1074`, barrier (`1077`).
- `spt0_pin_take:784`: `vol_iter_inodes_at(...,spn_walk_ino)`:821 builds `c.map`/`c.dig`; the old map is read at `856`; `spn_reclaim` at `861`; capture refusal when the root is not allocated at `886-894`.
- exact refusals of `spn_reclaim:721`: no old_map (`726`), incomplete new_map (`729`), `tz_reg_owned_blocks` error (`732`), `spn_meta_mark`/`btree_mark_reachable` error (`737-748`); otherwise only the bits `old_map & !new_map & bitmap & !tz_reg & !meta` are freed (`755-765`).
- pin file: writing hdr/dig/map (`910-927`), `vmux_barrier:942`, `spn_desc_store:956`, repeated barrier (`960`), then frees of old pin (`974`); on error — `vol_free_blocks(s.pba)` (`946,958,963`).
- capture refusal: `root_pba` not in bitmap → `-1` with no pin and no save point (`886-894`); no space for the pin → `-1`, but the SPT0 descriptor is zeroed (`902-913`).
- `fold_reclaim_hook:216`: `vol_reclaim_drain` (`221`), root-slot guard (`227-231`), `vol_reclaim_mark_and_free` (`237`), orphan pass (`240`).
- exact reclaim refusal: `vol_reclaim_drain:177-208` returns `-1` if `g_readers_in_flight != 0` after `reclaim_drain_timeout_ms` expires (`193-202`); fold then skips freeing entirely (`vol_fold.c:221-226`).
- `vol_reclaim_mark_and_free:231-239` → `btree_reclaim_pinned`; refusal: `!v` (`vol_btree.c:2107`), `old_root.pba==0` (`2110`), mark keep/pinned failure (`2117-2122`), `bt_free_rec` failure (`2123-2126`).
- rollback side: `spt0_restore:1344` refuses on a damaged `base_root` (`1367-1372`) or `!spt0_data_ok` (`1373-1380`); `spt0_data_ok:1301` requires delta integrity (`1273-1299`) and segment checks (`spt0_seg_verify:1094`).
- `spt0_drop:1423` clears ARMED in SPN0 (`1438-1448`) and writes an empty SPT0 (`1450-1455`); `spt0_verify_live:1518` checks magic/version/CRC and `base_root/delta_end/flags` (`1542-1551`).

## 6. INVARIANTS IN CODE
- `vol_sweep.c:1028`, `1031` — active write by id/name → no-op; `1100-1109` — RAW inode/size/type/zone mandatory.
- `vol_sweep.c:1037-1075` — class replay: generation guard for UNCOMPRESSIBLE/GUARD, re-addressing of EXER/JXL/container.
- `vol_sweep.c:1412-1439` — publication only after serialize+store+delta_put; on error frees of new blocks, the file stays RAW.
- `vol_sweep.c:946-990` — `ast_frame_ok`, `orig_len<=SEGMENT_SIZE`, exact decompression size; otherwise the task is invalid.
- `vol_sweep.c:1790-1815` — enospc frontier; `1816-1835` — exact decode+memcmp; `1844-1851` — memlimit/guard.
- `vol_exer.c:145-180` — EXER magic, `n<=EXE_MAX_MEDIA`, `8+17n<=pay_len`, non-overlapping ranges and `member_sum+glue==file_size`.
- `vol_exer.c:227-241` — media ≥64KiB and ≥5% of the file; `250-257` — decompose memlimit; `332-354` — rebuild guard; `360-367` — sum of blobs < original.
- `vol_cpack.c:3572-3576` — `is_container`, name limit; `3605-3638` — enumerate/table/memberlimit; `3663-3707` — arc/mem/ENOSPC.
- `vol_cpack.c:3780` — the size of every extracted member must match the table; `3845-3870` — rebuild equivalence and size guard.
- `vol_cpack.c:4055-4079` — `cpack_map_guard`, rollback on refusal; `4060-4078` — mandatory `!mbrmap` inode.
- `vol_dedupe.c:312-327` — skip TEXT and JXL/APE/EXER, validation of `csize` and ≤16MiB; `147-181` — fine-grained remap only when `block_id` and `cur_pba` match.
- `vol_dedupe.c:365,528` — only write-enabled volume and `vol_mark_dirty` before remap.
- `vol_migrate.c:580-596` — walk acknowledgement: unfinished walk → `walk_refused`, nothing moves; `301-314` — check of the written frame; `326-332` — downward only (`pba_new < old_pba`).
- `vol_walk.h:48-51`, `vol_walk.c:59-67,70-98` — `rc==0 && found<=n`; without `vol_walk_result` commit/abandon → `-1`.
- `vol_spt0.c:57-100` — SPT0/ANCH0 magic+version+CRC; `145-230` — `spt0_tree_ok`; `1461-1487` — `spt0_block_pinned` requires `spn_armed && spn_bitmap && !spn_nopin`.

## 7. SUSPICIOUS
- `vol_sweep.c:471` accepts a small binary before the anchored/hot guards (`481,490`).
- `vol_sweep.c:538-540` duplicates `full[0]!=0/1` before the check `full[0]==0x1F`.
- `vol_spt0.c:861` can free the old pin before the refusal at `886` and before the pin-placement error at `902-913`.
- `vol_spt0.c:808-814` with `spn_nopin` and a `calloc` failure continues the walk with `map==NULL`; consequences for `c.map` NOT-IN-CODE.
- `vol_cpack.c:3342-3349` with `newino==inode_id` explicitly leaves superseded segments unreclaimed.
- `vol_cpack.c:4124-4132` `INVFS_SWEEP_NO_COMPRESS=1` skips member deferral, not only compression.
- `vol_sweep.c:1440-1450` `UNCOMPRESSIBLE` is decided by `new_total` of only the rewritten segments against the full `in.size`.
- `vol_reclaim.c:318-325` calls `mbuf_free` without checking `spt0_block_pinned`; the check exists only in `volume.c:3152-3154`.


---

[pi-9router-ext] startup model discovery skipped: fetch failed
## 1. CODEC REGISTRY
- Builtin table: `static const invfs_codec registry[]` (`src/codecs/codec.c:1332`), 15 rows, `_Static_assert(REGISTRY_N < 64)` (`src/codecs/codec.c:1388`); working table `g_all[REGISTRY_N + INVFS_PACK_MAX]` (`src/codecs/codec.c:1393`), rebuilt by `packs_ensure()` (`src/codecs/codec.c:1779`).
- Lookup: `invfs_codec_by_algo()` — linear pass over `g_all` (`src/codecs/codec.c:1918`), `invfs_codec_all()` (`src/codecs/codec.c:1927`), the vol variant `invfs_codec_by_algo_vol()` with priority for this volume's packs (`src/codecs/codec.c:1694`).
- External registration: `pack_register_inner()` (`src/codecs/codec.c:1499`), `invfs_codec_register_pack_mem()` (`src/codecs/codec.c:1638`), trampolines `pack_sniff_N` (`src/codecs/codec.c:1111`) and `pack_fns[]` (`src/codecs/codec.c:1195`); reset `invfs_codec_probe_reset()` (`src/codecs/codec.c:1301`).
- Builtins, sniff=NULL: `none` (`src/codecs/codec.c:1333`), `lz4` (`:1336`), `zstd` (`:1339`).
- Magic sniffs (all `magic_at()` → 100): zip `PK\3\4` (`:250`), tarr `ustar`@257 (`:257`), gzr `1F 8B` (`:264`), pngr `\x89PNG\r\n\x1A\n` (`:271`), flacr `fLaC` (`:278`), ape `MAC ` (`:311`), wv `wvpk` (`:318`).
- Heuristic sniffs: pmp `ID3`/`FF Exxx` (`:285`), jxl box/`FF 0A`/JPEG SOI (`:295`), exer = `invfs_binary_family()>0 ? 40 : 0` (`:327`), ppmd = `invfs_text_family()?50:0` (`:177`), nbli reuses `sniff_pngr` (`:1378`).
- Pack sniff: `pack_sniff_impl()` — magic 100, extension 50 (`src/codecs/codec.c:884`, `:910`).

## 2. PACK LAYER
- Pack ABI descriptor: `ivpack_desc{api_version,name,version,pack_class,flags}` (`src/include/ivpack_api.h:22`), versions 2/min 1 (`src/include/ivpack_api.h:8`), check `d->api_version == INVFS_CODEC_ABI_MAJOR` (=2, `src/core/vol_plugin_inproc.h:13`, `src/core/vol_plugin_inproc.c:282`); return codes `IVPACK_RC_OK/ERROR/USAGE/DECLINE` (`src/include/ivpack_impl.h:24`). Exports: `ivpack_get_desc`, `ivpack_container_cmd`, `ivpack_container_estimate` (`src/core/vol_plugin_inproc.c:272`).
- Capabilities: bits `SEEK/BATCHED/WHOLEFILE/CONTAINER/EXTERNAL/PACKONLY` (`src/codecs/codec.h:11`), manifest `caps=` strings parsed by `parse_caps()` (`src/codecs/codec.c:430`); the manifest stores name/algo/os/arch/caps/dec_mem/generation/magic/commands (`src/codecs/codec.c:346`), the public contract is `invfs_pack_def` (`src/codecs/codec.h:61`).
- Pack commands: `ENUMERATE=0 … EXTRACT_BATCH=6` (`src/codecs/codec.h:136`); map to ivpack commands 1..5 (`src/core/vol_cpack.c:743`) and argument verification/dispatch (`src/include/ivpack_impl.h:139`); `EXTRACT_BATCH` goes only through the CLI template `def->batch` (`src/core/vol_cpack.c:757`).
- Loading: `.codecpack/manifest` directories and the ZIP entry `manifest` inside `.ivpack` (`src/codecs/codec.c:1706`, `:1738`), `INVFS_CODECPACKS` / `/usr/lib/invfs/codecpacks` (`src/codecs/codec.c:1755`); ZIP entries must be Stored (`src/core/ivpack_loader.c:125`), `.so` is loaded from memfd (`src/core/ivpack_loader.c:279`), the binary via `fexecve` (`:304`). A volume pack is registered from memory (`src/codecs/codec.c:1638`) and materialized (`src/codecs/codec.c:1854`).
- CLI pack sandbox: `tool_exec_lim()` with `invfs_helper_sandbox{pack_dir,ro_path,rw_dir,requires}` (`src/core/vol_cpack.c:681`, `src/core/helper_exec.h:15`); in the child — netns, RLIMIT_AS/CPU/NOFILE/NPROC/FSIZE, `no_new_privs`, `PDEATHSIG`, Landlock, env cleanup, privilege drop (`src/core/helper_exec.c:629`, `:230`, `:151`), fork (`src/core/helper_exec.c:738`).
- `.so` alternatives: a worker pool via `/tmp/invfs_plugin_pool.sock` + shm (`src/core/vol_plugin_client.c:66`, `:115`) or in-process `dlopen` for jxl only after gating (`src/core/vol_plugin_inproc.c:197`, `:272`).

## 3. ALGO ID MAP
- Occupied by builtins: `0 none`, `1 zstd`, `2 ppmd`, `3 ape`, `4 jxl`, `5 lz4`, `6 wv`, `7 flacr`, `8 tarr`, `9 gzr`, `10 pngr`, `11 pmp`, `12 zip`, `15 exer`, `25 nbli` — table rows `src/codecs/codec.c:1333`–`:1381`; constants `src/core/invarifs.h:51`–`:79`, `src/codecs/codec.h:22`.
- Without a codec entry in the registry: `13 rawimg` (`src/core/invarifs.h:69`), `14 zstd_bcj` (`:73`, handled in `src/core/vol_read.c:131`), `24 window_src` (`:82`), `27 child` (`:85`).
- Pack fixture names from the code: `40 evil` (`src/cli/noautopack_test.c:388`), `41 evilc` (`:443`), `42 fakeimg` (`src/cli/codec_test.c:720`), `43 spltmini` (`:749`), `44 badcont` (`:763`), `45 spltmap` (`:775`) and `badpack` (`src/cli/noautopack_test.c:117`), `46 extbad` (`src/cli/codec_test.c:804`) and `vxp` (`src/cli/noautopack_test.c:181`), `61 scope` (`src/cli/codec_test.c:1074`). Real pack manifests in the tree are NOT-IN-CODE.

## 4. EXTERNAL SURFACE
- **Volume lifecycle**:
- `invf-mkfs` — create/format an image or 2 devices: `main()` `src/cli/mkfs.c:100`, usage `:150`.
- `invf-resize` — resize a volume: `main()` `src/cli/resize.c:275`, usage `:304`.
- `invf-rollback` — roll back a volume: `main()` `tools/invf-rollback.c:12`, usage `:21`.
- `invf-import` — bulk import of a directory: `main()` `tools/invf-import.c:350`, usage `:339`, `vol_open` `:383`.
- `invf-cp` — add a host file: `main()` `src/cli/cp.c:29`, usage `:41`.
- `invf-cat` — extract a named file: `main()` `src/cli/cat.c:17`, usage `:38`.
- `invf-ls` — list names: `main()` `src/cli/ls.c:16`, usage `:25`.
- `invf-vnode` — put/get/del inode rows: `main()` `src/cli/vnode.c:268`, dispatch `:289`.
- `invf-verify` — verify a volume, deep/seal: `main()` `src/cli/verify.c:152`, usage `:170`.
- `invf-fsck` — check/repair: `main()` `src/cli/fsck.c:10`, usage `:25`.
- `invf-reconsolidate` — consolidation of hot extents: `main()` `tools/invf-reconsolidate.c:207`, usage `:238`.
- **Inspection**:
- `invf-stats` / `invf-stat` — volume statistics; `main()` `tools/invf-stats.c:114`, usage `:133`; alias `Makefile:1921`.
- `invf-verbatim` — report on verbatim/leaves: `main()` `tools/invf-verbatim.c:191`, usage `:223`.
- `invf-why` — explain the codec choice for a path: `main()` `src/cli/why.c:84`, usage `:97`.
- `ivpack_probe` — check the ABI of a .so (desc/est/cmd): `main()` `tools/ivpack_probe.c:39`, usage `:47` (not in `TOOLS`, `Makefile:189`).
- **Sweep**:
- `invf-sweep` — sweep/transcode with `--dry-run/--fast/--compact`: `main()` `tools/invf-sweep.c:1392`, usage `:1475`.
- **Packs**:
- `invf-pack` — list/info/install/keygen/sign/remove/replace/upgrade/verify/inproc: `main()` `src/cli/pack.c:4453`, dispatch `:4470`; signatures `src/cli/pack_sig.c:18`/`:44`.
- `invf-plugin-host` — worker pool daemon (shm + Unix socket): `main()` `tools/invf-plugin-host.c:257`, paths `:260`.
- `invf-zip` — list a zip inside a volume: `main()` `src/recipes/zip.c:147`, usage `:151`.
- **FUSE**:
- `invf-fuse` — mount an image, `--probe-uuid`: `main()` `src/cli/fuse_fs.c:3392`, usage `:3430`; build `Makefile:343`.

## 5. TEST SURFACE
- Registry/sniff/profiles: `codec_test.c:157` — ppmd-text-heuristic is last in `invfs_codec_all()`.
- Pack registration, caps, override: `codec_test.c:966` — a pack with `algo=4` overrides the builtin EXTERNAL.
- Manifest parsing and limits: `codec_test.c:833` — name/algo come from the manifest.
- Autopack guard: `noautopack_test.c:271` — under a guard `pack_exec` must refuse.
- Pack signatures/ed25519: `pack_sign_test.c:216` — a substituted helper gives `PACK_SIG_BAD`.
- in-process trust: `dlopen_trust_test.c:119` — `invfs_inproc_allowed("jxl")` is 0 by default.
- ivpack ABI `.so`: `ivpack_packs_test.c:159` — the three mandatory exports are present.
- Helper sandbox/limits: `helper_exec_test.c:109` — `RLIMIT_AS` is visible as 256 MiB.
- Scratch policy: `scratch_policy_test.c:236` — the chosen tmpdir fits within the allowed root.
- Plugin pool: `plugin_mt_test.c:119` — concurrent `container_cmd_mem` calls do not corrupt the response.
- Container map: `cpack_map_san_test.c:335` — parallel reads return only their own bytes.
- Decomposition limits: `cpack_guard_test.c:77` — exceeding the limit gives a refusal with a reason.
- Sweep: `sweep_orig_len_test.c:123` — an overflowed orig_len does not crash the process.
- Recipes/containers: `recipe_tree_test.c:64` — serialization of a recipe with CHILD records; `tar_cap_test.c:181` — 4000 members are read byte-for-byte.
- FUSE path: `walk_status_fuse_test.c:369` — after svc the table is consistent again.
- Data codecs: `deflate_repro_test.c:81`, `crc32c_test.c:152`, `gz_header_test.c:187`.

## 6. INVARIANTS IN CODE
- A pack cannot be a helper (`src/codecs/codec.c:1525`), cannot lack an encode/decode pair (`:1530`) or be a container without enumerate/extract/strip/rebuild (`:1527`).
- A pack's algo must be `<64` (`:1533`) and must not take over a builtin codec without `CAP_EXTERNAL` (`:1539`); duplicate algo/name within one `origin_vol` are forbidden (`:1549`).
- Containers are forced to `CONTAINER|EXTERNAL|WHOLEFILE`, `SEEK` only when `map` is present, `encode/decode` are zeroed (`src/codecs/codec.c:1602`–`:1608`); `map`/`batch` are accepted only from a container (`:1573`).
- blob/raw suffixes pass a `.`+alnum filter (`src/codecs/codec.c:558`); `sniff.magic` is limited to 8 rules (`:337`).
- The pack's host platform is checked against `uname()` (`:1466`); the pack table is limited to `INVFS_PACK_MAX=64` (`:811`).
- All pack-code execution is blocked by `INVFS_NO_AUTOPACK`/the setter: probe (`src/codecs/codec.c:817`), materialization (`:1854`), exec (`src/core/vol_cpack.c:668`), cmd (`:714`).
- Pack commands are available only to containers, the command template must exist, argv ≤24 (`src/core/vol_cpack.c:726`, `:762`, `:711`).
- in-process `.so`: only `jxl`, only opt-in, only a pinned local path, only with a signature (`src/core/vol_plugin_inproc.c:202`, `:210`, `:220`, `:230`); volume packs are not allowed in inproc (`:152`). ABI/name check in a separate child (`src/core/vol_plugin_inproc.c:282`, `:284`, `:397`).
- Pack installation goes through `pack_sig_gate_install()` (`src/cli/pack.c:3563`), and `EXTRACT_BATCH` is not routed to an ivpack .so (`src/core/vol_cpack.c:747`).

## 7. SUSPICIOUS
- `ivpack_dlmopen()` on failure of `dlmopen(LM_ID_NEWLM)` silently falls back to plain `dlopen()` — namespace isolation is lost (`src/core/ivpack_loader.c:291`, `:295`).
- ZIP entry bounds: checks of `local_off + 30` and `payload_off + usize` without checking for `uint32_t`/`size_t` overflow (`src/core/ivpack_loader.c:129`, `:135`).
- Pack registration from env directories and `.ivpack` does not invoke a signature check; the signature is gated only on `invf-pack install` (`src/codecs/codec.c:1748`, `src/cli/pack.c:3563`).
- For `.ivpack` `manifest_tool_ok()` when the entry is absent falls back to searching the host `PATH` (`src/codecs/codec.c:663`, `:688`).
- `INVFS_PACK_SANDBOX=0` completely disables sandbox mode; without Landlock mode 1 remains with no FS restrictions (`src/core/helper_exec.c:391`, `:414`).
- Plugin pool readiness is determined only by the existence of `/tmp/invfs_plugin_pool.sock` (`src/core/vol_client.c:68`); the path and shm are fixed (`src/core/vol_plugin_client.c:123`).

Last file path fix: `src/core/vol_client.c:68` → `src/core/vol_plugin_client.c:68`.

- Plugin pool readiness is determined only by the existence of `/tmp/invfs_plugin_pool.sock` (`src/core/vol_plugin_client.c:68`); the path is fixed (`:123`).


---

## Cross-check: blind findings ↔ failing tests

The most valuable thing here is the matches. The blind agents knew neither about the tests nor about our
ADRs, and yet pointed at the same nodes that turn red in e2e:

| finding (blind) | failing test | match |
|---|---|---|
| `vol_heat.c:44-51` repeated `heat_tab_touch` does not increment the counter; `:146-149` heat is raised only on insertion | `test-heat`: `INVFS_HEAT_INIT did not seed (rheat=0)` | **direct** |
| `vol_spt0.c:861` can free the old pin BEFORE the refusal at `:886` and before the pin-placement error `:902-913` | `test-rollback`, `test-watermark` (SPT0 refuse), unit shard 4 | **direct** |
| `vol_reclaim.c:318-325` calls `mbuf_free` without checking `spt0_block_pinned` (check exists only in `volume.c:3152-3154`) | `test-resize` rc=139 (SIGSEGV) — candidate | probably |
| `vol_cpack.c:3342-3349` with `newino==inode_id` leaves superseded segments unfreed | `test-dedupe` "free blocks did not stabilize" | probably |
| `vol_delta.c:2225-2236, 2385-2400` truncate frees blocks of new segments BEFORE the final barrier, while the RT30 pointer changes earlier | — (crash-loss window) | not covered by a test |
| `vol_metabuf.c:381-383` slot is selected as `seq&1` before the increment, while `mbuf_root_read()` `:435-437,463-468` treats `seq&1` as "new" after | — (risk of reading the wrong slot) | not covered by a test |
| `ivpack_loader.c:291,295` on `dlmopen` failure silently falls back to `dlopen` (namespace isolation is lost) | `test-helper-isolation` — **passes**, i.e. the hole is not checked | not covered by a test |
| `codec.c:1748` pack registration from env directories/`.ivpack` does not check the signature (only `invf-pack install`) | `test-pack-sign` — **passes** (checks only the install path) | not covered by a test |
| `vol_seal.c:77-83` `seal_recover_segment` always `-1`, but `vol_read.c:95-99` is the only recovery path; `vol_seal2_repair` `:2015-2021` only prints a refusal, `vol_repair.c` is empty | — (recovery effectively absent) | not covered by a test |
| `vol_png.c:949-953` on error frees `pba`/`rblob`, but not `cblob` | — (memory leak) | not covered by a test |
| `vol_tier.c:8-19` `wp25_map`/`wp25_unmap` are empty stubs | `test-multidev` "no demotion under pressure" — candidate | probably |

Conclusion: three of the six "group E" in `OPEN-ISSUES.md` (heat, multidev, resize) have
a suspect named by a blind agent down to the line. That is the benefit of
the exercise — not "documentation", but a list of places where the code diverges from expectation.
