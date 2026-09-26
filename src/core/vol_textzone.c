/* vol_textzone.c — WP10 text-zone write path: accumulator, owner
 * inode, batch seal/commit/flush, text-zone GC (+ WP14a binary batches).
 * Split from volume.c. */

#include "volume_internal.h"

#define TZ_BATCH_MAX  (4ull << 20)    /* user-confirmed PPMd optimum (§3) */


/* one slice of one member file, inside one batch */
typedef struct {
    uint64_t batch_seq;
    uint64_t file_off;
    uint32_t batch_off;
    uint32_t len;
} tz_slice;


/* per-candidate build state while a flush runs */
typedef struct {
    tz_slice *slices;
    size_t n_slices, cap_slices;
    uint64_t file_size;      /* authoritative size when fed (fresh read) */
    uint64_t new_id;         /* allocated at commit (0 = not yet) */
    uint32_t algo;           /* batch algo its slices carry (PPMD / ZSTD /
                              * ZSTD_BCJ) -- WP14a */
    int bcj;                 /* member is x86-BCJ-prefiltered per slice */
    int complete;            /* all bytes of the candidate fed */
    int fallback;            /* its batch failed -> generic per-file path */
    int committed;           /* member record rewritten */
} tz_member;


/* a batch that made it to disk during this flush */
typedef struct {
    uint64_t seq, pba;
    uint32_t phys;
    uint32_t algo;           /* the batch payload codec tag (WP14a) */
} tz_sealed;


/* ---- accumulator (deferral side) ---- */

/* one accumulator is a (array, n, cap) triple on the volume: v->tz for
 * text candidates (WP10), v->bz for binary ones (WP14a). Same growth
 * and dedup rules for both. */
static int acc_defer(tz_candidate **arr, size_t *np, size_t *capp,
                     uint64_t inode_id, const char *name,
                     uint64_t size, uint32_t family)
{
    size_t i, nl = strlen(name);
    tz_candidate *nt;
    if (nl == 0 || nl > INVFS_MAX_NAME) return -1;
    for (i = 0; i < *np; i++)
        if ((*arr)[i].inode_id == inode_id) return 0;   /* already deferred */
    if (*np == *capp) {
        size_t nc = *capp ? *capp * 2 : 64;
        nt = (tz_candidate *)realloc(*arr, nc * sizeof(tz_candidate));
        if (!nt) return -1;
        *arr = nt;
        *capp = nc;
    }
    nt = &(*arr)[(*np)++];
    nt->inode_id = inode_id;
    nt->size = size;
    nt->family = family;
    memcpy(nt->name, name, nl + 1);
    return 0;
}

int tz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family)
{
    return acc_defer(&v->tz, &v->tz_n, &v->tz_cap, inode_id, name, size,
                     family);
}


/* WP14a: defer an executable (binary-family) file into the binary
 * accumulator; vol_tz_flush seals it into shared ZSTD(+BCJ) batches. */
int bz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family)
{
    return acc_defer(&v->bz, &v->bz_n, &v->bz_cap, inode_id, name, size,
                     family);
}


/* ---- owner inode ---- */

/* Load the owner record: entries, ext, position. Absent owner => *o zeroed
 * and o->pos = 0 (caller creates it). Returns 0 on success. */
int tz_owner_load(invfs_volume *v, uint64_t owner, tz_owner *o)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents;
    size_t base;
    size_t elen = 0;
    uint32_t i;

    memset(o, 0, sizeof *o);
    if (meta_read_record_by_id(v, owner, &buf, &rl, NULL, 0, &o->pos) != 0 ||
        !o->pos) {
        free(buf);
        return -1;
    }
    base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)buf) - buf);
    if (rl < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(buf + base, rl - base, &ah) != 0) {
        free(buf);
        return -1;
    }
    if (rl < base + ah.hdr_len +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        free(buf);
        return -1;
    }
    o->ctime = ((const invfs_inode_rec *)buf)->ctime;
    if (ah.num_blocks) {
        o->ents = (invfs_ast_block_entry *)malloc((size_t)ah.num_blocks *
                                                  sizeof(*o->ents));
        if (!o->ents) { free(buf); return -1; }
        ents = (const invfs_ast_block_entry *)(buf + base + ah.hdr_len);
        for (i = 0; i < ah.num_blocks; i++) {
            o->ents[i] = ents[i];
            /* seq assignment is strictly monotone even after GC removed
             * middle entries: max surviving block_id + 1 */
            if ((uint64_t)ents[i].block_id + 1 > o->next_seq)
                o->next_seq = (uint64_t)ents[i].block_id + 1;
        }
        o->n = ah.num_blocks;
        o->cap = ah.num_blocks;
    }
    {
        const uint8_t *ext = meta_locate_ext(buf, rl, &elen);
        if (ext && elen && elen <= 0xFFFF) {
            o->ext = (uint8_t *)malloc(elen);
            if (o->ext) { memcpy(o->ext, ext, elen); o->ext_len = (uint32_t)elen; }
        }
    }
    free(buf);
    return 0;
}

void tz_owner_free(tz_owner *o)
{
    free(o->ents);
    free(o->ext);
    memset(o, 0, sizeof *o);
}


/* Append [INOD(owner, entries)][DELT(position-kill previous)] as one write.
 * The owner keeps its inode id so the owner L2P maps stay valid. Entry
 * file_offsets are rebuilt as the cumulative concatenation of the given
 * entries (so the owner itself stays readable as the concatenation of its
 * live batches, and GC repacks the same way). The name is a parameter so
 * the WP20 seal owners ("\x01parity*") reuse the exact same pattern. */
int tz_owner_write(invfs_volume *v, uint64_t owner, const char *name,
                          tz_owner *o)
{
    size_t rec_len, total, off, nlen, tomb_len;
    uint8_t *combo;
    invfs_inode_rec *rh;
    uint8_t ah[INVFS_AST_HDR_V2_LEN];
    size_t ahlen;
    uint32_t crc_rec, crc_tomb;
    uint64_t run = 0, new_pos;
    uint32_t i;

    for (i = 0; i < o->n; i++) {
        o->ents[i].file_offset = run;
        run += o->ents[i].length;
    }
    /* WP22a: past 4 GB of live batches the owner record needs the v2
     * recipe header (u64 file_size / u32 num_blocks) -- the helper picks
     * it; past MAX_FILE_SIZE/2^24 entries nothing can hold the truth */
    if (run > MAX_FILE_SIZE || o->n > MAX_SEGMENTS_V2) return -1;
    ahlen = invfs_ast_hdr_write(ah, run, o->n, 0);
    if (!ahlen) return -1;

    nlen = strlen(name);
    if (nlen > INVFS_MAX_NAME) nlen = INVFS_MAX_NAME;
    tomb_len = INVFS_REC_HDR_LEN + nlen + 1;
    rec_len = INVFS_REC_HDR_LEN + nlen + 1 + ahlen +
              (size_t)o->n * sizeof(invfs_ast_block_entry) + o->ext_len;
    total = rec_len + 4 + (o->pos ? tomb_len + 4 : 0);
    /* WP52: on a mapper volume meta_get_append_pos sizes (or grows) the
     * active extent to hold the whole record+tombstone, so no legacy room
     * pre-check applies. The legacy contiguous area keeps the online churn
     * backstop, before the combo is built: the compaction moves every
     * record, so the position-kill target is re-derived after it. */
    if (!(v->met0_present && v->meta_mapper) &&
        v->inode_area_pos + total > v->inode_area_end) {
        if (inode_area_make_room(v, total) != 0)
            return -1;
        o->pos = idx_get_id(v, owner);
        total = rec_len + 4 + (o->pos ? tomb_len + 4 : 0);
    }
    combo = (uint8_t *)calloc(1, total);
    if (!combo) return -1;

    rh = (invfs_inode_rec *)combo;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_len;
    rh->inode_id = owner;
    rh->file_size = run;
    rh->ctime = o->ctime;
    rec_set_name(rh, name);
    {
        uint8_t *body = invfs_rec_body(rh);
        memcpy(body, ah, ahlen);
        if (o->n)
            memcpy(body + ahlen, o->ents,
                   (size_t)o->n * sizeof(invfs_ast_block_entry));
    }
    if (o->ext_len)
        memcpy(combo + rec_len - o->ext_len, o->ext, o->ext_len);
    crc_rec = invfs_crc32c(combo, rec_len);
    memcpy(combo + rec_len, &crc_rec, 4);

    off = rec_len + 4;
    if (o->pos) {
        invfs_inode_rec *th = (invfs_inode_rec *)(combo + off);
        th->magic = TOMBSTONE_MAGIC;
        v->hot.tombstones++;
        th->rec_len = (uint32_t)tomb_len;
        th->inode_id = owner;
        th->file_size = o->pos;         /* v2 position kill */
        rec_set_name(th, name);
        crc_tomb = invfs_crc32c((uint8_t *)th, tomb_len);
        memcpy(combo + off + tomb_len, &crc_tomb, 4);
    }

    if (vol_mark_dirty(v) != 0) { free(combo); return -1; }
    /* WP52: one extent-sized append for the whole record+tombstone. On a
     * mapper volume vol_append_slot refuses a slot that would run past its
     * extent (sizing inside meta_get_append_pos); on a legacy volume it is
     * the contiguous cursor, bumped below. */
    {
        int arc = vol_append_owner_slot(v, total, &o->ext_idx, &new_pos);
        if (arc != 0) { free(combo); return -1; }
    }
    /* WP52: an in-place rewrite (dedicated extent reused, new_pos == the
     * previous position) has nothing to position-kill; a self-tombstone
     * would kill the record just written. Write only the record+CRC. */
    {
        size_t wlen = (o->pos && new_pos == o->pos) ? rec_len + 4 : total;
        if (io_seek(&v->io, new_pos) != 0 ||
            io_write(&v->io, combo, wlen) != 0) { free(combo); return -1; }
    }
    /* on a mapper volume the owner lives in its own extent and the shared
     * file-record cursor must NOT be dragged to it */
    if (!(v->met0_present && v->meta_mapper))
        v->inode_area_pos = new_pos + total;
    free(combo);
    idx_put(v, name, strlen(name), owner, new_pos, run, o->ctime);
    idx_put_id(v, owner, new_pos);
    o->pos = new_pos;
    return 0;
}


