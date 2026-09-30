/* vol_spt0.c — WP-M16: v3 save point (SPT0 descriptor).
 *
 * Implements save point capture, rollback-to-savepoint, and drop.
 * See vol_spt0.h for the API contract.
 *
 * WP96 added the save point's DATA half. SPT0 pins {base_root, delta_end} --
 * the save point's metadata -- and nothing else, so a sweep that re-encodes a
 * file publishes a new recipe and frees the old segments right away, and a
 * later rollback republishes a recipe over blocks that now hold somebody
 * else's bytes (invf-rollback returns 0, invf-fsck says OK, the read fails).
 * Two independent layers answer that here:
 *
 *   (a) the pin (SPN0, below): at capture, mark every block the captured
 *       generation's recipes name, persist the mark set, and hold those
 *       blocks allocated until the generation they belong to is gone. A
 *       bare sweep drops the previous window and captures a new one before
 *       its walk, so a replaced recipe's blocks are held for exactly one
 *       generation and reclaimed by the next capture's reclaim pass;
 *   (b) the restore-time data check (spt0_data_ok): before publishing the
 *       pinned root, walk that generation's recipes and verify every segment
 *       they name. A failure is SPT0_RC_DAMAGED -- a refusal, with nothing
 *       written -- so a hole in (a) degrades to "rollback unavailable"
 *       instead of "silent corruption".
 */

#include "volume_internal.h"
#include "vol_spt0.h"
#include "vol_btree.h"
#include "vol_delta.h"
#include "vol_metabuf.h"
#include "vol_anchor.h"
#include "vol_reclaim.h"

#include <string.h>

/* WP96 internals, defined further down: spt0_load needs the pin descriptor
 * and the map, and the capture path needs the walk. */
static int  spn_desc_load(invfs_volume *v, invfs_spn0 *out);
static int  spn_desc_store(invfs_volume *v, const invfs_spn0 *s);
static int  spn_map_load(invfs_volume *v);
static uint8_t *spn_old_map_load(invfs_volume *v, uint64_t pba);
static void spn_map_free(invfs_volume *v);
static void spn_sync_armed(invfs_volume *v);
static int  spt0_pin_take(invfs_volume *v, uint64_t root_pba,
                           uint64_t delta_end, uint64_t delta_segs,
                           uint64_t delta_head_pba);
static int  spt0_data_ok(invfs_volume *v, char *err, size_t errlen);

/* Fill v->pinned_root from a base-root pba.
 *
 * The SPT0 descriptor stores a base root as a BARE PBA, so rebuilding
 * pinned_root from it means rebuilding a whole invfs_blkptr, and all four
 * fields matter. mbuf_read_ptr (vol_metabuf.c:161) refuses a pointer whose
 * checksum or gen does not match the page's own header, and every
 * reachability walk in the reclaim paths reaches its roots through
 * mbuf_read_ptr: bt_mark_rec (vol_btree.c:1727) for the fold diff,
 * bt_mark_rec_deep (vol_btree.c:2126) for the orphan collector.
 *
 * So a pinned_root with checksum = 0 and gen = 0 is not a loose pointer, it
 * is a pointer NOBODY can ever verify. The walk reaches the pinned root,
 * the page does not match the pointer, and the walk returns -1 -- which
 * aborts the fold diff outright (vol_fold.c:298 discards the result) and
 * aborts the orphan collector's entire collection, which is what invf-sweep
 * prints as "warning: v3 orphan reclaim failed" while still exiting 0.
 *
 * On failure the checksum and gen stay zero, deliberately, and that is the
 * safe direction: a walk that cannot verify the save point's tree refuses to
 * free anything, which costs space. Dropping the pba instead -- so the
 * pinned tree is not in the mark set at all -- would let the collector free
 * a tree the save point can still restore, which costs DATA. */
static void spt0_pinned_from_pba(invfs_volume *v, uint64_t base_root)
{
    uint8_t page[INVFS_BLOCK_SIZE];

    memset(&v->pinned_root, 0, sizeof v->pinned_root);
    v->pinned_root.pba = base_root;
    if (!base_root)
        return;
    if (mbuf_read(v, base_root, page) != 0 || !mbuf_page_validate(page))
        return;                        /* unverifiable: fail closed, see above */
    mbuf_ptr_set(&v->pinned_root, base_root, page,
                 INVFS_BP_ROOT | INVFS_BP_PINNED);
}

static uint32_t spt0_crc(const invfs_spt0 *s)
{
    invfs_spt0 t = *s;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}

int spt0_load(invfs_volume *v)
{
    invfs_spt0 s;
    if (!v)
        return -1;
    v->savepoint_live = 0;
    memset(&v->spt0, 0, sizeof v->spt0);
    memset(&v->pinned_root, 0, sizeof v->pinned_root);
    /* WP96: the hold is a VOLUME property, so it is keyed on the on-disk pin
     * descriptor (like v2's ck_present), not on this session having armed
     * anything: a FUSE write's retire path and a delete must see it too. */
    v->spn_nopin = getenv("INVFS_SPT0_NOPIN") ? 1 : 0;
    v->spn_armed = 0;
    v->spn_pba = v->spn_blocks = v->spn_npinned = 0;
    /* WP137: a fresh session has discharged nothing yet, so the ladder must
     * not read a previous session's reclaim as its own signal. */
    v->spn_reclaim_freed = 0;
    v->spn_delta_segs = v->spn_delta_head = 0;
    spn_map_free(v);

    if (io_pread(&v->io, INVFS_SPT0_OFF, &s, sizeof s) != 0)
        return -1;
    if (memcmp(s.magic, "SPT0", 4) == 0 &&
        s.version == INVFS_SPT0_VERSION &&
        spt0_crc(&s) == s.crc32c) {
        v->spt0 = s;
        v->savepoint_live = 1;
    } else if (v->anchor_state == INVFS_ANCHOR_OK &&
               memcmp(v->anchor.spt0.magic, "SPT0", 4) == 0 &&
               v->anchor.spt0.version == INVFS_SPT0_VERSION &&
               spt0_crc(&v->anchor.spt0) == v->anchor.spt0.crc32c) {
        /* The same rule as the RT30 recovery, applied PER DESCRIPTOR: a
         * block-0 descriptor that does not validate is replaced by its
         * mirror, and one that does is left strictly alone. So a volume that
         * lost only RT30's 48 bytes keeps a perfectly good SPT0, and one
         * that lost both is restored from both. The mirror is never mixed
         * into a healthy descriptor, because "the anchor is fresher" is a
         * claim, and the one thing a fallback must never do is overwrite
         * something that was not broken. */
        s = v->anchor.spt0;
        v->spt0 = s;
        v->savepoint_live = 1;
        v->anchor_adopted = 1;
        fprintf(stderr,
                "vol_open: *** ANC0 TAIL ANCHOR ADOPTED *** block %llu: the "
                "SPT0 save-point descriptor in block 0 is unreadable (offset "
                "0x%X), so the save point is being taken from the mirror at "
                "block %llu (base_root=%llu delta_end=%llu). Block 0 is "
                "damaged and must be replaced.\n",
                (unsigned long long)anchor_pba(v), (unsigned)INVFS_SPT0_OFF,
                (unsigned long long)anchor_pba(v),
                (unsigned long long)s.base_root,
                (unsigned long long)s.delta_end);
    } else
        return 1;
    /* The descriptor carries a pba, so the blkptr has to be rebuilt from
     * the page it names -- see spt0_pinned_from_pba. This is the path that
     * runs at every vol_open on a volume with a live window, i.e. the next
     * sweep after this one. */
    spt0_pinned_from_pba(v, s.base_root);
    /* the restore's data check needs the pinned log geometry even when no
     * pin was taken, and an armed pin needs its mark set in memory */
    {
        invfs_spn0 sp;
        if (spn_desc_load(v, &sp) == 0) {
            v->spn_delta_segs = sp.delta_segs;
            v->spn_delta_head = sp.delta_head_pba;
        }
    }
    spn_sync_armed(v);
    if (v->spn_armed && spn_map_load(v) != 0) {
        fprintf(stderr, "vol_open: the save point's data pin is unreadable; "
                "the hold is inactive for this session and a rollback will "
                "be refused by the restore-time data check\n");
        v->spn_armed = 0;
    }
    return 0;
}

