/* vol_recshare.c -- whole-file recipe sharing.
 *
 * Two files with the same bytes need ONE recipe, not two. Recipes are already
 * content-addressed (vol_recipe_store hashes the blob and searches the key
 * before writing), so a loser can simply name the donor's blob: no new format,
 * no serialisation, no entry rewriting. What that frees is the loser's own
 * extents -- and only those whose last owner is that loser, which is why the
 * hand-back goes through vol_retire_pbas's share-aware drain and why the count
 * reported here is BLOCKS, never logical bytes.
 *
 * The identity key is the hash of the DECODED stream, not of the stored bytes.
 * The segment dedupe in vol_dedupe.c hashes what is on disk, which is the right
 * question there ("is this exact segment stored twice"); here the question is
 * "do these two files read back the same", and two identical files can be
 * stored differently (a codec choice, a sweep that packed one and not the
 * other). Files that match on stored bytes but not on content would be a wrong
 * share, so the decoded stream is the only honest key.
 *
 * Ownership is already per REFERENCE: pba_ref's builder walks live inode rows
 * and adds one reference per entry per row, so two rows naming one recipe read
 * as owners=2 and the extent survives until the second row moves off. That is
 * the whole reason this leg needs no new ownership model -- see
 * impl_docs/OPEN-ISSUES.md, "The two gates".
 *
 * What this leg does NOT do: reclaim the recipe blobs that stop being named.
 * Nothing in the tree does that today (a separate defect, recorded), and the
 * share makes it strictly better, not worse: the losers stop having blobs of
 * their own.
 */
#include "volume.h"
#include "volume_internal.h"
#include "vol_delta.h"          /* vol_retire_pbas */
#include "blake3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RS_CHUNK (1u << 20)

typedef struct {
    uint64_t ino;
    uint64_t size;
    uint8_t  hash[32];
} rs_key;

typedef struct {
    invfs_volume *v;
    rs_key       *keys;
    size_t        n, cap;
    size_t        skipped_unreadable;
    int           err;
} rs_ctx;

static int rs_cmp(const void *a, const void *b)
{
    const rs_key *x = (const rs_key *)a, *y = (const rs_key *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return memcmp(x->hash, y->hash, 32);
}

/* Hash the decoded stream of one inode. The read goes through vol_read_range,
 * i.e. exactly the path a user's read takes: if the content a reader would get
 * is not identical, the hash is not identical, and no share happens. */
static int rs_hash_inode(invfs_volume *v, uint64_t ino, uint64_t size,
                         uint8_t out[32])
{
    blake3_hasher hx;
    uint8_t *buf = malloc(RS_CHUNK);
    uint64_t off = 0;

    if (!buf) return -1;
    blake3_hasher_init(&hx);
    while (off < size) {
        uint64_t want = size - off;
        uint64_t got;
        if (want > RS_CHUNK) want = RS_CHUNK;
        got = (uint64_t)vol_read_range(v, ino, off, (size_t)want, buf);
        if (got != want) { free(buf); return -1; }
        blake3_hasher_update(&hx, buf, (size_t)got);
        off += got;
    }
    blake3_hasher_finalize(&hx, out, 32);
    free(buf);
    return 0;
}

static int rs_walk_cb(void *ctx_, const char *path, uint64_t inode_id,
                      uint32_t type, uint64_t size, int64_t mtime)
{
    rs_ctx *c = (rs_ctx *)ctx_;
    rs_key k;
    (void)mtime;

    if (type == INVFS_ITYP_DIR || !inode_id) return 0;
    if (path && (unsigned char)path[0] == 0x01) return 0;
    /* A symlink's blob is its target string, content-addressed like any
     * recipe, and its "content" is not a stream this leg may key on. Stated
     * through the same predicate the read path dispatches on, as the segment
     * dedupe does -- two places asking the same question must not drift. */
    if (invfs_inode_content_is_raw_blob(type)) return 0;
    if (size == 0) return 0;

    if (rs_hash_inode(c->v, inode_id, size, k.hash) != 0) {
        c->skipped_unreadable++;
        return 0;
    }
    k.ino = inode_id;
    k.size = size;
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 1024;
        rs_key *nk = (rs_key *)realloc(c->keys, ncap * sizeof *nk);
        if (!nk) { c->err = 1; return 1; }
        c->keys = nk;
        c->cap = ncap;
    }
    c->keys[c->n++] = k;
    return 0;
}

/* Point `loser` at `donor_addr` and hand the loser's own extents back.
 *
 * The order is dedup_remap_file_impl's, minus everything a whole-file share
 * does not need: the row publish is staged, so the retire MUST come after it
 * (its span capture needs the staged row); the map is flagged rather than
 * patched by hand; and the retire carries no keep-list, which vol_retire_pbas
 * supports and whose drain resolves against the live map -- an extent another
 * live recipe still names is left alone there, not here. */