/* Find the batch owner, creating it lazily (empty record) on first use. */
static uint64_t tz_owner_id(invfs_volume *v)
{
    uint64_t id = vol_find(v, TZ_OWNER_NAME);
    if (id) return id;
    return vol_create_file(v, TZ_OWNER_NAME, NULL, 0);
}


/* ---- small policy helpers used by the walk predicate ---- */

/* Cheap head sniff across the registry (UNCOMPRESSIBLE retry gate): one
 * 8 KB window, registry order (= sniff priority), any positive answer
 * qualifies. */
int tz_sniff_any(invfs_volume *v, uint64_t inode_id, const char *name)
{
    uint8_t head[8192];
    int got = vol_read_range(v, inode_id, 0, sizeof head, head);
    size_t n = 0, i;
    const invfs_codec *all;
    if (got <= 0) return 0;
    all = invfs_codec_all(&n);
    for (i = 0; i < n; i++)
        if (all[i].sniff && all[i].sniff(head, (size_t)got, name) > 0)
            return 1;
    return 0;
}


/* Read the usize of a TEXT member's batch segment without decoding it
 * (WP10 §6: "store decoded unit sizes to speed this up" -- the [4B usize]
 * sits right behind the 8-byte framing). 1 = batch exceeds unit_limit. */
int tz_member_oversized(invfs_volume *v, uint64_t inode_id,
                               uint64_t unit_limit)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents;
    size_t base;
    uint32_t i;
    int over = 0;

    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;   /* unreadable: not GC/policy business here */
    base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)buf) - buf);
    if (rl < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(buf + base, rl - base, &ah) != 0) {
        free(buf);
        return 0;
    }
    if (rl < base + ah.hdr_len +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        free(buf);
        return 0;
    }
    ents = (const invfs_ast_block_entry *)(buf + base + ah.hdr_len);
    for (i = 0; i < ah.num_blocks && !over; i++) {
        uint64_t pba = 0;
        uint8_t hb[12];
        uint32_t usize;
        if (ents[i].zone != INVFS_ZONE_TEXT) continue;
        /* WP27: the member's entry carries the batch's pba */
        pba = ents[i].pba;
        if (!pba || pba >= v->sb.total_blocks) continue;
        if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, hb, sizeof hb) != 0) continue;
        memcpy(&usize, hb + 8, 4);
        if (usize > unit_limit) over = 1;
    }
    free(buf);
    return over;
}


/* Decode and re-store through the generic per-segment path, then stamp
 * cls{calgo}. Two callers:
 *  - policy downgrade (WP10 §6, both directions): stamps GENERIC_MEMLIMIT
 *    {codec} -- NOT bare GENERIC, because the limit is the reason, and
 *    MEMLIMIT is what makes the re-sweep after a RAISED limit find the
 *    file again (§2 table);
 *  - WP19 heat promotion: a read-hot PPMd batch member is extracted to
 *    standalone per-segment ZSTD and stamped GENERIC{ZSTD} (its terminal
 *    form -- a latency upgrade out of the shared batch, not a policy
 *    retry candidate).
 * The old record's blocks retire as usual; TEXT members' batch blocks
 * survive via the retire gate and their L2P dups vanish with the old
 * record (the batch keeps a hole -- GC/compactor territory). */
int vol_store_generic(invfs_volume *v, uint64_t inode_id,
                             const char *name, uint8_t cls, uint8_t calgo)
{
    uint8_t *data = NULL;
    size_t len = 0;
    invfs_meta_pub keep;
    int have_keep;
    uint64_t nid;

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    if (vol_read_file(v, inode_id, &data, &len) != 0)
        return -1;
    nid = vol_replace_file(v, name, data, len);   /* fresh RAW record */
    free(data);
    if (!nid) return -1;
    if (have_keep) vol_apply_meta(v, name, &keep);
    /* generic_only: a downgraded JPEG must not loop straight back into JXL */
    if (vol_sweep_file_inner(v, nid, 1) < 0) return -1;
    /* the inner sweep re-ids the file (append-only store): stamp the LIVE
     * record, never the intermediate -- stamping the deleted id would
     * resurrect it and hijack the name */
    {
        uint64_t live = vol_find(v, name);
        vol_stamp_class(v, live ? live : nid, cls,
                        calgo, tz_codec_gen(calgo));
    }
    return 0;
}


/* ---- flush: seal batches, commit members ---- */

typedef struct {
    invfs_volume *v;
    tz_owner owner;
    uint64_t owner_id;
    uint64_t next_seq;
    /* the batch currently being filled */
    uint8_t *bbuf;
    size_t blen, bcap;          /* bcap = batch target */
    uint64_t open_seq;
    /* WP14a: 0 = text flush (PPMd), 1 = binary flush (ZSTD[+BCJ]).
     * open_algo/open_bcj describe the batch currently being filled:
     * the first member of a batch sets the tone, and a member of the
     * other BCJ kind seals the open batch first (binary batches are
     * prefilter-homogeneous -- the (family,size) sort makes BCJ families
     * adjacent, so this split only ever fires at family boundaries). */
    int binary;
    uint32_t open_algo;         /* PPMD / ZSTD / ZSTD_BCJ */
    int open_bcj;
    /* sealed batches of this flush (for member-commit pba lookup) */
    tz_sealed *sealed;
    size_t n_sealed, cap_sealed;
    int sealed_any;
    int skip_commit;            /* INVFS_TZ_SKIP_COMMIT fault-injection hook */
} tz_ctx;


static int tz_slice_push(tz_member *m, uint64_t seq, uint64_t file_off,
                         uint32_t batch_off, uint32_t len)
{
    if (m->n_slices == m->cap_slices) {
        size_t nc = m->cap_slices ? m->cap_slices * 2 : 4;
        tz_slice *ns = (tz_slice *)realloc(m->slices, nc * sizeof(tz_slice));
        if (!ns) return -1;
        m->slices = ns;
        m->cap_slices = nc;
    }
    m->slices[m->n_slices].batch_seq = seq;
    m->slices[m->n_slices].file_off = file_off;
    m->slices[m->n_slices].batch_off = batch_off;
    m->slices[m->n_slices].len = len;
    m->n_slices++;
    return 0;
}


static const tz_sealed *tz_sealed_find(const tz_ctx *c, uint64_t seq)
{
    size_t i;
    for (i = 0; i < c->n_sealed; i++)
        if (c->sealed[i].seq == seq) return &c->sealed[i];
    return NULL;
}


/* Rewrite one member's record under its pre-allocated new id (dup maps are
 * already written and flushed by tz_commit_ready) with TEXT-zone entries,
 * carrying the old record's ext (INO2 + xattrs) verbatim, then retire the
 * old id (its RAW blocks free normally -- the TEXT gate only covers batches)
 * and stamp the batching class (TEXT for PPMd batches, BATCHED_BIN for
 * WP14a binary batches). The entry algo comes from the sealed batch the
 * slice landed in: PPMD for text, ZSTD or ZSTD_BCJ for binary. */