int spt0_store(invfs_volume *v)
{
    invfs_spt0 s;
    if (!v)
        return -1;
    s = v->spt0;
    s.crc32c = 0;
    s.crc32c = spt0_crc(&s);
    if (io_pwrite(&v->io, INVFS_SPT0_OFF, &s, sizeof s) != 0)
        return -1;
    /* The mirror went stale the instant that landed. Same reasoning, and the
     * same non-failing behaviour, as the RT30 refresh: the primary store
     * succeeded, so a failing tail must not stop a save point from being
     * captured -- but anchor_refresh() says so out loud and latches it. */
    anchor_refresh(v);
    return 0;
}

/* WP86: a save point is only useful if the base it pins can be walked. The
 * page CRC of the root alone is not enough -- a torn page deeper in the tree
 * makes the pinned root unusable while every check on the root page passes.
 * spt0_tree_ok() walks the whole subtree, so neither capture nor restore can
 * pin (or roll back onto) a damaged base; the caller gets a reason instead of
 * a save point that would hand back an unreadable volume. */
static int spt0_tree_ok(invfs_volume *v, uint64_t pba, char *err, size_t errlen)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_blkptr root;
    char e[128];

    if (mbuf_read(v, pba, page) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "root page %llu is unreadable",
                     (unsigned long long)pba);
        return 0;
    }
    if (!mbuf_page_validate(page)) {
        if (err && errlen)
            snprintf(err, errlen, "root page %llu fails its CRC",
                     (unsigned long long)pba);
        return 0;
    }
    /* WP-D: CRC is not the only thing a root page can fail. Ask the
     * allocation bitmap too, for the reason this function exists in the
     * first place: it is the gate that decides whether a save point is
     * allowed to pin a base at all, so a base the volume has already given
     * back to the allocator must not be capturable. The subtree walk below
     * goes through mbuf_read_ptr and is hardened already; this is the root
     * page itself, which the walk never re-reads through that path.
     *
     * This makes capture refuse, it does not make a read fail: a savepoint
     * on a fully-pinned generation has its bits set (the SPN0 hold is what
     * keeps vol_free_blocks from clearing them), so a live base is
     * unaffected. */
    if (mbuf_page_allocated(v, pba) == 0) {
        if (err && errlen)
            snprintf(err, errlen, "root page %llu is FREE in the allocation "
                     "bitmap -- the volume has given that block back and a "
                     "save point cannot pin it", (unsigned long long)pba);
        return 0;
    }
    mbuf_ptr_set(&root, pba, page,
                 mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                 ? INVFS_BP_ROOT | INVFS_BP_LEAF
                 : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    e[0] = 0;
    if (btree_check(v, root, NULL, e, sizeof e) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "%s", e[0] ? e : "base tree is invalid");
        return 0;
    }
    return 1;
}

/* ===================================================================
 * WP96 layer 1: the save point's DATA pin (SPN0).
 *
 * The shape is WP21's retired per-run retmap, re-expressed for v3: a bitmap
 * over total_blocks, one bit per block, persisted in its own run of blocks
 * and named by a block-0 descriptor. While the pin is ARMED (a live save
 * point), vol_free_blocks refuses to free a block the bitmap names, so the
 * pinned generation's data survives every sweep that replaces a recipe.
 *
 * The leak-free ordering, which is the whole reason this shape works:
 *
 *   sweep N:      drop pin N-1 -> capture pin N (walk) -> walk/frees
 *   sweep N+1:    drop pin N   -> capture pin N+1 (walk) -> reclaim
 *                 (pin N \ pin N+1) -> walk/frees
 *
 * A recipe the sweep N replaced is not in pin N+1, so its blocks are freed
 * at the capture of N+1 -- held for exactly one generation. invf-sweep
 * --realize (spt0_drop) is the explicit "I do not need the window": it
 * disarms the pin, so the very next free gives the blocks back, and the pin
 * run itself is released by the next capture.
 * =================================================================== */

/* One pinned segment's identity: where it starts, and the two words that
 * make its content checkable again after a free + realloc. See
 * spn_file_hdr for why the mark bitmap alone cannot do this job. */
typedef struct {
    uint64_t pba;
    uint32_t csize;
    uint32_t crc;
} spn_dig;

/* The pin file: one block run holding [header][digests][mark bitmap]. The
 * bitmap is the HOLD (every free probes it); the digests are the PROOF (the
 * restore compares the block against what it held at capture). Both are
 * needed, and they are written together, because they answer different
 * questions: a block can be held and still be wrong (the sweep overwrote it
 * in place), and a block can be free without ever having been held. */
typedef struct {
    char     magic[4];       /* "SPNF" */
    uint32_t version;        /* 1 */
    uint32_t ndig;           /* digest entries that follow */
    uint64_t bitmap_bytes;   /* total_blocks/8 */
    uint64_t npinned;        /* set bits (diagnostics) */
    uint64_t broken;         /* segments already unreadable AT capture */
    uint32_t crc32c;         /* over this header with the field read 0 */
} spn_file_hdr;

typedef struct {
    invfs_volume *v;
    uint8_t *map;
    spn_dig *dig;
    size_t ndig, cdig;
    uint64_t broken;         /* a segment that was already damaged at capture */
    uint64_t marked;
    uint64_t recipes;
    uint64_t unreadable;     /* a recipe we could not even load */
    uint64_t n_inodes;
} spn_walk_ctx;

static uint32_t spn_crc(const invfs_spn0 *s)
{
    invfs_spn0 t = *s;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}

static size_t spn_map_bytes(const invfs_volume *v)
{
    return (size_t)((v->sb.total_blocks + 7) / 8);
}

/* Read the SPN0 descriptor. 0 = present and valid, 1 = absent, -1 = io. */
static int spn_desc_load(invfs_volume *v, invfs_spn0 *out)
{
    invfs_spn0 s;

    memset(out, 0, sizeof *out);
    if (io_pread(&v->io, INVFS_SPN0_OFF, &s, sizeof s) != 0)
        return -1;
    if (memcmp(s.magic, "SPN0", 4) != 0 ||
        s.version != INVFS_SPN0_VERSION ||
        spn_crc(&s) != s.crc32c)
        return 1;
    *out = s;
    return 0;
}

static int spn_desc_store(invfs_volume *v, const invfs_spn0 *s)
{
    invfs_spn0 t = *s;
    t.crc32c = 0;
    t.crc32c = spn_crc(&t);
    if (io_pwrite(&v->io, INVFS_SPN0_OFF, &t, sizeof t) != 0)
        return -1;
    return 0;
}

