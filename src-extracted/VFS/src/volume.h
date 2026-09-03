/* volume.h — InvariantFS volume access layer API */
#ifndef INVARIFS_VOLUME_H
#define INVARIFS_VOLUME_H

#include "invarifs.h"
#include "arc.h"

typedef struct invfs_volume invfs_volume;

/* Longest name the on-disk record can hold, in bytes. invfs_inode_rec.name is
   256 bytes with a NUL, and name_len must agree with what is actually stored,
   so a longer name is refused rather than truncated. Callers that build names
   (the decomposition paths append "!part<N>" and friends) need this to check
   the result fits before they start writing children. */
#define INVFS_MAX_NAME 255

invfs_volume *vol_open(const char *path, int *err);
void vol_close(invfs_volume *v);
int  vol_flush(invfs_volume *v);
/* vol_flush + a real storage barrier (fsync on image files, no-op-ish on
 * write-through devices): the FUSE fsync/fdatasync path. 0 = durable. */
int  vol_sync(invfs_volume *v);

uint64_t vol_write_raw(invfs_volume *v, const uint8_t *data, size_t len);

/* fsck/repair: rebuild L2P + used-bitmap from the inode area */
typedef struct {
    uint64_t live_files;
    uint64_t l2p_entries;
    uint64_t l2p_miss;
    uint64_t orphans;
    uint64_t missing;
    uint64_t bad_recs;
    /* WP22d consistent cut: torn newest versions hidden with a live
     * fallback (regressed), and names with no readable version at all
     * (their segments are counted in l2p_miss). -f quarantines both. */
    uint64_t cut_records;
    uint64_t lost_files;
    /* -f content pass: live records whose segment bytes fail the CRC
     * (a drop window took the data after the maps survived) -- the
     * content-level consistent cut. Quarantined like the map-level cut. */
    uint64_t corrupt_files;
    /* Allocated-but-unreferenced blocks while a sweep checkpoint is live:
     * retention holds them (never reused), but non-arming processes do not
     * register them in \x01reten, so they are indistinguishable from true
     * orphans until the checkpoint resolves. Reported separately, never
     * counted as issues, and reclaimed by fsck -f after the checkpoint is
     * gone (report mode then shows them as orphans again). */
    uint64_t held_ckpt;
} invfs_fsck_report;
int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix);
int  vol_map(invfs_volume *v, uint64_t inode, uint64_t lba, uint64_t pba, uint32_t length);
uint64_t vol_lookup(invfs_volume *v, uint64_t inode, uint64_t lba);
int  vol_lookup_entry(invfs_volume *v, uint64_t inode, uint64_t lba,
                      uint64_t *pba_out, uint64_t *len_out);
int  vol_read_block(invfs_volume *v, uint64_t pba, void *buf);

uint64_t vol_create_file(invfs_volume *v, const char *name,
                         const uint8_t *data, size_t len);
void vol_l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba);

/* AST children (containers: ZIP members etc.) */
uint64_t vol_create_container_file(invfs_volume *v, const char *name,
                                   const uint8_t *data, size_t len,
                                   const invfs_ast_child_entry *children,
                                   size_t nchildren);
int vol_get_children(invfs_volume *v, uint64_t inode_id,
                     invfs_ast_child_entry **out, size_t *n_out);
int vol_zip_extract_member(const uint8_t *z, size_t zlen,
                           const invfs_ast_child_entry *ch,
                           uint8_t **out, size_t *out_len);
int vol_read_named(invfs_volume *v, const char *name,
                   uint8_t **out, size_t *out_len);
int vol_zip_parse_children(const uint8_t *z, size_t zlen,
                           invfs_ast_child_entry *ch, size_t maxch);
uint64_t vol_find(invfs_volume *v, const char *name);
/* The live version of a name under the consistent cut (WP22d): the inode
 * id, or 0 when absent; fills the live record's size/ctime. Raw area
 * walkers (invf-ls) must defer to this -- they see torn versions the
 * index has hidden. */
uint64_t vol_find_ex(invfs_volume *v, const char *name,
                     uint64_t *size_out, uint64_t *ctime_out);