static int tz_commit_member(tz_ctx *c, tz_member *m, const tz_candidate *cand)
{
    invfs_volume *v = c->v;
    uint8_t *old = NULL;
    uint32_t orl = 0;
    uint64_t old_pos = 0;
    char name[256];
    const uint8_t *ext;
    size_t ext_len = 0;
    uint64_t new_id, fsize, ctime;
    size_t rec_size, i, nlen, tomb_len;
    uint8_t *rec;
    invfs_inode_rec *rh;
    uint8_t ah[INVFS_AST_HDR_V2_LEN];
    size_t ahlen;
    invfs_ast_block_entry *ne;
    uint32_t crc;
    int rc = -1;

    if (meta_read_record_by_id(v, cand->inode_id, &old, &orl, name,
                               sizeof name, &old_pos) != 0)
        return -1;
    ext = meta_locate_ext(old, orl, &ext_len);   /* may be NULL (v1 record) */
    ctime = ((const invfs_inode_rec *)old)->ctime;
    fsize = m->file_size;

    new_id = m->new_id;
    /* WP22a: a member bigger than 4 GB (or with > 65535 slices) needs the
     * v2 recipe header; the helper picks, 0 = past the format caps */
    ahlen = invfs_ast_hdr_write(ah, fsize, (uint32_t)m->n_slices, 0);
    if (!ahlen) { free(old); return -1; }
    nlen = strlen(name);
    if (nlen > INVFS_MAX_NAME) nlen = INVFS_MAX_NAME;
    tomb_len = INVFS_REC_HDR_LEN + nlen + 1;
    rec_size = INVFS_REC_HDR_LEN + nlen + 1 + ahlen +
               m->n_slices * sizeof(invfs_ast_block_entry) + ext_len;
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) { free(old); return -1; }

    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = new_id;
    rh->file_size = fsize;
    rh->ctime = ctime;
    rec_set_name(rh, name);
    memcpy(invfs_rec_body(rh), ah, ahlen);
    ne = (invfs_ast_block_entry *)(invfs_rec_body(rh) + ahlen);
    for (i = 0; i < m->n_slices; i++) {
        const tz_sealed *s = tz_sealed_find(c, m->slices[i].batch_seq);
        if (!s) goto out;   /* can only be a bug in the flush */
        memset(&ne[i], 0, sizeof ne[i]);
        ne[i].file_offset = m->slices[i].file_off;
        ne[i].length = m->slices[i].len;
        ne[i].zone = INVFS_ZONE_TEXT;      /* TEXT zone == "batched" (WP14a) */
        ne[i].algo = s->algo;              /* PPMD / ZSTD / ZSTD_BCJ */
        ne[i].block_id = (uint32_t)m->slices[i].batch_seq;
        ne[i].block_offset = m->slices[i].batch_off;
        /* WP27: the member's entry names the batch's pba directly --
         * the v1 L2P dup is gone; the owner-WAL map (written at seal)
         * stays as the batch's durability/GC record */
        ne[i].pba = s->pba;
    }
    if (ext_len)
        memcpy(rec + rec_size - ext_len, ext, ext_len);
    crc = invfs_crc32c(rec, rec_size);

    /* room for the record AND the tombstone the retire appends (the
     * churn backstop reclaims the dead prefix first; the retire's
     * tombstone is an id-kill, and `old` rides content-only, so the
     * compaction's position moves touch nothing held here). Area-full
     * with compaction impossible (a live checkpoint) is a SOFT skip:
     * the member stays RAW and retries next sweep. */
    if (!(v->met0_present && v->meta_mapper) &&
        inode_area_make_room(v, (uint64_t)rec_size + 4 +
                             tomb_len + 4) != 0)
        { rc = 1; goto out; }
    {
        uint64_t npos;
        int rc2 = vol_append_slot(v, (uint64_t)rec_size + 4, &npos);
        if (rc2 != 0) { rc = 1; goto out; }
        if (io_seek(&v->io, npos) != 0 ||
            io_write(&v->io, rec, rec_size) != 0 ||
            io_write(&v->io, &crc, 4) != 0)
            goto out;
        idx_put(v, name, strlen(name), new_id, npos, fsize, ctime);
        idx_put_id(v, new_id, npos);
    }

    /* the retire frees the old RAW/generic blocks; the record had no TEXT
     * entries, so the batch gate does not engage */
    if (vol_delete_inode(v, cand->inode_id, name) != 0)
        fprintf(stderr, "tz: %s committed but the old record survived; "
                        "invf-fsck -f will reclaim it\n", name);
    if (c->binary)
        vol_stamp_class(v, new_id, INVFS_CLASS_BATCHED_BIN,
                        (uint8_t)m->algo, invfs_registry_generation());
    else
        vol_stamp_class(v, new_id, INVFS_CLASS_TEXT, INVFS_ALGO_PPMD,
                        invfs_registry_generation());
    rc = 0;
out:
    free(rec);
    free(old);
    return rc;
}


/* Commit every complete, non-fallback member whose slices all live in
 * sealed batches. WP27: the member record carries the batch pba in each
 * entry, so there are no dup maps to write: the batch segment and the
 * owner record landed durable at seal time (vol_pre_record there), and
 * the bitmap was flushed before the member record names the batch (the
 * pre-commit flush below). */
static int tz_commit_ready(tz_ctx *c, const tz_candidate *cands,
                           tz_member *members, size_t n)
{
    invfs_volume *v = c->v;
    size_t i, j;
    int any = 0, rc = 0;

    if (c->skip_commit) return 0;   /* crash-safety fault injection */
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (!m->complete || m->fallback || m->committed || m->new_id ||
            !m->n_slices)
            continue;
        for (j = 0; j < m->n_slices; j++)
            if (!tz_sealed_find(c, m->slices[j].batch_seq))
                break;
        if (j < m->n_slices) continue;   /* still feeding an open batch */
        m->new_id = v->next_inode_id++;
        any = 1;
    }
    if (any && vol_pre_record(v) != 0)
        return -1;
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        int crc;
        if (!m->new_id || m->committed) continue;
        crc = tz_commit_member(c, m, &cands[i]);
        if (crc < 0) {
            fprintf(stderr, "tz: commit failed for %s\n", cands[i].name);
            rc = -1;   /* the old record is untouched; the member stays RAW */
        }
        /* crc > 0: soft-skipped (area full, compaction impossible) --
         * the member stays RAW and retries next sweep */
        m->committed = 1;
    }
    return rc;
}


/* cumulative decoded bytes the owner covers (= its readable size) */
static uint64_t tz_owner_total(const tz_owner *o)
{
    uint64_t t = 0;
    uint32_t i;
    for (i = 0; i < o->n; i++) t += o->ents[i].length;
    return t;
}


/* Seal the open batch: encode (PPMd for text, ZSTD-19 for binary -- the
 * registry's zstd entry, mirroring the generic sweep level), MANDATORY
 * decode+memcmp verify (doc/06 invariant), size guard, write the segment
 * in the shadow zone, map it under the owner, flush the journal, append
 * the owner AST entry. On ANY refusal (encode/verify/guard/ENOSPC) the
 * batch is dropped unwritten and every member with a slice in it falls
 * back to the generic per-file path -- the same "not smaller, keep the
 * original" answer every other path gives.
 * Returns 0 on seal/fallback, -1 on hard error after the segment landed. */
