/* vol_records.c — inode area append-only records, INO2 metadata,
 * xattr TLVs, storage-class flag, record retire/delete. Split from volume.c. */

#include "volume_internal.h"
#include "vol_fault.h"

/* ---- inode area (append-only records) ---- */

/* write inode record: name + AST blocks; returns inode_id (0 on error) */
uint64_t vol_create_file(invfs_volume *v, const char *name,
                        const uint8_t *data, size_t len)
{
    /* WP-M6: a v3 node is a dirent + inode row; content goes through the
     * WP-M9 session path (WP-M21b glue: vol_v3_write_bulk). */
    return (data && len) ? vol_v3_write_bulk(v, name, data, len, NULL)
                         : vol_v3_create_node(v, name, NULL);
}


/* create_file + embed INO2 metadata in one record append.
 * Combines the file creation and vol_apply_meta into a single inode-area
 * write + vol_pre_record flush, cutting import cost from 2 appends to 1. */
uint64_t vol_create_file_with_meta(invfs_volume *v, const char *name,
                                   const uint8_t *data, size_t len,
                                   const invfs_meta_pub *meta)
{
    /* WP-M6: a v3 node is a dirent + inode row; content goes through the
     * WP-M9 session path (WP-M21b glue: vol_v3_write_bulk). */
    return (data && len) ? vol_v3_write_bulk(v, name, data, len, meta)
                         : vol_v3_create_node(v, name, meta);
}


/*
 * Delete a file: free its data blocks, drop its owner-WAL maps, append
 * tombstone. Returns 0 on success, -1 if not found.
 */
/* delete the specific inode (NOT by name — safe for atomic sweeps:
 * create-new-first then delete-old; the new inode stays untouched). */
/* Retire an inode: tombstone it and drop its maps, with or without freeing
 * the blocks those maps name.
 *
 * `free_data = 0` exists for two callers that must NOT free:
 *
 *  - fsck's orphan reclaim. Dedupe remaps several inodes onto one canonical
 *    pba (sweep_dedupe), so "this inode's blocks" is not the same set as
 *    "blocks only this inode uses". Freeing directly would punch a hole in a
 *    live file that shares a deduplicated segment -- identical FLAC cover art
 *    across an album is exactly the case dedupe was built for. fsck instead
 *    rebuilds the bitmap from whatever records remain live, which frees
 *    precisely what nothing references any more.
 *  - rename, which hands the same blocks to a second inode id before
 *    retiring the first.
 *
 * Ordering note for the freeing case: the tombstone is written with io_write
 * while the bitmap is only dirtied in RAM, so the tombstone is durable before
 * the frees it authorizes. The reverse order would let a crash leave a live
 * record whose blocks are free and reusable.
 */
/* WP27 retire-time free, file-class records: the dying record's own AST
 * entries name the blocks. Sharing (dedupe merges, WP4b aliases, hardlink
 * twins) is answered by the session pba reference map; the physical
 * extent derives from the segment's framed header, cross-checked against
 * the bitmap before anything is freed (a torn header degrades to a leak,
 * never an over-free). */



/* WP58b: overwrite an owner-class record in its dedicated mapper extent with
 * a v2 position-kill tombstone. Owner extents hold exactly one record and are
 * rewritten in place (tz_owner_write / vol_append_owner_slot), so the delete
 * must write the DELT at the owner's own position -- appending to the shared
 * file-record extent lands before the owner in walk order and the kill is
 * lost (see the comment at the tombstone site). The tombstone is smaller than
 * the owner INOD it replaces (no AST body), so the write stays inside the
 * extent; the record's rec_len governs the next boundary. */






/* v3 retires an inode by moving the dirent, not by retiring a record:
 * vol_v3_unlink drops the dirent and vol_v3_free_recipe_blocks frees the
 * segments when the last name goes. There is nothing left for a
 * record-level retire to do. */
int vol_delete_inode(invfs_volume *v, uint64_t inode_id, const char *name)
{
    (void)inode_id; (void)name;
    fprintf(stderr, "vol_delete_inode(%s): no record stream to retire -- "
            "the dirent is the name; use vol_unlink\n",
            name ? name : "?");
    return -1;
}


