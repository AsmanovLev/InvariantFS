/* vol_write.c — WP4b incremental ranged-write sessions.
 * Split from volume.c. */

#include "volume_internal.h"

typedef struct invfs_wsession invfs_wsession;

struct invfs_wsession {
    invfs_volume *v;
    char     name[256];
    uint64_t new_id, old_id, old_size;
    int      have_old, committed, loaded, truncating;
    int      owns_siblings;
    uint8_t  wheat_carry;    /* write-heat for session-born segments */
    uint8_t *old_ext;
    uint32_t old_ext_len;
    invfs_ast_block_entry *ents;
    uint32_t n_ents, cap_ents;
    uint32_t aliased_n;      /* [0,aliased_n): (new_id,i) aliases old pba */
    uint8_t *touched;        /* [i] = segment i rewritten by this session */
    uint32_t touched_cap;
    uint32_t mat_upto;       /* highest contiguously materialized seg +1 */
    uint64_t logical_size;
    int      dirty;          /* any write/truncate landed */
    invfs_wsession *next;    /* v->wsessions link (live sessions) */
};


/* 1 while a session for `name` is mid-flight: sweeps (all entry points)
 * skip such a file. The session has forked the file's segment layout --
 * a transcode retiring the old record mid-session would drop the old
 * id's L2P maps the session's aliases resolve through, and the in-place
 * sweep path would free blocks the aliases still name. */