static int tz_seal(tz_ctx *c, const tz_candidate *cands,
                   tz_member *members, size_t n_members)
{
    invfs_volume *v = c->v;
    size_t cap = c->blen + c->blen / 2 + 4096;
    uint8_t *enc = NULL, *ver = NULL, *seg = NULL;
    size_t enc_len = 0;
    uint64_t pba = 0, phys = 0;
    uint32_t csize = 0, crc;
    int fail = 0;
    size_t i, j;
    int rc = 0;

    if (c->blen == 0) return 0;

    enc = (uint8_t *)malloc(cap);
    ver = (uint8_t *)malloc(c->blen);
    if (!enc || !ver) fail = 1;
    if (!fail) {
        if (c->binary) {
            /* WP14a: one zstd stream per batch. The payload codec is the
             * registry ZSTD entry; BCJ (algo tag 14 on the AST) already
             * ran per member slice before the bytes landed in bbuf. */
            const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
            if (!zc || !zc->encode ||
                zc->encode(c->bbuf, c->blen, enc, cap, &enc_len) != 0)
                fail = 1;
            /* invariant: the batch must round-trip byte-exactly before it
             * is stored */
            if (!fail &&
                (zc->decode(enc, enc_len, ver, c->blen) != 0 ||
                 memcmp(ver, c->bbuf, c->blen) != 0)) {
                fprintf(stderr, "tz: ZSTD round-trip mismatch -- binary "
                                "batch dropped, members stay generic\n");
                fail = 1;
            }
        } else {
            if (invfs_ppmd_encode(c->bbuf, c->blen, enc, cap, &enc_len) != 0)
                fail = 1;
            /* invariant: the batch must round-trip byte-exactly before it
             * is stored */
            if (!fail &&
                (invfs_ppmd_decode(enc, enc_len, ver, c->blen) != 0 ||
                 memcmp(ver, c->bbuf, c->blen) != 0)) {
                fprintf(stderr, "tz: PPMd round-trip mismatch -- batch dropped, "
                                "members stay generic\n");
                fail = 1;
            }
        }
    }
    /* size guard: the batch must beat storing the slices raw (§4: ppmd<raw,
     * no ZSTD comparison -- 3 s/4 MB is too expensive) */
    if (!fail && enc_len + 4 >= c->blen)
        fail = 1;
    /* the owner's recipe header is v2-capable (WP22a): the cap that must
     * not be crossed is the format's MAX_FILE_SIZE, not v1's u32 */
    if (!fail && tz_owner_total(&c->owner) + c->blen > MAX_FILE_SIZE)
        fail = 1;

    if (!fail) {
        /* payload = [4B usize LE][PPMd wire]; standard [csize][crc] framing */
        uint32_t usize = (uint32_t)c->blen;
        uint8_t hdr[8];
        csize = (uint32_t)(4 + enc_len);
        phys = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           phys, 1, INVFS_ALLOC_DATA);
        if (!pba) {
            fail = 1;
        } else {
            seg = (uint8_t *)malloc((size_t)phys * INVFS_BLOCK_SIZE);
            if (!seg) { vol_free_blocks(v, pba, phys); pba = 0; fail = 1; }
        }
        if (!fail) {
            memcpy(seg + 8, &usize, 4);
            memcpy(seg + 12, enc, enc_len);
            crc = invfs_crc32c(seg + 8, csize);
            hdr[0] = (uint8_t)(csize & 0xFF);
            hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
            hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
            hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
            hdr[4] = (uint8_t)(crc & 0xFF);
            hdr[5] = (uint8_t)((crc >> 8) & 0xFF);
            hdr[6] = (uint8_t)((crc >> 16) & 0xFF);
            hdr[7] = (uint8_t)((crc >> 24) & 0xFF);
            memcpy(seg, hdr, 8);
            if (write_segment_blocks(v, pba, seg, (size_t)csize + 8,
                                     phys) != 0) {
                vol_free_blocks(v, pba, phys);
                pba = 0;
                fail = 1;
            }
        }
        if (!fail && vol_map(v, c->owner_id, c->open_seq, pba,
                             (uint32_t)phys) != 0) {
            vol_free_blocks(v, pba, phys);
            pba = 0;
            fail = 1;
        }
    }

    if (fail) {
        /* nothing durable references the batch: every member with a slice
         * in it goes through the generic per-file path. A member that ALSO
         * has slices in earlier, sealed batches leaves those as holes in
         * otherwise-live batches -- GC/compactor territory, never a read
         * hazard (no record ever names them). */
        for (i = 0; i < n_members; i++) {
            tz_member *m = &members[i];
            if (m->committed || m->fallback) continue;
            for (j = 0; j < m->n_slices; j++)
                if (m->slices[j].batch_seq == c->open_seq) {
                    m->fallback = 1;
                    break;
                }
        }
        c->blen = 0;
        free(enc); free(ver); free(seg);
        return 0;
    }

    /* the owner map must be durable BEFORE the owner record names the pba */
    if (vol_pre_record(v) != 0) {
        free(enc); free(ver); free(seg);
        return -1;
    }
    /* owner AST += {cumulative offset, usize, TEXT, <batch algo>, seq, 0} */
    if (c->owner.n == c->owner.cap) {
        uint32_t nc = c->owner.cap ? c->owner.cap * 2 : 16;
        invfs_ast_block_entry *ne =
            (invfs_ast_block_entry *)realloc(c->owner.ents,
                                             nc * sizeof(*ne));
        if (!ne) { free(enc); free(ver); free(seg); return -1; }
        c->owner.ents = ne;
        c->owner.cap = nc;
    }
    {
        invfs_ast_block_entry *e = &c->owner.ents[c->owner.n];
        memset(e, 0, sizeof *e);
        e->length = c->blen;              /* file_offset rebuilt by write */
        e->zone = INVFS_ZONE_TEXT;
        e->algo = c->open_algo;           /* PPMD / ZSTD / ZSTD_BCJ */
        e->block_id = (uint32_t)c->open_seq;
        e->block_offset = 0;
        e->pba = pba;   /* WP27: the owner record is self-describing too */
        c->owner.n++;
    }
    if (tz_owner_write(v, c->owner_id, TZ_OWNER_NAME, &c->owner) != 0) {
        free(enc); free(ver); free(seg);
        return -1;   /* segment + map landed; no record names them: orphan,
                        fsck/GC territory -- members stay RAW, nothing torn */
    }
    /* remember the seal for member-commit pba lookup */
    if (c->n_sealed == c->cap_sealed) {
        size_t nc = c->cap_sealed ? c->cap_sealed * 2 : 16;
        tz_sealed *ns = (tz_sealed *)realloc(c->sealed, nc * sizeof(*ns));
        if (!ns) { free(enc); free(ver); free(seg); return -1; }
        c->sealed = ns;
        c->cap_sealed = nc;
    }
    c->sealed[c->n_sealed].seq = c->open_seq;
    c->sealed[c->n_sealed].pba = pba;
    c->sealed[c->n_sealed].phys = (uint32_t)phys;
    c->sealed[c->n_sealed].algo = c->open_algo;
    c->n_sealed++;
    c->sealed_any = 1;
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[tz] sealed %s batch %llu: %zu -> %zu bytes (pba %llu)\n",
                c->open_algo == INVFS_ALGO_PPMD ? "ppmd" :
                c->open_algo == INVFS_ALGO_ZSTD_BCJ ? "zstd+bcj" : "zstd",
                (unsigned long long)c->open_seq, c->blen, enc_len,
                (unsigned long long)pba);
    c->open_seq++;
    c->blen = 0;
    /* members whose slices ALL live in sealed batches commit now */
    rc = tz_commit_ready(c, cands, members, n_members);
    free(enc); free(ver); free(seg);
    return rc;
}


/* stable ordering key: (family, size, original index) -- the doc/06 language
 * grouping; qsort is not stable, so the index breaks residual ties */
static const tz_candidate *g_sort_cands;

static int tz_order_cmp(const void *a, const void *b)
{
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    const tz_candidate *x = &g_sort_cands[ia], *y = &g_sort_cands[ib];
    if (x->family != y->family) return x->family < y->family ? -1 : 1;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return ia < ib ? -1 : 1;
}


/* which stored classes may be re-batched at flush time (the candidate is
 * no longer RAW): the deferred retries (MEMLIMIT/UNCOMPRESSIBLE/GUARD
 * settled into generic storage) for both domains; plus, for the BINARY
 * accumulator, plain GENERIC -- the WP14a migration path re-batches
 * pre-existing per-segment-ZSTD binaries outright (binary batching shipped
 * with this build, so a GENERIC stamp on a binary-family file can only
 * predate it). Text keeps GENERIC terminal (WP10: no upgrade path). */
static int tz_flush_class_ok(int binary, uint8_t cc)
{
    if (cc == INVFS_CLASS_GENERIC_MEMLIMIT ||
        cc == INVFS_CLASS_UNCOMPRESSIBLE ||
        cc == INVFS_CLASS_GENERIC_GUARD)
        return 1;
    if (binary && cc == INVFS_CLASS_GENERIC)
        return 1;
    return 0;
}


/* Flush ONE accumulator (text: binary=0, WP10; binary: binary=1, WP14a).
 * The machinery is shared; what differs is exactly the codec (PPMd vs
 * ZSTD[+BCJ]), the class stamp, and the content re-validation. */