/* Does this recipe own "name!..." siblings? Asked by the v3 write session
 * before it republishes the row (vol_write.c), so the answer comes off the
 * OLD content's recipe. The v2 loader answered the same question off the
 * old record and the v2 commit retired them; both are gone, and one rule
 * answering once is why the v3 path does not strand the siblings. */
int ast_owns_siblings(const invfs_ast_hdr *ah,
                      const invfs_ast_block_entry *ents, size_t nents)
{
    uint32_t algo;

    if (!ah)
        return 1;
    if (ah->num_children != 0)
        return 1;
    if (ah->num_blocks == 0)
        return 0;
    if (!ents || nents == 0)
        return 1;
    algo = ents[0].algo;
    switch (algo) {
    case INVFS_ALGO_TARR:
    case INVFS_ALGO_GZR:
    case INVFS_ALGO_PNGR:
    case INVFS_ALGO_FLACR:
    case INVFS_ALGO_EXER:
        return 1;
    }
    /* WP16a: a container codecpack's recipe owns "!mbrNNNN" siblings. An
     * algo this build cannot resolve (the pack is not loaded right now)
     * gets the conservative answer: the scan costs one namespace walk and
     * can only find what is there -- the codec-pack case (raw_image et al)
     * simply has no siblings to find. */
    {
        const invfs_codec *pc = invfs_codec_by_algo(algo);
        const invfs_pack_def *pd;
        if (!pc) return 1;
        pd = invfs_codec_pack_def(pc);
        if (pd && pd->is_container) return 1;
    }
    return 0;
}


/* Delete every "name!..." sibling of `name`. Returns how many were retired.
 *
 * A transcoded file keeps its payload in sibling records the read path
 * resolves BY NAME -- "name!recipe", "name!coverN", "name!partN", "name!jxl".
 * Removing or replacing the file it belongs to left those behind: they are
 * valid live records under names nothing reaches any more, so fsck counts them
 * live, sweep skips them as internals, and no pass ever frees them. A 366 KB
 * FLAC overwritten by 15 bytes of text stranded a 36 KB !recipe permanently.
 *
 * Collect the names first: vol_delete_inode appends a tombstone, so a live
 * scan would walk into records it had just written. WP47: the collection
 * goes through the shared mapper-aware vol_records_walk() so siblings in
 * dynamic metadata extents are found on v0.3.0+ volumes; the per-record
 * policy (prefix-match, supersede check, first-name-wins) is unchanged. */
typedef struct {
    invfs_volume *v;
    char (*names)[256];
    size_t n, cap, nlen;
    const char *name;
    int oom;
} del_siblings_ctx;



static int del_siblings_v3_cb(void *ctx_, const char *path, uint64_t ino,
                              uint32_t type, uint64_t size, int64_t mtime)
{
    del_siblings_ctx *c = (del_siblings_ctx *)ctx_;
    size_t pl;
    (void)ino; (void)type; (void)size; (void)mtime;
    if (strncmp(path, c->name, c->nlen) != 0 || path[c->nlen] != '!')
        return 0;
    pl = strlen(path);
    if (pl >= 256) return 0;
    if (c->n >= c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 16;
        char (*nn)[256] = (char (*)[256])realloc(c->names, ncap * sizeof(*nn));
        if (!nn) { c->oom = 1; return 1; }
        c->names = nn;
        c->cap = ncap;
    }
    memcpy(c->names[c->n++], path, pl + 1);
    return 0;
}

int vol_delete_siblings(invfs_volume *v, const char *name)
{
    del_siblings_ctx c;
    size_t i;
    int n;
    memset(&c, 0, sizeof c);
    c.v = v;
    c.name = name;
    c.nlen = strlen(name);
    vol_v3_walk(v, del_siblings_v3_cb, &c);
    n = (int)c.n;
    for (i = 0; i < c.n; i++)
        vol_v3_unlink(v, c.names[i]);
    free(c.names);
    return n;
}