int vol_write_active_name(invfs_volume *v, const char *name)
{
    const invfs_wsession *s;
    for (s = v->wsessions; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return 1;
    return 0;
}


uint64_t vol_write_begin(invfs_volume *v, const char *name, int truncate,
                         invfs_wsession **out)
{
    invfs_wsession *s;
    if (!out) return 0;
    *out = NULL;
    if (!v || !name || name_too_long(name)) return 0;
    if (!vol_write_enabled(v)) return 0;
    s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->v = v;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->new_id = v->next_inode_id++;
    s->wheat_carry = 1;
    {
        uint64_t old_id = vol_find(v, name);
        if (old_id != 0) {
            s->have_old = 1;
            s->old_id = old_id;
            s->truncating = truncate;
        }
    }
    s->next = v->wsessions;
    v->wsessions = s;
    *out = s;
    return s->new_id;
}


/* drop the session from the volume's live list (commit/abort) */
static void wsession_unlink(invfs_wsession *s)
{
    invfs_wsession **pp = &s->v->wsessions;
    while (*pp) {
        if (*pp == s) { *pp = s->next; s->next = NULL; return; }
        pp = &(*pp)->next;
    }
}


/* The old record supports segment-aliasing only when it is exactly what
 * vol_create_file / a prior session commit produces: no container
 * children, every entry a plain per-segment NONE/LZ4 chunk keyed by its
 * own index. Anything else (ZSTD/PPMD/batched/container/pack/whole-file
 * blobs) goes through the materialize path instead. */
static int wsession_simple_old(const invfs_ast_recipe_header *ah,
                               const invfs_ast_block_entry *ents)
{
    uint32_t i;
    if (ah->num_children != 0) return 0;
    for (i = 0; i < ah->num_blocks; i++) {
        if (ents[i].length == 0 || ents[i].length > SEGMENT_SIZE ||
            ents[i].block_id != i || ents[i].block_offset != 0)
            return 0;
        if (ents[i].algo != INVFS_ALGO_NONE && ents[i].algo != INVFS_ALGO_LZ4)
            return 0;
    }
    return 1;
}


static int wsession_grow(invfs_wsession *s, uint32_t count)
{
    if (count > s->cap_ents) {
        uint32_t nc = s->cap_ents ? s->cap_ents : 64;
        invfs_ast_block_entry *ne;
        while (nc < count) nc *= 2;
        ne = realloc(s->ents, nc * sizeof(*ne));
        if (!ne) return -1;
        memset(ne + s->cap_ents, 0, (nc - s->cap_ents) * sizeof(*ne));
        s->ents = ne;
        s->cap_ents = nc;
    }
    if (count > s->touched_cap) {
        uint8_t *nt = realloc(s->touched, count);
        if (!nt) return -1;
        memset(nt + s->touched_cap, 0, count - s->touched_cap);
        s->touched = nt;
        s->touched_cap = count;
    }
    return 0;
}


/* Encode + write + map one segment for the session: enc_len plaintext
 * bytes (the entry's length -- the read path decompresses with length as
 * the output cap, so the stored payload must decode to exactly enc_len).
 * In-session writes always pass SEGMENT_SIZE; the commit's tail fix
 * passes the exact final tail. On re-touch the session's previous pba is
 * freed only after the new one is fully written and mapped.
 * 0 ok, -1 io/logic error, -2 ENOSPC. */
static int wsession_write_seg_n(invfs_wsession *s, uint32_t j,
                                const uint8_t *plain, size_t enc_len)
{
    invfs_volume *v = s->v;
    int cbound = LZ4_compressBound((int)enc_len);
    uint8_t *cbuf;
    uint8_t hdr[8];
    uint32_t csize = 0, seg_crc;
    uint64_t pba, phys_blocks, prev_pba = 0, prev_len = 0;
    int zone, lz4_used = 0, have_prev = 0;

    if (wsession_grow(s, j + 1) != 0) return -1;
    cbuf = malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
    if (!cbuf) return -1;
    if (v->profile != INVFS_PROFILE_TURBO)
        csize = (uint32_t)LZ4_compress_default((const char *)plain,
                                               (char *)(cbuf + 8),
                                               (int)enc_len, cbound);
    if (csize == 0 || csize >= (uint32_t)enc_len) {
        csize = (uint32_t)enc_len;
        memcpy(cbuf + 8, plain, enc_len);
    } else {
        lz4_used = 1;
    }
    seg_crc = invfs_crc32c(cbuf + 8, csize);
    hdr[0]=(uint8_t)(csize&0xFF); hdr[1]=(uint8_t)((csize>>8)&0xFF);
    hdr[2]=(uint8_t)((csize>>16)&0xFF); hdr[3]=(uint8_t)((csize>>24)&0xFF);
    hdr[4]=(uint8_t)(seg_crc&0xFF); hdr[5]=(uint8_t)((seg_crc>>8)&0xFF);
    hdr[6]=(uint8_t)((seg_crc>>16)&0xFF); hdr[7]=(uint8_t)((seg_crc>>24)&0xFF);
    memcpy(cbuf, hdr, 8);

    phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) /
                  INVFS_BLOCK_SIZE;
    if (j < s->touched_cap && s->touched[j] &&
        vol_lookup_entry(v, s->new_id, j, &prev_pba, &prev_len) == 0 &&
        prev_pba)
        have_prev = 1;
    pba = alloc_raw_or_shadow(v, phys_blocks, &zone);
    if (pba == 0) { free(cbuf); return -2; }
    if (write_segment_blocks(v, pba, cbuf, (size_t)csize + 8,
                             phys_blocks) != 0) {
        fprintf(stderr, "[wsession] seg %u write fail\n", j);
        vol_free_blocks(v, pba, phys_blocks);
        free(cbuf);
        return -1;
    }
    free(cbuf);
    /* re-mapping must REPLACE, not pile up: a superseded (new_id,j) entry
     * would sit in the journal forever and its pba would be "referenced"
     * at retire time -- an orphan block leak (newest-wins hides it from
     * reads, but not from the bitmap accounting) */
    l2p_remove(v, s->new_id, j);
    if (vol_map(v, s->new_id, j, pba, (uint32_t)phys_blocks) != 0) {
        vol_free_blocks(v, pba, phys_blocks);
        return -1;
    }
    /* WP19: session-born segments start read-cold (INVFS_HEAT_INIT may
     * pre-warm) and carry the file's write history (old max + 1); the
     * untouched aliases kept their old entries' heat at load time */
    {
        invfs_l2p_entry *ne = &v->l2p[v->l2p_count - 1];
        l2p_set_rheat(ne, v->heat_init);
        ne->pad[2] = s->wheat_carry;
    }
    /* superseded session segment: nothing references it once the new map
     * landed, so free it now instead of leaving it for fsck */
    if (have_prev)
        vol_free_blocks(v, prev_pba, prev_len);

    s->ents[j].file_offset = (uint64_t)j * SEGMENT_SIZE;
    s->ents[j].length = (uint64_t)enc_len;
    s->ents[j].zone = (uint32_t)zone;
    s->ents[j].algo = lz4_used ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
    s->ents[j].block_id = j;
    s->ents[j].block_offset = 0;
    if (j >= s->n_ents) s->n_ents = j + 1;
    s->touched[j] = 1;
    if (j + 1 > s->mat_upto) s->mat_upto = j + 1;
    s->dirty = 1;
    return 0;
}