static int tz_flush_one(invfs_volume *v, int binary)
{
    tz_ctx c;
    size_t n, i;
    size_t *order = NULL;
    tz_candidate *sc = NULL;   /* candidates in sorted order */
    tz_member *members = NULL;
    tz_candidate *acc = binary ? v->bz : v->tz;
    size_t acc_n = binary ? v->bz_n : v->tz_n;
    int rc = 0;

    if (acc_n == 0) return 1;   /* nothing pending */
    if (!vol_write_enabled(v)) {
        if (binary) v->bz_n = 0;   /* read-only volume: nothing can land */
        else        v->tz_n = 0;
        return 1;
    }

    memset(&c, 0, sizeof c);
    c.v = v;
    c.binary = binary;
    c.open_algo = binary ? INVFS_ALGO_ZSTD : INVFS_ALGO_PPMD;
    c.skip_commit = getenv("INVFS_TZ_SKIP_COMMIT") != NULL;
    n = acc_n;

    /* batch target: min(4 MB, arc/2) (arc refuses larger units); a disabled
     * cache (budget 0) does not bound the batch -- reads still decode */
    c.bcap = (size_t)TZ_BATCH_MAX;
    if (v->arc_budget && v->arc_budget / 2 < (uint64_t)c.bcap)
        c.bcap = (size_t)(v->arc_budget / 2);
    if (c.bcap < 4096) c.bcap = 4096;

    c.bbuf = (uint8_t *)malloc(c.bcap);
    order = (size_t *)malloc(n * sizeof(size_t));
    sc = (tz_candidate *)malloc(n * sizeof(tz_candidate));
    members = (tz_member *)calloc(n, sizeof(tz_member));
    if (!c.bbuf || !order || !sc || !members) { rc = -1; goto out; }

    /* owner: find-or-create, then load its current AST state. ONE owner
     * ("\x01tzb") holds text and binary batches alike: the batch_seq space,
     * the L2P maps and the GC mark rule (zone==TEXT) are codec-agnostic. */
    c.owner_id = tz_owner_id(v);
    if (!c.owner_id || tz_owner_load(v, c.owner_id, &c.owner) != 0) {
        rc = -1;
        goto out;
    }
    c.open_seq = c.owner.next_seq;

    /* stable sort by (family, size) into the working copy */
    g_sort_cands = acc;
    for (i = 0; i < n; i++) order[i] = i;
    qsort(order, n, sizeof(size_t), tz_order_cmp);
    for (i = 0; i < n; i++) sc[i] = acc[order[i]];

    /* feed every candidate into the batch stream, sealing as batches fill */
    for (i = 0; i < n; i++) {
        tz_candidate *cand = &sc[i];
        tz_member *m = &members[i];
        uint8_t *data = NULL;
        size_t dlen = 0, off = 0;
        int z;

        /* re-validate against the LIVE name: the deferral is only a hint,
         * the content may have moved on (replaced => new id; swept by a
         * sibling path meanwhile => no longer ours to batch) */
        if (vol_find(v, cand->name) != cand->inode_id)
            continue;
        z = vol_inode_first_zone(v, cand->inode_id);
        if (z != INVFS_ZONE_RAW) {
            uint8_t cc = 0, ca = 0;
            uint16_t cg = 0;
            /* a deferred retry (MEMLIMIT/UNCOMPRESSIBLE/Guard settled into
             * generic storage) is fine to batch -- and, for the binary
             * accumulator, so is plain GENERIC (the WP14a migration);
             * anything else was swept meanwhile and is left alone */
            if (z < 0)
                continue;
            if (vol_get_class(v, cand->inode_id, &cc, &ca, &cg) != 0) {
                /* WP14b: a container part carries no class stamp; the walk
                 * deferred it out of the absent-stamp + BINARY zone +
                 * generic-algos shape, and the name->id pin above still
                 * holds the record that shape was checked against. */
                if (z != INVFS_ZONE_BINARY || !strchr(cand->name, '!'))
                    continue;
            } else if (!tz_flush_class_ok(binary, cc))
                continue;
        }
        if (vol_read_file(v, cand->inode_id, &data, &dlen) != 0 || !dlen) {
            free(data);
            continue;
        }
        if (binary) {
            /* the BCJ prefilter decision rides on the FRESH content's
             * family, not the defer-time sort key */
            int bfam = invfs_binary_family(data, dlen, cand->name);
            if (bfam == 0) {
                /* content changed class since the deferral: plain generic */
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->bcj = (bfam == INVFS_BIN_FAMILY_ELF_X64 ||
                      bfam == INVFS_BIN_FAMILY_ELF_X86);
            m->algo = m->bcj ? INVFS_ALGO_ZSTD_BCJ : INVFS_ALGO_ZSTD;
            /* batches are prefilter-homogeneous (a batch's algo is uniform
             * for all its members): a member of the other BCJ kind seals
             * the open batch first. The (family,size) sort keeps the BCJ
             * families (20,21) adjacent, so this split only ever fires at
             * a family boundary. */
            if (c.blen && c.open_bcj != m->bcj &&
                tz_seal(&c, sc, members, n) != 0) {
                rc = -1; free(data); goto out;
            }
        } else {
            if (invfs_text_family(cand->name, data, dlen) == 0) {
                /* content changed class since the deferral: plain generic */
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->algo = INVFS_ALGO_PPMD;
        }
        m->file_size = dlen;
        while (off < dlen) {
            size_t take;
            if (c.blen == c.bcap &&
                tz_seal(&c, sc, members, n) != 0) { rc = -1; free(data); goto out; }
            if (m->fallback) break;   /* its batch was dropped mid-file */
            if (c.blen == 0) {   /* first member of a batch sets the tone */
                c.open_bcj = m->bcj;
                c.open_algo = m->algo;
            }
            take = c.bcap - c.blen;
            if (take > dlen - off) take = dlen - off;
            if (tz_slice_push(m, c.open_seq, off, (uint32_t)c.blen,
                              (uint32_t)take) != 0) { rc = -1; free(data); goto out; }
            memcpy(c.bbuf + c.blen, data + off, take);
            /* WP14a: the BCJ prefilter runs on the member slice STANDALONE
             * (pc=0, state=0), before the batch exists as a whole. The read
             * path inverts exactly this window at pc=0 after the batch
             * decode -- enc and dec windows coincide, so the transform is
             * bijective regardless of where the slice sits in the batch. */
            if (m->bcj)
                invfs_bcj_x86_enc(c.bbuf + c.blen, take);
            c.blen += take;
            off += take;
        }
        free(data);
        m->complete = 1;
        if (c.blen == c.bcap &&
            tz_seal(&c, sc, members, n) != 0) { rc = -1; goto out; }
    }
    /* drain the partial batch, then commit whatever is left */
    if (rc == 0 && tz_seal(&c, sc, members, n) != 0) rc = -1;
    if (rc == 0 && tz_commit_ready(&c, sc, members, n) != 0) rc = -1;
    /* fallbacks go through the generic per-file path now; never-fed
     * candidates simply keep their files for the next run */
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (m->fallback && !m->committed)
            vol_sweep_file(v, sc[i].inode_id);
        free(m->slices);
    }
out:
    free(order);
    free(sc);
    free(members);
    free(c.bbuf);
    free(c.sealed);
    tz_owner_free(&c.owner);
    if (binary) v->bz_n = 0;   /* the accumulator is drained either way */
    else        v->tz_n = 0;
    if (rc != 0) return rc;
    return c.sealed_any ? 0 : 1;
}


/* ===================================================================
 * WP78: v3 text/binary batching.
 *
 * A v3 volume has no v2 record stream and no owner-L2P map, but the read
 * path already serves a shared batch through an AST entry with
 * zone==INVFS_ZONE_TEXT that names the batch segment by pba (WP27), exactly
 * as on v2. So the v3 flush only has to (a) build the batch segments, (b)
 * rewrite each member's recipe with TEXT entries pointing at them, and
 * (c) keep a registry of live batches so a later GC can free the dead ones.
 * The registry is a hidden 0x01 file (TZ_OWNER_NAME) holding a flat array of
 * {seq, algo, pba, phys}; GC marks every pba referenced by a live recipe's
 * TEXT entries and frees the unmarked registry rows.
 * =================================================================== */

#define TZ_V3_REG_MAGIC 0x33565a54u   /* "TZV3" */

static int tz_v3_u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct {
    uint32_t seq;
    uint32_t algo;
    uint64_t pba;
    uint32_t phys;
} tz_v3_reg_ent;

typedef struct {
    tz_v3_reg_ent *ents;
    size_t n, cap;
    uint32_t next_seq;
    uint64_t owner_id;
} tz_v3_reg;

static int tz_v3_reg_load(invfs_volume *v, tz_v3_reg *r)
{
    uint8_t *buf = NULL;
    size_t len = 0;

    memset(r, 0, sizeof *r);
    r->next_seq = 1;
    r->owner_id = vol_find(v, TZ_OWNER_NAME);
    if (!r->owner_id) return 0;
    if (vol_read_file(v, r->owner_id, &buf, &len) != 0 || !buf) {
        free(buf);
        return 0;
    }
    if (len >= 8) {
        uint32_t magic = 0, n = 0;
        memcpy(&magic, buf, 4);
        memcpy(&n, buf + 4, 4);
        if (magic == TZ_V3_REG_MAGIC) {
            size_t avail = (len - 8) / sizeof(tz_v3_reg_ent);
            if ((size_t)n > avail) n = (uint32_t)avail;
            if (n) {
                r->ents = (tz_v3_reg_ent *)malloc((size_t)n * sizeof *r->ents);
                if (r->ents) {
                    memcpy(r->ents, buf + 8, (size_t)n * sizeof *r->ents);
                    r->n = r->cap = n;
                }
            }
            for (size_t i = 0; i < r->n; i++)
                if (r->ents[i].seq >= r->next_seq)
                    r->next_seq = r->ents[i].seq + 1;
        }
    }
    free(buf);
    return 0;
}

static int tz_v3_reg_store(invfs_volume *v, tz_v3_reg *r)
{
    size_t len = 8 + r->n * sizeof(tz_v3_reg_ent);
    uint8_t *buf = (uint8_t *)calloc(1, len);
    uint32_t magic = TZ_V3_REG_MAGIC, n = (uint32_t)r->n;
    uint64_t id;

    if (!buf) return -1;
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &n, 4);
    if (r->n) memcpy(buf + 8, r->ents, r->n * sizeof *r->ents);
    id = vol_find(v, TZ_OWNER_NAME);
    if (id) {
        if (!vol_v3_publish_blob_inode(v, id, buf, len, len, INVFS_ALGO_NONE)) {
            free(buf);
            return -1;
        }
    } else {
        id = vol_create_blob_file(v, TZ_OWNER_NAME, buf, len, len,
                                  INVFS_ALGO_NONE);
        if (!id) { free(buf); return -1; }
    }
    free(buf);
    return 0;
}