/* virtual directories (prefix-based; anchor "dir/" is an empty file) */
/* size/ctime come straight out of the name index, so a listing needs no
   per-entry stat call. size is 0 for directories. */
typedef struct {
    char name[256];
    int is_dir;
    uint64_t size;
    uint64_t ctime;
} invfs_dirent;
int vol_is_dir(invfs_volume *v, const char *name);
uint64_t vol_mkdir(invfs_volume *v, const char *name);
int vol_rmdir(invfs_volume *v, const char *name);   /* -2 = ENOTEMPTY */
int vol_list_dir(invfs_volume *v, const char *dir, invfs_dirent *ents, int max);
/* auto-create parent directory anchors for a path ("a/b.txt" -> "a/") */
int vol_ensure_path(invfs_volume *v, const char *name);
int  vol_read_file(invfs_volume *v, uint64_t inode_id, uint8_t **out, size_t *out_len);
int  vol_read_raw(invfs_volume *v, uint64_t offset, void *buf, size_t len);
void vol_free_blocks(invfs_volume *v, uint64_t pba, uint64_t nblocks);
int  vol_sweep_file(invfs_volume *v, uint64_t inode_id);

/* On-demand sweep (embedded in the FS daemon, same process): mark an
   inode as pending transcode, then drain the pending list in the
   background. The volume stays open with one L2P/journal in memory —
   no second process, no file locking. Pending is in-RAM only: a crash
   simply leaves files un-swept (invariant: nothing lost, the offline
   sweep pass still finds them). */
void vol_mark_pending(invfs_volume *v, uint64_t inode_id);
void vol_unmark_pending(invfs_volume *v, uint64_t inode_id);
size_t vol_pending_count(invfs_volume *v);
int  vol_sweep_pending(invfs_volume *v);
int  vol_sweep_one(invfs_volume *v, uint64_t inode_id, const char *name);
/* resolve the live name of an inode id for vol_sweep_one drivers that
 * collected only ids (vol_collect_sweepables); 1 = found, 0 = gone */
int  vol_sweep_name_of(invfs_volume *v, uint64_t id, char *nm, size_t cap);
int  vol_stat(invfs_volume *v, const char *name, uint64_t *size_out);
/* inode id + size + ctime in one O(1) index lookup */
int  vol_stat_full(invfs_volume *v, const char *name, uint64_t *id_out,
                   uint64_t *size_out, uint64_t *ctime_out);
uint64_t vol_name_count(invfs_volume *v);
int  vol_read_range(invfs_volume *v, uint64_t inode_id, uint64_t offset,
                    size_t len, void *buf);
int  vol_delete_file(invfs_volume *v, const char *name);
int  vol_delete_inode(invfs_volume *v, uint64_t inode_id, const char *name);
/* unlink a file AND its "name!recipe"/"!partN"/"!coverN"/"!jxl" siblings */
int  vol_unlink(invfs_volume *v, const char *name);
/* remove a single name of a hardlinked inode without retiring blocks */
int  vol_unlink_name(invfs_volume *v, const char *name);
int  vol_forget_name(invfs_volume *v, const char *name);
/* overwrite (or create) a file without a window in which neither version
   exists: append the new record, then tombstone the old one */
uint64_t vol_replace_file(invfs_volume *v, const char *name,
                          const uint8_t *data, size_t len);
/* rename a file (with its "name!..." transcode siblings) or a directory
   (whole "dir/" prefix). 0 = ok, -1 = ENOENT/EINVAL, -2 = EEXIST, -3 = ENOSPC */
int  vol_rename(invfs_volume *v, const char *from, const char *to);

/* ---- format v2 per-inode metadata ("INO2" ext block) ----
 * Friendly view of invfs_meta_ext_hdr + symlink target. Records written
 * before v2 have no ext: vol_get_meta returns -ENOENT-ish (-1) and every
 * writer falls back to v1 defaults (uid/gid 0, mode by type). */
typedef struct {
    uint8_t  type;                 /* INVFS_ITYP_* */
    uint16_t mode;                 /* permission bits */
    uint32_t uid;
    uint32_t gid;
    int64_t  mtime;
    int64_t  atime;
    uint32_t nlink;
    uint64_t rdev;                 /* CHR/BLK device number */
    char     target[INVFS_META_TARGET_MAX];  /* symlink target (NUL-terminated) */
} invfs_meta_pub;

