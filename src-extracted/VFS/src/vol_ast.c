/* vol_ast.c — AST children: serialize / deserialize / container
 * creation + lookup by name. Split from volume.c. */

#include "volume_internal.h"



/* ---- AST children: serialize / deserialize / container creation ---- */

/* serialize children after the block entries; malloc'd buf or NULL */
static uint8_t *vol_serialize_children(const invfs_ast_child_entry *ch,
                                       size_t n, size_t *len_out)
{
    size_t total = 0;
    uint8_t *buf, *p;
    for (size_t i = 0; i < n; i++) {
        if (ch[i].name_len > MAX_AST_CHILD_NAME) return NULL;
        total += 2 + ch[i].name_len + 2 + 4 + 4 + 4 + 4;
        if (total > INVFS_MAX_CHILD_BLOB) return NULL;  /* cap children blob */
    }
    buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return NULL;
    p = buf;
    for (size_t i = 0; i < n; i++) {
        uint16_t nl = (uint16_t)ch[i].name_len;
        uint16_t method = ch[i].method;
        memcpy(p, &nl, 2); p += 2;
        memcpy(p, ch[i].name, nl); p += nl;
        memcpy(p, &method, 2); p += 2;
        memcpy(p, &ch[i].csize, 4); p += 4;
        memcpy(p, &ch[i].usize, 4); p += 4;
        memcpy(p, &ch[i].crc, 4); p += 4;
        memcpy(p, &ch[i].data_off, 4); p += 4;
    }
    *len_out = total;
    return buf;
}


/* deserialize children with hard bounds against blob_len (no OOM) */
static int vol_deserialize_children(const uint8_t *blob, size_t blob_len,
                                    invfs_ast_child_entry **out, size_t *n_out)
{
    const uint8_t *p = blob, *end = blob + blob_len;
    invfs_ast_child_entry *ch;
    size_t n = 0, cap = 64;
    ch = (invfs_ast_child_entry *)malloc(cap * sizeof(*ch));
    if (!ch) return -1;
    while (p + 2 <= end && n < MAX_AST_CHILDREN) {
        uint16_t nl;
        memcpy(&nl, p, 2); p += 2;
        if (nl > MAX_AST_CHILD_NAME || p + nl + 18 > end) break;  /* corrupt */
        if (n == cap) {
            cap *= 2;
            invfs_ast_child_entry *nc =
                (invfs_ast_child_entry *)realloc(ch, cap * sizeof(*ch));
            if (!nc) { free(ch); return -1; }
            ch = nc;
        }
        ch[n].name_len = nl;
        memcpy(ch[n].name, p, nl); ch[n].name[nl] = 0;
        p += nl;
        if (p + 20 > end) break;
        memcpy(&ch[n].method, p, 2); p += 2;
        memcpy(&ch[n].csize, p, 4); p += 4;
        memcpy(&ch[n].usize, p, 4); p += 4;
        memcpy(&ch[n].crc, p, 4); p += 4;
        memcpy(&ch[n].data_off, p, 4); p += 4;
        n++;
    }
    if (n == 0) { free(ch); *out = NULL; *n_out = 0; return 0; }
    *out = ch;
    *n_out = n;
    return 0;
}


