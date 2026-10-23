/* vol_ast.c — AST children: serialize / deserialize / container
 * creation + lookup by name. Split from volume.c. */

#include "volume_internal.h"
#include "vol_delta.h"   /* WP612 retire API */



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
    return vol_ast_recipe_serialize_tail(file_size, ents, n, wins, n_wins,
                                         NULL, 0, blob_out, blen_out);
}

/* WP635: the same serializer for a recipe carrying a CHLD table. Kept a
 * separate entry point so no caller can pass windows and children at once --
 * the two tails are mutually exclusive by shape, not by caller discipline. */
int vol_ast_recipe_serialize_tree(uint64_t file_size,
                                  const invfs_ast_block_entry *ents, uint32_t n,
                                  const invfs_ast_child_ref *chld,
                                  uint32_t n_chld,
                                  uint8_t **blob_out, size_t *blen_out)
{
    return vol_ast_recipe_serialize_tail(file_size, ents, n, NULL, 0, chld,
                                         n_chld, blob_out, blen_out);
}

/* WP635: the one serializer both tail forms go through. `wins` and `chld` are
 * mutually exclusive (a recipe is container windows OR a child table); passing
 * neither is the flat blob every pre-WP635 volume holds, byte for byte. The
 * child table is BLAKE3 addresses of child recipes, appended exactly like the
 * window table so an old reader refuses (pba == 0) rather than misreads. */
int vol_ast_recipe_serialize_tail(uint64_t file_size,
                                  const invfs_ast_block_entry *ents, uint32_t n,
                                  const invfs_ast_window_entry *wins,
                                  uint32_t n_wins,
                                  const invfs_ast_child_ref *chld,
                                  uint32_t n_chld,
                                  uint8_t **blob_out, size_t *blen_out)
{
    uint8_t hdr[INVFS_AST_HDR_V3_LEN];
    size_t hlen, total, wlen = 0;
    uint8_t *blob;
    /* WP639: v3 emission is test-forced only (INVFS_RECIPE_FORCE_V3=1).
     * No in-tree writer can produce wide fields (the in-memory entry is
     * still 24-bit block_id / 6-bit algo / 2-bit zone), so production
     * output stays v1/v2 byte-identical; the v3 path is proved by the
     * unit test and the forced e2e leg until a producer needs it. */
    int force_v3 = 0;
    size_t esz = sizeof(*ents);
    const char *f3 = getenv("INVFS_RECIPE_FORCE_V3");
    if (f3 && f3[0] == '1' && f3[1] == '\0')
        force_v3 = 1;

    if (!blob_out || !blen_out || (n && !ents) || (n_wins && !wins) ||
        (n_chld && !chld))
        return -1;
    if (n_wins && n_chld)
        return -1;   /* one trailing section, never two */
    if (file_size > MAX_FILE_SIZE || n > MAX_SEGMENTS_V2) {
        fprintf(stderr, "invfs: recipe refused: %u entries (limit %u) for a "
                "%llu-byte file (limit %llu)\n", n, (unsigned)MAX_SEGMENTS_V2,
                (unsigned long long)file_size, (unsigned long long)MAX_FILE_SIZE);
        return -1;
    }
    if (force_v3) {
        hlen = invfs_ast_hdr_write_v3(hdr, file_size, n, 0);
        esz = 40;
    } else {
        hlen = invfs_ast_hdr_write(hdr, file_size, n, 0);
    }
    if (!hlen)
        return -1;
    if (n_wins) {
        if (n_wins > MAX_SEGMENTS_V2)
            return -1;
        wlen = 8 + (size_t)n_wins * sizeof(*wins);
    } else if (n_chld) {
        if (n_chld > MAX_SEGMENTS_V2)
            return -1;
        wlen = 8 + (size_t)n_chld * sizeof(*chld);
    }
    total = hlen + (size_t)n * esz + wlen;
    if (total > INVFS_RECIPE_STREAM_MAX) {
        fprintf(stderr, "invfs: recipe refused: %zu bytes for %u entries "
                "(file %llu bytes) exceeds the %u-byte recipe stream cap; "
                "the file is too large or too fragmented for this format\n",
                total, n, (unsigned long long)file_size,
                (unsigned)INVFS_RECIPE_STREAM_MAX);
        return -1;   /* WP-M25: recipe stream cap, INVFS_RECIPE_STREAM_MAX */
    }
    blob = (uint8_t *)malloc(total ? total : 1);
    if (!blob)
        return -1;
    memcpy(blob, hdr, hlen);
    if (force_v3) {
        /* 32 B in-memory -> 40 B on-disk + CRC, then self-verify: the
         * verify loop catches pack bugs before the blob is attested. */
        size_t i;
        for (i = 0; i < n; i++)
            invfs_ast_entry_v3_pack(blob + hlen + i * 40,
                                    ents[i].file_offset, ents[i].length,
                                    ents[i].zone, ents[i].algo,
                                    ents[i].block_id, ents[i].block_offset,
                                    ents[i].pba);
        for (i = 0; i < n; i++)
            if (invfs_ast_entry_v3_verify(blob + hlen + i * 40, 40) != 0) {
                free(blob);
                return -1;
            }
    } else if (n) {
        memcpy(blob + hlen, ents, (size_t)n * sizeof(*ents));
    }
    if (n_wins) {
        uint8_t *w = blob + hlen + (size_t)n * esz;
        memcpy(w, INVFS_AST_WINDOW_MAGIC, 4);
        memcpy(w + 4, &n_wins, 4);
        memcpy(w + 8, wins, (size_t)n_wins * sizeof(*wins));
    } else if (n_chld) {
        uint8_t *w = blob + hlen + (size_t)n * esz;
        memcpy(w, INVFS_AST_CHILD_MAGIC, 4);
        memcpy(w + 4, &n_chld, 4);
        memcpy(w + 8, chld, (size_t)n_chld * sizeof(*chld));
    }
    *blob_out = blob;
    *blen_out = total;
    return 0;
}

