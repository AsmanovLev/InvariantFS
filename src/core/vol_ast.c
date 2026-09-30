/* vol_ast.c — AST children: serialize / deserialize / container
 * creation + lookup by name. Split from volume.c. */

#include "volume_internal.h"



/* ---- WP-M8: immutable recipe blob serialize / deserialize -----------
 * A v3 recipe blob is exactly the bytes a v2 inode record carries after
 * its name: the AST header (v1 or v2) followed by the block entries. The
 * writer builds it from the session's entry table; the reader parses it
 * and hands the entries to the shared segment decoder. Children blobs are
 * v2 container metadata and are not part of a v3 recipe (WP-M8 scope:
 * plain per-segment files); num_children is always 0 here. */

int vol_ast_recipe_serialize(uint64_t file_size,
                             const invfs_ast_block_entry *ents, uint32_t n,
                             uint8_t **blob_out, size_t *blen_out)
{
    return vol_ast_recipe_serialize_win(file_size, ents, n, NULL, 0,
                                       blob_out, blen_out);
}

/* ADR-010 amendment 2: same, plus the trailing window table. Passing
 * (NULL, 0) produces the byte-identical blob the v1 writer always produced,
 * which is what keeps every pre-existing volume stable. */
int vol_ast_recipe_serialize_win(uint64_t file_size,
                                 const invfs_ast_block_entry *ents, uint32_t n,
                                 const invfs_ast_window_entry *wins,
                                 uint32_t n_wins,
                                 uint8_t **blob_out, size_t *blen_out)
{
    uint8_t hdr[INVFS_AST_HDR_V2_LEN];
    size_t hlen, total, wlen = 0;
    uint8_t *blob;

    if (!blob_out || !blen_out || (n && !ents) || (n_wins && !wins))
        return -1;
    if (file_size > MAX_FILE_SIZE || n > MAX_SEGMENTS_V2)
        return -1;
    hlen = invfs_ast_hdr_write(hdr, file_size, n, 0);
    if (!hlen)
        return -1;
    if (n_wins) {
        if (n_wins > MAX_SEGMENTS_V2)
            return -1;
        wlen = 8 + (size_t)n_wins * sizeof(*wins);
    }
    total = hlen + (size_t)n * sizeof(*ents) + wlen;
    if (total > INVFS_V3_RECIPE_STREAM_MAX)
        return -1;   /* WP-M25: recipe stream capped at 64 MiB */
    blob = (uint8_t *)malloc(total ? total : 1);
    if (!blob)
        return -1;
    memcpy(blob, hdr, hlen);
    if (n)
        memcpy(blob + hlen, ents, (size_t)n * sizeof(*ents));
    if (n_wins) {
        uint8_t *w = blob + hlen + (size_t)n * sizeof(*ents);
        memcpy(w, INVFS_AST_WINDOW_MAGIC, 4);
        memcpy(w + 4, &n_wins, 4);
        memcpy(w + 8, wins, (size_t)n_wins * sizeof(*wins));
    }
    *blob_out = blob;
    *blen_out = total;
    return 0;
}

/* Locate the trailing window table, if any. Returns 0 and sets *wins_out to
 * NULL / *n_out to 0 for every pre-ADR-010 recipe (their blob ends exactly
 * after the last entry), so the read path pays one length compare. -1 = the
 * section is present but malformed, which must fail the read loudly rather
 * than silently serve zeros. */
int vol_ast_recipe_windows(const uint8_t *blob, size_t blen,
                           const invfs_ast_hdr *hdr,
                           const invfs_ast_window_entry **wins_out,
                           uint32_t *n_out)
{
    size_t used;
    uint32_t nw = 0;

    *wins_out = NULL;
    *n_out = 0;
    if (!blob || !hdr || blen < hdr->hdr_len)
        return -1;
    used = hdr->hdr_len + (size_t)hdr->num_blocks * sizeof(invfs_ast_block_entry);
    if (used > blen)
        return -1;                 /* the entries themselves are truncated */
    if (used == blen)
        return 0;                  /* no window section: the v1 shape */
    if (blen - used < 8 || memcmp(blob + used, INVFS_AST_WINDOW_MAGIC, 4) != 0)
        return -1;                 /* trailing bytes we do not understand */
    memcpy(&nw, blob + used + 4, 4);
    if (nw == 0 || (size_t)nw > MAX_SEGMENTS_V2)
        return -1;
    if (blen - used - 8 != (size_t)nw * sizeof(invfs_ast_window_entry))
        return -1;                 /* count/size disagree */
    *wins_out = (const invfs_ast_window_entry *)(blob + used + 8);
    *n_out = nw;
    return 0;
}