/* create file with container children (ZIP members) — bounded allocs */
uint64_t vol_create_container_file(invfs_volume *v, const char *name,
                                   const uint8_t *data, size_t len,
                                   const invfs_ast_child_entry *children,
                                   size_t nchildren)
{
    if (name_too_long(name)) return 0;
    size_t i, ast_entries;
    uint64_t inode_id = v->next_inode_id++;
    uint8_t ast_h[INVFS_AST_HDR_V2_LEN];
    size_t ast_hlen;
    size_t rec_size, children_blob_len = 0;
    uint8_t *rec, *children_blob = NULL;
    invfs_inode_rec *rec_h;
    invfs_ast_block_entry *entries;
    uint32_t crc_rec;
    int *seg_lz4 = NULL;
    uint32_t *seg_csize = NULL;

    if (nchildren > MAX_AST_CHILDREN) return 0;
    children_blob = vol_serialize_children(children, nchildren, &children_blob_len);
    if (nchildren && !children_blob) return 0;

    ast_entries = (len + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    /* A container keeps the ORIGINAL archive bytes and the children windows
       address them with u32 fields (csize/usize/data_off) — >4 GB containers
       (ZIP64 et al) are a different format, not a bigger header: refuse.
       This is the one writer that never needs the v2 recipe header. */
    if (len > 0xFFFFFFFFu) { free(children_blob); return 0; }
    if (ast_entries > MAX_SEGMENTS) { free(children_blob); return 0; }
    entries = (invfs_ast_block_entry *)calloc(ast_entries, sizeof(invfs_ast_block_entry));
    seg_lz4 = (int *)calloc(ast_entries, sizeof(int));
    seg_csize = (uint32_t *)calloc(ast_entries, sizeof(uint32_t));
    if (!entries || !seg_lz4 || !seg_csize) {
        free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
        return 0;
    }

    for (i = 0; i < ast_entries; i++) {
        const uint8_t *src = data + (size_t)i * SEGMENT_SIZE;
        size_t slen = (i + 1 == ast_entries) ? len - (size_t)i * SEGMENT_SIZE : SEGMENT_SIZE;
        int cbound = LZ4_compressBound((int)slen);
        /* one block of slack past the payload (see vol_create_file) */
        uint8_t *cbuf = (uint8_t *)malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        uint8_t hdr[8];
        uint64_t pba, phys_blocks; int seg_zone;
        uint32_t csize, seg_crc;

        if (!cbuf) { free(entries); free(seg_lz4); free(seg_csize); free(children_blob); return 0; }
        csize = (uint32_t)LZ4_compress_default((const char *)src, (char *)(cbuf + 8),
                                               (int)slen, cbound);
        if (csize == 0 || csize >= slen) {
            csize = (uint32_t)slen;
            memcpy(cbuf + 8, src, slen);
            seg_lz4[i] = 0;
        } else {
            seg_lz4[i] = 1;
        }
        seg_csize[i] = csize;
        seg_crc = invfs_crc32c(cbuf + 8, csize);
        /* segment header: [4B csize][4B crc32c(data)][data] */
        hdr[0] = (uint8_t)(csize & 0xFF);
        hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
        hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
        hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
        hdr[4] = (uint8_t)(seg_crc & 0xFF);
        hdr[5] = (uint8_t)((seg_crc >> 8) & 0xFF);
        hdr[6] = (uint8_t)((seg_crc >> 16) & 0xFF);
        hdr[7] = (uint8_t)((seg_crc >> 24) & 0xFF);
        memcpy(cbuf, hdr, 8);

        phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks, phys_blocks, 0);
        if (pba == 0) {
            fprintf(stderr, "[create] ENOSPC seg %zu\n", i);
            /* reclaim already-written segments (no orphans) */
            {
                size_t k;
                for (k = 0; k < i; k++) {
                    uint64_t pba_k = 0, len_k = 0;
                    if (vol_lookup_entry(v, inode_id, (uint64_t)k,
                                         &pba_k, &len_k) == 0 && pba_k) {
                        vol_free_blocks(v, pba_k, len_k);
                        l2p_remove(v, inode_id, (uint64_t)k);
                    }
                }
            }
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
            return 0;
        }
        if (write_segment_blocks(v, pba, cbuf, (size_t)csize + 8,
                                 phys_blocks) != 0) {
            fprintf(stderr, "[create] write fail seg %zu\n", i);
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
            return 0;
        }
        free(cbuf);
        if (vol_map(v, inode_id, (uint64_t)i, pba, (uint32_t)phys_blocks) != 0) {
            fprintf(stderr, "[create] L2P fail seg %zu\n", i);
            free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
            return 0;
        }
        entries[i].file_offset = (uint64_t)i * SEGMENT_SIZE;
        entries[i].length = slen;
        entries[i].zone = INVFS_ZONE_RAW;
        entries[i].algo = seg_lz4[i] ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
        entries[i].block_id = (uint32_t)i;
        entries[i].block_offset = 0;
    }
    free(seg_lz4);
    free(seg_csize);

    /* v1 by construction: the 4 GB / 65535-segment refusals above keep the
     * header's fields inside the v1 ranges (helper double-checks). */
    ast_hlen = invfs_ast_hdr_write(ast_h, len, (uint32_t)ast_entries,
                                   (uint32_t)(nchildren > 0xFFFF ?
                                              0xFFFF : nchildren));
    if (!ast_hlen) { free(entries); free(children_blob); return 0; }

    rec_size = sizeof(invfs_inode_rec) + ast_hlen +
               ast_entries * sizeof(invfs_ast_block_entry) + children_blob_len;
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) { free(entries); free(children_blob); return 0; }
    rec_h = (invfs_inode_rec *)rec;
    rec_h->magic = INODE_REC_MAGIC;
    rec_h->rec_len = (uint32_t)rec_size;
    rec_h->inode_id = inode_id;
    rec_h->file_size = (uint64_t)len;   /* original archive size (1:1) */
    rec_h->ctime = (uint64_t)time(NULL);
    rec_set_name(rec_h, name);

    memcpy(rec + sizeof(invfs_inode_rec), ast_h, ast_hlen);
    memcpy(rec + sizeof(invfs_inode_rec) + ast_hlen,
           entries, ast_entries * sizeof(invfs_ast_block_entry));
    if (children_blob_len)
        memcpy(rec + sizeof(invfs_inode_rec) + ast_hlen +
               ast_entries * sizeof(invfs_ast_block_entry),
               children_blob, children_blob_len);
    free(entries);
    free(children_blob);

    crc_rec = invfs_crc32c(rec, rec_size);
    if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) {
        fprintf(stderr, "inode area full\n");
        free(rec);
        return 0;
    }
    if (vol_pre_record(v) != 0) { free(rec); return 0; }
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc_rec, 4) != 0) {
        free(rec);
        return 0;
    }
    v->inode_area_pos += rec_size + 4;
    idx_put(v, name, strlen(name), inode_id, v->inode_area_pos - rec_size - 4,
            rec_h->file_size, rec_h->ctime);
    idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
    free(rec);
    return inode_id;
}