static int wsession_write_seg(invfs_wsession *s, uint32_t j,
                              const uint8_t *plain)
{
    return wsession_write_seg_n(s, j, plain, SEGMENT_SIZE);
}


/* Current plaintext of segment j, zero-padded to SEGMENT_SIZE: the
 * session's own rewrite when touched, the old file's bytes when aliased,
 * zeros beyond both. 0 ok, -1 unreadable. */
static int wsession_seg_current(invfs_wsession *s, uint32_t j,
                                uint8_t *plain)
{
    memset(plain, 0, SEGMENT_SIZE);
    if (j >= s->n_ents) return 0;
    if (j < s->touched_cap && s->touched[j]) {
        uint64_t pba = 0, plen = 0;
        uint32_t csize = 0;
        uint8_t *blob = NULL;
        int rc;
        if (vol_lookup_entry(s->v, s->new_id, j, &pba, &plen) != 0 || !pba)
            return -1;
        if (seg_read_checked(s->v, pba, plen, 1, &csize, &blob) != 0)
            return -1;
        if (s->ents[j].algo == INVFS_ALGO_LZ4) {
            /* in-session payloads are whole-segment encodes (the tail
             * fix runs at commit, after the last possible read-back) */
            rc = LZ4_decompress_safe((const char *)blob, (char *)plain,
                                     (int)csize, (int)SEGMENT_SIZE)
                 == (int)s->ents[j].length ? 0 : -1;
            if (rc == 0 && s->ents[j].length < SEGMENT_SIZE)
                memset(plain + s->ents[j].length, 0,
                       SEGMENT_SIZE - (size_t)s->ents[j].length);
        } else {
            rc = csize == s->ents[j].length ? 0 : -1;
            if (rc == 0) memcpy(plain, blob, csize);
            if (rc == 0 && csize < SEGMENT_SIZE)
                memset(plain + csize, 0, SEGMENT_SIZE - csize);
        }
        free(blob);
        return rc;
    }
    if (s->have_old && j < s->aliased_n) {
        /* Aliased segment: read through the session's OWN (new_id,j) map.
         * Same pba the old record uses, but independent of the old
         * record's lifetime: a concurrent retire of old_id (unlink while
         * the handle is open -- the sweep is session-guarded) drops the
         * old id's maps, and resolving through old_id would miss or,
         * worse, the map could be gone after its blocks were reallocated.
         * ents[j] is the old record's entry (plain NONE/LZ4, its own
         * index -- wsession_simple_old guaranteed it). */
        uint64_t pba = 0, plen = 0;
        uint32_t csize = 0;
        uint8_t *blob = NULL;
        int rc;
        if (vol_lookup_entry(s->v, s->new_id, j, &pba, &plen) != 0 || !pba)
            return -1;
        if (seg_read_checked(s->v, pba, plen, 1, &csize, &blob) != 0)
            return -1;
        if (s->ents[j].algo == INVFS_ALGO_LZ4) {
            rc = LZ4_decompress_safe((const char *)blob, (char *)plain,
                                     (int)csize, (int)SEGMENT_SIZE)
                 == (int)s->ents[j].length ? 0 : -1;
        } else {
            rc = csize == s->ents[j].length ? 0 : -1;
            if (rc == 0) memcpy(plain, blob, csize);
        }
        free(blob);
        return rc;
    }
    return 0;
}


/* materialize zero segments [from,to) so no unmapped LBA ever exists */
static int wsession_zero_fill(invfs_wsession *s, uint32_t from, uint32_t to)
{
    uint32_t k;
    uint8_t *zbuf;
    int rc = 0;
    if (to <= from) return 0;
    zbuf = calloc(1, SEGMENT_SIZE);
    if (!zbuf) return -1;
    for (k = from; k < to; k++) {
        rc = wsession_write_seg(s, k, zbuf);
        if (rc != 0) break;
    }
    free(zbuf);
    return rc;
}


/* segments [0, filled) are readable in-session (aliased or written) */
static uint32_t wsession_filled(const invfs_wsession *s)
{
    return s->mat_upto > s->aliased_n ? s->mat_upto : s->aliased_n;
}