/* Parse a recipe blob. On success *hdr_out is filled and *ents_out points
 * INTO `blob` (valid while the caller keeps it); *nents_out is the entry
 * count. 0 = ok, -1 = malformed/truncated. */
int vol_ast_recipe_parse(const uint8_t *blob, size_t blen,
                         invfs_ast_hdr *hdr_out,
                         const invfs_ast_block_entry **ents_out,
                         size_t *nents_out)
{
    if (!blob || !hdr_out)
        return -1;
    if (invfs_ast_hdr_parse(blob, blen, hdr_out) != 0)
        return -1;
    if ((size_t)hdr_out->num_blocks * sizeof(invfs_ast_block_entry) >
        blen - hdr_out->hdr_len)
        return -1;
    if (ents_out)
        *ents_out = (const invfs_ast_block_entry *)(blob + hdr_out->hdr_len);
    if (nents_out)
        *nents_out = hdr_out->num_blocks;
    return 0;
}

/* WP201: free every data block a v3 recipe names. 0 = the recipe parsed and
 * every block it named is now free (or it named none); -1 = nothing was
 * freed because the recipe could not be loaded OR did not parse.
 *
 * The parse leg used to fall through to `return 0` -- reporting success
 * having freed nothing. The v2 twin bails on exactly this
 * (vol_records.c:551-557), and "the retired twin is right, the current
 * implementation diverged" is the shape this whole lane keeps hitting. A
 * caller that trusts the return believes the space is back, deletes the
 * row, and leaves every block the recipe named allocated, referenced by
 * nothing and reachable by no name. */
int vol_v3_free_recipe_blocks(invfs_volume *v,
                             const uint8_t recipe_addr[INVFS_V3_RECIPE_ADDR_LEN],
                             uint64_t keep_pba)
{
    static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN] = {0};
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    size_t i, k;
    int parsed;

    if (!v || !recipe_addr)
        return -1;
    if (memcmp(recipe_addr, zero_addr, sizeof zero_addr) == 0)
        return 0;
    if (vol_v3_recipe_load(v, recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    parsed = (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 &&
              ents != NULL);
    if (parsed) {
        for (i = 0; i < n_ents; i++) {
            uint64_t pba = ents[i].pba;
            int dup = 0;
            if (!pba || pba == keep_pba)
                continue;
            /* WP78: a zone==TEXT entry names a SHARED batch segment owned
             * by the batch registry -- dropping this member's reference
             * must not free it; tz_v3_gc reclaims it when no live member
             * names it any more. */
            if (ents[i].zone == INVFS_ZONE_TEXT)
                continue;
            for (k = 0; k < i; k++) {
                if (ents[k].pba == pba) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                uint64_t plen = 0;
                pba_ref_ensure(v);
                pba_ref_modify(v, pba, -1);
                if (pba_ref_count(v, pba) == 0) {
                    if (seg_extent_checked(v, pba, &plen) == 0 && plen > 0)
                        vol_free_blocks(v, pba, plen);
                }
            }
        }
    }
    free(blob);
    return parsed ? 0 : -1;
}


/* WP202: release the blocks a SUPERSEDED inode recipe owned. One
 * implementation, three callers -- the containerpack commit
 * (cpack_release_superseded in vol_cpack.c, which is now a one-line
 * forward to this) and the builtin container lanes in
 * sweep_dispatch (vol_sweep.c), which did the same supersede through
 * vol_create_blob_file -> vol_v3_create_content_node and, until now,
 * gave the space back to nobody.
 *
 * The caller must have captured `old_addr` from the inode row BEFORE
 * the supersede; vol_create_blob_file is handed the NEW address and
 * never the old one, which is the whole reason the capture has to
 * happen at the call site (the comment on that function says so).
 *
 * Two guards, both load-bearing:
 *
 *  - "only when the row really MOVED": a recipe address is the content
 *    hash of the recipe, so a re-run that reproduces the SAME recipe (a
 *    lost class stamp re-arms the file; a batched member promoted twice)
 *    lands on the SAME address, the live inode still points at those very
 *    segments, and freeing them would strand the file. This is the
 *    guard cpack_release_superseded already carried.
 *
 *  - `zone == INVFS_ZONE_TEXT` entries are skipped inside
 *    vol_v3_free_recipe_blocks (a shared batch segment is owned by the
 *    batch registry, not by this member), and the pba refcount means a
 *    block another live recipe still names is not freed here. */
void vol_v3_release_superseded_blob(
    invfs_volume *v, uint64_t inode_id,
    const uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN])
{
    static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN] = {0};
    invfs_v3_inode now;

    if (!v || !old_addr ||
        memcmp(old_addr, zero_addr, sizeof zero_addr) == 0)
        return;
    if (vol_v3_inode_get(v, inode_id, &now) != 1)
        return;
    if (memcmp(now.recipe_addr, old_addr, INVFS_V3_RECIPE_ADDR_LEN) == 0)
        return;
    vol_v3_free_recipe_blocks(v, old_addr, 0);
}