/* The pin file is ONE run of blocks holding three sections back to back:
 *
 *     [spn_file_hdr][ndig * spn_dig][mark bitmap]
 *
 * so every section offset is a BYTE offset inside the run. These two helpers
 * are the only code that knows that; everything above asks for a section. */
/* The run base is passed EXPLICITLY, never read from v->spn_pba: the capture
 * path writes the run it has just allocated, and the descriptor field that
 * names it is only updated once the bytes are durable. A writer that trusted
 * v->spn_pba here would write the pin file over block 0. */
static uint64_t spn_run_base(uint64_t pba)
{
    return pba * (uint64_t)INVFS_BLOCK_SIZE;
}

static int spn_run_read(invfs_volume *v, uint64_t pba, uint64_t off,
                        uint8_t *buf, size_t len)
{
    uint64_t done = 0;
    while (done < len) {
        uint64_t at = spn_run_base(pba) + off + done;
        size_t chunk = len - (size_t)done;
        if (chunk > INVFS_BLOCK_SIZE)
            chunk = INVFS_BLOCK_SIZE;
        if (io_pread(&v->io, at, buf + done, chunk) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static int spn_run_write(invfs_volume *v, uint64_t pba, uint64_t off,
                         const uint8_t *buf, size_t len)
{
    uint64_t done = 0;
    while (done < len) {
        uint64_t at = spn_run_base(pba) + off + done;
        size_t chunk = len - (size_t)done;
        if (chunk > INVFS_BLOCK_SIZE)
            chunk = INVFS_BLOCK_SIZE;
        if (io_pwrite(&v->io, at, buf + done, chunk) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

/* Where the mark bitmap starts, from the header's own numbers. */
static uint64_t spn_map_off(uint32_t ndig)
{
    return (uint64_t)sizeof(spn_file_hdr) + (uint64_t)ndig * sizeof(spn_dig);
}

/* Read a pin file's header. pba = 0 means "this handle's run".
 * 0 = ok, -1 = absent/unreadable. */
static int spn_file_hdr_read(invfs_volume *v, uint64_t pba, spn_file_hdr *out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    spn_file_hdr fh;
    uint32_t stored;

    memset(out, 0, sizeof *out);
    if (!pba)
        return -1;
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE,
                 page, sizeof page) != 0)
        return -1;
    memcpy(&fh, page, sizeof fh);
    if (memcmp(fh.magic, "SPNF", 4) != 0 || fh.version != 1)
        return -1;
    stored = fh.crc32c;
    fh.crc32c = 0;
    if (invfs_crc32c(&fh, sizeof fh) != stored)
        return -1;
    fh.crc32c = stored;
    *out = fh;
    return 0;
}

/* Pull the persisted mark bitmap into memory (v->spn_bitmap). Idempotent.
 * 0 = loaded (or nothing to load), -1 = io/alloc error. The digests that
 * precede the bitmap in the run are NOT read here: only the restore-time
 * proof needs them, and a process that merely writes should not pay for
 * them. */
static int spn_map_load(invfs_volume *v)
{
    spn_file_hdr fh;

    if (v->spn_bitmap)
        return 0;
    if (!v->spn_pba || !v->spn_blocks)
        return 0;
    if (spn_file_hdr_read(v, v->spn_pba, &fh) != 0)
        return -1;
    v->spn_bitmap = (uint8_t *)calloc(1, spn_map_bytes(v));
    if (!v->spn_bitmap)
        return -1;
    return spn_run_read(v, v->spn_pba, spn_map_off(fh.ndig), v->spn_bitmap,
                        spn_map_bytes(v));
}

/* Read the pin file's digest table (sorted by pba; a shared pba can appear
 * more than once). The restore compares every segment the pinned recipes
 * name against it, so a block that was freed and REUSED is caught even
 * though a reused block is itself a well-formed segment whose own CRC
 * matches. *dig_out is malloc'd. 0 = ok (NULL when the table is empty),
 * -1 = error. */
static int spn_dig_load(invfs_volume *v, spn_dig **dig_out, size_t *n_out)
{
    spn_file_hdr fh;
    uint8_t *buf;
    size_t n;

    *dig_out = NULL;
    *n_out = 0;
    if (spn_file_hdr_read(v, v->spn_pba, &fh) != 0)
        return -1;
    n = fh.ndig;
    if (n == 0)
        return 0;
    buf = (uint8_t *)malloc(n * sizeof(spn_dig));
    if (!buf)
        return -1;
    if (spn_run_read(v, v->spn_pba, sizeof(spn_file_hdr), buf,
                     n * sizeof(spn_dig)) != 0) {
        free(buf);
        return -1;
    }
    *dig_out = (spn_dig *)buf;
    *n_out = n;
    return 0;
}

/* The PREVIOUS generation's mark bitmap: the reclaim worklist. The old run's
 * layout is not recorded in the descriptor, so its header is re-read (it is
 * always the first thing in the run). */
static uint8_t *spn_old_map_load(invfs_volume *v, uint64_t pba)
{
    spn_file_hdr fh;
    uint8_t *m;

    if (spn_file_hdr_read(v, pba, &fh) != 0)
        return NULL;
    m = (uint8_t *)calloc(1, spn_map_bytes(v));
    if (!m)
        return NULL;
    if (spn_run_read(v, pba, spn_map_off(fh.ndig), m,
                     spn_map_bytes(v)) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

static void spn_map_free(invfs_volume *v)
{
    free(v->spn_bitmap);
    v->spn_bitmap = NULL;
}

/* Arm/disarm from the on-disk descriptor. Called at open (so a foreign
 * process' hold is honoured) and by capture/drop. */
static void spn_sync_armed(invfs_volume *v)
{
    invfs_spn0 s;
    int rc;

    if (!v || !(v->sb.vol_flags & VOLF_V3)) {
        if (v) v->spn_armed = 0;
        return;
    }
    rc = spn_desc_load(v, &s);
    if (rc != 0) {
        v->spn_armed = 0;
        v->spn_pba = v->spn_blocks = v->spn_npinned = 0;
        return;
    }
    v->spn_pba = s.pba;
    v->spn_blocks = s.blocks;
    v->spn_npinned = s.npinned;
    /* ARMED is a claim about the volume, not about this session: a save point
     * that is not live cannot pin anything, whatever the descriptor says. */
    v->spn_armed = (s.flags & INVFS_SPN0_F_ARMED) && v->savepoint_live;
    if (!v->spn_armed)
        spn_map_free(v);
}

/* Record one pinned segment's capture-time identity. The array grows as the
 * walk goes; the caller sorts it by pba so the restore can binary-search it. */
static int spn_dig_push(spn_walk_ctx *c, uint64_t pba, uint32_t csize,
                        uint32_t crc)
{
    if (c->ndig == c->cdig) {
        size_t nc = c->cdig ? c->cdig * 2 : 256;
        spn_dig *nd = (spn_dig *)realloc(c->dig, nc * sizeof *nd);
        if (!nd)
            return -1;
        c->dig = nd;
        c->cdig = nc;
    }
    c->dig[c->ndig].pba = pba;
    c->dig[c->ndig].csize = csize;
    c->dig[c->ndig].crc = crc;
    c->ndig++;
    return 0;
}

static int spn_dig_cmp(const void *a, const void *b)
{
    uint64_t x = ((const spn_dig *)a)->pba, y = ((const spn_dig *)b)->pba;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* The walk: for every live inode of the captured generation, mark the blocks
 * its recipe names and record each segment's identity. */
static int spn_walk_ino(invfs_volume *v, uint64_t inode_id,
                        const invfs_v3_inode *in, void *ctx_)
{
    spn_walk_ctx *c = (spn_walk_ctx *)ctx_;
    uint8_t *blob = NULL;
    size_t blen = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;

    c->n_inodes++;
    if (memcmp(in->recipe_addr, "\0\0\0\0\0\0\0\0",
               INVFS_V3_RECIPE_ADDR_LEN) == 0)
        return 0;
    if (vol_v3_recipe_load(v, in->recipe_addr, &blob, &blen) != 0 || !blob) {
        c->unreadable++;
        return 0;               /* keep walking; the caller reports it */
    }
    c->recipes++;
    /* Same predicate as the restore-side check above, and for the same
     * reason: a symlink's blob is a raw object, not an AST. It is
     * harmless HERE only by accident -- the parse fails, so the loop below
     * is skipped and nothing is marked. Accidental is not a property:
     * invfs_ast_hdr_parse accepts any target whose first four bytes are a
     * v1/v2 AST version word, so a crafted target parses and every
     * attacker-controlled block entry would be marked into the pin (held
     * for a generation by spn_reclaim) and recorded in the digest the
     * restore cross-checks (a spurious SPT0_RC_DAMAGED on top of the one
     * this predicate fixes). Stated explicitly, the two ends agree. */
    if (invfs_inode_content_is_raw_blob(in->type)) {
        free(blob);
        return 0;
    }
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 && ents) {
        for (i = 0; i < n_ents; i++) {
            uint64_t pba = ents[i].pba, plen = 0, b;
            if (!pba)
                continue;
            /* a WINDOW_SRC entry's pba field is the SOURCE INODE ID, not a
             * block address: pinning it would pin an unrelated block and
             * leak it for a generation. The source's own recipe pins its
             * data (the window resolves through it). */
            if (ents[i].algo == INVFS_ALGO_WINDOW_SRC)
                continue;
            if (pba >= v->sb.total_blocks)
                continue;
            if (seg_extent_checked(v, pba, &plen) != 0 || plen == 0 ||
                pba + plen > v->sb.total_blocks) {
                /* the header is already unusable: pin the head block so the
                 * free path cannot hand it out, and record the damage so
                 * the restore-time check refuses instead of "restoring" it */
                if (!bit_get(c->map, pba)) {
                    bit_set(c->map, pba);
                    c->marked++;
                }
                c->broken++;
                if (spn_dig_push(c, pba, 0, 0) != 0)
                    return -1;
                continue;
            }
            {   /* the identity the restore re-checks: [4B csize][4B crc] */
                uint8_t hdr[8];
                uint32_t csize = 0, crc = 0;
                if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE,
                             hdr, 8) == 0) {
                    memcpy(&csize, hdr, 4);
                    memcpy(&crc, hdr + 4, 4);
                }
                if (spn_dig_push(c, pba, csize, crc) != 0)
                    return -1;
            }
            for (b = pba; b < pba + plen; b++)
                if (!bit_get(c->map, b)) {
                    bit_set(c->map, b);
                    c->marked++;
                }
        }
    }
    free(blob);
    return 0;
}

/* ---- the reclaim pass ------------------------------------------------ */

/* Is block `b` inside an extent the v3 batch registry still claims? The
 * extents are sorted by head pba and, being distinct allocations, do not
 * overlap -- so the only candidate is the last one that starts at or before
 * `b`. Called once per block of the volume, hence the binary search. */
static int spn_reg_owns(const tz_v3_extent *reg, size_t n, uint64_t b)
{
    size_t lo = 0, hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (reg[mid].pba <= b) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return 0;
    return b < reg[lo - 1].pba + reg[lo - 1].phys;
}

/* Free every block the PREVIOUS generation's pin named that no live recipe
 * names now. That is exactly the set the previous sweep wanted to free and
 * had to hold: a recipe it replaced is not in this generation's mark set, and
 * a recipe that survived is. A block nothing names and no pin named (an
 * orphan from some other path) is NOT touched -- this pass only discharges
 * the pin's own debt, so it can never race a write session, whose segments
 * were never in a pin.
 *
 * Runs before the new pin is armed, so the new set is the only live claim;
 * retain_release additionally bypasses the retention hook for the pin's own
 * deliberate frees (the v2 registry delete did the same). */
/* Free [pba, pba+n) and report how many blocks ACTUALLY changed from
 * allocated to free. The reclaim's whole value is its log line, and a counter
 * that counts the run length instead of the freed blocks is what let the
 * loop bug below ship as "62 blocks reclaimed" while nothing was freed --
 * so the number the operator sees is the number the bitmap agrees with. */
static uint64_t spn_free_counted(invfs_volume *v, uint64_t pba, uint64_t n)
{
    uint64_t i, end = pba + n, was = 0;

    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    for (i = pba; i < end; i++)
        if (bit_get(v->bitmap, i))
            was++;
    vol_free_blocks(v, pba, n);
    return was;
}

static uint64_t spn_reclaim(invfs_volume *v, const uint8_t *old_map,
                            const uint8_t *new_map)
{
    uint64_t total = v->sb.total_blocks;
    uint64_t b = 0, freed = 0, run = 0, start = 0;
    tz_v3_extent *reg = NULL;
    size_t n_reg = 0;

    if (!old_map)
        return 0;
    /* A RECIPE is not the only owner of a pinned block. The v3 batch
     * registry (the hidden TZ_OWNER_NAME file) owns its batches' extents
     * independently, and a batch that no live recipe names any more is dead
     * while its registry row is still on disk -- the row is retired later, by
     * tz_v3_gc at sweep stage 6. Freeing such a block HERE, one stage before
     * the registry admits the batch is garbage, hands the block straight back
     * to the shared free pool, where the transform's very next mbuf_alloc can
     * take it as a base B+-tree page (one pool, no hard regions -- §2.3).
     * tz_v3_gc would then free that live page through the row it never
     * dropped, and the fold after it would rebuild the base from the
     * pre-transform root: a file written seconds earlier becomes unreadable.
     *
     * So the registry is an owner set here, exactly as a live recipe is. The
     * cost is one registry read per capture (a name lookup plus a small
     * blob), and the debt is not deferred forever: tz_v3_gc frees the dead
     * batch in the same sweep, after the row is gone, so the block is
     * reclaimed one stage later rather than leaked.
     *
     * An unreadable registry (out of memory) is NOT treated as "no owner":
     * that is the one answer that can lose data, so it fails closed and the
     * reclaim does nothing. */
    if (tz_v3_reg_owned_blocks(v, &reg, &n_reg) != 0)
        return 0;
    /* One pass, one maximal run of reclaimable blocks freed as it completes.
     *
     * WP96 FIX: the first version of this loop ran its inner scan to the end
     * of the volume before freeing anything (`continue` instead of breaking
     * out of the run), so it issued exactly ONE free, for [total-run, run) --
     * a range at the tail of the volume that has nothing to do with the debt.
     * Every block the previous pin held therefore stayed allocated forever,
     * while the function still returned a plausible "N blocks reclaimed" and
     * the sweep log said so. Measured: a bare sweep on a 120 KB corpus grew
     * the volume by ~32 blocks EVERY sweep, unbounded, with every file
     * bit-exact -- the exact "guard without a working reclaim" leak, hidden
     * by a counter that counted the run length instead of the blocks freed.
     * The counter below is the honest one: it counts what was handed to the
     * free path, and the caller reports it as reclaimed. */
    v->retain_release = 1;
    for (b = 0; b < total; b++) {
        if (bit_get(old_map, b) && !bit_get(new_map, b) &&
            bit_get(v->bitmap, b) && !spn_reg_owns(reg, n_reg, b)) {
            if (!run) start = b;
            run++;
            continue;
        }
        if (run) {
            freed += spn_free_counted(v, start, run);
            run = 0;
        }
    }
    if (run)
        freed += spn_free_counted(v, start, run);
    v->retain_release = 0;
    free(reg);
    return freed;
}

/* ---- the capture-time pin ------------------------------------------- */

/* Walk the generation, take the mark set, reclaim what the previous pin held,
 * persist the new set and arm it. 0 = pinned, -1 = the pin could not be
 * taken (the save point itself is still valid; see spt0_capture). */
static int spt0_pin_take(invfs_volume *v, uint64_t root_pba,
                         uint64_t delta_end, uint64_t delta_segs,
                         uint64_t delta_head_pba)
{
    invfs_spn0 s;
    uint8_t *map = NULL, *old_map = NULL;
    spn_walk_ctx c;
    uint64_t old_pba = 0, old_blocks = 0, npinned = 0, need, dig_bytes = 0;

    memset(&s, 0, sizeof s);
    memcpy(s.magic, "SPN0", 4);
    s.version = INVFS_SPN0_VERSION;
    s.delta_segs = delta_segs;
    s.delta_head_pba = v->spn_delta_head;

    /* the previous generation's set: the reclaim worklist AND (if it is
     * still armed on disk) the set whose blocks are being held right now */
    {
        invfs_spn0 prev;
        if (spn_desc_load(v, &prev) == 0 && prev.pba && prev.blocks) {
            old_pba = prev.pba;
            old_blocks = prev.blocks;
        }
    }
    spn_map_free(v);
    v->spn_pba = 0;
    v->spn_blocks = 0;
    v->spn_npinned = 0;
    v->spn_armed = 0;
    /* WP137: every capture restarts the debt ledger. A capture with no
     * previous window has discharged nothing, and that zero -- not a stale
     * count from an earlier pass -- is what the ladder is told. */
    v->spn_reclaim_freed = 0;

    memset(&c, 0, sizeof c);
    c.v = v;
    if (v->spn_nopin) {
        /* the debug kill switch: no mark set, so the sweep frees exactly as
         * it always did and only the restore-time proof stands between a
         * rollback and a corrupt volume. The digests are still recorded --
         * that is what makes the proof independent of the hold -- but the
         * reclaim worklist is dropped with the mark set. */
        map = (uint8_t *)calloc(1, spn_map_bytes(v));
        if (map)
            c.map = map;
    } else {
        map = (uint8_t *)calloc(1, spn_map_bytes(v));
        if (!map)
            return -1;
        c.map = map;
    }
    if (vol_v3_iter_inodes_at(v, root_pba, delta_end, delta_segs,
                              delta_head_pba, spn_walk_ino, &c) != 0) {
        free(map);
        free(c.dig);
        return -1;
    }
    npinned = c.marked;
    if (c.unreadable)
        fprintf(stderr, "[spt0] save point: %llu of %llu recipes in the "
                "captured generation could not be read; their blocks cannot "
                "be pinned, and the restore-time data check will refuse a "
                "rollback onto them\n",
                (unsigned long long)c.unreadable,
                (unsigned long long)(c.recipes + c.unreadable));

    /* reclaim the previous generation's debt BEFORE arming the new set */
    if (old_pba && old_blocks && map) {
        old_map = spn_old_map_load(v, old_pba);
        if (old_map) {
            uint64_t freed = spn_reclaim(v, old_map, map);
            /* WP137: publish the discharge. This is the ONLY place debt is
             * ever collected, so it is also the only place the FUSE ladder
             * can learn that the fill it is reading high is (or is no
             * longer) a hold rather than live data. */
            v->spn_reclaim_freed = freed;
            if (freed)
                fprintf(stderr, "[spt0] reclaim: %llu blocks the previous "
                        "save point held are no longer referenced by any live "
                        "recipe\n", (unsigned long long)freed);
        } else {
            fprintf(stderr, "[spt0] the previous pin's block set is "
                    "unreadable; nothing reclaimed this run\n");
        }
    }

    /* [header][digests][mark bitmap] in one run */
    if (c.ndig > 1)
        qsort(c.dig, c.ndig, sizeof *c.dig, spn_dig_cmp);
    dig_bytes = (uint64_t)c.ndig * sizeof(spn_dig);
    need = (sizeof(spn_file_hdr) + dig_bytes + spn_map_bytes(v) +
            INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;

    /* structure before reference: the new run is durable and named before
     * the old one is released, so a crash in between leaks one run (fsck
     * reclaims it) and never leaves the descriptor naming freed blocks */
    s.pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                         need, 1, INVFS_ALLOC_META);
    if (!s.pba) {
        fprintf(stderr, "[spt0] save point: no room for the %llu-block data "
                        "pin; the sweep will free replaced segments and the "
                        "restore-time data check becomes the only guard\n",
                (unsigned long long)need);
        free(map);
        free(c.dig);
        s.pba = 0;
        s.blocks = 0;
        s.npinned = 0;
        s.flags = 0;
        (void)spn_desc_store(v, &s);
        return -1;
    }
    {
        spn_file_hdr fh;
        int wrc = 0;

        memset(&fh, 0, sizeof fh);
        memcpy(fh.magic, "SPNF", 4);
        fh.version = 1;
        fh.ndig = (uint32_t)c.ndig;
        fh.bitmap_bytes = (uint64_t)spn_map_bytes(v);
        fh.npinned = npinned;
        fh.broken = c.broken;
        fh.crc32c = 0;
        fh.crc32c = invfs_crc32c(&fh, sizeof fh);
        if (spn_run_write(v, s.pba, 0, (const uint8_t *)&fh,
                          sizeof fh) != 0)
            wrc = -1;
        if (!wrc && c.ndig)
            wrc = spn_run_write(v, s.pba, sizeof(spn_file_hdr),
                                (const uint8_t *)c.dig, (size_t)dig_bytes);
        if (!wrc && map)
            wrc = spn_run_write(v, s.pba, spn_map_off((uint32_t)c.ndig), map,
                                spn_map_bytes(v));
        if (wrc != 0) {
            vol_free_blocks(v, s.pba, need);
            free(map);
            free(c.dig);
            return -1;
        }
    }
    s.blocks = need;
    s.npinned = npinned;
    s.flags = v->spn_nopin ? 0 : INVFS_SPN0_F_ARMED;
    free(map);
    free(c.dig);
    if (vmux_barrier(v, "spt0 pin") < 0) {
        vol_free_blocks(v, s.pba, need);
        free(old_map);
        return -1;
    }
    if (spn_desc_store(v, &s) != 0) {
        vol_free_blocks(v, s.pba, need);
        free(old_map);
        return -1;
    }
    if (vmux_barrier(v, "spt0 pin descriptor") < 0) {
        free(old_map);
        return -1;
    }
    /* the old run is released only now that nothing names it */
    if (old_pba && old_blocks)
        vol_free_blocks(v, old_pba, old_blocks);
    free(old_map);

    v->spn_bitmap = NULL;
    v->spn_pba = s.pba;
    v->spn_blocks = s.blocks;
    v->spn_npinned = s.npinned;
    v->spn_armed = s.flags & INVFS_SPN0_F_ARMED;
    if (v->spn_armed && spn_map_load(v) != 0) {
        fprintf(stderr, "[spt0] save point: the data pin is on disk but "
                "unreadable in memory; the hold is inactive for this "
                "process\n");
        v->spn_bitmap = NULL;
    }
    fprintf(stderr, "[spt0] save point: %s%llu blocks (%llu segments, "
            "%llu already damaged) in %llu blocks of mark set at pba %llu\n",
            v->spn_armed ? "pinned " : "pin DISABLED, would have pinned ",
            (unsigned long long)npinned, (unsigned long long)c.ndig,
            (unsigned long long)c.broken, (unsigned long long)need,
            (unsigned long long)s.pba);
    return 0;
}

int spt0_capture(invfs_volume *v)
{
    invfs_blkptr root;
    int rc = 0;

    if (!v)
        return -1;
    if (!(v->sb.vol_flags & VOLF_V3))
        return 1;
    if (v->savepoint_live)
        return 1;

    /* Reclaim reader epoch. Announced BEFORE the capture, released after
     * spt0_pin_take -- the capture is not a point read: spt0_tree_ok and
     * spt0_pin_take between them walk the whole generation this root names,
     * so an announce released at the capture would protect nothing.
     *
     * In FUSE this cannot actually stall a drain: every trigger that arms a
     * save point (the watermark pass, USR1, the xattr, the offline sweep)
     * runs the capture under g_io_lock, which is the same lock the fold's
     * drain runs under, so the two are mutually exclusive. It is announced
     * anyway because that mutual exclusion is a property of the callers and
     * not of this function, and a capture that outlived its lock would
     * otherwise free nothing and read pages that are gone. */
    (void)vol_reclaim_reader_snapshot();

    if (vol_v3_base_root(v, &root) != 0) {
        rc = -1;
        goto out;
    }

    /* WP86: refuse to pin a damaged base. */
    if (!spt0_tree_ok(v, root.pba, NULL, 0)) {
        rc = SPT0_RC_DAMAGED;
        goto out;
    }

    memset(&v->spt0, 0, sizeof v->spt0);
    memcpy(v->spt0.magic, "SPT0", 4);
    v->spt0.version = INVFS_SPT0_VERSION;
    v->spt0.flags = 0;
    v->spt0.base_root = root.pba;
    v->spt0.delta_end = 0;

    {
        uint64_t delta_bytes = 0;
        uint64_t cur = v->delta_seg_pba;
        size_t nsegs = 0;
        invfs_delta_seg_hdr hdr;

        while (cur && nsegs < DELTA_MAX_SEGMENTS) {
            nsegs++;
            if (delta_read_hdr(v, cur, &hdr) != 0)
                break;
            if (nsegs > 1 || v->delta_bump > INVFS_DELTA_SEG_HDR_LEN) {
                delta_bytes += (nsegs == 1) ? v->delta_bump : INVFS_DELTA_SEG_BYTES;
            }
            if (hdr.prev_pba == cur)
                break;
            cur = hdr.prev_pba;
        }
        v->spt0.delta_end = delta_bytes;
        /* WP96: the same walk also yields the geometry spt0_data_ok needs to
         * tell "a full older segment" from "the captured head's used
         * prefix" (and the head pba it re-checks for reachability). delta_bytes
         * is the authority on whether a segment counted at all: `cur` is 0
         * again after a one-segment chain, whose prev_pba is 0. */
        v->spn_delta_segs = delta_bytes ? nsegs : 0;
        v->spn_delta_head = v->spn_delta_segs ? v->delta_seg_pba : 0;
    }

    /* WP96: take the data pin for this generation BEFORE any sweep free can
     * run. Failure here is not fatal: the save point is still recorded, and
     * the restore-time data check (layer 2) refuses a rollback whose data
     * went missing. Losing the pin degrades rollback, never correctness. */
    (void)spt0_pin_take(v, root.pba, v->spt0.delta_end, v->spn_delta_segs,
                        v->spn_delta_head);

    /* `root` is the blkptr vol_v3_base_root() just read and verified, so it
     * is copied whole rather than rebuilt field by field: pinned_root is
     * walked through mbuf_read_ptr like any other root, and a hand-assembled
     * one with checksum = 0 / gen = 0 fails that check (spt0_pinned_from_pba).
     * No extra I/O: the page is already in hand. */
    v->pinned_root = root;
    v->pinned_root.flags |= INVFS_BP_PINNED;
    v->savepoint_live = 1;

    if (spt0_store(v) != 0) {
        rc = -1;
        goto out;
    }
    if (vmux_barrier(v, "spt0 capture") < 0)
        rc = -1;

out:
    vol_reclaim_reader_release();
    return rc;
}

/* ===================================================================
 * WP96 layer 2: the restore-time DATA check.
 *
 * Layer 1 (the pin) is a bet that no free path can take a pinned block. This
 * is the verification that the bet came true: before the pinned root is
 * published, walk the pinned generation's recipes and read every segment they
 * name. A segment whose framed CRC no longer matches, whose header is out of
 * bounds, or whose blocks are no longer allocated is a block that was reused
 * -- i.e. exactly the failure the P0 describes, where invf-rollback returns
 * 0, invf-fsck says OK, and the first read of the file fails.
 *
 * The check is deliberately independent of the pin: a volume whose pin was
 * never taken (INVFS_SPT0_NOPIN=1, a capture that could not allocate the
 * mark set, a save point written by a pre-WP96 build) still gets it. It
 * reads the segments in bounded chunks (never a whole segment in RAM) and
 * stops at the first failure, reporting which inode and block.
 * =================================================================== */

/* Verify one framed segment ([4B csize LE][4B crc32c(payload)] at pba)
 * against the identity it had at capture, in bounded memory. A reused block
 * is itself a well-formed segment, so its own CRC proves nothing: what
 * matters is that (csize, crc) still equal what the capture recorded, that
 * the extent is still allocated, and that the payload still matches. */
static int spt0_seg_verify(invfs_volume *v, uint64_t pba, const spn_dig *want,
                           char *err, size_t errlen)
{
    uint8_t hdr[8], buf[64 * 1024];
    uint32_t csize, crc_hdr, crc = 0;
    uint64_t off = 0, b, nblk;

    if (pba == 0 || pba >= v->sb.total_blocks) {
        if (err) snprintf(err, errlen, "segment pba %llu is out of bounds",
                          (unsigned long long)pba);
        return -1;
    }
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, hdr, 8) != 0) {
        if (err) snprintf(err, errlen, "segment %llu is unreadable",
                          (unsigned long long)pba);
        return -1;
    }
    memcpy(&csize, hdr, 4);
    memcpy(&crc_hdr, hdr + 4, 4);
    if (csize != want->csize || crc_hdr != want->crc) {
        if (err) snprintf(err, errlen,
                          "segment %llu now holds [csize %u, crc %08x], the "
                          "save point recorded [csize %u, crc %08x]: reused",
                          (unsigned long long)pba,
                          csize, crc_hdr, want->csize, want->crc);
        return -1;
    }
    if (want->csize == 0) {
        if (err) snprintf(err, errlen,
                          "segment %llu was already unreadable when the save "
                          "point was captured", (unsigned long long)pba);
        return -1;
    }
    /* every block of the extent must still be allocated: a freed run reads
     * as zeroes, and a reallocated one is somebody else's data */
    nblk = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    if (pba + nblk > v->sb.total_blocks) {
        if (err) snprintf(err, errlen,
                          "segment %llu runs past the end of the volume",
                          (unsigned long long)pba);
        return -1;
    }
    for (b = pba; b < pba + nblk; b++)
        if (!bit_get(v->bitmap, b)) {
            if (err) snprintf(err, errlen,
                              "segment %llu spans block %llu, which is no "
                              "longer allocated", (unsigned long long)pba,
                              (unsigned long long)b);
            return -1;
        }
    while (off < csize) {
        size_t chunk = (size_t)(csize - off);
        if (chunk > sizeof buf)
            chunk = sizeof buf;
        if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE + 8 + off,
                     buf, chunk) != 0) {
            if (err) snprintf(err, errlen, "segment %llu payload is "
                              "unreadable", (unsigned long long)pba);
            return -1;
        }
        crc = invfs_crc32c_update(crc, buf, chunk);
        off += chunk;
    }
    if (crc != want->crc) {
        if (err) snprintf(err, errlen, "segment %llu fails its CRC (the "
                          "block now holds other data)",
                          (unsigned long long)pba);
        return -1;
    }
    return 0;
}