/* read metadata for an inode: 0 = found, -1 = none/corrupt/not indexed.
 * target is only filled for INVFS_ITYP_LNK. */
int      vol_get_meta(invfs_volume *v, uint64_t inode_id, invfs_meta_pub *out);
/* rewrite `name`'s record carrying `meta` (xattrs preserved); data blocks and
 * the AST are untouched — the L2P keys move to the new inode id internally.
 * Returns the new inode id, or 0 on failure (volume untouched). */
uint64_t vol_apply_meta(invfs_volume *v, const char *name,
                        const invfs_meta_pub *meta);
/* create a symlink (target stored in the record's meta ext) or a special
 * node (FIFO/SOCK/CHR/BLK; no data segments). Return inode id / 0. */
uint64_t vol_create_symlink(invfs_volume *v, const char *name,
                            const char *target);
uint64_t vol_create_special(invfs_volume *v, const char *name,
                            uint8_t type, uint16_t mode, uint64_t rdev);
/* hard link: second name for the same inode (record clone with new name).
 * CAVEAT: no block refcounts yet -- unlinking EITHER name retires the
 * shared blocks and dangles the survivor (WP6). Intended for transient
 * locks/atomic-replace patterns (portage), not permanent aliasing. */
int vol_hardlink(invfs_volume *v, const char *from, const char *to);
size_t vol_collect_sweepables(invfs_volume *v, uint64_t *ids, size_t max);

/* ---- WP4b: incremental ranged-write sessions ----
 * Streaming write path: begin a session for `name` (truncate != 0 drops
 * the old content at first use), then vol_write_range any [off,len)
 * window in any order -- each INVFS_SEGMENT_SIZE segment is
 * read-modify-written to a NEW pba (never in place) the first time it is
 * touched; vol_truncate extends (zero-filled) or shrinks the logical
 * size; commit appends the new inode record and only then retires the
 * old version (the vol_replace_file ordering); abort rolls back to the
 * pre-session state.
 * A file stored swept (ZSTD/PPMD/batched/container) is materialized back
 * to RAW segments at the first write -- a write is an implicit downgrade
 * (v1 policy). Return values: 0 ok, -2 ENOSPC (reserve guard), -1 other.
 * Per-session memory: O(segments) metadata, no file-data buffering.
 * Engine-level crash contract: pre-commit crash leaves the old version
 * live; fsck purges orphaned maps/blocks. */
#define INVFS_SEGMENT_SIZE (64u * 1024u)   /* RAW segment granularity */
typedef struct invfs_wsession invfs_wsession;
uint64_t vol_write_begin(invfs_volume *v, const char *name, int truncate,
                         invfs_wsession **out);
int  vol_write_range(invfs_wsession *ws, uint64_t offset,
                     const uint8_t *data, size_t len);
int  vol_write_truncate(invfs_wsession *ws, uint64_t len);
/* read the session's current (uncommitted) logical view -- lets a mount
 * serve .write data before commit; returns bytes read or -1 */
int  vol_write_read(invfs_wsession *ws, uint64_t offset,
                    uint8_t *buf, size_t len);
int  vol_write_commit(invfs_wsession *ws);
void vol_write_abort(invfs_wsession *ws);

/* whole-volume statistics for tools/UIs (read-only walk). */
typedef struct {
    uint64_t files, dirs, links, special;
    uint64_t tombstones, bad_records;
    uint64_t logical_bytes, biggest_size;
    char     biggest_name[256];
    /* per-zone split: physical = bitmap bits*4K, logical attributed
     * through each file's AST block entries (zone bitfield) */
    uint64_t raw_used_bytes, shadow_used_bytes;
    uint64_t logic_raw_bytes, logic_shadow_bytes;
    uint64_t logic_text_bytes;   /* zone==TEXT (PPMd batch members), WP10 */
} invfs_volume_stats;
int vol_compute_stats(invfs_volume *v, invfs_volume_stats *out);
uint64_t vol_zone_used_bytes(invfs_volume *v, uint64_t start_blk, uint64_t end_blk);
void vol_hot_counters(invfs_volume *v, uint64_t *files, uint64_t *dirs,
                      uint64_t *tombstones, uint64_t *logical_bytes);