/* A transcode that gives up partway has already committed some of its
   children. Their names ("name!partN", "name!recipe", "name!coverN") stay live
   records that no pass can reach: sweep skips internal '!' names and fsck
   counts them as live files, so nothing will ever free them. Purge them on
   every abort -- including the "not smaller, keep the original" verdict, which
   for tar/gz is reached only after the parts are already down, so the cheapest
   possible outcome was silently the most expensive one. */
uint64_t vol_transcode_abort(invfs_volume *v, const char *name)
{
    int n = vol_delete_siblings(v, name);
    if (n > 0 && getenv("INVFS_DEBUG"))
        fprintf(stderr, "[vol] %s: transcode aborted, purged %d orphan(s)\n",
                name, n);
    return 0;
}


/* ==================== format v2 metadata (INO2 ext block) ====================
 *
 * A v2 record is [base][AST blob]["INO2" ext][CRC32C]. The ext carries the
 * POSIX identity of the inode: type, mode, uid/gid, mtime/atime, nlink,
 * rdev, symlink target and xattr TLVs. Everything a Linux rootfs needs that
 * the v1 base record cannot hold.
 *
 * Rewrites NEVER change the inode id: the L2P keys stay valid, so metadata
 * updates are crash-safe without touching data blocks. The old version is
 * killed by appending a tombstone whose UNUSED file_size field holds the
 * byte offset of the exact INOD being retired ("position kill"). Legacy
 * tombstones leave file_size 0 and keep the old kill-by-id semantics, so
 * v1 volumes replay identically. Both records land in ONE io_write: a torn
 * tail fails the trailing CRC and the scan stops at the previous boundary.
 */

/* total AST blob length after the fixed header (recipe + children), or
 * (size_t)-1 if bounds are violated */



/* parse one xattr TLV at p; returns bytes consumed or 0 on corruption */






/* locate the ext inside a raw record; NULL when absent (v1 record) */



/* WP48: fallback locator for meta_read_record_by_id. Fed by the bounded,
 * index-ordered vol_records_walk; captures the first record whose id
 * matches (the walker's semantics; the caller only needs a version of that
 * id). */
typedef struct {
    uint64_t want;
    uint8_t *buf;
    uint32_t rl;
    uint64_t pos;
} byid_ctx;




/* read the latest live record for an inode id; returns malloc'd buffer and
 * optionally its name/position. Uses the id-index hint when valid, repairs
 * the index once if stale, and otherwise falls back to a bounded walk. */






static void meta_pub_from_hdr_defaults(invfs_meta_pub *m, uint8_t type)
{
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->mode = (type == INVFS_ITYP_DIR) ? 0755 :
              (type == INVFS_ITYP_LNK) ? 0777 : 0644;
    m->nlink = (type == INVFS_ITYP_DIR) ? 2 : 1;
}


int vol_get_meta(invfs_volume *v, uint64_t inode_id, invfs_meta_pub *out)
{
    invfs_v3_inode in;
    if (!out) return -1;
    /* WP-M5/WP-M24: there is no INO2 ext -- the row in the base tree is the
     * authority. Map it onto the same public view (size/recipe included);
     * the symlink target rides in the recipe blob (WP-M8/WP-M24). */
    if (vol_v3_inode_get(v, inode_id, &in) != 1)
        return -1;
    memset(out, 0, sizeof(*out));
    out->type  = (uint8_t)in.type;
    out->mode  = in.mode;
    out->uid   = in.uid;
    out->gid   = in.gid;
    out->mtime = in.mtime;
    out->atime = in.atime;
    out->nlink = in.nlink;
    out->rdev  = in.rdev;
    out->size  = in.size;
    out->recipe = in.recipe;
    if (in.type == INVFS_ITYP_LNK && in.size > 0) {
        uint8_t *blob = NULL;
        size_t blen = 0;
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) == 0 && blob) {
            size_t cpsz = blen < sizeof(out->target) - 1 ? blen : sizeof(out->target) - 1;
            memcpy(out->target, blob, cpsz);
            out->target[cpsz] = '\0';
            free(blob);
        }
    }
    return 0;
}


/* append [INOD(same id, updated ext)][DELT(position-kill)] as one write */