/* parse ZIP central directory into children (bounded, no OOM);
 * returns number of children, -1 on structural failure */
int vol_zip_parse_children(const uint8_t *z, size_t zlen,
                           invfs_ast_child_entry *ch, size_t maxch)
{
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


/* read children from an inode record (bounded); 0 = none, -1 = corrupt */
int vol_get_children(invfs_volume *v, uint64_t inode_id,
                     invfs_ast_child_entry **out, size_t *n_out)
{
    invfs_ast_hdr ast_h;
    uint8_t *rec = NULL;
    size_t rec_len;
    uint32_t nblocks;
    uint64_t p;
    uint32_t magic;
    uint64_t ino, fsz;
    uint32_t rl;

    *out = NULL;
    *n_out = 0;
    p = vol_inode_area_start(v);
    /* Jump straight to the record. This used to scan the whole inode area for
       every call, and invf-ls calls it once per listed file -- listing 40k
       files meant 40k full-area scans. 0 means not indexed; fall back to the
       scan, which is also what keeps the old semantics for a stale id. */
    {
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= p && ip + sizeof(invfs_inode_rec) <= v->inode_area_pos)
            p = ip;
    }
    while ((p = vol_inode_next(v, p, &magic, &ino, &fsz, NULL, 0, &rl)) != 0) {
        if (magic != INODE_REC_MAGIC || ino != inode_id) continue;
        rec = (uint8_t *)malloc(rl);
        if (!rec) return -1;
        if (vol_read_raw(v, p - rl - 4, rec, rl) != 0) { free(rec); return -1; }
        rec_len = rl;
        if (rec_len < sizeof(invfs_inode_rec) + INVFS_AST_HDR_V1_LEN ||
            invfs_ast_hdr_parse(rec + sizeof(invfs_inode_rec),
                                rec_len - sizeof(invfs_inode_rec),
                                &ast_h) != 0) {
            free(rec);
            return -1;
        }
        nblocks = ast_h.num_blocks;
        if (rec_len < sizeof(invfs_inode_rec) + ast_h.hdr_len +
                      (size_t)nblocks * sizeof(invfs_ast_block_entry)) {
            free(rec);
            return -1;
        }
        if (ast_h.num_children == 0) { free(rec); *out = NULL; *n_out = 0; return 0; }
        {
            const uint8_t *children_blob = rec + sizeof(invfs_inode_rec) +
                                           ast_h.hdr_len +
                                           (size_t)nblocks * sizeof(invfs_ast_block_entry);
            int rc = vol_deserialize_children(children_blob,
                                              rec_len - (size_t)(children_blob - rec),
                                              out, n_out);
            free(rec);
            return rc;
        }
    }
    return -1;  /* inode not found */
}


/* find inode record by name; returns inode_id (0 = not found) */
uint64_t vol_find(invfs_volume *v, const char *name)
{
    /* O(1) via the in-memory index; last-record-wins and the
       tombstone-kills-only-its-own-version rule are applied when the
       index is built and maintained, not re-derived here. */
    const name_index_entry *e = idx_get(v, name, strlen(name));
    return e ? e->inode_id : 0;
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