static int wsession_load_old(invfs_wsession *s)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0, i;
    invfs_ast_recipe_header ah;
    const uint8_t *ext;
    size_t ext_len = 0;
    size_t base = sizeof(invfs_inode_rec);

    if (s->loaded) return 0;
    s->loaded = 1;
    if (!s->have_old) return 0;
    if (meta_read_record_by_id(s->v, s->old_id, &buf, &rl, NULL, 0,
                               NULL) != 0)
        return -1;
    if (rl < base + sizeof(ah)) { free(buf); return -1; }
    memcpy(&ah, buf + base, sizeof(ah));
    if (rl < base + sizeof(ah) + (size_t)ah.num_blocks * sizeof(*s->ents)) {
        free(buf);
        return -1;
    }
    s->old_size = ah.file_size;
    s->owns_siblings = record_owns_siblings(buf, rl);
    /* WP19: write-heat is the one counter a rewrite must not reset --
     * carry max(old)+1 onto this session's born segments (aliased
     * segments keep their own entries' heat instead) */
    {
        uint8_t w = heat_file_maxw(s->v, s->old_id);
        s->wheat_carry = (w == 0xFF) ? 0xFF : (uint8_t)(w + 1);
    }
    ext = meta_locate_ext(buf, rl, &ext_len);
    if (ext && ext_len && ext_len <= 0xFFFF) {
        s->old_ext = malloc(ext_len);
        if (s->old_ext) {
            memcpy(s->old_ext, ext, ext_len);
            s->old_ext_len = (uint32_t)ext_len;
        }
    }
    if (s->truncating) { free(buf); return 0; }   /* content dropped */

    s->logical_size = ah.file_size;
    s->n_ents = ah.num_blocks;
    if (s->n_ents) {
        s->cap_ents = s->n_ents;
        s->ents = malloc(s->cap_ents * sizeof(*s->ents));
        if (!s->ents) { free(buf); return -1; }
        memcpy(s->ents, buf + base + sizeof(ah), s->n_ents * sizeof(*s->ents));
        s->touched_cap = s->n_ents;
        s->touched = calloc(s->touched_cap, 1);
        if (!s->touched) { free(buf); return -1; }
    }
    if (wsession_simple_old(&ah, s->ents)) {
        /* alias old segments into the new id's L2P (no data copies) */
        for (i = 0; i < s->n_ents; i++) {
            uint64_t pba = 0, plen = 0;
            uint16_t r;
            uint8_t w;
            if (vol_lookup_entry(s->v, s->old_id, i, &pba, &plen) != 0 ||
                !pba) {
                free(buf);
                return -1;   /* L2P hole: cannot fork safely */
            }
            heat_grab(s->v, s->old_id, i, &r, &w);
            if (vol_map(s->v, s->new_id, i, pba, (uint32_t)plen) != 0) {
                free(buf);
                return -1;
            }
            heat_stamp(s->v, s->new_id, i, r, w);
        }
        s->aliased_n = s->n_ents;
    } else if (s->n_ents) {
        /* swept / container / batched content: a write is an implicit
         * downgrade to RAW. Re-read the whole file through the real read
         * path in 64K windows and rewrite each as a fresh session
         * segment; bounded memory (one 64K bounce buffer). NOTE the loop
         * count is derived from the file SIZE, not the old AST shape: a
         * batched member has a single entry covering the whole file.
         * The old record keeps owning its blocks until commit, and its
         * "name!..." siblings die there. */
        uint32_t nseg = (uint32_t)((s->old_size + SEGMENT_SIZE - 1) /
                                   SEGMENT_SIZE);
        uint8_t *plain = malloc(SEGMENT_SIZE);
        if (!plain) { free(buf); return -1; }
        fprintf(stderr, "[wsession] %s: write to swept/container file: "
                "materializing %u segment(s) to RAW\n", s->name, nseg);
        s->n_ents = 0;   /* re-built segment-by-segment below */
        for (i = 0; i < nseg; i++) {
            uint64_t off = (uint64_t)i * SEGMENT_SIZE;
            uint64_t room = s->old_size - off;
            size_t want = (size_t)(room < SEGMENT_SIZE ? room : SEGMENT_SIZE);
            int got, rc;
            memset(plain, 0, SEGMENT_SIZE);
            got = vol_read_range(s->v, s->old_id, off, want, plain);
            if (got != (int)want) { free(plain); free(buf); return -1; }
            rc = wsession_write_seg(s, i, plain);
            if (rc != 0) { free(plain); free(buf); return rc; }
        }
        free(plain);
    }
    free(buf);
    return 0;
}