static int rs_share_one(invfs_volume *v, uint64_t loser,
                        const uint8_t donor_addr[INVFS_RECIPE_ADDR_LEN],
                        uint64_t *blocks_out)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0, i;
    uint64_t *pbas = NULL;
    size_t npbas = 0, produced = 0;

    *blocks_out = 0;
    if (vol_inode_get(v, loser, &in) != 1) return 1;
    if (in.size == 0 || invfs_inode_content_is_raw_blob(in.type)) return 1;
    if (memcmp(in.recipe_addr, donor_addr, INVFS_RECIPE_ADDR_LEN) == 0)
        return 1;                       /* already shared: nothing to publish */

    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return 1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 || !ents) {
        free(blob);
        return 1;
    }
    free(blob);

    pbas = n_ents ? (uint64_t *)malloc(n_ents * sizeof *pbas) : NULL;
    if (n_ents && !pbas) { vol_ast_recipe_parsed_free(&ah, ents); return -1; }
    for (i = 0; i < n_ents; i++)
        if (ents[i].pba && ents[i].zone != INVFS_ZONE_TEXT)
            pbas[npbas++] = ents[i].pba;
    vol_ast_recipe_parsed_free(&ah, ents);

    memcpy(in.recipe_addr, donor_addr, INVFS_RECIPE_ADDR_LEN);
    if (vol_inode_delta_put(v, loser, &in) != 0) { free(pbas); return -1; }
    pba_ref_invalidate(v);
    if (npbas)
        vol_retire_pbas(v, loser, pbas, npbas, NULL, 0);

    /* Blocks, counted the way the segment dedupe counts them: over the distinct
     * extents this loser stopped naming. The drain is what actually frees, and
     * it may keep some of these (another recipe still names them), so this is
     * an upper bound and it is reported as one. */
    for (i = 0; i < npbas; i++) {
        uint64_t plen = 0;
        size_t j;
        int seen = 0;
        for (j = 0; j < i; j++)
            if (pbas[j] == pbas[i]) { seen = 1; break; }
        if (seen) continue;
        if (seg_extent_checked(v, pbas[i], &plen) == 0 && plen > 0)
            *blocks_out += plen;
    }
    produced = npbas;
    (void)produced;
    free(pbas);
    return 0;
}

int vol_sweep_recipe_share_ex(invfs_volume *v, invfs_recshare_stats *stats,
                              invfs_dedupe_progress_fn progress,
                              void *progress_user)
{
    invfs_recshare_stats local;
    invfs_recshare_stats *out_stats = stats;
    rs_ctx ctx;
    size_t i, groups = 0;
    uint64_t blocks = 0;
    int marked = 0;
    int rc = 0;

    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);
    if (!v) return -1;
    if (!vol_write_enabled(v)) return 0;

    memset(&ctx, 0, sizeof ctx);
    ctx.v = v;
    if (vol_walk(v, rs_walk_cb, &ctx) != 0 && ctx.err) {
        free(ctx.keys);
        return -1;
    }
    stats->files_hashed = ctx.n;
    stats->files_unreadable = ctx.skipped_unreadable;

    if (ctx.n > 1)
        qsort(ctx.keys, ctx.n, sizeof *ctx.keys, rs_cmp);

    i = 0;
    while (i < ctx.n) {
        size_t j = i + 1, k;
        uint8_t donor_addr[INVFS_RECIPE_ADDR_LEN];
        invfs_inode din;

        while (j < ctx.n && ctx.keys[j].size == ctx.keys[i].size &&
               memcmp(ctx.keys[j].hash, ctx.keys[i].hash, 32) == 0)
            j++;
        if (j - i > 1) {
            /* The donor is the first LIVE row of the group; if it will not
             * read, the group is skipped whole rather than reshuffled, so a
             * second run reaches the same decision on the same input. */
            if (vol_inode_get(v, ctx.keys[i].ino, &din) != 1) { i = j; continue; }
            memcpy(donor_addr, din.recipe_addr, INVFS_RECIPE_ADDR_LEN);
            if (memcmp(donor_addr, (uint8_t[INVFS_RECIPE_ADDR_LEN]){0},
                       INVFS_RECIPE_ADDR_LEN) == 0) { i = j; continue; }
            groups++;
            for (k = i + 1; k < j; k++) {
                uint64_t one = 0;
                int s;
                if (!marked && vol_mark_dirty(v) != 0) { rc = -1; i = j; break; }
                marked = 1;
                s = rs_share_one(v, ctx.keys[k].ino, donor_addr, &one);
                if (s < 0) { rc = -1; i = j; break; }
                if (s == 1) continue;         /* already shared / not shareable */
                stats->files_shared++;
                stats->logical_bytes_shared += ctx.keys[k].size;
                blocks += one;
                if (progress) {
                    invfs_dedupe_progress p;
                    p.phase = "recshare";
                    p.done = stats->files_shared;
                    p.total = stats->files_shared;
                    p.candidates = 0;
                    p.merged = 0;
                    p.cross_merged = 0;
                    p.intra_merged = 0;
                    p.freed = blocks;
                    progress(progress_user, &p);
                }
            }
            if (rc != 0) break;
        }
        i = j;
    }

    stats->duplicate_groups = groups;
    stats->blocks_retired = blocks;
    free(ctx.keys);

    if (progress) {
        invfs_dedupe_progress p;
        p.phase = "done";
        p.done = stats->files_shared;
        p.total = stats->files_shared;
        p.candidates = stats->duplicate_groups;
        p.merged = stats->files_shared;
        p.cross_merged = 0;
        p.intra_merged = 0;
        p.freed = blocks;
        progress(progress_user, &p);
    } else if (!out_stats) {
        /* Only the convenience entry point prints. A caller that asked for the
         * numbers decides how to present them -- two summaries of one run is how
         * a log starts contradicting itself (and the CLI's is the one that
         * carries the units). */
        printf("recshare: hashed %llu files, %llu duplicate groups, %llu shared, "
               "%llu blocks retired (upper bound)\n",
               (unsigned long long)stats->files_hashed,
               (unsigned long long)stats->duplicate_groups,
               (unsigned long long)stats->files_shared,
               (unsigned long long)stats->blocks_retired);
        if (stats->files_unreadable)
            printf("recshare: %llu files could not be read and were skipped\n",
                   (unsigned long long)stats->files_unreadable);
    }
    return rc;
}

int vol_sweep_recipe_share(invfs_volume *v)
{
    return vol_sweep_recipe_share_ex(v, NULL, NULL, NULL);
}
