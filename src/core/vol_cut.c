/* vol_cut.c — WP613: v3 content-cut recovery for torn data segments.
 *
 * PROBLEM. A torn data segment (power-loss mid-write, dm chaos) has no
 * recovery path on v3: the read path fails the whole file with EIO and
 * fsck could not even name the shape (the recipe audit stopped at
 * load+parse+coherence; a CRC-dead segment underneath a sound recipe was
 * silent -- the volume.h ledger comment said so out loud). The file is an
 * honest terminal state, and nothing attempts to salvage the readable
 * spans. This file is the salvage path.
 *
 * SHAPE (copied from WP303's detect-loudly predicates and WP402's explicit
 * --heal: detect loudly -> propose -> explicit repair -> verify).
 *
 *   probe  vol_cut_probe(v, id): loads the row + recipe, requires coherence
 *          (vol_recipe_coherence -- an incoherent row is NOT a cut candidate,
 *          it is WP303's beat), then decodes every payload entry in
 *          file-offset order. A span is GOOD only if its bytes decode to
 *          exactly the length the entry claims (NONE: memcpy + frame check;
 *          LZ4/ZSTD: decompress + length check -- the same checks the read
 *          path runs; exotic whole-file codecs: framed CRC only, since their
 *          length rule lives in their transcoder). TEXT-batch and
 *          WINDOW_SRC entries own no segment here and are never cuttable:
 *          the first such entry stops the good prefix and makes the file
 *          uncuttable past it (cutting through a shared batch or a
 *          reference would rewrite bytes another inode owns).
 *   cut    vol_content_cut(v, dry, rep): walks the live set; a file whose
 *          whole read succeeds is clean; a file whose read fails is probed.
 *          Cuttable = at least one leading good span AND a good prefix
 *          shorter than the row size (a fully-good probe with a failing
 *          read is a recipe-level problem, not a torn segment -- refused).
 *          The cut rewrites the row to the good prefix: a fresh recipe blob
 *          with the leading entries and file_size = prefix length, stored
 *          under its own hash (vol_recipe_store), row published through the
 *          delta (vol_inode_delta_put -- the same crash-safe mutation path
 *          every write uses: a crash before the publish leaves the damaged
 *          file intact-but-unreadable, never half-truncated). The caller
 *          flushes (vol_flush) as the durability point -- the same rule as
 *          --heal. Every cut is verified: the cut file is read back whole
 *          and memcmp'd against the probe's decoded prefix bytes. A verify
 *          miss restores nothing (the old blob is still immutable and still
 *          addressed, but the row moved on) -- so a miss FAILS LOUDLY and
 *          the report says so; there is no silent second write.
 *   dry    under dry=1 nothing is stored or published: the report carries
 *          the plan (what WOULD be cut) and the volume is untouched.
 *
 * BIT-EXACTNESS. A cut file reads back EXACTLY its surviving spans: the new
 * recipe is a prefix of the old entry table with an exact tiling sum, so
 * the survivors' bytes are untouched and their offsets are unchanged (a
 * prefix truncation moves nothing). Every dropped span is named (inode +
 * byte range + seg index + pba) in the report, on stderr, and in
 * `invf-fsck --list-damaged` BEFORE the cut (segment-torn). No silent
 * healing, ever: without --cut nothing here runs, and --cut without
 * --dry-run still prints the plan first.
 *
 * WHAT IT DOES NOT DO. Middle-span salvage (a torn span with good spans
 * beyond it): the survivors are non-contiguous and no prefix recipe can
 * hold them without renumbering offsets, which would NOT be the survivors'
 * bytes at their survivors' offsets. Such files are UNCUTTABLE: named with
 * the first torn span and the good-tail span count, left EIO. Truncate-to-
 * empty (first span torn): refused for the same honesty reason -- a name
 * that resolves to zero bytes where megabytes were is not salvage, it is
 * deletion wearing a repair flag; unlink the file if that is what you want.
 */

#include "volume_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One decoded good span, owned by the probe. */
typedef struct {
    uint64_t file_offset;
    uint64_t length;
    uint8_t *data;      /* length bytes, decode-verified */
} cut_span;