uint64_t vol_apply_meta(invfs_volume *v, const char *name,
                        const invfs_meta_pub *meta)
{
    /* WP-M6: update the v3 inode row behind the name (no INO2 ext). */
    return vol_v3_set_meta(v, name, meta);
}


uint64_t vol_create_symlink(invfs_volume *v, const char *name,
                            const char *target)
{
    invfs_meta_pub m;
    size_t tl;
    if (!v || !name || !target) return 0;
    if (name_too_long(name)) return 0;
    tl = strlen(target);
    if (tl == 0 || tl >= INVFS_META_TARGET_MAX) return 0;
    meta_pub_from_hdr_defaults(&m, INVFS_ITYP_LNK);
    m.mode = 0777;
    m.size = tl;
    memcpy(m.target, target, tl + 1);
    m.mtime = m.atime = (int64_t)time(NULL);
    return vol_v3_create_node(v, name, &m);
}


uint64_t vol_create_special(invfs_volume *v, const char *name,
                            uint8_t type, uint16_t mode, uint64_t rdev)
{
    invfs_meta_pub m;
    if (!v || !name) return 0;
    if (name_too_long(name)) return 0;
    if (type != INVFS_ITYP_FIFO && type != INVFS_ITYP_SOCK &&
        type != INVFS_ITYP_CHR && type != INVFS_ITYP_BLK)
        return 0;
    meta_pub_from_hdr_defaults(&m, type);
    m.mode = mode;
    m.rdev = rdev;
    m.mtime = m.atime = (int64_t)time(NULL);
    return vol_v3_create_node(v, name, &m);
}


/* ---- xattr TLV helpers ---- */

/* See volume.h for the return contract, which is the load-bearing part:
 * 0 / -ENODATA / -EIO / -ERANGE / -EINVAL are five different answers, and
 * -ENODATA in particular must never be handed back for a row that could not
 * be read. */
int vol_get_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  void *val, size_t *vlen)
{
    /* WP-M7: v3 named xattrs live in the base B+-tree, not the INO2 ext. */
    return vol_v3_xattr_get(v, inode_id, xn, val, vlen);
}


int vol_set_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  const void *val, size_t vlen)
{
    return vol_v3_xattr_delta_set(v, inode_id, xn, val, vlen);
}


int vol_remove_xattr(invfs_volume *v, uint64_t inode_id, const char *xn)
{
    return vol_v3_xattr_delta_del(v, inode_id, xn);
}


/* WP-M7: listxattr adapter over the v3 xattr tree. The tree orders keys by
 * (name_len, name), so collect and sort by name to match the v2 listing
 * expectation, then apply the v2 buffer semantics: positive = total bytes
 * needed, -2 = buffer too small. */
typedef struct {
    char **names;
    size_t n, cap;
    int    oom;
} v3_xattr_namevec;

static int v3_xattr_collect_name_cb(void *ctx_, const char *name, size_t nlen)
{
    v3_xattr_namevec *c = (v3_xattr_namevec *)ctx_;
    char *s;
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 16;
        char **nv = (char **)realloc(c->names, ncap * sizeof *nv);
        if (!nv) { c->oom = 1; return 1; }
        c->names = nv;
        c->cap = ncap;
    }
    s = (char *)malloc(nlen + 1);
    if (!s) { c->oom = 1; return 1; }
    memcpy(s, name, nlen);
    s[nlen] = 0;
    c->names[c->n++] = s;
    return 0;
}

static int v3_xattr_name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void v3_xattr_namevec_free(v3_xattr_namevec *c)
{
    size_t i;
    for (i = 0; i < c->n; i++)
        free(c->names[i]);
    free(c->names);
    c->names = NULL;
    c->n = 0;
}

/* Returns the number of bytes the NUL-separated name list needs (written to
 * `buf` when it is non-NULL and big enough), 0 when the inode has no xattrs,
 * or a negative errno: -2 (ERANGE) for a buffer too small, -EIO when the
 * xattr store could not be read. It never reports a read failure as 0 -- a
 * caller that read 0 as "no xattrs" would tell the user an object carries
 * none, which is what an unreadable xattr store looked like. */