/* xattrs stored inside the same INO2 ext (TLVs). val semantics like
 * getxattr(2): size query via *vlen==0. list returns NUL-separated names. */
int vol_get_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  void *val, size_t *vlen);
int vol_set_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  const void *val, size_t vlen);
int vol_remove_xattr(invfs_volume *v, uint64_t inode_id, const char *xn);
int vol_list_xattr(invfs_volume *v, uint64_t inode_id,
                   char *buf, size_t bcap);

/* accessors for tools */
const invfs_superblock *vol_sb(invfs_volume *v);

/* Reconstructed-content cache (arc.h). Reported rather than hidden because
   "the cache is present" and "the cache is being used" are different claims,
   and only the counters can tell them apart -- a ranged read that silently
   rebuilt the file every time would pass every correctness assertion. */
void vol_arc_stats(invfs_volume *v, invfs_arc_stats *out);

/* ENOSPC policy helpers (defined in volume.c) */
int vol_write_enabled(invfs_volume *v);
void vol_set_readonly(invfs_volume *v, int ro);
uint64_t vol_free_blocks_cached(invfs_volume *v);
uint64_t vol_write_guard(invfs_volume *v);
const uint8_t *vol_bitmap(invfs_volume *v, uint64_t *blocks_out);
const invfs_l2p_entry *vol_l2p(invfs_volume *v, size_t *count_out);
uint64_t vol_inode_area_pos(invfs_volume *v);
uint64_t vol_inode_area_start(invfs_volume *v);
uint64_t vol_inode_area_end(invfs_volume *v);
/* bytes still available for new inode records (append-only area) */
uint64_t vol_inode_area_free(invfs_volume *v);
uint64_t vol_journal_pos(invfs_volume *v);

/* iterate inode area: returns next record offset or 0 at end;
 * fills hdr if magic matches INOD/TOMBSTONE */
#define INODE_REC_MAGIC 0x444F4E49u  /* "INOD" LE */
#define TOMBSTONE_MAGIC 0x544C4544u  /* "DELT" LE */
uint64_t vol_inode_next(invfs_volume *v, uint64_t pos, uint32_t *magic_out,
                        uint64_t *inode_out, uint64_t *size_out,
                        char *name_out, size_t name_cap, uint32_t *rec_len_out);

#endif
uint64_t vol_count_free(invfs_volume *v);
/* per-zone free counters (maintained incrementally by alloc/free):
 * 0 ok, -1 no volume. For tests/probes that need the engine's own view
 * of zone pressure (e.g. test-rawadapt's fill targeting). */
int vol_zone_free(invfs_volume *v, uint64_t *raw_free, uint64_t *raw_total,
                  uint64_t *shadow_free, uint64_t *shadow_total);
int invfs_jxl_compress(const uint8_t *jpeg, size_t jpeg_len,
                       uint8_t **jxl_out, size_t *jxl_len);
int invfs_jxl_decompress(const uint8_t *jxl, size_t jxl_len,
                         uint8_t **jpg_out, size_t *jpg_len);
uint64_t vol_create_jxl_file(invfs_volume *v, const char *name,
                             const uint8_t *jxl, size_t jxl_len,
                             uint64_t orig_size);
int invfs_ape_compress(const uint8_t *flac, size_t flac_len,
                       uint8_t **ape_out, size_t *ape_len);
int invfs_ape_decompress(const uint8_t *ape, size_t ape_len,
                         uint8_t **flac_out, size_t *flac_len);
int invfs_ape_to_wav(const uint8_t *ape, size_t ape_len,
                     uint8_t **wav_out, size_t *wav_len);
uint64_t vol_create_flac_file(invfs_volume *v, const char *name,
                              const uint8_t *flac, size_t flac_len);
uint64_t vol_create_tar_file(invfs_volume *v, const char *name,
                             const uint8_t *tar, size_t tar_len);