int vol_write_range(invfs_wsession *ws, uint64_t offset,
                    const uint8_t *data, size_t len)
{
    invfs_wsession *s = ws;
    uint32_t first, last, j;
    size_t done = 0;
    int rc;

    if (!s || s->committed || !data) return -1;
    if (len == 0) return 0;
    if (offset + len > 0xFFFFFFFFull) return -1;   /* u32 file_size cap */
    rc = wsession_load_old(s);
    if (rc != 0) return rc;

    first = (uint32_t)(offset / SEGMENT_SIZE);
    last  = (uint32_t)((offset + len - 1) / SEGMENT_SIZE);
    if ((uint64_t)last + 1 > MAX_SEGMENTS) return -1;

    /* Atomic ENOSPC precheck, same shape as vol_create_file: every
     * not-yet-materialized segment this call touches (plus any zero-fill
     * gap) may need a full uncompressed slot. Refuse the whole call
     * before writing anything when the worst case cannot fit above the
     * reserve. */
    {
        uint32_t filled = wsession_filled(s);
        uint64_t fresh = 0, need;
        if (first > filled) fresh += first - filled;
        for (j = first; j <= last; j++)
            if (!(j < s->touched_cap && s->touched[j])) fresh++;
        need = fresh * (SEGMENT_SIZE / INVFS_BLOCK_SIZE + 1);
        if (s->v->free_blocks <= s->v->sb.reserved_blocks +
                                 s->v->sb.hard_min_blocks + need)
            return -2;
        if (first > filled) {
            rc = wsession_zero_fill(s, filled, first);
            if (rc != 0) return rc;
        }
    }

    for (j = first; j <= last && done < len; j++) {
        size_t base = (size_t)j * SEGMENT_SIZE;
        size_t seg_off = (size_t)(offset + done - base);
        size_t take = SEGMENT_SIZE - seg_off;
        if (take > len - done) take = len - done;

        if (seg_off == 0 && take == SEGMENT_SIZE) {
            /* full segment: no read-modify-write needed */
            rc = wsession_write_seg(s, j, data + done);
        } else {
            uint8_t plain[SEGMENT_SIZE];
            rc = wsession_seg_current(s, j, plain);
            if (rc == 0) {
                memcpy(plain + seg_off, data + done, take);
                rc = wsession_write_seg(s, j, plain);
            }
        }
        if (rc != 0) return rc;
        {
            uint64_t dend = (uint64_t)j * SEGMENT_SIZE + seg_off + take;
            if (dend > s->logical_size) s->logical_size = dend;
        }
        done += take;
    }
    return 0;
}


int vol_write_truncate(invfs_wsession *ws, uint64_t len)
{
    invfs_wsession *s = ws;
    uint32_t keep, j, filled;
    int rc;

    if (!s || s->committed) return -1;
    if (len > 0xFFFFFFFFull) return -1;
    rc = wsession_load_old(s);
    if (rc != 0) return rc;
    if (len == s->logical_size) return 0;
    keep = (uint32_t)((len + SEGMENT_SIZE - 1) / SEGMENT_SIZE);
    if (keep > MAX_SEGMENTS) return -1;

    if (keep < s->n_ents) {
        /* shrink: drop tail segments. Session-owned pbas are freed;
         * aliases are only unmapped -- the old record keeps owning them
         * (the commit's retire frees the ones the new record dropped). */
        for (j = keep; j < s->n_ents; j++) {
            uint64_t pba = 0, plen = 0;
            if (j < s->touched_cap && s->touched[j] &&
                vol_lookup_entry(s->v, s->new_id, j, &pba, &plen) == 0 &&
                pba)
                vol_free_blocks(s->v, pba, plen);
            l2p_remove(s->v, s->new_id, j);
        }
        s->n_ents = keep;
        if (s->mat_upto > keep) s->mat_upto = keep;
        if (s->aliased_n > keep) s->aliased_n = keep;
    }
    /* A mid-segment cut zeroes the tail of the surviving segment: bytes
     * past the cut must read as zeros, never resurrected old content.
     * The same applies at the old end of an extend (the hole is zeros). */
    {
        uint64_t cut = len < s->logical_size ? len : s->logical_size;
        if (cut % SEGMENT_SIZE != 0) {
            uint32_t j0 = (uint32_t)(cut / SEGMENT_SIZE);
            if (j0 < s->n_ents) {
                uint8_t plain[SEGMENT_SIZE];
                rc = wsession_seg_current(s, j0, plain);
                if (rc != 0) return rc;
                memset(plain + (size_t)(cut % SEGMENT_SIZE), 0,
                       SEGMENT_SIZE - (size_t)(cut % SEGMENT_SIZE));
                rc = wsession_write_seg(s, j0, plain);
                if (rc != 0) return rc;
            }
        }
    }
    if (len > s->logical_size) {
        filled = wsession_filled(s);
        if (keep > filled) {
            uint64_t need = (uint64_t)(keep - filled) *
                            (SEGMENT_SIZE / INVFS_BLOCK_SIZE + 1);
            if (s->v->free_blocks <= s->v->sb.reserved_blocks +
                                     s->v->sb.hard_min_blocks + need)
                return -2;
            rc = wsession_zero_fill(s, filled, keep);
            if (rc != 0) return rc;
        }
    }
    s->logical_size = len;
    s->dirty = 1;
    return 0;
}