/* the capture-time identity of `pba`, or NULL when the pin never recorded
 * one (a pre-WP96 save point, or a recipe the walk could not read) */
static const spn_dig *spn_dig_find(const spn_dig *d, size_t n, uint64_t pba)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d[mid].pba < pba)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < n && d[lo].pba == pba)
        return &d[lo];
    return NULL;
}

typedef struct {
    invfs_volume *v;
    const spn_dig *dig;      /* capture-time identities, sorted by pba */
    size_t ndig;
    int have_dig;            /* 0 = no pin file: identity checks impossible */
    char err[192];
    uint64_t inodes;
    uint64_t segments;
} spt0_data_ctx;

/* the per-segment reason buffer must leave room for the inode/segment prefix
 * the caller prepends, so keep it well under c->err */
#define SPT0_SEG_ERR 128

static int spt0_data_ino(invfs_volume *v, uint64_t inode_id,
                         const invfs_v3_inode *in, void *ctx_)
{
    spt0_data_ctx *c = (spt0_data_ctx *)ctx_;
    uint8_t *blob = NULL;
    size_t blen = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;

    if (memcmp(in->recipe_addr, "\0\0\0\0\0\0\0\0",
               INVFS_V3_RECIPE_ADDR_LEN) == 0)
        return 0;
    if (vol_v3_recipe_load(v, in->recipe_addr, &blob, &blen) != 0 || !blob) {
        snprintf(c->err, sizeof c->err,
                 "inode %llu's pinned recipe is unreadable",
                 (unsigned long long)inode_id);
        return -1;
    }
    c->inodes++;
    /* The blob LOADED, which is the whole obligation for a raw-blob type.
     * A symlink's content is its target string, stored content-addressed
     * exactly like a recipe (vol_v3_create_node), so recipe_addr is
     * non-zero and this row walked straight into vol_ast_recipe_parse --
     * which cannot parse "usr/lib", refused the rollback, and named damage
     * that does not exist. invf_inode_content_is_raw_blob is the same
     * predicate the read path dispatches on and the fsck recipe audit is
     * routed through: skip the PARSE, never the LOAD (a symlink whose blob
     * really is missing is still caught by the load above). */
    if (invfs_inode_content_is_raw_blob(in->type)) {
        free(blob);
        return 0;
    }
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 || !ents) {
        snprintf(c->err, sizeof c->err,
                 "inode %llu's pinned recipe does not parse",
                 (unsigned long long)inode_id);
        free(blob);
        return -1;
    }
    for (i = 0; i < n_ents; i++) {
        uint64_t pba = ents[i].pba;
        const spn_dig *want;
        char e[SPT0_SEG_ERR];
        if (ents[i].algo == INVFS_ALGO_WINDOW_SRC)
            continue;                    /* names the source inode, not a block */
        if (!pba)
            continue;
        if (!c->have_dig) {
            /* No identity table (a save point captured before WP96). The
             * header and allocation checks are all that is possible then; say
             * so rather than pretending the segment was proven. */
            snprintf(c->err, sizeof c->err,
                     "inode %llu segment %zu: this save point predates the "
                     "WP96 pin, so its segments can only be checked against "
                     "their own headers",
                     (unsigned long long)inode_id, i);
            free(blob);
            return 0;                     /* not a damage verdict: report once */
        }
        want = spn_dig_find(c->dig, c->ndig, pba);
        if (!want) {
            snprintf(c->err, sizeof c->err,
                     "inode %llu segment %zu names block %llu, which the "
                     "save point did not record -- the pinned generation is "
                     "not the one that was captured",
                     (unsigned long long)inode_id, i,
                     (unsigned long long)pba);
            free(blob);
            return -1;
        }
        if (spt0_seg_verify(v, pba, want, e, sizeof e) != 0) {
            snprintf(c->err, sizeof c->err, "inode %llu segment %zu: %s",
                     (unsigned long long)inode_id, i, e);
            free(blob);
            return -1;
        }
        c->segments++;
    }
    free(blob);
    return 0;
}