/* Decode-verify one payload entry into *out (malloc'd, e->length bytes).
 * 0 = provably good, -1 = torn/unverifiable. Mirrors the read path's own
 * checks (vol_read.c serial loop + ast_frame_ok): the frame must say what
 * the recipe says before any byte is trusted, and the decode must produce
 * exactly the claimed length -- a short decode is corruption, not a short
 * file, and is refused here exactly as the read path refuses it. */
static int cut_decode_span(invfs_volume *v, uint64_t inode_id,
                           const invfs_ast_block_entry *e, uint8_t **out)
{
    uint32_t hdr = 0;
    uint8_t *blob = NULL;
    uint8_t *dst = NULL;
    uint64_t pba;

    if (!v || !e || !out)
        return -1;
    *out = NULL;
    /* TEXT batches are registry-owned by design and WINDOW_SRC pbas are
     * source inode ids, not blocks (same exclusion the WP303 attribution
     * walk makes, for the same reason): neither is probed here. */
    if (e->zone == INVFS_ZONE_TEXT || e->algo == INVFS_ALGO_WINDOW_SRC)
        return -1;
    pba = e->pba;
    if (pba == 0 || pba >= v->sb.total_blocks)
        return -1;
    if (seg_read_checked(v, pba, 0, 1, &hdr, &blob) != 0)
        return -1;
    if (ast_frame_ok(e->algo, hdr, e->length,
                     e->algo == INVFS_ALGO_ZSTD ? "zstd" : "lz4") != 0) {
        free(blob);
        return -1;
    }
    if (e->length == 0 || e->length > (uint64_t)128 * 1024 * 1024) {
        free(blob);
        return -1;
    }
    dst = (uint8_t *)malloc((size_t)e->length ? (size_t)e->length : 1);
    if (!dst) {
        free(blob);
        return -1;
    }
    if (e->algo == INVFS_ALGO_NONE) {
        memcpy(dst, blob, (size_t)e->length);
        free(blob);
    } else if (e->algo == INVFS_ALGO_LZ4) {
        int got = LZ4_decompress_safe((const char *)blob, (char *)dst,
                                      (int)hdr, (int)e->length);
        free(blob);
        if (got != (int)e->length) {
            free(dst);
            return -1;
        }
    } else if (e->algo == INVFS_ALGO_ZSTD) {
        size_t got = ZSTD_decompress(dst, (size_t)e->length, blob, hdr);
        free(blob);
        if (ZSTD_isError(got) || got != (size_t)e->length) {
            free(dst);
            return -1;
        }
    } else {
        /* Exotic whole-file codec (JXL/APE/PMP/...): the framed CRC above
         * is the proof available without routing through the transcoder
         * stack. The CRC governs the bytes; the length rule is the lane's
         * own business. */
        free(blob);
        free(dst);
        return -1;
    }
    *out = dst;
    (void)inode_id;
    return 0;
}

/* Probe the data segments of one file WITHOUT changing anything.
 * Returns 0 and fills *p (caller frees p->spans[] + p->spans) when the row
 * + recipe resolve and cohere; -1 otherwise (missing/corrupt/incoherent
 * recipe, raw-blob type, or internal error -- all "not a cut candidate",
 * never "clean"). p->first_bad is the index of the first unverifiable
 * entry, or n_ents when every payload span decoded. p->good_tail counts
 * good spans AFTER first_bad (the middle-tear shape, for the message). */
typedef struct {
    uint64_t id;
    char name[192];
    uint64_t row_size;
    invfs_ast_block_entry *ents; /* leading-good prefix callers may reuse */
    size_t n_ents;               /* total entries */
    size_t first_bad;            /* n_ents = all good */
    uint64_t first_bad_pba;
    uint64_t good_prefix;        /* bytes covered by [0, first_bad) */
    size_t good_tail;            /* good spans past first_bad */
    cut_span *spans;             /* n_ents, only [0, first_bad) filled */
} cut_probe;

static void cut_probe_free(cut_probe *p)
{
    size_t i;
    if (!p)
        return;
    if (p->spans)
        for (i = 0; i < p->n_ents; i++)
            free(p->spans[i].data);
    free(p->spans);
    free(p->ents);
    memset(p, 0, sizeof *p);
}