/* Seal one batch: encode (PPMd for text, ZSTD for binary -- the BCJ
 * prefilter already ran per member slice), mandatory decode+memcmp guard,
 * size guard, write the segment in Shadow. 0 = sealed, 1 = refused
 * (fallback; no segment written), -1 = hard error. */
static int tz_v3_seal(invfs_volume *v, int binary, const uint8_t *bbuf,
                      size_t blen, uint32_t algo, uint64_t seq,
                      tz_v3_reg_ent *out)
{
    size_t cap = blen + blen / 2 + 4096;
    uint8_t *enc = NULL, *ver = NULL, *seg = NULL;
    size_t enc_len = 0;
    uint64_t pba = 0, phys = 0;
    uint32_t csize, crc, usize;
    uint8_t hdr[8];
    int fail = 0;

    if (blen == 0) return 1;
    enc = (uint8_t *)malloc(cap);
    ver = (uint8_t *)malloc(blen);
    if (!enc || !ver) fail = 1;
    if (!fail) {
        if (binary) {
            const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
            if (!zc || !zc->encode ||
                zc->encode(bbuf, blen, enc, cap, &enc_len) != 0)
                fail = 1;
            if (!fail && (zc->decode(enc, enc_len, ver, blen) != 0 ||
                          memcmp(ver, bbuf, blen) != 0))
                fail = 1;
        } else {
            if (invfs_ppmd_encode(bbuf, blen, enc, cap, &enc_len) != 0)
                fail = 1;
            if (!fail && (invfs_ppmd_decode(enc, enc_len, ver, blen) != 0 ||
                          memcmp(ver, bbuf, blen) != 0))
                fail = 1;
        }
    }
    /* the batch must beat storing the slices raw */
    if (!fail && enc_len + 4 >= blen) fail = 1;
    if (fail) { free(enc); free(ver); return 1; }

    /* payload = [4B usize LE][encoded]; standard [csize][crc] framing */
    usize = (uint32_t)blen;
    csize = (uint32_t)(4 + enc_len);
    phys = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       phys, 1, INVFS_ALLOC_DATA);
    if (!pba) { free(enc); free(ver); return 1; }
    seg = (uint8_t *)malloc((size_t)phys * INVFS_BLOCK_SIZE);
    if (!seg) { vol_free_blocks(v, pba, phys); free(enc); free(ver); return 1; }
    memcpy(seg + 8, &usize, 4);
    memcpy(seg + 12, enc, enc_len);
    crc = invfs_crc32c(seg + 8, csize);
    hdr[0] = (uint8_t)(csize & 0xFF);
    hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
    hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
    hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
    hdr[4] = (uint8_t)(crc & 0xFF);
    hdr[5] = (uint8_t)((crc >> 8) & 0xFF);
    hdr[6] = (uint8_t)((crc >> 16) & 0xFF);
    hdr[7] = (uint8_t)((crc >> 24) & 0xFF);
    memcpy(seg, hdr, 8);
    if (write_segment_blocks(v, pba, seg, (size_t)csize + 8, phys) != 0) {
        vol_free_blocks(v, pba, phys);
        free(seg); free(enc); free(ver);
        return 1;
    }
    free(seg); free(enc); free(ver);
    out->seq = (uint32_t)seq;
    out->algo = algo;
    out->pba = pba;
    out->phys = (uint32_t)phys;
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[tz-v3] sealed %s batch %llu: %zu -> %zu bytes "
                        "(pba %llu)\n",
                binary ? "binary" : "ppmd", (unsigned long long)seq, blen,
                enc_len, (unsigned long long)pba);
    return 0;
}

/* Rewrite one member's v3 recipe with TEXT entries naming the sealed
 * batches. Returns 0 on success/soft-skip, -1 on hard error. */
static int tz_v3_commit_member(invfs_volume *v, int binary,
                               const tz_candidate *cand, tz_member *m,
                               const tz_sealed *sealed, size_t n_sealed)
{
    invfs_ast_block_entry *ents;
    uint8_t *rblob = NULL;
    size_t rlen = 0, i, j;
    uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_v3_inode in;
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];

    if (!m->complete || m->fallback || m->committed || !m->n_slices)
        return 0;
    ents = (invfs_ast_block_entry *)calloc(m->n_slices, sizeof *ents);
    if (!ents) return -1;
    for (i = 0; i < m->n_slices; i++) {
        const tz_sealed *s = NULL;
        for (j = 0; j < n_sealed; j++)
            if (sealed[j].seq == m->slices[i].batch_seq) { s = &sealed[j]; break; }
        if (!s) { free(ents); return 0; }   /* still feeding an open batch */
        memset(&ents[i], 0, sizeof ents[i]);
        ents[i].file_offset = m->slices[i].file_off;
        ents[i].length = m->slices[i].len;
        ents[i].zone = INVFS_ZONE_TEXT;
        ents[i].algo = s->algo;
        ents[i].block_id = (uint32_t)m->slices[i].batch_seq;
        ents[i].block_offset = m->slices[i].batch_off;
        ents[i].pba = s->pba;
    }
    if (vol_ast_recipe_serialize(m->file_size, ents, (uint32_t)m->n_slices,
                                 &rblob, &rlen) != 0) {
        free(ents);
        return -1;
    }
    free(ents);
    if (vol_v3_recipe_store(v, rblob, rlen, addr) != 0) {
        free(rblob);
        return -1;
    }
    free(rblob);
    if (vol_v3_inode_get(v, cand->inode_id, &in) != 1)
        return -1;
    memcpy(old_addr, in.recipe_addr, sizeof old_addr);
    in.size = m->file_size;
    memset(&in.recipe, 0, sizeof in.recipe);
    memcpy(in.recipe_addr, addr, sizeof addr);
    if (vol_v3_inode_delta_put(v, cand->inode_id, &in) != 0)
        return -1;
    vol_v3_free_recipe_blocks(v, old_addr, 0);
    if (binary)
        vol_stamp_class(v, cand->inode_id, INVFS_CLASS_BATCHED_BIN,
                        (uint8_t)m->algo, invfs_registry_generation());
    else
        vol_stamp_class(v, cand->inode_id, INVFS_CLASS_TEXT,
                        INVFS_ALGO_PPMD, invfs_registry_generation());
    m->committed = 1;
    return 0;
}

/* Flush ONE v3 accumulator. Same shape as tz_flush_one (sort by
 * (family,size), feed into batches, BCJ-homogeneous binary batches), but
 * publication is v3 recipe deltas + the batch registry. */
