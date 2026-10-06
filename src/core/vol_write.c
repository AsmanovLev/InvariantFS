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
    /* WP85: the metadata generation this session was opened against, and
     * the one-shot "already refused" latch (see wsession_stale). */
    uint64_t gen;
    int      stale;
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
    uint64_t *old_pbas;      /* WP-M8 v3: pbas owned by the old recipe */
    uint32_t old_n_ents;     /* WP-M8 v3: old entry count */
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
    if (!v || !name) return 0;
    for (s = v->wsessions; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return 1;
    return 0;
}

/* WP-M23: check active write session by inode id (both old_id and new_id) */
int vol_write_active_id(invfs_volume *v, uint64_t inode_id)
{
    const invfs_wsession *s;
    if (!v || !inode_id) return 0;
    for (s = v->wsessions; s; s = s->next)
        if (s->old_id == inode_id || s->new_id == inode_id)
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
    /* WP135: DELIBERATELY NO '!' CHECK HERE, and the reason is the same one
     * that keeps the check out of vol_write_bulk's caller and out of
     * vol_create_file: this function is LANE-CAPABLE. A transcode commits its
     * children through it, under the very names that carry the separator --
     * vol_cpack.c:3622 creates a "!mbrNNNN-<san>" member with vol_create_file,
     * which lands in vol_write_bulk, which calls this (:1043). A refusal
     * here silently stops every containerpack lane from decomposing anything;
     * tools/test-p7z-batch.sh reported "0 member siblings" for a 308-member
     * 7z when the check was tried here.
     *
     * It is also not a create: both FUSE callers of this function
     * (fuse_fs.c:1607 on an opened handle, :2787 on a resize) act on a name
     * that already EXISTS. Writing to a legacy '!' name must keep working --
     * that is how the data on such a volume gets copied off it. Creation is
     * refused at vol_replace_file, vol_replace_file_with_meta,
     * vol_create_file_with_meta, vol_create_symlink, vol_create_special,
     * vol_mkdir, vol_rename, vol_v3_hardlink, invf-cp's own site, and
     * the six FUSE name-introducing ops. */
    if (!vol_write_enabled(v)) return 0;
    s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->v = v;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->new_id = v->next_inode_id++;
    s->wheat_carry = 1;
    s->gen = v->write_gen;     /* WP85: anchor the session to the live
                                * generation; a rollback bumps v->write_gen
                                * and this session goes stale */
    {
        /* vol_find() collapses "no such name" and "the lookup could not be
         * completed" into the single value 0, because it returns a uint64_t
         * inode id. So a LOOKUP THAT FAILED left s->have_old == 0, and the
         * commit's retire loop -- guarded on `if (s->old_pbas)` -- never ran:
         * the write succeeded, read back perfectly, and the old recipe's
         * blocks stayed allocated under no reachable name. **Space, not
         * data**, which is why it hides: nothing is corrupt and nothing is
         * missing, the volume just quietly stopped giving blocks back.
         *
         * The distinction already exists -- vol_find_rc keeps 1/0/-1 apart,
         * and it exists precisely because this confusion has produced three
         * separate defects (the ACL fail-open, the rename overwrite, and the
         * name-table eviction). Use it here rather than widening vol_find,
         * which 102 call sites test against 0.
         *
         * An absent name (0) is NORMAL and is the only case that leaves
         * have_old clear. A FAILED lookup leaves the session unable to say
         * what it is superseding, so it refuses rather than proceed blind. */
        uint64_t old_id = 0;
        int frc = vol_find_rc(v, name, &old_id);
        if (frc == 1) {
            s->have_old = 1;
            s->old_id = old_id;
            s->truncating = truncate;
        } else if (frc < 0) {
            free(s);
            return 0;   /* could not establish what this write supersedes */
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


/* WP85 (INCIDENTS.md:734): refuse a session whose generation the volume has
 * rolled back, loudly and exactly once per session.
 *
 * Why refusal and not re-anchoring: a save point pins {base_root, delta_end}
 * and nothing else, while an uncommitted session's segments live in neither
 * (WP27: they ride the session's own entry table and never touch the
 * journal). So the moment a restore republishes the save point's root, the
 * session's bytes belong to a generation that no longer exists AND no live
 * recipe names -- re-anchoring would silently relocate data the rollback was
 * called to remove, and the old id it aliases is not the file any more.
 *
 * The first refusal also RETIRES the session: it leaves v->wsessions, so the
 * sweep guard (vol_write_active_name/_id) stops naming a session that can no
 * longer be committed. The blocks it wrote are not freed here -- vol_write_abort
 * does that on release, exactly as for any other uncommitted session, which
 * is what returns the space instead of leaking it. Every later call keeps
 * returning ESTALE, so a handle that writes again after the refusal cannot
 * slip through on a half-retired session.
 *
 * 0 = the session is still live (carry on), -ESTALE = refused. */
static int wsession_stale(invfs_wsession *s)
{
    if (!s || s->committed) return 0;
    if (s->gen == s->v->write_gen) return 0;
    if (!s->stale) {   /* one shot: retire + say so, once per session */
        s->stale = 1;
        wsession_unlink(s);
        fprintf(stderr, "invfs: %s: write session refused (ESTALE): the "
                "volume was rolled back under this handle\n", s->name);
    }
    return -ESTALE;
}


/* The old record supports segment-aliasing only when it is exactly what
 * vol_create_file / a prior session commit produces: no container
 * children, every entry a plain per-segment NONE/LZ4 chunk keyed by its
 * own index (WP23: or a ZSTD chunk -- the adaptive RAW effort writes
 * those under fill pressure). Anything else (PPMD/batched/container/
 * pack/whole-file blobs) goes through the materialize path instead. */
static int wsession_simple_old(const invfs_ast_hdr *ah,
                               const invfs_ast_block_entry *ents)
{
    uint32_t i;
    if (ah->num_children != 0) return 0;
    for (i = 0; i < ah->num_blocks; i++) {
        if (ents[i].length == 0 || ents[i].length > SEGMENT_SIZE ||
            ents[i].block_id != i || ents[i].block_offset != 0)
            return 0;
        if (ents[i].algo != INVFS_ALGO_NONE && ents[i].algo != INVFS_ALGO_LZ4 &&
            ents[i].algo != INVFS_ALGO_ZSTD)
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


/* ---- WP23: adaptive RAW-zone compression effort ----------------------
 * Per-64K-segment codec choice at WRITE time, keyed on the RAW zone's
 * fill pressure (v->raw_free is maintained incrementally by alloc/free,
 * so the check is two loads and a divide):
 *
 *     RAW fill < 80%   ->  LZ4      (the historical default, unchanged)
 *     RAW fill >= 80%  ->  ZSTD level 3
 *     RAW fill >= 95%  ->  ZSTD level 6
 *
 * Rationale: throttle-time-into-compression. Past 80% the volume is
 * climbing doc/12's watermark ladder anyway -- writers are about to be
 * throttled and the sweep woken, so the writer's CPU is the cheap
 * resource. Spend it making the segment smaller: every block the codec
 * shaves off is a block the RAW zone still has.
 *
 * Precedence: an explicit operator choice beats pressure, always --
 * INVFS_PROFILE=turbo still stores verbatim (NONE) without even a codec
 * attempt, and INVFS_RAW_ADAPT=0 pins the legacy LZ4-only behaviour.
 * A segment that does not shrink under the chosen codec falls back to
 * verbatim NONE, exactly like the LZ4 path before it.
 *
 * On disk there is nothing new: the AST entry's algo field already
 * carries the per-segment codec and the read path dispatches per
 * segment, so one file may freely mix NONE/LZ4/ZSTD segments -- a write
 * that crosses the 80% line mid-file is representable by construction. */
static const struct { unsigned fill_pct; int zlevel; } raw_effort_ladder[] = {
    { 95, 6 },   /* highest pressure first: first match wins */
    { 80, 3 },
};

/* INVFS_RAW_ADAPT=0 opts out (default ON). Parsed once per process -- the
 * same single-volume discipline the INVFS_PROFILE parse documents (the
 * tools hold one volume per process, so the env IS the volume's
 * context). */
static int raw_adapt_enabled(void)
{
    static int memo = -1;
    if (memo < 0) {
        const char *ra = getenv("INVFS_RAW_ADAPT");
        memo = !(ra && strcmp(ra, "0") == 0);
    }
    return memo;
}

/* The ZSTD effort level the current RAW-zone fill asks for; 0 = stay
 * with LZ4. */
static int raw_effort_zlevel(const invfs_volume *v)
{
    uint64_t total = v->sb.raw_zone_blocks;
    unsigned pct;
    size_t i;
    if (!raw_adapt_enabled() || total == 0) return 0;
    /* RAW exhausted means the segment overflows into shadow-space blocks
     * (WP-DZ: 100% pressure, still raw-class): maximum effort either way
     * -- the smaller the segment, the less it costs wherever it lands. */
    pct = (unsigned)(((total - v->raw_free) * 100) / total);
    for (i = 0; i < sizeof raw_effort_ladder / sizeof raw_effort_ladder[0]; i++)
        if (pct >= raw_effort_ladder[i].fill_pct)
            return raw_effort_ladder[i].zlevel;
    return 0;
}


/* Encode + write + register one segment for the session: enc_len plaintext
 * bytes (the entry's length -- the read path decompresses with length as
 * the output cap, so the stored payload must decode to exactly enc_len).
 * In-session writes always pass SEGMENT_SIZE; the commit's tail fix
 * passes the exact final tail. WP27: the segment's address lives in the
 * session's own entry (s->ents[j].pba); nothing touches the journal. On
 * re-touch the session's previous pba is freed only after the new one is
 * fully written.
 * 0 ok, -1 io/logic error, -2 ENOSPC. */
static int wsession_write_seg_n(invfs_wsession *s, uint32_t j,
                                const uint8_t *plain, size_t enc_len)
{
    invfs_volume *v = s->v;
    /* one buffer serves both codecs: ZSTD's bound covers LZ4's for every
     * size this path encodes (<= 64 KB), but take the max explicitly --
     * the bound is a contract, not a benchmark result */
    int cbound = LZ4_compressBound((int)enc_len);
    uint8_t *cbuf;
    uint8_t hdr[8];
    uint32_t csize = 0, seg_crc;
    uint64_t pba, phys_blocks, prev_pba = 0;
    int zone, algo_used = INVFS_ALGO_NONE, have_prev = 0;
    {
        size_t zb = ZSTD_compressBound(enc_len);
        if (zb > (size_t)cbound) cbound = (int)zb;
    }

    if (wsession_grow(s, j + 1) != 0) return -1;
    cbuf = malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
    if (!cbuf) return -1;
    if (v->profile != INVFS_PROFILE_TURBO) {
        int zlevel = raw_effort_zlevel(v);
        if (getenv("INVFS_DEBUG_FILE")) {
            FILE *df = fopen(getenv("INVFS_DEBUG_FILE"), "a");
            if (df) { fprintf(df, "[dbg] wseg %s seg %u profile=%u zlevel=%d fill=%llu/%llu\n",
                        s->name, j, v->profile, zlevel,
                        (unsigned long long)(v->sb.raw_zone_blocks - v->raw_free),
                        (unsigned long long)v->sb.raw_zone_blocks); fclose(df); }
        }
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[rawadapt] %s seg %u: raw fill %llu/%llu, zlevel %d\n",
                    s->name, j,
                    (unsigned long long)(v->sb.raw_zone_blocks - v->raw_free),
                    (unsigned long long)v->sb.raw_zone_blocks, zlevel);
        if (zlevel) {
            /* pressure rung (WP23): ZSTD at the ladder's level. A rung
             * that cannot shrink the segment means LZ4 would not either
             * -- skip the retry, store verbatim below. */
            size_t zr = ZSTD_compress((char *)(cbuf + 8), (size_t)cbound,
                                      (const char *)plain, enc_len, zlevel);
            if (!ZSTD_isError(zr) && zr < (size_t)enc_len) {
                csize = (uint32_t)zr;
                algo_used = INVFS_ALGO_ZSTD;
            }
        } else {
            csize = (uint32_t)LZ4_compress_default((const char *)plain,
                                                   (char *)(cbuf + 8),
                                                   (int)enc_len, cbound);
            if (csize != 0 && csize < (uint32_t)enc_len)
                algo_used = INVFS_ALGO_LZ4;
        }
    }
    if (algo_used == INVFS_ALGO_NONE) {
        csize = (uint32_t)enc_len;
        memcpy(cbuf + 8, plain, enc_len);
    }
    seg_crc = invfs_crc32c(cbuf + 8, csize);
    hdr[0]=(uint8_t)(csize&0xFF); hdr[1]=(uint8_t)((csize>>8)&0xFF);
    hdr[2]=(uint8_t)((csize>>16)&0xFF); hdr[3]=(uint8_t)((csize>>24)&0xFF);
    hdr[4]=(uint8_t)(seg_crc&0xFF); hdr[5]=(uint8_t)((seg_crc>>8)&0xFF);
    hdr[6]=(uint8_t)((seg_crc>>16)&0xFF); hdr[7]=(uint8_t)((seg_crc>>24)&0xFF);
    memcpy(cbuf, hdr, 8);

    phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) /
                  INVFS_BLOCK_SIZE;
    /* A re-touch supersedes the session's OWN earlier write of j -- free
     * it once the replacement is down. An ALIASED segment's pba belongs to
     * the old record: never freed here (the commit's retire drops the old
     * record's reference and frees exactly what the new record does not
     * keep). */
    if (j < s->touched_cap && s->touched[j] && s->ents[j].pba) {
        prev_pba = s->ents[j].pba;
        have_prev = 1;
    }
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
    /* superseded session segment: nothing references it once the entry
     * moves, so free it now instead of leaving it for fsck */
    if (have_prev) {
        uint64_t plen = 0;
        if (seg_extent_checked(v, prev_pba, &plen) == 0)
            vol_free_blocks(v, prev_pba, plen);
    }

    s->ents[j].file_offset = (uint64_t)j * SEGMENT_SIZE;
    s->ents[j].length = (uint64_t)enc_len;
    s->ents[j].zone = (uint32_t)zone;
    s->ents[j].algo = (uint32_t)algo_used;
    s->ents[j].block_id = j;
    s->ents[j].block_offset = 0;
    s->ents[j].pba = pba;
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
 * zeros beyond both. WP27: both resolve through the session's own entry
 * table -- a touched entry names the session's fresh pba, an aliased one
 * still carries the old record's pba (its block stays alive until the
 * commit's retire; the sweep is locked out of this file by
 * vol_write_active_name). 0 ok, -1 unreadable. */
static int wsession_seg_current(invfs_wsession *s, uint32_t j,
                                uint8_t *plain)
{
    memset(plain, 0, SEGMENT_SIZE);
    if (j >= s->n_ents) return 0;
    if ((j < s->touched_cap && s->touched[j]) ||
        (s->have_old && j < s->aliased_n)) {
        uint64_t pba = s->ents[j].pba;
        uint32_t csize = 0;
        uint8_t *blob = NULL;
        int rc;
        if (!pba || pba >= s->v->sb.total_blocks)
            return -1;
        if (seg_read_checked(s->v, pba, 0, 1, &csize, &blob) != 0)
            return -1;
        if (s->ents[j].algo == INVFS_ALGO_LZ4) {
            /* in-session payloads are whole-segment encodes (the tail
             * fix runs at commit, after the last possible read-back);
             * an aliased segment is the old record's plain per-segment
             * chunk (wsession_simple_old guaranteed the shape) */
            rc = LZ4_decompress_safe((const char *)blob, (char *)plain,
                                     (int)csize, (int)SEGMENT_SIZE)
                 == (int)s->ents[j].length ? 0 : -1;
            if (rc == 0 && s->ents[j].length < SEGMENT_SIZE)
                memset(plain + s->ents[j].length, 0,
                       SEGMENT_SIZE - (size_t)s->ents[j].length);
        } else if (s->ents[j].algo == INVFS_ALGO_ZSTD) {
            /* WP23: this session's own pressure-rung segment, or an
             * aliased adaptive-effort segment of the old record */
            size_t zg = ZSTD_decompress((char *)plain, SEGMENT_SIZE,
                                        blob, csize);
            rc = (!ZSTD_isError(zg) && zg == s->ents[j].length) ? 0 : -1;
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


/* WP-M8: load a v3 file's previous content into the session from its
 * content-addressed recipe blob. Mirrors wsession_load_old's contract:
 * entries are copied into s->ents and aliased (their pbas stay owned by the
 * old recipe until commit, which frees the dropped ones). A missing/empty
 * recipe is an empty old file. */
static int wsession_load_old_v3(invfs_wsession *s)
{
    static const uint8_t zero_addr[INVFS_RECIPE_ADDR_LEN];
    invfs_inode in;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t nents = 0, i;
    uint8_t *blob = NULL;
    size_t blen = 0;

    if (!s->have_old)
        return 0;
    if (vol_inode_get(s->v, s->old_id, &in) != 1)
        return -1;
    s->old_size = in.size;
    s->wheat_carry = 1;   /* heat is not part of the v3 row (WP-M9) */
    if (in.size == 0 ||
        memcmp(in.recipe_addr, zero_addr, INVFS_RECIPE_ADDR_LEN) == 0)
        return 0;         /* empty old content: nothing to alias */

    if (vol_recipe_load(s->v, in.recipe_addr, &blob, &blen) != 0)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &nents) != 0) {
        free(blob);
        return -1;
    }
    if (ah.file_size != in.size) {
        free(blob);
        return -1;        /* row/blob disagreement: corruption */
    }

    /* Does the file being replaced own "name!..." siblings? The v2 loader
     * answers the same question off the old record (vol_write.c:583) and
     * the v2 commit retires them (:1131); on v3 they are real dirents, so
     * the answer has to come from the recipe blob instead -- off the SAME
     * ast_owns_siblings rule, or the two formats answer differently for
     * the same file and the v3 one strands the siblings. Asked BEFORE the
     * row is republished: this is the old content's recipe. */
    s->owns_siblings = ast_owns_siblings(&ah, ents, nents);

    /* snapshot the old pbas so commit can free whatever the new recipe
     * drops. WP78: TEXT entries name SHARED batch segments owned by the
     * batch registry, so they are recorded as 0 (never freed here) --
     * tz_gc reclaims a dead batch once no live member names it. */
    s->old_n_ents = (uint32_t)nents;
    if (nents) {
        s->old_pbas = (uint64_t *)malloc(nents * sizeof(uint64_t));
        if (!s->old_pbas) { free(blob); return -1; }
        for (i = 0; i < nents; i++)
            s->old_pbas[i] = (ents[i].zone == INVFS_ZONE_TEXT)
                           ? 0 : ents[i].pba;
    }
    if (s->truncating) {   /* content dropped; commit frees the old pbas */
        free(blob);
        return 0;
    }

    s->logical_size = ah.file_size;
    s->n_ents = (uint32_t)nents;
    if (nents) {
        s->cap_ents = (uint32_t)nents;
        s->ents = (invfs_ast_block_entry *)malloc(s->cap_ents * sizeof(*s->ents));
        s->touched_cap = (uint32_t)nents;
        s->touched = (uint8_t *)calloc(s->touched_cap, 1);
        if (!s->ents || !s->touched) { free(blob); return -1; }
        memcpy(s->ents, ents, nents * sizeof(*s->ents));
        if (wsession_simple_old(&ah, s->ents)) {
            s->aliased_n = (uint32_t)nents;
            s->mat_upto = (uint32_t)nents;
        } else {
            /* WP78: batched (TEXT) / container / whole-file content -- a
             * write is an implicit downgrade to RAW, exactly like the v2
             * path: re-read through the real read path in 64K windows and
             * rebuild each as a fresh session segment. Aliasing the old
             * entry while materializing touched ranges would leave an
             * overlapping recipe (a TEXT entry covers the whole file). */
            uint32_t nseg = (uint32_t)((s->old_size + SEGMENT_SIZE - 1) /
                                       SEGMENT_SIZE);
            uint8_t *plain = malloc(SEGMENT_SIZE);
            if (!plain) { free(blob); return -1; }
            fprintf(stderr, "[wsession] %s: write to swept/container file: "
                    "materializing %u segment(s) to RAW\n", s->name, nseg);
            s->n_ents = 0;
            s->aliased_n = 0;
            s->mat_upto = 0;
            for (i = 0; i < nseg; i++) {
                uint64_t off = (uint64_t)i * SEGMENT_SIZE;
                uint64_t room = s->old_size - off;
                size_t want = (size_t)(room < SEGMENT_SIZE ? room : SEGMENT_SIZE);
                int got, rc;
                memset(plain, 0, SEGMENT_SIZE);
                got = vol_read_range(s->v, s->old_id, off, want, plain);
                if (got != (int)want) { free(plain); free(blob); return -1; }
                rc = wsession_write_seg(s, i, plain);
                if (rc != 0) { free(plain); free(blob); return rc; }
            }
            free(plain);
        }
    }
    free(blob);
    return 0;
}

static int wsession_load_old(invfs_wsession *s)
{
    if (s->loaded) return 0;
    s->loaded = 1;
    /* WP-M8: a v3 file's previous content lives in an immutable recipe
     * blob, not a record. Load it into the session's entry table so
     * ranged writes/truncate alias and re-encode the way they always did. */
    return wsession_load_old_v3(s);
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
    if (offset + len > MAX_FILE_SIZE) return -1;   /* format sanity bound;
            the v1->v2 recipe header choice happens at commit */
    /* WP85: refuse BEFORE touching the recipe or the segment writer. */
    rc = wsession_stale(s);
    if (rc != 0) return rc;
    rc = wsession_load_old(s);
    if (rc != 0) return rc;

    first = (uint32_t)(offset / SEGMENT_SIZE);
    last  = (uint32_t)((offset + len - 1) / SEGMENT_SIZE);
    if ((uint64_t)last + 1 > MAX_SEGMENTS_V2) return -1;

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
    if (len > MAX_FILE_SIZE) return -1;
    /* WP85: ftruncate through a handle the rollback retired is the same
     * write as an append -- refuse it the same way. */
    rc = wsession_stale(s);
    if (rc != 0) return rc;
    rc = wsession_load_old(s);
    if (rc != 0) return rc;
    if (len == s->logical_size) return 0;
    keep = (uint32_t)((len + SEGMENT_SIZE - 1) / SEGMENT_SIZE);
    if (keep > MAX_SEGMENTS_V2) return -1;

    if (keep < s->n_ents) {
        /* shrink: drop tail segments. Session-owned pbas are freed;
         * aliases are only dropped from the entry table -- the old record
         * keeps owning them (the commit's retire frees the ones the new
         * record dropped). WP27: the pba rides in the session's entry. */
        for (j = keep; j < s->n_ents; j++) {
            if (j < s->touched_cap && s->touched[j] && s->ents[j].pba) {
                uint64_t plen = 0;
                if (seg_extent_checked(s->v, s->ents[j].pba, &plen) == 0)
                    vol_free_blocks(s->v, s->ents[j].pba, plen);
                /* WP-M9: the session's pba is gone -- clear the entry so a
                 * later extend/write of this segment cannot free it a second
                 * time (the shrink+extend matrix hits exactly this). */
                s->ents[j].pba = 0;
                s->touched[j] = 0;
            }
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
    /* WP85: a stale handle must not serve pre-rollback bytes either -- the
     * caller surfaces ESTALE, and a fresh open reads the live generation. */
    rc = wsession_stale(s);
    if (rc != 0) return rc;
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


/* WP-M8: commit a v3 write session. The session's segment table becomes an
 * immutable, content-addressed recipe blob; the inode row behind the name
 * gets the new address + size (same inode id, so the dirent is untouched).
 * Data segments already landed through the normal write path. No delta,
 * no v2 record: the base tree is the authority. */
static int vol_write_commit_v3(invfs_wsession *s)
{
    invfs_volume *v = s->v;
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    invfs_inode in;
    uint64_t id, now = (uint64_t)time(NULL);
    uint32_t i, k;
    int rc;

    if (s->logical_size > MAX_FILE_SIZE || s->n_ents > MAX_SEGMENTS_V2)
        return -1;
    /* Drop any segment that lives entirely past the logical end, then
     * re-encode an unaligned tail at its exact length -- the same two
     * fixes the v2 commit applies, so entry.length always equals the
     * decoded payload size the reader expects. */
    while (s->n_ents > 0 &&
           (uint64_t)(s->n_ents - 1) * SEGMENT_SIZE >= s->logical_size) {
        uint32_t j = s->n_ents - 1;
        if (j < s->touched_cap && s->touched[j] && s->ents[j].pba) {
            uint64_t plen = 0;
            if (seg_extent_checked(v, s->ents[j].pba, &plen) == 0)
                vol_free_blocks(v, s->ents[j].pba, plen);
        }
        s->n_ents--;
    }
    if (s->n_ents > 0) {
        uint32_t j = s->n_ents - 1;
        uint64_t base = (uint64_t)j * SEGMENT_SIZE;
        uint64_t tail = s->logical_size - base;
        if (tail > SEGMENT_SIZE) tail = SEGMENT_SIZE;
        if (s->ents[j].length != tail) {
            uint8_t plain[SEGMENT_SIZE];
            rc = wsession_seg_current(s, j, plain);
            if (rc != 0) return rc;
            rc = wsession_write_seg_n(s, j, plain, (size_t)tail);
            if (rc != 0) return rc;
        }
    }
    /* An empty file has no content address: the read path keys off size 0,
     * so storing a header-only blob would just leak a page. */
    if (s->logical_size == 0) {
        memset(addr, 0, sizeof addr);
    } else {
        if (vol_ast_recipe_serialize(s->logical_size, s->ents, s->n_ents,
                                     &blob, &blen) != 0)
            return -1;   /* TODO(WP-M9): recipe larger than one base page
                          * (multi-page/streamed blob) -- deferred. */
        if (vol_recipe_store(v, blob, blen, addr) != 0) {
            free(blob);
            return -1;
        }
        free(blob);
    }

    if (s->have_old) {
        /* in-place update: the row keeps its id, so the dirent (and any
         * hardlink) stays pointed at it. */
        if (vol_inode_get(v, s->old_id, &in) != 1)
            return -1;
        id = s->old_id;
        in.size = s->logical_size;
        memcpy(in.recipe_addr, addr, INVFS_RECIPE_ADDR_LEN);
        memset(&in.recipe, 0, sizeof in.recipe);
        if (in.nlink == 0)
            in.nlink = 1;
        in.mtime = now;
        if (vol_inode_delta_put(v, id, &in) != 0)
            return -1;
    } else {
        /* new name: row (with content) first, then the dirent */
        id = vol_create_content_node(v, s->name, s->logical_size, addr);
        if (!id)
            return -1;
    }

    /* Free old data segments the new recipe no longer names. A segment
     * the session re-wrote has a fresh pba; its OLD pba still belongs to
     * the retired recipe and was never freed by wsession_write_seg_n (that
     * only frees the session's own superseded writes), so it must be freed
     * here whenever the new entry table does not keep it. The superseded
     * recipe blob page itself is left for WP-M15. */
    if (s->old_pbas) {
        /* WP78: the new recipe is already published, so a fresh ref map
         * (TEXT batch pbas are excluded by the v3 walker) tells us whether
         * any other live file still names each dropped pba -- a dedupe
         * share or a sibling batch member must survive one file's rewrite. */
        pba_ref_reset(v);
        pba_ref_ensure(v);
        for (i = 0; i < s->old_n_ents; i++) {
            uint64_t pba = s->old_pbas[i];
            int keep = 0;
            if (!pba)
                continue;
            for (k = 0; k < i; k++)
                if (s->old_pbas[k] == pba) { keep = 1; break; }
            if (!keep)
                for (k = 0; k < s->n_ents; k++)
                    if (s->ents[k].pba == pba) { keep = 1; break; }
            if (!keep && pba_ref_count(v, pba) == 0) {
                uint64_t plen = 0;
                if (seg_extent_checked(v, pba, &plen) == 0)
                    vol_free_blocks(v, pba, plen);
            }
        }
    }
    free(s->old_pbas);
    s->old_pbas = NULL;

    /* Retire the superseded content's "name!..." siblings -- the leg the v2
     * commit has always had (vol_write_commit:1131, gated on the same
     * ast_owns_siblings answer the v3 loader now takes at wsession_load_old_v3).
     *
     * A decomposed container keeps its payload in sibling INODES on v3 too
     * (vol_create_tar_file -> vol_create_blob_file("%s!part%u")), so without
     * this a v3 overwrite of a swept a.tar leaves a.tar!part0..N live for
     * ever: the sweep walks but never retires an internal '!' name, and
     * spn_reclaim cannot free a block a live recipe names -- unlike the
     * old recipe's own segments, which the loop above does free. The new
     * row is already published and the siblings are separate inodes, so
     * this cannot affect what `name` reads back. */
    if (s->owns_siblings)
        vol_delete_siblings(v, s->name);

    wsession_unlink(s);
    s->committed = 1;
    return 0;
}

int vol_write_commit(invfs_wsession *ws)
{
    invfs_wsession *s = ws;
    invfs_volume *v = ws ? ws->v : NULL;
    int rc;

    if (!s || s->committed) return -1;
    /* WP85: the commit is the last chance to publish the retired
     * generation's segments as a live recipe. A refused session never gets
     * here -- the caller aborts it, which frees the segments it wrote. */
    rc = wsession_stale(s);
    if (rc != 0) return rc;
    rc = wsession_load_old(s);
    if (rc != 0) return rc;
    /* WP-M8: v3 content lives in a recipe blob + inode row, not a record.
     * The guard is a precondition on the session, not a format choice: vol_open
     * refuses a non-v3 volume, so this only fires on a malformed session. */
    if (v && (v->sb.vol_flags & VOLF_META))
        return vol_write_commit_v3(s);
    return -1;
}



void vol_write_abort(invfs_wsession *ws)
{
    invfs_wsession *s = ws;
    uint32_t i;
    if (!s) return;
    wsession_unlink(s);   /* drop the sweep guard BEFORE freeing: the volume
                           * list must never name a dead session */
    if (!s->committed) {
        /* free exactly the segments this session itself wrote (touched);
         * aliased entries belong to the old record, which stays live */
        for (i = 0; s->touched && i < s->n_ents && i < s->touched_cap; i++) {
            if (s->touched[i] && s->ents[i].pba) {
                uint64_t plen = 0;
                if (seg_extent_checked(s->v, s->ents[i].pba, &plen) == 0)
                    vol_free_blocks(s->v, s->ents[i].pba, plen);
            }
        }
    }
    free(s->ents);
    free(s->touched);
    free(s->old_ext);
    free(s->old_pbas);
    free(s);
}


/* WP-M21b: bulk content write on a v3 volume through the WP-M9 session
 * path. This is the missing glue between the offline CLI tools (invf-cp,
 * invf-import -- they call the whole-buffer vol_create_file/replace_file
 * family) and the v3 metadata model: the WP-M6 stubs answered 0 for
 * "content on v3", so every offline write failed with a misleading
 * "write failed (volume full?)". Order mirrors the FUSE create+write
 * pattern the M9 legs exercised: node first (dirent + inode row), then
 * the streaming session (begin/range/commit). meta, when given, is
 * applied after the commit (the row rewrite is a delta append; commit
 * carries the created row's fields, set_meta is the authoritative
 * last word for mode/uid/gid/times). Returns the live inode id or 0. */
uint64_t vol_write_bulk(invfs_volume *v, const char *name,
                           const uint8_t *data, size_t len,
                           const invfs_meta_pub *meta)
{
    invfs_wsession *ws = NULL;
    uint64_t nid;
    int rc;

    if (!v || !name || !(v->sb.vol_flags & VOLF_META)) return 0;
    if (len > MAX_FILE_SIZE) {
        fprintf(stderr, "invarifs: %s: %llu bytes exceeds the format "
                "limit (%llu bytes)\n", name, (unsigned long long)len,
                (unsigned long long)MAX_FILE_SIZE);
        return 0;
    }
    if (!vol_write_enabled(v)) {
        fprintf(stderr, "invarifs: volume is read-only%s -- write refused "
                "(EROFS)\n", v->degraded ? " (DEGRADED)" : "");
        return 0;
    }
    /* THE DECISION, and the one place on this path where getting it wrong
     * loses bytes rather than space.
     *
     * This used to be:
     *
     *     nid = vol_find(v, name);
     *     if (!nid) { nid = vol_create_node(v, name, meta); ... }
     *
     * and vol_find returns a uint64_t, so "there is no such name" and "the
     * lookup could not be COMPLETED" are both the value 0. On the second one
     * this takes the CREATE branch -- and create-on-v3 is not "add a name",
     * it is "install an EMPTY node over whatever was there": same inode id,
     * recipe address zeroed, old recipe's blocks freed
     * (src/core/vol_dirs.c:340-344 and :365-367). So if vol_write_begin
     * then fails (:998), or the range write fails (:1000), or len == 0
     * (:1000 writes nothing at all), the name's PRIOR CONTENT IS GONE with
     * nothing written back. That is the whole of invf-import's and
     * invf-cp's write path.
     *
     * NOTE THAT :80/:99 DOES NOT COVER THIS. vol_write_begin's vol_find_rc
     * (WP "7791a87") is a different call in a different function and it runs
     * at :998 -- strictly AFTER vol_create_node has already emptied the
     * row and released the blocks. By the time it is asked, the content it
     * was supposed to supersede is already gone; no amount of correctness
     * there can put it back.
     *
     * So the distinction has to be made HERE, at the call that decides:
     * a lookup that could not be completed is not entitled to assert that
     * the name is absent. That is the same asymmetry 42ea0a6 fixed in
     * table_sync_one_locked, and it cuts the same way -- the only safe
     * answer to "I do not know what is there" is to change nothing. */
    {
        uint64_t found = 0;
        int frc = vol_find_rc(v, name, &found);
        if (frc < 0) {
            fprintf(stderr, "invarifs: %s: the name's lookup did not COMPLETE "
                    "(rc=%d), so it is not known whether the name is absent. "
                    "A failed read is not entitled to assert that the name is "
                    "not there, and taking the create path would empty the "
                    "name's existing inode and free its blocks before a "
                    "single byte was written. Nothing was written; whatever "
                    "the name held is untouched.\n", name, frc);
            return 0;
        }
        nid = found;
    }
    if (!nid) {
        nid = vol_create_node(v, name, meta);
        if (!nid) return 0;
    }
    if (vol_write_begin(v, name, 1, &ws) == 0 || !ws)
        return 0;
    rc = len ? vol_write_range(ws, 0, data, len) : 0;
    if (rc != 0) {
        fprintf(stderr, "invarifs: %s: v3 write session failed (rc=%d%s)\n",
                name, rc, rc == -2 ? ", ENOSPC" : "");
        vol_write_abort(ws);
        return 0;
    }
    if (vol_write_commit(ws) != 0) {
        vol_write_abort(ws);
        return 0;
    }
    if (meta && vol_set_meta(v, name, meta) == 0)
        fprintf(stderr, "invarifs: %s: warning: metadata apply failed "
                "(content is committed)\n", name);
    /* The content is DURABLE at this point. The re-lookup exists only to hand
     * back the live inode id, and it used to be a bare vol_find, so a lookup
     * that could not be completed returned 0 -- telling invf-cp and
     * invf-import that a write which IS on the volume did not happen, and
     * (via cp.c:99's "write failed (volume full?)") blaming the disk for a
     * read error. The uncertainty here is about the ANSWER, not about the
     * write, so it is reported rather than collapsed, and the id this call
     * already established is returned. `nid` is that id in both branches: on
     * the present path it is the row the commit updated in place
     * (vol_write_commit_v3, s->have_old), and on the create path it is the
     * row vol_create_node installed, which vol_write_begin then resolved
     * and the commit updated. */
    {
        uint64_t done = 0;
        int frc = vol_find_rc(v, name, &done);
        if (frc == 1 && done)
            return done;
        fprintf(stderr, "invarifs: %s: warning: the content is committed, but "
                "the confirming re-lookup did not COMPLETE (rc=%d), so the "
                "inode id is the one this write established (%llu), not a "
                "freshly read one\n", name, frc, (unsigned long long)nid);
        return nid;
    }
}