/* Locate the trailing section, whichever form it takes. 0 and an all-NULL/
 * all-zero view for every recipe written before either section existed (their
 * blob ends exactly after the last entry), so the read path pays one length
 * compare. -1 = the section is present but malformed, which must fail the read
 * loudly rather than silently serve zeros. */
int vol_ast_recipe_tail(const uint8_t *blob, size_t blen,
                        const invfs_ast_hdr *hdr,
                        invfs_ast_tail *out)
{
    size_t used;
    uint32_t nsec = 0;
    size_t stride = invfs_ast_entry_stride(hdr->version);

    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    if (!blob || !hdr || !stride || blen < hdr->hdr_len)
        return -1;
    used = hdr->hdr_len + (size_t)hdr->num_blocks * stride;
    if (used > blen)
        return -1;                 /* the entries themselves are truncated */
    if (used == blen)
        return 0;                  /* no trailing section: the v1 shape */
    if (blen - used < 8)
        return -1;
    if (memcmp(blob + used, INVFS_AST_WINDOW_MAGIC, 4) == 0) {
        memcpy(&nsec, blob + used + 4, 4);
        if (nsec == 0 || (size_t)nsec > MAX_SEGMENTS_V2)
            return -1;
        if (blen - used - 8 != (size_t)nsec * sizeof(invfs_ast_window_entry))
            return -1;             /* count/size disagree */
        out->wins = (const invfs_ast_window_entry *)(blob + used + 8);
        out->n_wins = nsec;
        return 0;
    }
    if (memcmp(blob + used, INVFS_AST_CHILD_MAGIC, 4) == 0) {
        memcpy(&nsec, blob + used + 4, 4);
        if (nsec == 0 || (size_t)nsec > MAX_SEGMENTS_V2)
            return -1;
        if (blen - used - 8 != (size_t)nsec * sizeof(invfs_ast_child_ref))
            return -1;             /* count/size disagree */
        out->chld = blob + used + 8;
        out->n_chld = nsec;
        return 0;
    }
    return -1;                     /* trailing bytes we do not understand */
}