uint64_t vol_create_png_file(invfs_volume *v, const char *name,
                            const uint8_t *png, size_t png_len);
uint64_t vol_create_gz_file(invfs_volume *v, const char *name,
                            const uint8_t *gz, size_t gz_len);
uint64_t vol_create_ape_file(invfs_volume *v, const char *name,
                             const uint8_t *ape, size_t ape_len,
                             uint64_t orig_size);
int invfs_pmp_compress(const uint8_t *mp3, size_t mp3_len,
                       uint8_t **pmp_out, size_t *pmp_len);
int invfs_pmp_decompress(const uint8_t *pmp, size_t pmp_len,
                         uint8_t **mp3_out, size_t *mp3_len);
uint64_t vol_create_pmp_file(invfs_volume *v, const char *name,
                             const uint8_t *pmp, size_t pmp_len,
                             uint64_t orig_size);

/* ---- WP10: memory policy + storage class + text batching ---- */

/* Memory policy setters (mount options arc_limit=/dec_mem_limit=, CLI tools
 * use env INVFS_ARC_BYTES/INVFS_DEC_MEM_LIMIT). Admission is enforced at
 * SWEEP time only; the read path never refuses stored data. */
void     vol_set_arc_budget(invfs_volume *v, uint64_t bytes);     /* 0 = keep */
void     vol_set_dec_mem_limit(invfs_volume *v, uint64_t bytes);  /* 0 = default */
uint64_t vol_get_dec_mem_limit(invfs_volume *v);

/* WP16b: the volume's codec profile (INVFS_PROFILE_* from codec.h), parsed
 * from INVFS_PROFILE at vol_open (default INVFS_PROFILE_BALANCED). */
unsigned vol_get_profile(const invfs_volume *v);

/* Seal + commit the partial PPMd text batch. Drivers (invf-sweep, daemon
 * drain) call this at the end of a sweep run. 0 = ok/flushed, 1 = nothing
 * pending, <0 = error. */
int vol_tz_flush(invfs_volume *v);

/* Candidates currently sitting in the sweep-run batching accumulators
 * (binary = the WP14a binary one, 0 = text). For the sweep driver's flush
 * summary: parts deferred at container-explode time never produced a
 * per-file walk line, so only the accumulator count is the truth. */
size_t vol_acc_pending(const invfs_volume *v, int binary);

/* WP14b M2: part count of the exe carve behind the last rc-11 answer of
 * vol_sweep_one (the sweep driver prints it: "exe media -> JXL (N parts)") */
unsigned vol_exer_last_parts(const invfs_volume *v);

/* Text-zone GC (WP10 §7): reclaim owner batches no live member references.
 * Runs between the dedupe pass and vol_tz_flush in invf-sweep. Returns the
 * number of dead batches reclaimed, 0 = none, <0 = error. */
int vol_tz_gc(invfs_volume *v);

/* Offline per-segment dedupe (WP12(h)): BLAKE3 the stored bytes of every
 * live segment, keep one physical copy per hash, remap duplicate
 * (inode,lba) L2P entries onto it and free the loser blocks. Skips
 * zone==TEXT entries (WP10 §11), whole-file JXL/APE blobs, and inodes
 * deferred into the running sweep's text accumulator (their records are
 * retired by this run's vol_tz_flush). Runs between the sweep walk and
 * vol_tz_gc in invf-sweep; the caller's vol_flush persists the remaps.
 * Returns the number of merged segments, <0 on error. */
int vol_sweep_dedupe(invfs_volume *v);

/* ---- WP19: heat counters + adaptive tiering ----
 * Heat lives in invfs_l2p_entry.pad (see invarifs.h): u16 LE read-heat
 * (+1 per open-session touch of an (inode,lba) via the vol_read_* paths;
 * seeded by INVFS_HEAT_INIT on create) and u8 write-heat (born 1, old+1
 * carried across rewrites). Persisted through the journal (flush copies
 * whole entries; replay likewise), cold on pre-WP19 volumes and after an
 * fsck -f rebuild.
 *
 * vol_heat_sweep_begin: the once-per-RUN decay pass (rheat >>= 1,
 * wheat -= 1). Drivers call it before the sweep walk; vol_sweep_pending
 * runs it internally.
 *
 * vol_heat_promote: extract read-hot PPMd batch members (rheat >= 8 after
 * the decay -- a single burst promotes iff it survives one halving) to
 * standalone per-segment ZSTD, stamped GENERIC{ZSTD}; the batch keeps a
 * hole vol_tz_gc reclaims as today. Never promotes BATCHED_BIN members,
 * dedup-shared members, or past the budget (min(64, 10% of live TEXT
 * members)). Runs after the walk, before the dedupe pass; the caller's
 * vol_flush persists. Returns promotions, <0 on error. */