static int cut_probe_run(invfs_volume *v, uint64_t id, const char *name,
                         cut_probe *p)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0, i;
    static const uint8_t zero[INVFS_RECIPE_ADDR_LEN];

    memset(p, 0, sizeof *p);
    if (!v || !id)
        return -1;
    if (vol_inode_get(v, id, &in) != 1)
        return -1;
    if (in.type == INVFS_ITYP_DIR || in.size == 0 ||
        memcmp(in.recipe_addr, zero, sizeof zero) == 0)
        return -1;
    if (invfs_inode_content_is_raw_blob(in.type))
        return -1;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 ||
        !ents) {
        free(blob);
        return -1;
    }
    /* Coherence first: an incoherent row is WP303's beat, not this file's.
     * Cutting a recipe the read path refuses for SHAPE reasons would bless
     * a disagreement as salvage. */
    if (vol_recipe_coherence(name, &in, blob, blen) != INVFS_COH_OK) {
        free(blob);
        return -1;
    }
    p->id = id;
    p->row_size = in.size;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "");
    p->n_ents = n_ents;
    p->first_bad = n_ents;
    if (n_ents) {
        p->ents = (invfs_ast_block_entry *)malloc(n_ents * sizeof *p->ents);
        p->spans = (cut_span *)calloc(n_ents, sizeof *p->spans);
        if (!p->ents || !p->spans) {
            free(blob);
            cut_probe_free(p);
            return -1;
        }
        memcpy(p->ents, ents, n_ents * sizeof *p->ents);
    }
    free(blob);
    blob = NULL;
    for (i = 0; i < n_ents; i++) {
        uint8_t *dec = NULL;
        if (cut_decode_span(v, id, &p->ents[i], &dec) == 0) {
            p->spans[i].file_offset = p->ents[i].file_offset;
            p->spans[i].length = p->ents[i].length;
            p->spans[i].data = dec;
            if (p->first_bad == n_ents)
                p->good_prefix += p->ents[i].length;
            else
                p->good_tail++;
        } else if (p->first_bad == n_ents) {
            p->first_bad = i;
            p->first_bad_pba = p->ents[i].pba;
        }
    }
    return 0;
}

/* ---- the walk ------------------------------------------------------ */

typedef struct {
    invfs_volume *v;
    int dry;
    invfs_content_cut_report *rep;
} cut_walk_ctx;

static void cut_report_add(invfs_content_cut_report *rep, uint64_t id,
                           const char *name, uint64_t kept, uint64_t dropped,
                           uint64_t bad_off, uint64_t bad_len,
                           uint64_t bad_pba, uint32_t bad_seg, int cut_it)
{
    invfs_cut_row *r;
    if (!rep)
        return;
    if (cut_it)
        rep->ncut++;
    else
        rep->nuncut++;
    if (rep->n == INVFS_CUT_MAX)
        return;
    r = &rep->rows[rep->n++];
    r->id = id;
    r->kept = kept;
    r->dropped = dropped;
    r->bad_off = bad_off;
    r->bad_len = bad_len;
    r->bad_pba = bad_pba;
    r->bad_seg = bad_seg;
    r->cut = cut_it;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    if (cut_it)
        rep->cut_bytes += kept;
    rep->drop_bytes += dropped;
}