static int tz_flush_one_v3(invfs_volume *v, int binary)
{
    tz_candidate *acc = binary ? v->bz : v->tz;
    size_t n = binary ? v->bz_n : v->tz_n;
    tz_v3_reg reg;
    tz_sealed *sealed = NULL;
    size_t n_sealed = 0, cap_sealed = 0;
    uint8_t *bbuf = NULL;
    size_t bcap, i, j, blen = 0;
    size_t *order = NULL;
    tz_candidate *sc = NULL;
    tz_member *members = NULL;
    uint32_t open_seq, open_algo;
    int open_bcj = 0, rc = 0, sealed_any = 0;

    if (n == 0) return 1;
    if (!vol_write_enabled(v)) {
        if (binary) v->bz_n = 0; else v->tz_n = 0;
        return 1;
    }
    if (tz_v3_reg_load(v, &reg) != 0) return -1;
    open_seq = reg.next_seq;
    open_algo = binary ? INVFS_ALGO_ZSTD : INVFS_ALGO_PPMD;

    bcap = (size_t)TZ_BATCH_MAX;
    if (v->arc_budget && v->arc_budget / 2 < (uint64_t)bcap)
        bcap = (size_t)(v->arc_budget / 2);
    if (bcap < 4096) bcap = 4096;

    bbuf = (uint8_t *)malloc(bcap);
    order = (size_t *)malloc(n * sizeof *order);
    sc = (tz_candidate *)malloc(n * sizeof *sc);
    members = (tz_member *)calloc(n, sizeof *members);
    if (!bbuf || !order || !sc || !members) { rc = -1; goto out; }

    g_sort_cands = acc;
    for (i = 0; i < n; i++) order[i] = i;
    qsort(order, n, sizeof(size_t), tz_order_cmp);
    for (i = 0; i < n; i++) sc[i] = acc[order[i]];

    for (i = 0; i < n; i++) {
        tz_candidate *cand = &sc[i];
        tz_member *m = &members[i];
        uint8_t *data = NULL;
        size_t dlen = 0, off = 0;
        int z;

        if (vol_find(v, cand->name) != cand->inode_id) continue;
        z = vol_inode_first_zone(v, cand->inode_id);
        if (z != INVFS_ZONE_RAW) {
            uint8_t cc = 0, ca = 0;
            uint16_t cg = 0;
            if (z < 0) continue;
            if (vol_get_class(v, cand->inode_id, &cc, &ca, &cg) != 0) {
                if (z != INVFS_ZONE_BINARY || !strchr(cand->name, '!'))
                    continue;
            } else if (!tz_flush_class_ok(binary, cc))
                continue;
        }
        if (vol_read_file(v, cand->inode_id, &data, &dlen) != 0 || !dlen) {
            free(data);
            continue;
        }
        if (binary) {
            int bfam = invfs_binary_family(data, dlen, cand->name);
            if (bfam == 0) {
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->bcj = (bfam == INVFS_BIN_FAMILY_ELF_X64 ||
                      bfam == INVFS_BIN_FAMILY_ELF_X86);
            m->algo = m->bcj ? INVFS_ALGO_ZSTD_BCJ : INVFS_ALGO_ZSTD;
            if (blen && open_bcj != m->bcj) {
                tz_v3_reg_ent re;
                int sr = tz_v3_seal(v, binary, bbuf, blen, open_algo,
                                    open_seq, &re);
                if (sr < 0) { rc = -1; free(data); goto out; }
                if (sr == 0) {
                    if (n_sealed == cap_sealed) {
                        size_t nc = cap_sealed ? cap_sealed * 2 : 16;
                        tz_sealed *ns = (tz_sealed *)realloc(sealed,
                                            nc * sizeof *ns);
                        if (!ns) { rc = -1; free(data); goto out; }
                        sealed = ns; cap_sealed = nc;
                    }
                    sealed[n_sealed].seq = re.seq;
                    sealed[n_sealed].pba = re.pba;
                    sealed[n_sealed].phys = re.phys;
                    sealed[n_sealed].algo = re.algo;
                    n_sealed++;
                    sealed_any = 1;
                } else {
                    for (j = 0; j < n; j++)
                        for (size_t k = 0; k < members[j].n_slices; k++)
                            if (members[j].slices[k].batch_seq == open_seq)
                                members[j].fallback = 1;
                }
                open_seq++;
                blen = 0;
            }
        } else {
            if (invfs_text_family(cand->name, data, dlen) == 0) {
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->algo = INVFS_ALGO_PPMD;
        }
        m->file_size = dlen;
        while (off < dlen) {
            size_t take;
            if (blen == bcap) {
                tz_v3_reg_ent re;
                int sr = tz_v3_seal(v, binary, bbuf, blen, open_algo,
                                    open_seq, &re);
                if (sr < 0) { rc = -1; free(data); goto out; }
                if (sr == 0) {
                    if (n_sealed == cap_sealed) {
                        size_t nc = cap_sealed ? cap_sealed * 2 : 16;
                        tz_sealed *ns = (tz_sealed *)realloc(sealed,
                                            nc * sizeof *ns);
                        if (!ns) { rc = -1; free(data); goto out; }
                        sealed = ns; cap_sealed = nc;
                    }
                    sealed[n_sealed].seq = re.seq;
                    sealed[n_sealed].pba = re.pba;
                    sealed[n_sealed].phys = re.phys;
                    sealed[n_sealed].algo = re.algo;
                    n_sealed++;
                    sealed_any = 1;
                } else {
                    for (j = 0; j < n; j++)
                        for (size_t k = 0; k < members[j].n_slices; k++)
                            if (members[j].slices[k].batch_seq == open_seq)
                                members[j].fallback = 1;
                }
                open_seq++;
                blen = 0;
            }
            if (m->fallback) break;
            if (blen == 0) {
                open_bcj = m->bcj;
                open_algo = m->algo;
            }
            take = bcap - blen;
            if (take > dlen - off) take = dlen - off;
            if (tz_slice_push(m, open_seq, off, (uint32_t)blen,
                              (uint32_t)take) != 0) {
                rc = -1; free(data); goto out;
            }
            memcpy(bbuf + blen, data + off, take);
            if (m->bcj)
                invfs_bcj_x86_enc(bbuf + blen, take);
            blen += take;
            off += take;
        }
        free(data);
        m->complete = 1;
    }
    /* drain the partial batch */
    if (blen) {
        tz_v3_reg_ent re;
        int sr = tz_v3_seal(v, binary, bbuf, blen, open_algo, open_seq, &re);
        if (sr < 0) { rc = -1; goto out; }
        if (sr == 0) {
            if (n_sealed == cap_sealed) {
                size_t nc = cap_sealed ? cap_sealed * 2 : 16;
                tz_sealed *ns = (tz_sealed *)realloc(sealed, nc * sizeof *ns);
                if (!ns) { rc = -1; goto out; }
                sealed = ns; cap_sealed = nc;
            }
            sealed[n_sealed].seq = re.seq;
            sealed[n_sealed].pba = re.pba;
            sealed[n_sealed].phys = re.phys;
            sealed[n_sealed].algo = re.algo;
            n_sealed++;
            sealed_any = 1;
        } else {
            for (j = 0; j < n; j++)
                for (size_t k = 0; k < members[j].n_slices; k++)
                    if (members[j].slices[k].batch_seq == open_seq)
                        members[j].fallback = 1;
        }
        open_seq++;
        blen = 0;
    }
    /* commit every complete member whose slices all reference sealed batches */
    for (i = 0; i < n; i++) {
        if (tz_v3_commit_member(v, binary, &sc[i], &members[i], sealed,
                                n_sealed) != 0) {
            rc = -1;
            goto out;
        }
    }
    /* persist the registry (append this run's sealed batches) */
    if (sealed_any) {
        for (i = 0; i < n_sealed; i++) {
            if (reg.n == reg.cap) {
                size_t nc = reg.cap ? reg.cap * 2 : 16;
                tz_v3_reg_ent *ne = (tz_v3_reg_ent *)realloc(reg.ents,
                                        nc * sizeof *ne);
                if (!ne) { rc = -1; goto out; }
                reg.ents = ne; reg.cap = nc;
            }
            reg.ents[reg.n].seq = (uint32_t)sealed[i].seq;
            reg.ents[reg.n].algo = sealed[i].algo;
            reg.ents[reg.n].pba = sealed[i].pba;
            reg.ents[reg.n].phys = sealed[i].phys;
            reg.n++;
        }
        if (tz_v3_reg_store(v, &reg) != 0) rc = -1;
    }
out:
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (m->fallback && !m->committed)
            vol_sweep_file(v, sc[i].inode_id);
        free(m->slices);
    }
    free(order);
    free(sc);
    free(members);
    free(bbuf);
    free(sealed);
    free(reg.ents);
    if (binary) v->bz_n = 0;
    else        v->tz_n = 0;
    if (rc != 0) return rc;
    return sealed_any ? 0 : 1;
}

/* WP78: v3 batch GC. A batch is live iff some live recipe has a zone==TEXT
 * entry naming its pba; unmarked registry rows are freed. */
static int tz_v3_gc(invfs_volume *v)
{
    tz_v3_reg reg;
    uint64_t *ids = NULL;
    size_t cap = 4096, n_ids = 0, i, j;
    uint64_t *live = NULL;
    size_t n_live = 0, cap_live = 0;
    int freed = 0;

    if (tz_v3_reg_load(v, &reg) != 0) return -1;
    if (reg.n == 0) { free(reg.ents); return 0; }
    if (!vol_write_enabled(v)) { free(reg.ents); return 0; }

    ids = (uint64_t *)malloc(cap * sizeof *ids);
    if (!ids) { free(reg.ents); return -1; }
    for (;;) {
        size_t got = vol_collect_sweepables(v, ids, cap);
        if (got < cap) { n_ids = got; break; }
        cap *= 2;
        uint64_t *ni = (uint64_t *)realloc(ids, cap * sizeof *ni);
        if (!ni) { free(ids); free(reg.ents); return -1; }
        ids = ni;
    }
    for (i = 0; i < n_ids; i++) {
        invfs_v3_inode in;
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t ne = 0;
        if (vol_v3_inode_get(v, ids[i], &in) != 1) continue;
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
            free(blob);
            continue;
        }
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &ne) == 0 && ents) {
            for (j = 0; j < ne; j++) {
                if (ents[j].zone != INVFS_ZONE_TEXT || !ents[j].pba) continue;
                if (n_live == cap_live) {
                    size_t nc = cap_live ? cap_live * 2 : 64;
                    uint64_t *nl = (uint64_t *)realloc(live, nc * sizeof *nl);
                    if (!nl) { free(blob); free(ids); free(live); free(reg.ents); return -1; }
                    live = nl; cap_live = nc;
                }
                live[n_live++] = ents[j].pba;
            }
        }
        free(blob);
    }
    free(ids);
    if (n_live > 1)
        qsort(live, n_live, sizeof *live, tz_v3_u64_cmp);
    {
        size_t w = 0;
        for (i = 0; i < reg.n; i++) {
            int keep = 0;
            if (live) {
                size_t lo = 0, hi = n_live;
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    if (live[mid] < reg.ents[i].pba) lo = mid + 1;
                    else hi = mid;
                }
                keep = lo < n_live && live[lo] == reg.ents[i].pba;
            }
            if (keep) { reg.ents[w++] = reg.ents[i]; continue; }
            arc_invalidate(v->arc, reg.ents[i].pba | TZ_ARC_TAG);
            vol_free_blocks(v, reg.ents[i].pba, reg.ents[i].phys);
            freed++;
        }
        reg.n = w;
    }
    if (freed > 0 && tz_v3_reg_store(v, &reg) != 0) freed = -1;
    free(live);
    free(reg.ents);
    return freed;
}