void vol_heat_sweep_begin(invfs_volume *v);
int  vol_heat_promote(invfs_volume *v);
/* heat decode helpers for tools (meta_probe) */
uint16_t vol_heat_r(const invfs_l2p_entry *e);
uint8_t  vol_heat_w(const invfs_l2p_entry *e);

/* Storage-class flag (invfs.class xattr, see invarifs.h).
 * vol_get_class: 0 = found, 1 = absent. stamp writes only on change. */
int vol_get_class(invfs_volume *v, uint64_t inode_id,
                  uint8_t *cls, uint8_t *algo, uint16_t *gen);
int vol_stamp_class(invfs_volume *v, uint64_t inode_id,
                    uint8_t cls, uint8_t algo, uint16_t gen);

/* ---- WP20: --seal shadow-zone XOR parity ----
 * Stripes of k1 blocks over the shadow zone (RAW excluded; k1 = 32 by
 * default, 8..128 via the RDP0 descriptor); one parity block per stripe
 * holding the XOR of the stripe's occupied blocks.
 * WP20b adds layer 2: RS(32+m2, 32) stripes over the same occupancy model
 * (m2 parity blocks per 32 data blocks, GF(2^8), see rs.c), owned by
 * "\x01parity2*" records; layer-2 recovery is fsck-only (the runtime read
 * path is layer-1-only and still fails loudly beyond one bad block per
 * stripe).
 * Parity blocks are owned by hidden internal inodes "\x01parity",
 * "\x01parity1", ... (65535 stripes per owner record: AST num_blocks is
 * u16) via ordinary L2P maps, so fsck sees them as live. Sealing is
 * check-and-update: stripes are recomputed in memory and rewritten only
 * when the parity changed -- never delete-then-regenerate. */
typedef struct {
    uint64_t stripes;        /* stripes with >=1 covered block */
    uint64_t parity_blocks;  /* live parity blocks after the run */
    uint64_t updated;        /* parity blocks (re)written this run */
    uint64_t unchanged;      /* parity already correct (idempotent no-op) */
    uint64_t added;          /* stripes that gained parity this run */
    uint64_t freed;          /* parity blocks freed (empty stripes/unseal) */
    uint64_t unprotected;    /* stripes that wanted parity but got none */
    double   overhead_pct;   /* parity_blocks / covered blocks * 100 */
    /* WP20b: stripes skipped because no block in them changed since the
     * last successful reseal (the dirty bitmap; a fresh mount always
     * starts all-dirty, so this is nonzero only for second and later
     * reseals of one session) */
    uint64_t dirty_skipped;
    /* WP20b layer 2 (RS(32+m2,32) over GF(2^8)); all zero when off */
    uint64_t l2_stripes;
    uint64_t l2_parity_blocks;
    uint64_t l2_updated;
    uint64_t l2_unchanged;
    uint64_t l2_added;
    uint64_t l2_freed;
    uint64_t l2_unprotected;
    uint64_t l2_dirty_skipped;
    double   l2_overhead_pct;
} invfs_seal_report;

/* WP20b: set the redundancy configuration the next vol_seal applies and
 * persists in the RDP0 descriptor. k1 == 0 keeps the persisted/default
 * layer-1 stripe size; l2_algo < 0 keeps the persisted layer-2 shape,
 * l2_algo == 0 turns layer 2 off, l2_algo > 0 selects the codec
 * (INVFS_RDP0_L2_*, see invarifs.h/rs.h); m2 == 0 keeps the persisted
 * layer-2 width. Any change forces the next seal to a full pass. */