/* ---- AST children: serialize / deserialize / container creation ---- */

/* serialize children after the block entries; malloc'd buf or NULL */



/* deserialize children with hard bounds against blob_len (no OOM) */



/* create file with container children (ZIP members) — bounded allocs */



/* parse ZIP central directory into children (bounded, no OOM);
 * returns number of children, -1 on structural failure */
int vol_zip_parse_children(const uint8_t *z, size_t zlen,
                           invfs_ast_child_entry *ch, size_t maxch)
{
    if (zlen < 22) return -1;
    size_t eocd = zlen >= 22 ? zlen - 22 : 0;
    uint16_t ncen;
    uint32_t cen_off;
    size_t p;
    size_t n = 0;
    while (eocd > 0 && !(z[eocd] == 'P' && z[eocd + 1] == 'K' &&
                         z[eocd + 2] == 5 && z[eocd + 3] == 6))
        eocd--;
    if (!(z[eocd] == 'P' && z[eocd + 1] == 'K' && z[eocd + 2] == 5 && z[eocd + 3] == 6))
        return -1;
    memcpy(&ncen, z + eocd + 10, 2);
    memcpy(&cen_off, z + eocd + 16, 4);
    if (cen_off >= zlen) return -1;
    if (ncen > maxch) ncen = (uint16_t)maxch;
    p = cen_off;
    for (uint16_t i = 0; i < ncen; i++) {
        uint16_t nlen, elen, clen;
        uint32_t usize, crc, local_off;
        if (p + 46 > zlen || !(z[p] == 'P' && z[p + 1] == 'K' &&
                               z[p + 2] == 1 && z[p + 3] == 2))
            break;
        memcpy(&crc, z + p + 16, 4);
        memcpy(&usize, z + p + 24, 4);
        memcpy(&nlen, z + p + 28, 2);
        memcpy(&elen, z + p + 30, 2);
        memcpy(&clen, z + p + 32, 2);
        memcpy(&local_off, z + p + 42, 4);
        if (nlen > MAX_AST_CHILD_NAME || p + 46 + nlen > zlen) break;
        ch[n].name_len = nlen;
        memcpy(ch[n].name, z + p + 46, nlen);
        ch[n].name[nlen] = 0;
        ch[n].method = 0;
        ch[n].csize = 0;
        ch[n].usize = usize;
        ch[n].crc = crc;
        ch[n].data_off = 0;
        {
            /* window into the container: method + compressed size + data
             * offset, from the central directory and local header */
            uint16_t method, lh_nlen, lh_elen;
            uint32_t csize;
            memcpy(&method, z + p + 10, 2);
            memcpy(&csize, z + p + 20, 4);
            if (local_off + 30 <= zlen &&
                z[local_off] == 'P' && z[local_off + 1] == 'K' &&
                z[local_off + 2] == 3 && z[local_off + 3] == 4) {
                memcpy(&lh_nlen, z + local_off + 26, 2);
                memcpy(&lh_elen, z + local_off + 28, 2);
                if (local_off + 30 + lh_nlen + lh_elen + csize <= zlen) {
                    ch[n].method = method;
                    ch[n].csize = csize;
                    ch[n].data_off = (uint32_t)(local_off + 30 + lh_nlen + lh_elen);
                }
            }
        }
        n++;
        p += 46 + nlen + elen + clen;
    }
    return (int)n;
}