/* Read from the session's CURRENT logical view (uncommitted): each
 * segment comes from wsession_seg_current -- the session's own rewrite
 * when touched, the old bytes when aliased, zeros in a covered hole.
 * This is what lets the FUSE read path serve data that .write has
 * accepted but not yet committed. Returns bytes read, -1 on error. */
int vol_write_read(invfs_wsession *ws, uint64_t offset,
                   uint8_t *buf, size_t len)
{
    invfs_wsession *s = ws;
    size_t done = 0;
    int rc;

    if (!s || s->committed || !buf) return -1;
    rc = wsession_load_old(s);
    if (rc != 0) return rc;
    if (offset >= s->logical_size) return 0;
    if (offset + len > s->logical_size) len = (size_t)(s->logical_size - offset);
    while (done < len) {
        uint64_t off = offset + done;
        uint32_t j = (uint32_t)(off / SEGMENT_SIZE);
        size_t soff = (size_t)(off % SEGMENT_SIZE);
        size_t take = SEGMENT_SIZE - soff;
        uint8_t plain[SEGMENT_SIZE];
        if (take > len - done) take = len - done;
        rc = wsession_seg_current(s, j, plain);
        if (rc != 0) return -1;
        memcpy(buf + done, plain + soff, take);
        done += take;
    }
    return (int)done;
}