int vol_list_xattr(invfs_volume *v, uint64_t inode_id,
                   char *buf, size_t bcap)
{
    v3_xattr_namevec c;
    size_t i, used = 0;
    int rc, overflow = 0;
    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT; stands in
     * for the scan/allocation failures below and returns the same errno. */
    if (invfs_vol_fault("vol_list_xattr"))
        return -EIO;
    memset(&c, 0, sizeof c);
    rc = vol_v3_xattr_scan(v, inode_id, v3_xattr_collect_name_cb, &c);
    if ((rc != 0 && rc != 1) || c.oom) {
        v3_xattr_namevec_free(&c);
        return -EIO;
    }
    if (c.n > 1)
        qsort(c.names, c.n, sizeof *c.names, v3_xattr_name_cmp);
    for (i = 0; i < c.n; i++) {
        size_t nl = strlen(c.names[i]);
        if (buf) {
            if (used + nl + 1 > bcap) { overflow = 1; break; }
            memcpy(buf + used, c.names[i], nl);
            buf[used + nl] = 0;
        }
        used += nl + 1;
    }
    v3_xattr_namevec_free(&c);
    if (overflow)
        return -2;
    return (int)used;
}


/* ---- WP10 storage-class flag ("invfs.class" xattr, WP10 §2) ---- */

/* 0 = the stamp was read, 1 = there is no usable class on this inode.
 *
 * The second answer is deliberately COARSE and it is a decision, not an
 * accident: a malformed value has always read as unclassified, and it is now
 * also what a row read that FAILS reads as. The storage class is a hint the
 * sweep uses to pick a lane; "I could not read the hint" and "there is no
 * hint" lead to the same safe place -- the generic floor -- and a class that
 * cannot be read is not a permission, a length, or anything bit-exactness
 * depends on. The class is NOT an authority the way a POSIX ACL is, so unlike
 * perm_check_cred this caller does not need to fail closed.
 *
 * The 0/1 contract is load-bearing at six call sites that all test `== 0` or
 * `!= 0` (vol_sweep.c:828, :983, :1112; vol_heat.c:663; vol_textzone.c:634;
 * tools/meta_probe.c:91), so the coarse answer is spelled out here rather
 * than widened into a third value underneath all of them. The ambiguity is
 * therefore now STATED rather than accidental, which is the difference this
 * change was after.
 */
int vol_get_class(invfs_volume *v, uint64_t inode_id,
                  uint8_t *cls, uint8_t *algo, uint16_t *gen)
{
    invfs_class_tlv tlv;
    size_t vlen = sizeof(tlv);
    if (vol_get_xattr(v, inode_id, INVFS_XATTR_CLASS, &tlv, &vlen) != 0 ||
        vlen != sizeof(tlv))
        return 1;   /* absent, malformed, or unreadable -> unclassified */
    if (cls)  *cls  = tlv.cls;
    if (algo) *algo = tlv.algo;
    if (gen)  *gen  = tlv.gen;
    return 0;
}


int vol_stamp_class(invfs_volume *v, uint64_t inode_id,
                    uint8_t cls, uint8_t algo, uint16_t gen)
{
    invfs_class_tlv cur, want;
    size_t vlen = sizeof(cur);

    /* check-then-write: an unchanged stamp would still cost a meta_rewrite
     * (record append + position-kill tombstone) per file per sweep */
    if (vol_get_xattr(v, inode_id, INVFS_XATTR_CLASS, &cur, &vlen) == 0 &&
        vlen == sizeof(cur) &&
        cur.cls == cls && cur.algo == algo && cur.gen == gen)
        return 1;   /* unchanged */
    want.cls  = cls;
    want.algo = algo;
    want.gen  = gen;
    if (vol_set_xattr(v, inode_id, INVFS_XATTR_CLASS,
                      &want, sizeof(want)) != 0)
        return -1;
    return 0;
}




