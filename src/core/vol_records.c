/* vol_records.c — inode area append-only records, INO2 metadata,
 * xattr TLVs, storage-class flag, record retire/delete. Split from volume.c. */

#include "volume_internal.h"
#include "vol_fault.h"
#include "vol_walk.h"

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
 * goes through the shared extent-aware record scan so siblings in
 * dynamic metadata extents are found; the per-record policy (shape check,
 * supersede check, first-name-wins) is unchanged.
 *
 * WP135: "name!..." no longer means "anything after name!". It means one of
 * the shapes a lane actually mints -- see sib_suffix_is_internal below for
 * the list and the minting site of each. Before, a '!' in a user name made
 * that name collectable, and `rm a` destroyed `a!b` and reported success. */
typedef struct {
    invfs_volume *v;
    char (*names)[256];
    size_t n, cap, nlen;
    const char *name;
    int oom;
} del_siblings_ctx;


/* ---- is this text one of the shapes a lane actually mints? -----------
 *
 * WP135. The "is this my sibling" test below used to be a PREFIX match on
 * "name!" with nothing said about what followed, so it collected every name
 * that merely started with `name!` -- and `!` was not reserved, so for a
 * volume holding a user file `a!b` the unlink of `a` collected `a!b` and
 * destroyed it, reporting success. No fault injection: a complete, healthy
 * walk did it.
 *
 * This is the DEFENCE layer. It is necessary because the hole is reachable
 * without the name boundary -- invf-import writes a '!' name straight to the
 * volume, and a volume that ALREADY holds one is not rescued by refusing new
 * ones -- but it is not sufficient on its own, and the reason is worth
 * writing down: it couples this function to every lane's naming, and the
 * coupling fails OPEN. A lane that mints a ninth suffix is not merely
 * un-purged; its siblings become permanent orphans, which is exactly the
 * leak this function exists to prevent (see its own header above). The
 * reservation in name_is_internal_ns (volume_internal.h) is what makes the
 * hole unreachable for a suffix that does not exist yet; this list is what
 * protects the names already on disk.
 *
 * The shapes, and where each is minted -- a lane that adds one must add it
 * HERE in the same commit, which is the coupling this layer buys and the
 * reason it is not the whole fix:
 *
 *   recipe            vol_cpack.c:1232 :1369, vol_sweep.c:458
 *   mbrt              vol_cpack.c:2902 :3245 :3641 :3838 :3864 :3923
 *   mbrmap            vol_cpack.c:2903 :3000 :3683
 *   jxl               vol_png.c:442 :734, vol_sweep.c:516
 *   cover<digits>     vol_cpack.c:1266 :1403
 *   part<digits>      vol_cpack.c:1537 :1780, vol_sweep.c:250 :477 :497
 *   exr<digits>       vol_exer.c:266 :431
 *   mbr<4 digits>     vol_cpack.c:1925
 *   mbr<4 digits>-<san>  vol_cpack.c:1923, where <san> is a member name run
 *                       through cpack_sanitize (vol_cpack.c:1902) and so holds
 *                       only [A-Za-z0-9._-]
 *
 * `mbrNNNN` is pinned to FOUR digits because that is what "%04u" prints and
 * the read side matches it at that width; a longer run is not a shape this
 * tree mints, and accepting it would widen the hole for nothing.
 */
static int sib_suffix_is_internal(const char *suf, size_t n)
{
    size_t i = 0;

    /* fixed tags */
    if (n == 6 && (!memcmp(suf, "recipe", 6) || !memcmp(suf, "mbrmap", 6)))
        return 1;
    if (n == 4 && !memcmp(suf, "mbrt", 4)) return 1;
    if (n == 3 && !memcmp(suf, "jxl", 3))   return 1;

    /* <tag><digits>: cover/cover0/cover12, part0, exr0 ... */
    {
        static const char *dtag[] = { "cover", "part", "exr" };
        size_t t;
        for (t = 0; t < sizeof dtag / sizeof dtag[0]; t++) {
            size_t tl = strlen(dtag[t]);
            if (n <= tl || memcmp(suf, dtag[t], tl) != 0) continue;
            for (i = tl; i < n; i++)
                if (suf[i] < '0' || suf[i] > '9') break;
            if (i == n) return 1;
        }
    }

    /* mbr<4 digits>[-<san>] */
    if (n >= 7 && !memcmp(suf, "mbr", 3)) {
        for (i = 3; i < 7; i++)
            if (suf[i] < '0' || suf[i] > '9') return 0;
        if (n == 7) return 1;
        if (suf[7] != '-') return 0;
        /* the '-' form is only used when the sanitised name is non-empty
         * (cpack_mbr_name: `if (san && san[0])`), so a BARE trailing '-' is
         * not a shape this tree mints and must not be accepted as one. */
        if (n == 8) return 0;
        for (i = 8; i < n; i++) {
            char c = suf[i];
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) return 0;
        }
        return 1;
    }
    return 0;
}