/* Locate the trailing window table, if any. A recipe carrying a CHLD table is
 * NOT a window recipe: refuse it here rather than hand back "no windows" to a
 * caller that would then walk reference entries as if they were payloads. */
int vol_ast_recipe_windows(const uint8_t *blob, size_t blen,
                           const invfs_ast_hdr *hdr,
                           const invfs_ast_window_entry **wins_out,
                           uint32_t *n_out)
{
    invfs_ast_tail t;

    *wins_out = NULL;
    *n_out = 0;
    if (vol_ast_recipe_tail(blob, blen, hdr, &t) != 0)
        return -1;
    if (t.n_chld != 0)
        return -1;
    *wins_out = t.wins;
    *n_out = t.n_wins;
    return 0;
}

/* WP635: the CHLD table, if any. Same discipline as the twin above. */
int vol_ast_recipe_children(const uint8_t *blob, size_t blen,
                            const invfs_ast_hdr *hdr,
                            const uint8_t **chld_out, uint32_t *n_out)
{
    invfs_ast_tail t;

    if (!chld_out || !n_out)
        return -1;
    *chld_out = NULL;
    *n_out = 0;
    if (vol_ast_recipe_tail(blob, blen, hdr, &t) != 0)
        return -1;
    *chld_out = t.chld;
    *n_out = t.n_chld;
    return 0;
}

/* Parse a recipe blob. On success *hdr_out is filled. For v1/v2 *ents_out
 * points INTO `blob` (valid while the caller keeps it, freed with it).
 * For v3 the 40 B images are verified (CRC + zero rsv) and converted into
 * a HEAP 32 B array (values that do not fit the in-memory widths refuse
 * loudly); the caller frees it with vol_ast_recipe_parsed_free (no-op for
 * v1/v2). With ents_out == NULL no conversion happens, but v3 entries are
 * still all verified. 0 = ok, -1 = malformed/truncated. */
int vol_ast_recipe_parse(const uint8_t *blob, size_t blen,
                         invfs_ast_hdr *hdr_out,
                         const invfs_ast_block_entry **ents_out,
                         size_t *nents_out)
{
    size_t stride;
    if (!blob || !hdr_out)
        return -1;
    /* NULL-init first: every exit below leaves *ents_out either NULL
     * (failure, or not requested) or a valid pointer, so callers may
     * pass it to vol_ast_recipe_parsed_free unconditionally. */
    if (ents_out)
        *ents_out = NULL;
    if (invfs_ast_hdr_parse(blob, blen, hdr_out) != 0)
        return -1;
    stride = invfs_ast_entry_stride(hdr_out->version);
    if (!stride)
        return -1;
    if ((size_t)hdr_out->num_blocks * stride >
        blen - hdr_out->hdr_len)
        return -1;
    if (hdr_out->version == INVFS_AST_VERSION_V3) {
        size_t i, n = hdr_out->num_blocks;
        const uint8_t *base = blob + hdr_out->hdr_len;
        if (ents_out) {
            invfs_ast_block_entry *conv =
                (invfs_ast_block_entry *)malloc(n ? n * sizeof(*conv) : 1);
            if (!conv)
                return -1;
            for (i = 0; i < n; i++) {
                if (invfs_ast_entry_v3_verify(base + i * 40, 40) != 0 ||
                    invfs_ast_entry_v3_unpack(base + i * 40, &conv[i]) != 0) {
                    free(conv);
                    return -1;
                }
            }
            *ents_out = conv;
        } else {
            for (i = 0; i < n; i++)
                if (invfs_ast_entry_v3_verify(base + i * 40, 40) != 0)
                    return -1;
        }
        if (nents_out)
            *nents_out = n;
        return 0;
    }
    if (ents_out)
        *ents_out = (const invfs_ast_block_entry *)(blob + hdr_out->hdr_len);
    if (nents_out)
        *nents_out = hdr_out->num_blocks;
    return 0;
}