int vol_write_commit(invfs_wsession *ws)
{
    invfs_wsession *s = ws;
    invfs_volume *v = ws ? ws->v : NULL;
    size_t rec_size;
    uint8_t *rec;
    invfs_inode_rec *rh;
    invfs_ast_recipe_header ah;
    uint32_t crc_rec;
    uint64_t now = (uint64_t)time(NULL);
    uint64_t cur;
    int rc;

    if (!s || s->committed) return -1;
    rc = wsession_load_old(s);
    if (rc != 0) return rc;
    if (s->logical_size > 0xFFFFFFFFull || s->n_ents > MAX_SEGMENTS)
        return -1;

    /* no segment may live entirely past the logical end (defensive;
     * vol_write_truncate already drops them) */
    while (s->n_ents > 0 &&
           (uint64_t)(s->n_ents - 1) * SEGMENT_SIZE >= s->logical_size) {
        uint32_t j = s->n_ents - 1;
        uint64_t pba = 0, plen = 0;
        if (j < s->touched_cap && s->touched[j] &&
            vol_lookup_entry(v, s->new_id, j, &pba, &plen) == 0 && pba)
            vol_free_blocks(v, pba, plen);
        l2p_remove(v, s->new_id, j);
        s->n_ents--;
    }
    /* entry.length is the decoded-payload size (the read path
     * decompresses with length as the output cap). Session segments are
     * whole-64K encodes, so the last segment of a file whose size is not
     * segment-aligned is re-encoded once here at its exact tail length. */
    if (s->n_ents > 0) {
        uint32_t j = s->n_ents - 1;
        uint64_t base = (uint64_t)j * SEGMENT_SIZE;
        uint64_t tail = s->logical_size - base;   /* in 1..SEGMENT_SIZE */
        if (tail > SEGMENT_SIZE) tail = SEGMENT_SIZE;
        if (s->ents[j].length != tail) {
            uint8_t plain[SEGMENT_SIZE];
            rc = wsession_seg_current(s, j, plain);
            if (rc != 0) return rc;
            rc = wsession_write_seg_n(s, j, plain, (size_t)tail);
            if (rc != 0) return rc;
        }
    }

    memset(&ah, 0, sizeof(ah));
    ah.version = 1;
    ah.file_size = (uint32_t)s->logical_size;
    ah.num_blocks = (uint16_t)s->n_ents;

    rec_size = sizeof(invfs_inode_rec) + sizeof(ah)
             + (size_t)s->n_ents * sizeof(invfs_ast_block_entry)
             + s->old_ext_len;
    rec = calloc(1, rec_size);
    if (!rec) return -1;
    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = s->new_id;
    rh->file_size = s->logical_size;
    rh->ctime = now;
    rec_set_name(rh, s->name);
    memcpy(rec + sizeof(invfs_inode_rec), &ah, sizeof(ah));
    if (s->n_ents)
        memcpy(rec + sizeof(invfs_inode_rec) + sizeof(ah), s->ents,
               (size_t)s->n_ents * sizeof(invfs_ast_block_entry));
    if (s->old_ext_len)
        memcpy(rec + rec_size - s->old_ext_len, s->old_ext, s->old_ext_len);
    crc_rec = invfs_crc32c(rec, rec_size);

    /* room for the record AND the retire tombstone(s) that must follow:
     * running out between the two would strand the old version live */
    if (v->inode_area_pos + rec_size + 4 +
        (s->have_old ? 2 * (sizeof(invfs_inode_rec) + 4) : 0)
            > v->inode_area_end) {
        free(rec);
        return -2;
    }
    /* the version live RIGHT NOW, before our record lands: normally the
     * one begin() saw, but a concurrent session may have committed a
     * third version in between -- it must be retired too (last writer
     * wins, and its blocks must not leak) */
    cur = vol_find(v, s->name);

    if (vol_pre_record(v) != 0 ||
        io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc_rec, 4) != 0) {
        free(rec);
        return -1;
    }
    v->inode_area_pos += rec_size + 4;
    idx_put(v, s->name, strlen(s->name), s->new_id,
            v->inode_area_pos - rec_size - 4, s->logical_size, now);
    idx_put_id(v, s->new_id, v->inode_area_pos - rec_size - 4);
    free(rec);

    /* The vol_replace_file ordering: the new record is live first, then
     * the old one is retired. vol_delete_inode frees exactly the old
     * blocks no live mapping references any more (the new record re-owns
     * the aliased ones; dedupe-shared and TEXT-batch blocks are guarded)
     * and tombstones the old record. */
    if (s->have_old && vol_delete_inode(v, s->old_id, s->name) != 0)
        fprintf(stderr, "[wsession] %s: old-version retire failed; "
                "fsck will reconcile\n", s->name);
    if (cur && cur != s->old_id && cur != s->new_id) {
        int cur_sibs = 1;   /* unknown record: scan conservatively */
        uint8_t *cb = NULL;
        uint32_t crl = 0;
        if (meta_read_record_by_id(v, cur, &cb, &crl, NULL, 0, NULL) == 0)
            cur_sibs = record_owns_siblings(cb, crl);
        free(cb);
        vol_delete_inode(v, cur, s->name);
        if (cur_sibs) vol_delete_siblings(v, s->name);
    }
    if (s->owns_siblings)
        vol_delete_siblings(v, s->name);
    /* committed: the new record owns everything the session mapped, so the
     * sweep guard no longer needs to see this session. (abort unlinks too;
     * the unlink is idempotent.) */
    wsession_unlink(s);
    s->committed = 1;
    return 0;
}


void vol_write_abort(invfs_wsession *ws)
{
    invfs_wsession *s = ws;
    uint32_t i;
    if (!s) return;
    wsession_unlink(s);   /* drop the sweep guard BEFORE freeing: the volume
                           * list must never name a dead session */
    if (!s->committed) {
        for (i = 0; s->touched && i < s->n_ents && i < s->touched_cap; i++) {
            uint64_t pba = 0, plen = 0;
            if (s->touched[i] &&
                vol_lookup_entry(s->v, s->new_id, i, &pba, &plen) == 0 && pba) {
                vol_free_blocks(s->v, pba, plen);
                l2p_remove(s->v, s->new_id, i);
            }
        }
        for (i = 0; i < s->aliased_n; i++)
            l2p_remove(s->v, s->new_id, i);
    }
    free(s->ents);
    free(s->touched);
    free(s->old_ext);
    free(s);
}