/* WP47: is `ip` a plausible record position? On a v0.3.0+ mapper volume
 * records live inside dynamic metadata extents (idx_get_id returns the
 * absolute extent offset), so the legacy [inode_area_start, inode_area_pos)
 * bound rejected every valid hint. Accept any position inside any mapper
 * extent (mirroring vol_inode_next), or inside the legacy contiguous
 * region; anything else falls back to vol_records_walk(). */


typedef struct {
    uint64_t want;
    uint64_t pos;
    uint32_t rl;
    int found;
} ast_child_locate_ctx;



/* read children from an inode record (bounded); 0 = none, -1 = corrupt */



/* find inode record by name; returns inode_id (0 = not found) */
uint64_t vol_find(invfs_volume *v, const char *name)
{
    /* WP-M6: names resolve through the dirent tree. The v2 alternative was
     * an O(1) in-memory index lookup (idx_get), and that index has been a
     * no-op returning NULL since WP-M21 retired it. */
    uint64_t ino = 0;
    if (vol_v3_path_lookup(v, name, &ino) != 1)
        return 0;
    return ino;
}

/* vol_find, keeping the two ways of not finding a name apart.
 *   1 = found, *ino_out set. 0 = there is no such name. -1 = the lookup
 * could not be completed (a dirent row on the path is unreadable, the name
 * is unusable, or the walk hit a dangling entry).
 *
 * vol_find returns a uint64_t, so its "not found" and its "could not look"
 * are both the single value 0 and a caller cannot tell them apart. That is
 * harmless for most of the hundred-odd things that call it -- they just
 * proceed as though the name is not there -- but it is NOT harmless for a
 * permission check. perm_check_cred resolves the name, then reads the
 * inode's POSIX ACL through it; when the resolution fails it never gets as
 * far as the ACL, and the mount evaluates the plain mode triad instead,
 * which is how one unreadable dirent row became a file whose ACLs the mount
 * stopped enforcing. See src/cli/fuse_fs.c perm_check_cred.
 *
 * vol_find itself is unchanged: 102 call sites test it against 0 and
 * widening its return would change every one of them.
 */
int vol_find_rc(invfs_volume *v, const char *name, uint64_t *ino_out)
{
    uint64_t ino = 0;
    int rc;

    if (!ino_out)
        return -1;
    rc = vol_v3_path_lookup(v, name, &ino);
    if (rc != 1)
        return rc;
    *ino_out = ino;
    return 1;
}


/* The live version of a name under the consistent cut (WP22d): the index
 * holds exactly the post-cut view, so this is the answer listing tools
 * must print (raw area walks see torn versions the index has hidden). */
uint64_t vol_find_ex(invfs_volume *v, const char *name,
                     uint64_t *size_out, uint64_t *ctime_out)
{
    uint64_t ino = 0;
    if (vol_v3_path_stat(v, name, &ino, size_out, ctime_out) != 0)
        return 0;
    return ino;
}


/*
 * Extract one container member (window) from an in-memory archive buffer.
 * member data is at ch->data_off, compressed with ch->method (0=stored,
 * 8=deflate). Returns 0 on success; *out malloc'd, caller frees.
 */
int vol_zip_extract_member(const uint8_t *z, size_t zlen,
                           const invfs_ast_child_entry *ch,
                           uint8_t **out, size_t *out_len)
{
    uint8_t *buf;
    if (ch->data_off == 0 || ch->usize == 0)
        return -1;
    if ((size_t)ch->data_off + ch->csize > zlen)
        return -1;
    if (ch->method == 0) {   /* stored */
        if (ch->csize != ch->usize)
            return -1;
        buf = (uint8_t *)malloc(ch->usize);
        if (!buf) return -1;
        memcpy(buf, z + ch->data_off, ch->usize);
        *out = buf; *out_len = ch->usize;
        return 0;
    }
    if (ch->method == 8) {   /* deflate */
        buf = (uint8_t *)malloc(ch->usize ? ch->usize : 1);
        if (!buf) return -1;
        {
            size_t got = tinfl_decompress_mem_to_mem(
                buf, ch->usize, z + ch->data_off, ch->csize,
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if (got != ch->usize) { free(buf); return -1; }
        }
        *out = buf; *out_len = ch->usize;
        return 0;
    }
    return -1;  /* unsupported method */
}