void vol_redun_config(invfs_volume *v, uint32_t k1, int l2_algo, uint32_t m2);
/* current effective config (k1 always >= 8; l2_algo/m2 0 = layer 2 off).
 * Returns 1 when a live RDP0 descriptor was loaded at open. */
int  vol_redun_state(const invfs_volume *v, uint32_t *k1, int *l2_algo,
                     uint32_t *m2);

/* unseal != 0: free every parity block and remove the owners.
 * 0 = ok, -1 = error (incl. read-only volume: sealing mutates). */
int vol_seal(invfs_volume *v, int unseal, invfs_seal_report *rep);

/* verify --deep leg: recompute every sealed stripe against its stored
 * parity block. All four counters are 0 on an unsealed volume. */
typedef struct {
    uint64_t sealed;       /* sealed stripes re-verified */
    uint64_t mismatched;   /* stored parity != recomputed (drift/corruption) */
    uint64_t missing;      /* occupied stripes without parity */
    uint64_t extra;        /* parity blocks over stripes with no content */
    /* WP20b layer 2 (RS); all zero when layer 2 is off */
    uint64_t sealed2;
    uint64_t mismatched2;
    uint64_t missing2;
    uint64_t extra2;
} invfs_seal_verify;

int vol_seal_verify(invfs_volume *v, invfs_seal_verify *out);

/* WP20b layer-2 repair (invf-fsck --repair): scan every live shadow-zone
 * segment's framing CRC, then for each layer-2 stripe with failures try to
 * reconstruct the bad blocks with rs_decode (erasure search bounded by the
 * descriptor's m2, arbitrated by the stripes' stored parity AND the failed
 * segments' own CRC32C) and write the recovered blocks back. Stripes with
 * more damage than m2 (or whose reconstruction cannot prove itself) are
 * reported and left untouched -- never written with garbage. All counters
 * zero when no layer-2 seal is configured. */
typedef struct {
    uint64_t stripes_scanned;    /* layer-2 stripes holding a failed segment */
    uint64_t stripes_repaired;
    uint64_t blocks_rewritten;
    uint64_t unrecoverable;      /* stripes beyond repair, left untouched */
    uint64_t hypotheses;         /* decode attempts the erasure search spent */
} invfs_seal2_repair;

int vol_seal2_repair(invfs_volume *v, invfs_seal2_repair *rep);

/* ---- WP21: sweep checkpoint + rollback (CKP0 descriptor, invarifs.h) ----
 * invf-sweep arms a checkpoint BEFORE the walk (vol_ckp_begin): the CKP0
 * descriptor records the inode-area and journal append pointers plus a
 * staging run holding the journal prefix (the journal is append-only but
 * compactions replace the active slot, so the pre-sweep L2P survives the
 * sweep only as that copy). While a checkpoint-armed sweep runs,
 * vol_free_blocks does NOT free: the blocks stay allocated and are
 * remembered in the retention registry (the hidden "\x01reten" owner
 * inode, written by vol_ckp_end at sweep end).
 * Post-sweep sessions free immediately as before (documented best-effort
 * hole for deletes between sweep and rollback).
 *
 * vol_ckp_realize (invf-sweep --realize) deletes the registry -- freeing
 * every retained block -- and clears CKP0: the point of no return. A bare
 * sweep instead realizes AFTER arming (WP22d): vol_ckp_begin stages the
 * current journal and writes the new CKP0 first, and only then deletes
 * the old registry -- the volume always has one live net.
 *
 * vol_rollback (invf-rollback, offline) restores the staged journal and
 * truncates the inode area to the checkpoint pointers, then runs the
 * ordinary fsck rebuild: post-sweep records/journal entries vanish
 * wholesale, pre-sweep record versions resurrect with their (retained,
 * never-reallocated) blocks, and everything the sweep allocated is
 * reclaimed as orphans. */
/* 1 = a live (magic+CRC-valid) CKP0 descriptor was read at open */
int  vol_ckp_armed(const invfs_volume *v);
/* 1 = the previous session did not close cleanly (and did not
 * auto-recover); rollback tools proceed anyway -- they ARE the recovery */