int vol_tz_flush(invfs_volume *v)
{
    int rt, rb;

    if (!v) return -1;
    /* WP78: v3 has no owner records, but the read path serves a TEXT-zone
     * AST entry identically, so the same accumulators flush into v3 recipe
     * deltas + a hidden batch registry. */
    if (v->sb.vol_flags & VOLF_V3) {
        rt = tz_flush_one_v3(v, 0);
        rb = tz_flush_one_v3(v, 1);
        if (rt < 0 || rb < 0) return -1;
        return (rt == 0 || rb == 0) ? 0 : 1;
    }
    /* text first, then binary: two independent accumulators, one shared
     * owner inode (batch_seq space is handed out by tz_owner_load) */
    rt = tz_flush_one(v, 0);
    rb = tz_flush_one(v, 1);
    if (rt < 0 || rb < 0) return -1;
    return (rt == 0 || rb == 0) ? 0 : 1;   /* 0 = something sealed */
}


size_t vol_acc_pending(const invfs_volume *v, int binary)
{
    if (!v) return 0;
    return binary ? v->bz_n : v->tz_n;
}


/* u32 comparator for the GC mark set */
static int tz_u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}


/* WP47: GC mark pass over the shared mapper-aware walker. The legacy
 * contiguous loop saw no records on a v0.3.0+ mapper volume, so the mark
 * set came back empty and GC would free every live batch. The callback
 * keeps the exact legacy policy: skip tombstones and the owner record,
 * only the LIVE record of a name marks, and TEXT-zone entries contribute
 * their batch_seq (block_id). */
typedef struct {
    invfs_volume *v;
    uint64_t owner;
    uint32_t *live;
    size_t n_live, cap_live;
    int oom;
} tz_gc_mark_ctx;

static int tz_gc_mark_cb(void *ctx_, uint64_t rec_pos,
                         const invfs_inode_rec *h, const uint8_t *rec)
{
    tz_gc_mark_ctx *c = (tz_gc_mark_ctx *)ctx_;
    invfs_volume *v = c->v;
    invfs_ast_hdr ah;
    size_t base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    uint32_t nb, j;
    (void)rec_pos;

    if (h->magic != INODE_REC_MAGIC) return 0;   /* tombstones */
    if (h->inode_id == c->owner) return 0;
    if (h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1)
        return 0;
    {
        size_t nl = h->name_len;
        char nm[257];
        memcpy(nm, h->name, nl);
        nm[nl] = 0;
        /* only the LIVE record of a name marks anything (superseded ones
         * may still carry TEXT entries of an older batching generation) */
        if (vol_find(v, nm) != h->inode_id) return 0;
    }
    if (h->rec_len < base + INVFS_AST_HDR_V1_LEN)
        return 0;
    if (invfs_ast_hdr_parse(rec + base, h->rec_len - base, &ah) != 0)
        return 0;
    nb = ah.num_blocks;
    if (h->rec_len < base + ah.hdr_len +
            (size_t)nb * sizeof(invfs_ast_block_entry))
        return 0;
    for (j = 0; j < nb; j++) {
        const invfs_ast_block_entry *e =
            (const invfs_ast_block_entry *)
            (rec + base + ah.hdr_len + (size_t)j * sizeof(*e));
        if (e->zone != INVFS_ZONE_TEXT) continue;
        if (c->n_live == c->cap_live) {
            size_t nc = c->cap_live ? c->cap_live * 2 : 64;
            uint32_t *nl2 = (uint32_t *)realloc(c->live, nc * sizeof *nl2);
            if (!nl2) { c->oom = 1; return -1; }
            c->live = nl2;
            c->cap_live = nc;
        }
        c->live[c->n_live++] = e->block_id;
    }
    return 0;
}

/* Text-zone GC (WP10 §7): mark-and-sweep over the owner AST. A batch is
 * live iff at least one LIVE member record has a zone==TEXT entry naming
 * its batch_seq (marking by block_id rather than pba keeps the mark set
 * correct even if a member's L2P dup is damaged -- the dup only maps the
 * name, it does not define liveness). Owner entries outside the mark set
 * are dead: invalidate the tagged ARC key, free the blocks (the retire-time
 * TEXT gate does not apply here -- freeing is the whole point), drop the
 * owner L2P map, and rewrite the owner record without them.
 * Returns the number of dead batches reclaimed, 0 = nothing, <0 = error. */
int vol_tz_gc(invfs_volume *v)
{
    tz_owner o;
    uint64_t owner;
    uint32_t *live = NULL;     /* sorted live batch_seqs */
    size_t n_live = 0;
    uint32_t i;
    size_t w;
    int freed = 0;
    int rc = 0;

    if (!v) return -1;
    if (v->sb.vol_flags & VOLF_V3) return tz_v3_gc(v);
    owner = vol_find(v, TZ_OWNER_NAME);
    if (!owner) return 0;
    if (tz_owner_load(v, owner, &o) != 0) return -1;
    if (o.n == 0) { tz_owner_free(&o); return 0; }
    if (!vol_write_enabled(v)) { tz_owner_free(&o); return 0; }

    /* mark: walk live records, collect block_ids of zone==TEXT entries.
     * WP47: through the shared mapper-aware walker (the legacy contiguous
     * loop saw zero records on a mapper volume -> empty mark -> batch GC
     * would free live batches). */
    {
        tz_gc_mark_ctx mc;
        int wrc;
        memset(&mc, 0, sizeof mc);
        mc.v = v;
        mc.owner = owner;
        wrc = vol_records_walk(v, tz_gc_mark_cb, &mc);
        live = mc.live;
        n_live = mc.n_live;
        if (wrc != 0) { rc = -1; goto out; }
    }
    /* sort for the membership queries below */
    if (n_live > 1)
        qsort(live, n_live, sizeof *live, tz_u32_cmp);

    /* sweep: drop owner entries no live member names */
    if (vol_mark_dirty(v) != 0) { rc = -1; goto out; }
    w = 0;
    for (i = 0; i < o.n; i++) {
        uint32_t seq = o.ents[i].block_id;
        int keep = 0;
        if (live) {
            size_t lo = 0, hi = n_live;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (live[mid] < seq) lo = mid + 1; else hi = mid;
            }
            keep = lo < n_live && live[lo] == seq;
        }
        if (keep) {
            o.ents[w++] = o.ents[i];
            continue;
        }
        {
            uint64_t pba = 0, plen = 0;
            /* WP52: the owner entry is SELF-DESCRIBING (WP27: e.pba names
             * the batch; e.length is the DECODED size). Free from that
             * directly -- the batch's physical span comes from its frame --
             * instead of relying on the owner-WAL map, which is gone for a
             * batch whose members were all deleted (the retire drops the
             * member's maps, not the batch's, but a rebuild/GC cycle can
             * leave the lookup empty and the old code then reported freed=0
             * while still dropping the entry, so the test saw "GC reclaimed
             * nothing" even though dead batches were reclaimed). Fall back
             * to the WAL lookup if the entry carries no pba. */
            pba = o.ents[i].pba;
            if (pba && pba < v->sb.total_blocks &&
                seg_extent(v, pba, NULL, &plen) == 0 && plen) {
                arc_invalidate(v->arc, pba | TZ_ARC_TAG);
                vol_free_blocks(v, pba, plen);
                freed++;
            } else if (vol_lookup_entry(v, owner, seq, &pba, &plen) == 0 &&
                       pba) {
                arc_invalidate(v->arc, pba | TZ_ARC_TAG);
                vol_free_blocks(v, pba, plen);
                freed++;
            }
            l2p_remove(v, owner, seq);
        }
    }
    o.n = (uint32_t)w;
    if (freed > 0 && tz_owner_write(v, owner, TZ_OWNER_NAME, &o) != 0) rc = -1;
out:
    free(live);
    tz_owner_free(&o);
    return rc ? rc : freed;
}