static int del_siblings_v3_cb(void *ctx_, const char *path, uint64_t ino,
                              uint32_t type, uint64_t size, int64_t mtime)
{
    del_siblings_ctx *c = (del_siblings_ctx *)ctx_;
    size_t pl;
    (void)ino; (void)type; (void)size; (void)mtime;
    /* WP135: the prefix is necessary and NOT sufficient. Without the shape
     * check every "name!anything" is collected, and with '!' unreserved
     * (which it was, for years) that includes user files -- `rm a` destroyed
     * `a!b` and said so with a zero exit status. */
    if (strncmp(path, c->name, c->nlen) != 0 || path[c->nlen] != '!')
        return 0;
    pl = strlen(path);
    if (pl >= 256) return 0;
    if (!sib_suffix_is_internal(path + c->nlen + 1, pl - c->nlen - 1))
        return 0;
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

/* Retire every `name!` sibling of a container: the `!partN` payloads, the
 * `!recipe` / `!mbrt` blobs, whatever the lane that wrote them called them.
 * Returns the number unlinked.
 *
 * WP-delete-siblings-short-walk-leaves-orphans. The enumeration is a v3
 * WALK, and a walk is FALLIBLE: v3_walk_dir stops at a quarantined base page,
 * a failed listing, a depth cap or an OOM, and returns -1 having delivered a
 * PREFIX of the namespace. A stopped walk reports found == n -- it saw
 * exactly what it stored -- so with the status on the floor there is no way
 * to tell a whole sibling set from a prefix of one. Unlinking the prefix is
 * what this function used to do, and what it leaves behind is not
 * recoverable: the sweep walks the namespace but never retires an internal
 * '!' name, spn_reclaim cannot free a block a live recipe names, and
 * invf-fsck counts the survivors as live files. There is no decay pass and no
 * retry, so a short walk here is a PERMANENT leak -- the same walk-receipt
 * class as the heat stage (src/core/vol_heat.c), where the cost of refusing
 * was one sweep interval, and here it is not that.
 *
 * WHY IT REFUSES RATHER THAN PURGING THE PREFIX
 * =============================================
 *
 * Both answers leak the siblings outside the prefix; neither pass will ever
 * collect them. They are not equal, though, because the prefix purge also
 * DESTROYS what it did see, and a container's siblings are a set rather than
 * a bag: freeing `!part0..part2` and leaving `!part3` (and possibly the
 * `!recipe` that says how many parts there were) is a state no reader can
 * interpret and no pass can collect -- while reporting success. So:
 *
 *   refuse  -> costs DISK SPACE until a later pass. Visible in `df`, bounded
 *              by one container, and it comes back: the containerpack lane
 *              re-probes `name!mbrt` before every decomposition
 *              (src/core/vol_cpack.c:3244), the EXER lane re-probes
 *              `name!exr0` (src/core/vol_exer.c:265), and vol_transcode_abort
 *              runs on every abort of the lane that left them. The two
 *              callers with no later pass are vol_v3_unlink's cascade
 *              (src/core/vol_dirs.c:663) and the v3 write commit
 *              (src/core/vol_write.c:905); those are exactly the two where
 *              the space does not come back, and the message says so.
 *   prefix  -> costs CORRECTNESS, permanently and silently.
 *
 * And the answer must not depend on WHERE the walk happened to stop: a
 * caller that purges one container may otherwise legitimately get its
 * siblings freed and the next one's left behind, with nothing to tell the
 * difference.
 *
 * WHY THE STRICT WALK AND NOT JUST THE RECEIPT
 * ============================================
 *
 * The receipt below covers a walk that STOPS. It cannot cover a walk that
 * steps over an entry and still reports success, and the lenient walk does
 * exactly that: an entry whose inode row cannot be read is a `continue`
 * (src/core/vol_dirs.c:902-913), not an error. Measured on the fixture in
 * src/cli/sib_walk_test.c, one unreadable sibling of a four-part container
 * left the walk returning 0 -- and the old code then freed the other three.
 * That is the half-destroyed container the argument above is about, reached
 * through a walk that claims to be whole, so this function asks for
 * vol_v3_walk_strict: the one mode whose own comment (src/core/vol_dirs.c:937)
 * says its answer "must not be a partial view of the namespace".
 *
 * The strict mode is also the fail-CLOSED choice: one unreadable row
 * anywhere on the volume makes a destructive operation decline rather than
 * guess. That costs the purge on an otherwise healthy volume -- which is the
 * trade, and it is why the refusal is printed on stderr rather than left to
 * the latch alone. */
int vol_delete_siblings(invfs_volume *v, const char *name)
{
    del_siblings_ctx c;
    vol_walk_t w;
    size_t i;
    int rc, n;

    memset(&c, 0, sizeof c);
    c.v = v;
    c.name = name;
    c.nlen = strlen(name);

    rc = vol_v3_walk_strict(v, del_siblings_v3_cb, &c);

    /* WP-delete-siblings-short-walk-leaves-orphans: the RECEIPT, the same
     * mechanism and no second one (src/core/vol_walk.h). found == n on
     * purpose: this walk has no caller-imposed cap, so nothing but a walk
     * that did not finish can make it short. */
    vol_walk_init(&w, v, "vol_delete_siblings");
    vol_walk_result(&w, rc, c.n, c.n);
    if (vol_walk_commit(&w) != 0) {
        fprintf(stderr,
                "[vol] %s: sibling purge REFUSED -- the namespace walk did "
                "not complete (it reached %zu sibling(s) and then stopped "
                "rather than finishing), so it is not known whether these are "
                "all of them or a prefix. NOTHING was unlinked: unlinking a "
                "prefix would leave '!part0..partN' half gone with the rest "
                "live, and the sweep never retires an internal '!' name and "
                "invf-fsck counts the survivors as live files, so nothing "
                "would ever collect them. The space these siblings occupy is "
                "NOT reclaimed by this pass; it comes back on the next pass "
                "that completes the walk (the containerpack and EXER lanes "
                "re-probe their leftovers, and vol_transcode_abort runs on "
                "every abort of the lane that left them). Until then the "
                "container is stored TWICE on the volume: the superseded "
                "siblings below, and whatever the caller wrote to replace "
                "them. The two callers with no later pass are an unlink of a "
                "container (vol_v3_unlink) and a v3 overwrite of one "
                "(vol_write_commit); for those the space stays until the "
                "volume is rebuilt from a backup or the names are removed by "
                "hand.\n",
                name, c.n);
        free(c.names);
        return 0;
    }

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
 * index-ordered record scan; captures the first record whose id
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
    /* Kept at 0 / -1 for its ~100 callers, which all read the -1 as "no
     * metadata, use a default". vol_get_meta_rc is the same read with the
     * two reasons kept apart; widening this one would change every one of
     * those callers at once, so the split is made at the other end of the
     * wire instead. */
    return vol_get_meta_rc(v, inode_id, out) == 0 ? 0 : -1;
}


/* THE RETURN CONTRACT IS THREE DISTINCT ANSWERS, and that is the whole
 * point of this comment. This function used to return -1 for BOTH "this
 * inode has no meta row" and "the row could not be read" (the flattening is
 * still visible one line up). Those are not remotely the same thing, and the
 * difference is not academic:
 *
 *   -ENOENT  the row is not there. NORMAL, and the answer the v1->v2
 *            metadata upgrade path is built on: volume.c sets
 *            v3_mbuf_ready unconditionally on open, so on a volume whose
 *            records predate the v3 inode row -- i.e. every pre-v3 volume,
 *            see the contract note in volume.h -- vol_v3_inode_get answers
 *            0 and no row will ever be found. A caller stamping metadata
 *            onto such a record legitimately starts from type defaults
 *            (uid/gid 0, 0644 files / 0755 dirs) and applies its patch on
 *            top; the defaults stand in for a row that was never written.
 *
 *   -EIO     the row could not be READ. DAMAGE: a quarantined base page
 *            makes bt_read fail, btree_search return -1 and v3_base_get
 *            pass that through, and a value that will not decode lands here
 *            too. There is no honest default for an inode whose recorded
 *            mode and owner are exactly what could not be read -- the
 *            defaults are not a guess, they are a DIFFERENT inode.
 *
 * The FUSE side acted on the merged value: meta_for_path filled
 * meta_defaults() and returned success, and meta_apply_patch then WROTE
 * those defaults back, so a chmod / utimens / chown on an inode whose row
 * would not read silently reset its mode to 0644 and its owner to root. The
 * volume was changed by a call that reported success.
 *
 * -EIO covers every unreadable cause rather than naming them, because the
 * caller's correct response is the same for all of them and none of them is
 * a "no". */
int vol_get_meta_rc(invfs_volume *v, uint64_t inode_id, invfs_meta_pub *out)
{
    invfs_v3_inode in;
    int rc;
    if (!out) return -EIO;
    /* WP-M5/WP-M24: there is no INO2 ext -- the row in the base tree is the
     * authority. Map it onto the same public view (size/recipe included);
     * the symlink target rides in the recipe blob (WP-M8/WP-M24). */
    rc = vol_v3_inode_get(v, inode_id, &in);
    if (rc < 0) return -EIO;      /* the row could not be read */
    if (rc == 0) return -ENOENT;  /* there is no such row; see above */
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