int  vol_needs_recovery(invfs_volume *v);
/* 1 = a flush/sync failure latched the volume THIS session (WP22c). The
 * FUSE read path refuses on it (loud beats maybe-phantom); a volume that
 * merely opened dirty stays readable for inspection. */
int  vol_io_latched(invfs_volume *v);
/* 1 = live (+ a copy of the descriptor), 0 = absent */
int  vol_ckp_info(const invfs_volume *v, invfs_ckp0 *out);
/* sweep start: 1 = armed (retention active), 0 = declined (the volume is
 * read-only/recovering, a redundancy seal is live, or the environment
 * opted out), -1 = hard error. Declined is NOT an error: the sweep runs
 * uncheckpointed. */
int  vol_ckp_begin(invfs_volume *v);
/* sweep end: write the retention registry (the "\x01reten" owner + L2P
 * maps, sharded like the seal owners). 0 = ok (or nothing armed). */
int  vol_ckp_end(invfs_volume *v, uint64_t *ranges_out, uint64_t *blocks_out);
/* realize: delete the registry (freeing its blocks) + clear CKP0.
 * 1 = something was realized, 0 = nothing live, -1 = error. */
int  vol_ckp_realize(invfs_volume *v, uint64_t *freed_blocks_out);
/* rollback: 0 = rolled back, 1 = no checkpoint, -2 = refused (a live
 * redundancy seal would be invalidated; --free-redundant first), -3 = the
 * checkpoint or its staging failed verification (the post-sweep state is
 * untouched), -1 = io/rebuild error (re-run; the steps are idempotent).
 * *reclaimed_out (optional) takes the orphan-block count the rebuild freed. */
int  vol_rollback(invfs_volume *v, uint64_t *reclaimed_out);

/* ---- WP22e: online inode-area compaction (hot tail pruning) ----
 * The append-only inode area accumulates dead record versions and
 * tombstones; compaction rewrites it with just the live records (verbatim,
 * same inode ids, id order, tombstones dropped) and cuts the dead tail.
 * Crash protocol: the compacted stream is staged in free space (CMPS
 * header + payload CRC, verified by read-back), the CMP0 descriptor AND
 * the VOLF_READONLY latch are armed in one block-0 write, the staging is
 * copied over the area, a zero guard terminates it, and descriptor+latch
 * are cleared in one write. A crash before the arm leaves the old area
 * intact (the stranded staging run is an fsck orphan); a crash after the
 * arm is rolled forward by invf-fsck -f (vol_compact_recover). Never runs
 * while a CKP0 sweep checkpoint is live (rollback truncates to absolute
 * checkpoint positions), nor on read-only/recovering volumes.
 *
 * vol_inode_live_bytes: sum of rec_len+4 over the live records (what the
 * area would compact to); 0 on an empty area or a troubled scan -- never
 * trigger on it.
 * vol_inode_compact: 1 = compacted (the out params take the used bytes
 * before and after), 0 = declined (reason on stderr; the run is fine),
 * -1 = hard error.
 * vol_compact_pending: 1 = a CMP0 descriptor is armed (an interrupted
 * compaction is pending; write tools should refuse and point at fsck).
 * vol_compact_recover: 1 = rolled an interrupted pass forward (idempotent),
 * 0 = none pending, -1 = the descriptor/staging failed verification (the
 * area is left untouched for review). fsck-side only; the staging run is
 * left for the rebuild to reclaim as an orphan. */
uint64_t vol_inode_live_bytes(invfs_volume *v);
int  vol_inode_compact(invfs_volume *v, uint64_t *before_out,
                       uint64_t *after_out);
int  vol_compact_pending(invfs_volume *v);
int  vol_compact_recover(invfs_volume *v);

/* WP22e --fast sweep mode: the per-file decision narrowed to "generic or
 * nothing" -- RAW segments are recompressed per-segment at the volume's
 * profile level (the generic floor); classification, container
 * decomposition, codec transcodes and batching never run. Returns
 * vol_sweep_file_inner's: 0 swept, 1 nothing to do, -1 hard error. */
int  vol_sweep_file_generic(invfs_volume *v, uint64_t inode_id);