/* The pinned generation's log prefix must still be physically there: a fold
 * resets the delta chain and frees it, and the pinned base root does NOT
 * contain the rows that only ever lived in the log. Rolling back onto that
 * combination is not a rollback, it is a silent file-loss -- so a chain that
 * no longer reaches the captured head is a refusal. 1 = intact, 0 = gone,
 * -1 = no geometry recorded (pre-WP96 save point: skip, the tree + recipe
 * checks still run). */
static int spt0_delta_intact(invfs_volume *v, uint64_t delta_segs,
                             uint64_t head_pba)
{
    uint64_t cur = v->delta_seg_pba, hop = 0;

    if (delta_segs == 0)
        return 1;                        /* the generation had no log tier */
    if (!head_pba)
        return 0;
    while (hop < delta_segs) {
        invfs_delta_seg_hdr h;
        if (!cur || delta_read_hdr(v, cur, &h) != 0)
            return 0;                    /* chain broke before the head */
        if (cur == head_pba)
            return 1;
        if (h.prev_pba == cur)
            return 0;
        cur = h.prev_pba;
        hop++;
    }
    return 0;                            /* the head is no longer in the chain */
}

/* 1 = the pinned generation's data is intact, 0 = it is not (err says why). */
static int spt0_data_ok(invfs_volume *v, char *err, size_t errlen)
{
    spt0_data_ctx c;
    uint64_t delta_segs = v->spn_delta_segs, head = v->spn_delta_head;
    spn_dig *dig = NULL;
    size_t ndig = 0;

    if (!spt0_delta_intact(v, delta_segs, head)) {
        snprintf(err, errlen,
                 "the delta log the save point captured is gone (a fold "
                 "reset it; the pinned rows are not in the pinned base "
                 "root) -- rolling back would lose every file created since "
                 "the last fold");
        return 0;
    }
    memset(&c, 0, sizeof c);
    c.v = v;
    if (spn_dig_load(v, &dig, &ndig) == 0) {
        c.dig = dig;
        c.ndig = ndig;
        c.have_dig = 1;
    } else if (v->spn_pba) {
        snprintf(err, errlen,
                 "the save point's pin file is unreadable, so its data cannot "
                 "be proven -- refusing rather than rolling back blind");
        return 0;
    }
    if (vol_v3_iter_inodes_at(v, v->spt0.base_root, v->spt0.delta_end,
                              delta_segs, head, spt0_data_ino, &c) != 0) {
        snprintf(err, errlen, "%s", c.err[0] ? c.err
                              : "the pinned generation could not be walked");
        free(dig);
        return 0;
    }
    free(dig);
    if (c.err[0])
        fprintf(stderr, "invf-spt0: note: %s\n", c.err);
    fprintf(stderr, "[spt0] data check: %llu inodes, %llu segments verified "
            "against the save point's recorded identity -- all intact\n",
            (unsigned long long)c.inodes, (unsigned long long)c.segments);
    return 1;
}