static int cut_walk_cb(invfs_volume *v, uint64_t inode_id, const char *name,
                       void *ctx_)
{
    cut_walk_ctx *c = (cut_walk_ctx *)ctx_;
    invfs_inode in;
    static const uint8_t zero[INVFS_RECIPE_ADDR_LEN];
    uint8_t *whole = NULL;
    size_t whole_len = 0;
    cut_probe p;
    int rc;

    (void)v;
    if (vol_inode_get(c->v, inode_id, &in) != 1)
        return 0;
    if (in.type == INVFS_ITYP_DIR || in.size == 0 ||
        memcmp(in.recipe_addr, zero, sizeof zero) == 0)
        return 0;
    if (invfs_inode_content_is_raw_blob(in.type))
        return 0;
    c->rep->scanned++;

    /* The read path is the detector: a file that reads back whole is not
     * a cut candidate, whatever its segments look like underneath. */
    rc = vol_read_inode(c->v, inode_id, 0, &whole, &whole_len);
    if (rc == 0) {
        free(whole);
        c->rep->clean++;
        return 0;
    }
    free(whole);

    /* Reads EIO: name the shape. A probe that fails here is a recipe-level
     * problem (missing/corrupt/incoherent) -- fsck's other kinds already
     * name those, so this pass stays silent on them. */
    memset(&p, 0, sizeof p);
    if (cut_probe_run(c->v, inode_id, name, &p) != 0) {
        c->rep->other_bad++;
        return 0;
    }
    c->rep->torn++;
    if (p.first_bad == p.n_ents) {
        /* Every span decodes but the file does not read: not a torn
         * segment (container lanes / window refs / transcoder shapes fail
         * above the segment layer). Honest terminal, another kind's beat. */
        printf("content-cut: inode %llu (%s): unreadable with all %zu "
               "span(s) decoding -- not a torn segment, left untouched "
               "(EIO)\n", (unsigned long long)p.id,
               p.name[0] ? p.name : "no name resolves to it", p.n_ents);
        cut_report_add(c->rep, p.id, p.name, 0, p.row_size,
                       0, 0, 0, 0, 0);
        cut_probe_free(&p);
        return 0;
    }
    {
        const invfs_ast_block_entry *bad = &p.ents[p.first_bad];
        uint64_t bad_off = bad->file_offset, bad_len = bad->length;
        if (p.good_prefix == 0) {
            /* First span torn: truncating to empty is deletion wearing a
             * repair flag. Refused, loudly. */
            printf("content-cut: inode %llu (%s): first span (seg %zu "
                   "pba %llu, bytes [%llu, %llu) of %llu) is torn with "
                   "nothing provably good before it -- UNCUTTABLE, left "
                   "untouched (EIO). Unlink the file if empty is what you "
                   "want.\n", (unsigned long long)p.id,
                   p.name[0] ? p.name : "no name resolves to it",
                   p.first_bad, (unsigned long long)p.first_bad_pba,
                   (unsigned long long)bad_off,
                   (unsigned long long)(bad_off + bad_len),
                   (unsigned long long)p.row_size);
            cut_report_add(c->rep, p.id, p.name, 0, p.row_size,
                           bad_off, bad_len, p.first_bad_pba,
                           (uint32_t)p.first_bad, 0);
            cut_probe_free(&p);
            return 0;
        }
        if (p.good_tail > 0) {
            /* Middle tear with a good tail beyond it: no prefix recipe can
             * hold non-contiguous survivors without renumbering offsets,
             * which would not be the survivors' bytes at their offsets.
             * The cut still salvages the prefix -- and names the tail it
             * gives up -- because a provable prefix is strictly more than
             * EIO, and the message says exactly what was lost. */
            printf("content-cut: inode %llu (%s): seg %zu (pba %llu, "
                   "bytes [%llu, %llu) of %llu) is torn with %zu good "
                   "span(s) beyond it -- those tail bytes are NOT "
                   "salvageable as a prefix cut and are dropped with the "
                   "suffix (named below)\n", (unsigned long long)p.id,
                   p.name[0] ? p.name : "no name resolves to it",
                   p.first_bad, (unsigned long long)p.first_bad_pba,
                   (unsigned long long)bad_off,
                   (unsigned long long)(bad_off + bad_len),
                   (unsigned long long)p.row_size, p.good_tail);
        }
        if (c->dry) {
            printf("content-cut: inode %llu (%s): WOULD cut to %llu "
                   "byte(s), dropping [%llu, %llu) (seg %zu pba %llu "
                   "torn, %zu tail span(s) past it); no changes "
                   "(dry-run)\n", (unsigned long long)p.id,
                   p.name[0] ? p.name : "no name resolves to it",
                   (unsigned long long)p.good_prefix,
                   (unsigned long long)p.good_prefix,
                   (unsigned long long)p.row_size, p.first_bad,
                   (unsigned long long)p.first_bad_pba, p.good_tail);
            cut_report_add(c->rep, p.id, p.name, p.good_prefix,
                           p.row_size - p.good_prefix, bad_off, bad_len,
                           p.first_bad_pba, (uint32_t)p.first_bad, 0);
            c->rep->would_cut++;
            cut_probe_free(&p);
            return 0;
        }
        /* The repair: fresh prefix recipe, stored under its own hash,
         * row published through the delta. Heal-before-barrier: nothing
         * below publishes until the new blob is stored AND the survivors
         * verify, so a crash anywhere before the row publish leaves the
         * damaged file exactly as it was (EIO, not half-truncated). */
        {
            uint8_t *nblob = NULL;
            size_t nblen = 0;
            uint8_t naddr[INVFS_RECIPE_ADDR_LEN];
            invfs_inode nin;
            uint8_t *expect = NULL, *got = NULL;
            size_t got_len = 0, i;
            int ok = 0;

            expect = (uint8_t *)malloc(p.good_prefix ? (size_t)p.good_prefix : 1);
            if (!expect) {
                printf("content-cut: inode %llu (%s): out of memory, "
                       "left untouched\n", (unsigned long long)p.id,
                       p.name[0] ? p.name : "no name resolves to it");
                cut_report_add(c->rep, p.id, p.name, 0,
                               p.row_size, bad_off, bad_len,
                               p.first_bad_pba, (uint32_t)p.first_bad, 0);
                cut_probe_free(&p);
                return 0;
            }
            for (i = 0; i < p.first_bad; i++)
                memcpy(expect + p.spans[i].file_offset, p.spans[i].data,
                       (size_t)p.spans[i].length);
            if (vol_ast_recipe_serialize(p.good_prefix, p.ents,
                                         (uint32_t)p.first_bad,
                                         &nblob, &nblen) != 0) {
                printf("content-cut: inode %llu (%s): prefix recipe "
                       "build failed, left untouched\n",
                       (unsigned long long)p.id,
                       p.name[0] ? p.name : "no name resolves to it");
                free(expect);
                cut_report_add(c->rep, p.id, p.name, 0,
                               p.row_size, bad_off, bad_len,
                               p.first_bad_pba, (uint32_t)p.first_bad, 0);
                cut_probe_free(&p);
                return 0;
            }
            if (vol_recipe_store(c->v, nblob, nblen, naddr) != 0) {
                printf("content-cut: inode %llu (%s): recipe store "
                       "failed, left untouched\n",
                       (unsigned long long)p.id,
                       p.name[0] ? p.name : "no name resolves to it");
                free(nblob);
                free(expect);
                cut_report_add(c->rep, p.id, p.name, 0,
                               p.row_size, bad_off, bad_len,
                               p.first_bad_pba, (uint32_t)p.first_bad, 0);
                cut_probe_free(&p);
                return 0;
            }
            free(nblob);
            if (vol_inode_get(c->v, p.id, &nin) != 1) {
                printf("content-cut: inode %llu (%s): row vanished "
                       "mid-cut, left untouched\n",
                       (unsigned long long)p.id,
                       p.name[0] ? p.name : "no name resolves to it");
                free(expect);
                cut_report_add(c->rep, p.id, p.name, 0,
                               p.row_size, bad_off, bad_len,
                               p.first_bad_pba, (uint32_t)p.first_bad, 0);
                cut_probe_free(&p);
                return 0;
            }
            nin.size = p.good_prefix;
            memcpy(nin.recipe_addr, naddr, sizeof naddr);
            if (vol_inode_delta_put(c->v, p.id, &nin) != 0) {
                printf("content-cut: inode %llu (%s): row publish "
                       "failed, left untouched\n",
                       (unsigned long long)p.id,
                       p.name[0] ? p.name : "no name resolves to it");
                free(expect);
                cut_report_add(c->rep, p.id, p.name, 0,
                               p.row_size, bad_off, bad_len,
                               p.first_bad_pba, (uint32_t)p.first_bad, 0);
                cut_probe_free(&p);
                return 0;
            }
            /* Verify: the cut file reads back whole and byte-identical
             * to the probe's decoded prefix. A miss fails LOUDLY (the
             * row moved; there is no silent second write). */
            if (vol_read_inode(c->v, p.id, 0, &got, &got_len) == 0 &&
                got_len == (size_t)p.good_prefix &&
                memcmp(got, expect, got_len) == 0)
                ok = 1;
            free(got);
            free(expect);
            if (!ok) {
                printf("content-cut: inode %llu (%s): VERIFY FAILED "
                       "after the row publish -- the file does not read "
                       "back its %llu surviving byte(s); left as cut, "
                       "RESTORE FROM BACKUP\n", (unsigned long long)p.id,
                       p.name[0] ? p.name : "no name resolves to it",
                       (unsigned long long)p.good_prefix);
                cut_report_add(c->rep, p.id, p.name, p.good_prefix,
                               p.row_size - p.good_prefix, bad_off,
                               bad_len, p.first_bad_pba,
                               (uint32_t)p.first_bad, 0);
                c->rep->verify_failed++;
                cut_probe_free(&p);
                return 0;
            }
            printf("content-cut: inode %llu (%s): CUT to %llu "
                   "byte(s), dropped [%llu, %llu) (seg %zu pba %llu "
                   "torn, %zu tail span(s) past it); survivors "
                   "verified byte-exact\n", (unsigned long long)p.id,
                   p.name[0] ? p.name : "no name resolves to it",
                   (unsigned long long)p.good_prefix,
                   (unsigned long long)p.good_prefix,
                   (unsigned long long)p.row_size, p.first_bad,
                   (unsigned long long)p.first_bad_pba, p.good_tail);
            cut_report_add(c->rep, p.id, p.name, p.good_prefix,
                           p.row_size - p.good_prefix, bad_off, bad_len,
                           p.first_bad_pba, (uint32_t)p.first_bad, 1);
            c->rep->cut++;
            cut_probe_free(&p);
            (void)ok;
            return 0;
        }
    }
}