/* WP635: free what ONE parsed level names. Split out of
 * vol_free_recipe_blocks so the child recursion runs the identical loop over a
 * child's entries -- one implementation, so a reference level cannot diverge
 * from the flat one (TEXT-batch skip, pba sharing, extent resume).
 * `depth` bounds the recursion; `*bad_out` is set when a child could not be
 * loaded or parsed, which must reach the caller rather than be swallowed. */
static void free_recipe_level(invfs_volume *v,
                              const uint8_t *chld, uint32_t n_chld,
                              const invfs_ast_block_entry *ents, size_t n_ents,
                              uint64_t keep_pba, unsigned depth, int *bad_out)
{
    size_t i, k;

    for (i = 0; i < n_ents; i++) {
        uint64_t pba = ents[i].pba;
        int dup = 0;

        /* WP635: a reference entry owns no data block. Its child is a
         * separate recipe blob (a btree value behind 0x04||BLAKE3, retired
         * with the top one), and the blocks the child names are freed here
         * or nowhere: no walker can reach them from the top otherwise. */
        if (ents[i].algo == INVFS_ALGO_CHILD) {
            uint8_t *cblob = NULL;
            size_t cblen = 0;
            invfs_ast_hdr cah;
            const invfs_ast_block_entry *cents = NULL;
            size_t cn = 0;
            const uint8_t *caddr;

            if (depth + 1 > INVFS_AST_CHILD_DEPTH_MAX || !chld ||
                ents[i].block_id >= n_chld) {
                *bad_out = 1;
                continue;
            }
            caddr = chld + (size_t)ents[i].block_id *
                    sizeof(invfs_ast_child_ref);
            if (vol_recipe_load(v, caddr, &cblob, &cblen) != 0 || !cblob) {
                fprintf(stderr, "invfs: free: child recipe %u of a %zu-entry "
                        "recipe does not resolve; blocks stay allocated\n",
                        (unsigned)ents[i].block_id, n_ents);
                *bad_out = 1;
                continue;
            }
            if (vol_ast_recipe_parse(cblob, cblen, &cah, &cents, &cn) != 0 ||
                !cents) {
                fprintf(stderr, "invfs: free: child recipe %u does not "
                        "parse; blocks stay allocated\n",
                        (unsigned)ents[i].block_id);
                free(cblob);
                *bad_out = 1;
                continue;
            }
            free_recipe_level(v, NULL, 0, cents, cn, keep_pba, depth + 1,
                              bad_out);
            vol_ast_recipe_parsed_free(&cah, cents);
            free(cblob);
            continue;
        }
        if (!pba || pba == keep_pba)
            continue;
        /* WP78: a zone==TEXT entry names a SHARED batch segment owned
         * by the batch registry -- dropping this member's reference
         * must not free it; tz_gc reclaims it when no live member
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
            pba_ref_modify(v, pba, -1);
            if (pba_ref_count(v, pba) == 0) {
                if (seg_extent_checked(v, pba, &plen) == 0 && plen > 0)
                    vol_free_blocks(v, pba, plen);
            }
        }
    }
}

/* vol_ast_recipe_parsed_free lives in volume_internal.h (static inline:
 * partial core links such as cpack_map_conc_* resolve it without vol_ast.o). */

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
 * nothing and reachable by no name.
 *
 * WP635: a recipe may be a TREE. One level of child recipes is recursed here,
 * because the reference entries name no blocks (pba == 0) and nothing else
 * walks from the top to the children's data. A child that will not load or
 * parse is reported as -1 like an unparseable top: the caller then knows the
 * space is NOT back rather than being told it is. */
int vol_free_recipe_blocks(invfs_volume *v,
                             const uint8_t recipe_addr[INVFS_RECIPE_ADDR_LEN],
                             uint64_t keep_pba)
{
    static const uint8_t zero_addr[INVFS_RECIPE_ADDR_LEN] = {0};
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    const uint8_t *chld = NULL;
    uint32_t n_chld = 0;
    int parsed, bad = 0;

    if (!v || !recipe_addr)
        return -1;
    if (memcmp(recipe_addr, zero_addr, sizeof zero_addr) == 0)
        return 0;
    if (vol_recipe_load(v, recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    parsed = (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 &&
              ents != NULL);
    /* A malformed trailing section is a recipe this build cannot account for;
     * freeing part of it and calling it done is the exact shape the WP201 note
     * above is about, so it is a failure here too. */
    if (parsed &&
        vol_ast_recipe_children(blob, blen, &ah, &chld, &n_chld) != 0)
        parsed = 0;
    if (parsed)
        free_recipe_level(v, chld, n_chld, ents, n_ents, keep_pba, 0, &bad);
    vol_ast_recipe_parsed_free(&ah, ents);
    free(blob);
    return (parsed && !bad) ? 0 : -1;
}


/* WP202: release the blocks a SUPERSEDED inode recipe owned. One
 * implementation, three callers -- the containerpack commit
 * (cpack_release_superseded in vol_cpack.c, which is now a one-line
 * forward to this) and the builtin container lanes in
 * sweep_dispatch (vol_sweep.c), which did the same supersede through
 * vol_create_blob_file -> vol_create_content_node and, until now,
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
 *    vol_free_recipe_blocks (a shared batch segment is owned by the
 *    batch registry, not by this member), and the pba refcount means a
 *    block another live recipe still names is not freed here.
 *
 * WP612: the free is RETIRED, not run. The callers just staged the
 * superseding row (vol_create_blob_file -> vol_inode_delta_put), and a
 * staged row is not durable -- group commit may doom and unwind it, and
 * a fence failure restores the old recipe, which would then name freed
 * blocks (the WP607 s35/s23 use-after-free, sweep-lane form). The
 * delete-kind entry is wrong here (the row still exists, naming the NEW
 * recipe); the supersede-kind entry frees at the first successful fence
 * covering the new row, and only while that row is still the live one.
 * The moved-guard above stays: it is what keeps a same-address re-run
 * from retiring live blocks, and the drain's span+seq guard cannot see
 * it (both rows would stage the same address). Leaks (dropped entries,
 * over-cap) go to the sweep orphan collector, never to a double free. */
void vol_release_superseded_blob(
    invfs_volume *v, uint64_t inode_id,
    const uint8_t old_addr[INVFS_RECIPE_ADDR_LEN])
{
    static const uint8_t zero_addr[INVFS_RECIPE_ADDR_LEN] = {0};
    invfs_inode now;

    if (!v || !old_addr ||
        memcmp(old_addr, zero_addr, sizeof zero_addr) == 0)
        return;
    if (vol_inode_get(v, inode_id, &now) != 1)
        return;
    if (memcmp(now.recipe_addr, old_addr, INVFS_RECIPE_ADDR_LEN) == 0)
        return;
    CD("RETIRE inode=%llu", (unsigned long long)inode_id);
    vol_retire_recipe(v, inode_id, old_addr, 0);
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
 * absolute extent offset), so a bound over the contiguous record region
 * rejected every valid hint. Accept any position inside any mapper extent
 * (mirroring vol_inode_next); anything else is not a record position. */


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
    if (vol_path_lookup(v, name, &ino) != 1)
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
    rc = vol_path_lookup(v, name, &ino);
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
    if (vol_path_stat(v, name, &ino, size_out, ctime_out) != 0)
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