int spt0_restore(invfs_volume *v)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_page_hdr *h;
    uint64_t base_root;
    char derr[224];
    int rc;

    if (!v)
        return -1;
    if (!(v->sb.vol_flags & VOLF_V3))
        return 1;
    if (!v->savepoint_live)
        return 1;

    /* WP85: this is the point where the post-savepoint generation stops
     * being live. Every write session opened before this line is anchored
     * to metadata the restore just retired -- and to segments no live
     * recipe names, because SPT0 pins {base_root, delta_end} only and a
     * session's segments live in neither (WP27: they ride the session's
     * own entry table, never the journal). Bump the counter BEFORE the
     * first write so no session can slip an append through the window;
     * the sessions themselves are refused and retired by vol_write_*. */
    v->write_gen++;

    base_root = v->spt0.base_root;

    /* WP86: DETECT a damaged save point, do not use it. Publishing the pinned
     * root when its tree is torn would replace a live (if damaged) volume
     * with one whose every read fails, and would truncate the delta for a
     * state that was never reachable. */
    if (!spt0_tree_ok(v, base_root, NULL, 0)) {
        fprintf(stderr, "invf-spt0: save point base_root %llu is damaged -- "
                "refusing to roll back onto it; run invf-fsck -f to quarantine "
                "the unreadable pages first\n",
                (unsigned long long)base_root);
        return SPT0_RC_DAMAGED;
    }
    /* WP96: the same rule for the DATA that base root's recipes address. A
     * tree that walks proves nothing about the segments: the sweep that
     * re-encoded the file freed them, and the rollback would republish a
     * recipe over somebody else's bytes -- rc 0, fsck OK, every read fails. */
    if (!spt0_data_ok(v, derr, sizeof derr)) {
        fprintf(stderr, "invf-spt0: refusing to roll back: %s\n", derr);
        fprintf(stderr, "invf-spt0: the volume is untouched; the save point "
                "is still live (invf-sweep --realize drops it when you no "
                "longer need the window)\n");
        return SPT0_RC_DAMAGED;
    }
    /* The pinned generation is what the volume is about to BECOME, so the
     * hold has done its job: disarm before the first write below, or the
     * delta truncation's own segment frees would be refused. The mark set
     * stays on disk as the next capture's reclaim worklist. */
    v->spn_armed = 0;
    spn_map_free(v);

    if (mbuf_read(v, base_root, page) != 0)
        return -1;
    h = mbuf_page_hdr(page);
    if (!mbuf_page_validate(page))
        return SPT0_RC_DAMAGED;
    /* WP-D: refuse to roll back ONTO a block the volume has given back. A
     * freed page passes mbuf_page_validate (free does not scrub) and
     * passes the gen match, so without this the publish two lines below
     * would install a root whose pages belong to the free pool -- and the
     * next allocation would overwrite them under a live namespace.
     *
     * On a correct volume this never fires. The SPN0 hold armed at capture
     * time is exactly what stops vol_free_blocks clearing the pinned
     * blocks, and the restore only proceeds after spt0_data_ok() has
     * verified that pin; so reaching a clear bit here means the save point
     * is stale, and refusing it is the only answer that does not publish a
     * namespace over somebody else's bytes. Refusing a ROLLBACK is not
     * making a read fail: the volume is left exactly as it was, and the
     * caller reports SPT0_RC_DAMAGED, which is already this function's
     * answer for "the save point does not describe a usable base". */
    if (mbuf_page_allocated(v, base_root) == 0)
        return SPT0_RC_DAMAGED;

    if (mbuf_root_publish(v, base_root, h->gen) != 0)
        return -1;

    if (vol_delta_truncate(v, v->spt0.delta_end) != 0)
        return -1;

    vol_delta_close(v);
    if (vol_delta_mount(v) != 0)
        return -1;

    memset(&v->spt0, 0, sizeof v->spt0);
    v->savepoint_live = 0;
    v->spn_delta_segs = 0;
    v->spn_delta_head = 0;
    memset(&v->pinned_root, 0, sizeof v->pinned_root);

    rc = spt0_store(v);
    if (rc != 0)
        return rc;
    if (vmux_barrier(v, "spt0 restore") < 0)
        return -1;

    return 0;
}