/* Does a file whose AST looks like THIS own "name!..." siblings that must
 * die with it? A ZIP-style container lists AST children; the extraction
 * containers (TARR/GZR/PNGR/FLACR/EXER) carry num_children == 0 but keep
 * their payload in sibling inodes ("name!partN", "name!recipe", "name!jxl",
 * "name!coverN", "name!exrN") the read path resolves by name -- deleting
 * only the anchor strands them as live records nothing reaches (verified:
 * a TAR's parts survived vol_unlink). The sibling walk is O(namespace), so
 * plain files skip it.
 *
 * This is the ONE rule, on the parsed AST rather than on a container
 * format's bytes. Both callers ask it: the v2 record
 * (record_owns_siblings) and the v3 recipe blob
 * (wsession_load_old_v3, vol_write.c), so an overwrite retires exactly the
 * siblings its counterpart format retires. 1 = siblings possible. */



/* The v2 caller of ast_owns_siblings: the same answer, read off a record.
 * 1 = siblings possible (unknown record -> 1: scan conservatively). */



/* test-only export of the static parser */



/* WP-M21: vol_inode_compact / CMP0 / CMPS retired. The v3 inode area lives
 * in dynamic metadata extents (WP30); per-extent reclaim and the online
 * fold live in vol_fold.c / vol_reclaim.c. inode_area_make_room still
 * rejects ENOSPC on legacy (format_version=0) volumes -- those open
 * read-only, so the legacy fallback path never executes anyway. */

/* WP27 fold-churn backstop: the inode area is append-only and format v2
 * churns it harder than v1 ever did (every rewrite/meta-stamp/heat fold
 * appends). The fold path (vol_v3_fold) reclaims dead prefix bytes; with
 * vol_inode_compact gone, inode_area_make_room's mapper-extent allocator
 * is the only path that takes a v3 append from ENOSPC back to 0, and the
 * legacy inode area (format_version=0) is read-only on mount so no
 * reclaim is possible there.
 * and the caller re-checks. Returns 0 when `need` bytes fit. */
int inode_area_make_room(invfs_volume *v, uint64_t need)
{
    /* WP30: for v0.3.0+ the "inode area" is a sequence of dynamic metadata
     * extents tracked by the Mapper. Bounds are defined by the EXTENTS
     * themselves, NOT a single linear address range. The pre-check asks:
     * "can the next append land somewhere?" -- which means either
     *   (a) the active extent has room for `need` more bytes, OR
     *   (b) we can allocate a fresh extent (mapper + shadow zone free).
     * For legacy format_version=0 we fall back to the linear-end check. */
    if (v->met0_present && v->meta_mapper) {
        uint64_t entry = meta_mapper_get(v, (size_t)v->met0.active_extent);
        if (entry && (v->met0.active_offset + need) <=
                     invfs_meta_ext_size(entry))
            return 0;
        /* WP53: the active extent is full, or none exists yet
         * (extent_count==0). On a mapper volume the append allocator
         * (meta_get_append_pos) grows the extents itself, sized for the
         * record, so this is NOT a compaction case and must not fall
         * through to the checkpoint refusal below -- that path is what
         * made vol_ckp_end's retention-registry write fail at the end of
         * a sweep (vol_create_file calls this before meta_get_append_pos,
         * and a live sweep checkpoint then refused). The genuine ENOSPC
         * is "no mapper slots left".
         *
         * NOTE (shared with WP52): callers that then append through the
         * extent allocator (meta_get_append_pos / vol_append_slot) are
         * safe. The two legacy owner writers (`tz_owner_write`,
         * `wp25_owner_write`) still append at `inode_area_pos` instead,
         * so a record larger than the active extent still runs past its
         * end here; that overflow is WP52's scope, not this one. */
        return v->met0.extent_count < INVFS_META_EXT_ENTRIES ? 0 : -1;
    } else if (v->inode_area_pos + need <= v->inode_area_end) {
        return 0;
    }
    /* WP-M21: vol_inode_compact was the only way to reclaim inode-area bytes
     * on legacy (format_version=0) volumes. Those volumes open read-only in
     * v3 (see volume.c vol_open), so this branch is unreachable -- leaving
     * ENOSPC (a slow climb toward the metadata-zone end, exactly what the
     * read-only mount guards against). */
    (void)need;
    return -1;
}

