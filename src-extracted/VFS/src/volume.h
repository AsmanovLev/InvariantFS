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

uint64_t vol_write_raw(invfs_volume *v, const uint8_t *data, size_t len);

/* fsck/repair: rebuild L2P + used-bitmap from the inode area */
typedef struct {
    uint64_t live_files;
    uint64_t l2p_entries;
    uint64_t l2p_miss;
    uint64_t orphans;
    uint64_t missing;
    uint64_t bad_recs;
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

/* whole-volume statistics for tools/UIs (read-only walk). */
typedef struct {
    uint64_t files, dirs, links, special;
    uint64_t tombstones, bad_records;
    uint64_t logical_bytes, biggest_size;
    char     biggest_name[256];
} invfs_volume_stats;
int vol_compute_stats(invfs_volume *v, invfs_volume_stats *out);

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