int spt0_drop(invfs_volume *v)
{
    int was_live;

    if (!v)
        return -1;
    was_live = v->savepoint_live;

    v->savepoint_live = 0;
    memset(&v->pinned_root, 0, sizeof v->pinned_root);
    memset(&v->spt0, 0, sizeof v->spt0);
    /* WP96: dropping the window RELEASES the pin -- that is the whole point
     * of invf-sweep --realize, and a dropped save point that kept holding
     * blocks would be the space leak the pin is easy to grow into. The mark
     * set itself stays on disk (disarmed) as the next capture's reclaim
     * worklist: those blocks are now ordinary garbage that the next capture
     * frees if no live recipe names them. */
    v->spn_armed = 0;
    v->spn_delta_segs = 0;
    v->spn_delta_head = 0;
    spn_map_free(v);
    {
        invfs_spn0 sp;
        if (spn_desc_load(v, &sp) == 0 && (sp.flags & INVFS_SPN0_F_ARMED)) {
            sp.flags &= ~(uint32_t)INVFS_SPN0_F_ARMED;
            if (spn_desc_store(v, &sp) != 0)
                return -1;
            if (vmux_barrier(v, "spt0 pin release") < 0)
                return -1;
        }
    }

    if (spt0_store(v) != 0)
        return -1;
    if (vmux_barrier(v, "spt0 drop") < 0)
        return -1;

    return was_live ? 1 : 0;
}