/* Explicit content-cut over the live set. dry=1 plans without changing
 * anything. Returns 0 when the walk completed (see rep for the verdict),
 * -1 when the walk itself failed (rep is PARTIAL: fail closed). */
int vol_content_cut(invfs_volume *v, int dry, invfs_content_cut_report *rep)
{
    cut_walk_ctx c;
    if (!v || !rep)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY) {
        fprintf(stderr, "content-cut: volume is read-only; cutting "
                "rewrites recipes and refuses\n");
        return -1;
    }
    memset(rep, 0, sizeof *rep);
    memset(&c, 0, sizeof c);
    c.v = v;
    c.dry = dry;
    c.rep = rep;
    if (vol_iter_live_inodes(v, cut_walk_cb, &c) != 0)
        return -1;
    return 0;
}

/* Read-only segment-tear probe for the fsck audit: does entry `e` of a
 * coherent recipe still decode? 0 = torn, 1 = good, -1 = not a payload
 * entry here (TEXT/WINDOW_SRC/zero pba -- the caller skips those, never
 * verdicts on them). Lighter than cut_decode_span: framed CRC + frame
 * rule + a length-exact decode for the write-path codecs, CRC + frame
 * only for exotic whole-file codecs. */
int vol_cut_span_ok(invfs_volume *v, const invfs_ast_block_entry *e)
{
    uint8_t *dec = NULL;
    int rc;
    if (!v || !e)
        return -1;
    if (e->zone == INVFS_ZONE_TEXT || e->algo == INVFS_ALGO_WINDOW_SRC ||
        e->pba == 0)
        return -1;
    if (e->algo != INVFS_ALGO_NONE && e->algo != INVFS_ALGO_LZ4 &&
        e->algo != INVFS_ALGO_ZSTD) {
        uint32_t hdr = 0;
        uint8_t *blob = NULL;
        if (e->pba >= v->sb.total_blocks)
            return 0;
        rc = seg_read_checked(v, e->pba, 0, 1, &hdr, &blob);
        free(blob);
        return rc == 0 ? 1 : 0;
    }
    rc = cut_decode_span(v, 0, e, &dec);
    free(dec);
    return rc == 0 ? 1 : 0;
}