/* WP96: is any block in [pba, pba+n) held by the live save point's pin?
 * vol_free_blocks calls this at the single choke point every free passes
 * through, so no publisher (textzone batches, the two WINDOW_SRC ones, the
 * RAW->shadow drain), no delete and no session retire can hand a pinned
 * block back to the allocator -- which is what used to make a rollback
 * republish a recipe over somebody else's bytes. The blocks stay ALLOCATED
 * in the real bitmap; the next capture's reclaim pass is what gives them
 * back, once no live recipe names them. */
int spt0_block_pinned(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    uint64_t i, end;

    if (!v || !v->spn_armed || !v->spn_bitmap || v->spn_nopin)
        return 0;
    if (pba >= v->sb.total_blocks)
        return 0;
    end = pba + nblocks;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    for (i = pba; i < end; i++)
        if (bit_get(v->spn_bitmap, i))
            return 1;
    return 0;
}

int spt0_info(const invfs_volume *v, invfs_spt0 *out)
{
    if (!v)
        return 0;
    if (out)
        *out = v->spt0;
    return v->savepoint_live;
}

/* WP137: the debt the last capture discharged. Read it only immediately
 * after a capture (a capture is the only writer, and it resets the count
 * before reclaiming, so the value never spans two passes). */
uint64_t spt0_reclaim_last(const invfs_volume *v)
{
    return v ? v->spn_reclaim_freed : 0;
}
