/* vol_records.c — inode area append-only records, INO2 metadata,
 * xattr TLVs, storage-class flag, record retire/delete. Split from volume.c. */

#include "volume_internal.h"


/* ---- inode area (append-only records) ---- */

/* write inode record: name + AST blocks; returns inode_id (0 on error) */
uint64_t vol_create_file(invfs_volume *v, const char *name,
                         const uint8_t *data, size_t len)
{
    size_t i, ast_entries;
    /* Hard format limits: the AST recipe header stores file_size as uint32
       and num_blocks as uint16. Past either, the casts below silently fold
       the value and produce a record that looks valid but reconstructs the
       wrong bytes — the one failure mode this filesystem must not have.
       Refuse the write instead. (The container path already checked
       MAX_SEGMENTS; this one did not.) */
    if (len > 0xFFFFFFFFu ||
        (len + SEGMENT_SIZE - 1) / SEGMENT_SIZE > MAX_SEGMENTS) {
        fprintf(stderr, "invarifs: %s: %llu bytes exceeds the format limit "
                "(%u bytes / %u segments of %u)\n", name,
                (unsigned long long)len, 0xFFFFFFFFu, MAX_SEGMENTS,
                (unsigned)SEGMENT_SIZE);
        return 0;
    }
    if (name_too_long(name)) return 0;
    uint64_t inode_id = v->next_inode_id++;
    if (getenv("INVFS_DEBUG"))
        printf("[create_file] next_inode was %llu -> using %llu\n",
               (unsigned long long)(inode_id - 1), (unsigned long long)inode_id);
    invfs_ast_recipe_header ast_h;
    size_t rec_size;
    uint8_t *rec;
    invfs_inode_rec *rec_h;
    invfs_ast_block_entry *entries;
    uint32_t crc_rec;
    int *seg_lz4 = NULL;   /* per-segment: 1 = lz4, 0 = raw */
    uint32_t *seg_csize = NULL;

    if (len == 0) {
        /* empty file: inode record with 0 AST entries */
        invfs_ast_recipe_header ast_h0;
        size_t rec_size0 = sizeof(invfs_inode_rec) + sizeof(ast_h0);
        uint8_t *rec0 = (uint8_t *)calloc(1, rec_size0);
        invfs_inode_rec *rh0 = (invfs_inode_rec *)rec0;
        uint32_t crc0;
        if (!rec0) return 0;
        memset(&ast_h0, 0, sizeof(ast_h0));
        ast_h0.version = 1;
        ast_h0.file_size = 0;
        rh0->magic = INODE_REC_MAGIC;
        rh0->rec_len = (uint32_t)rec_size0;
        rh0->inode_id = inode_id;
        rh0->file_size = 0;
        rh0->ctime = (uint64_t)time(NULL);
        rec_set_name(rh0, name);
        memcpy(rec0 + sizeof(invfs_inode_rec), &ast_h0, sizeof(ast_h0));
        crc0 = invfs_crc32c(rec0, rec_size0);
        if (v->inode_area_pos + rec_size0 + 4 > v->inode_area_end) { free(rec0); return 0; }
        if (vol_pre_record(v) != 0) { free(rec0); return 0; }
        if (io_seek(&v->io, v->inode_area_pos) != 0 ||
            io_write(&v->io, rec0, rec_size0) != 0 ||
            io_write(&v->io, &crc0, 4) != 0) { free(rec0); return 0; }
        v->inode_area_pos += rec_size0 + 4;
        idx_put(v, name, strlen(name), inode_id,
                v->inode_area_pos - rec_size0 - 4, rh0->file_size, rh0->ctime);
        idx_put_id(v, inode_id, v->inode_area_pos - rec_size0 - 4);
        free(rec0);
        return inode_id;
    }

    /* 1. split file into SEGMENT_SIZE chunks, LZ4-compress each */
    ast_entries = (len + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    entries = (invfs_ast_block_entry *)calloc(ast_entries, sizeof(invfs_ast_block_entry));
    seg_lz4 = (int *)calloc(ast_entries, sizeof(int));
    seg_csize = (uint32_t *)calloc(ast_entries, sizeof(uint32_t));
    if (!entries || !seg_lz4 || !seg_csize) { free(entries); free(seg_lz4); free(seg_csize); return 0; }
    /* atomic ENOSPC: refuse before writing any segment when the whole
     * file (uncompressed worst case) cannot fit above the reserve+floor */
    {
        uint64_t need = (len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE
                        + ast_entries;   /* LZ4 overhead blocks */
        if (v->free_blocks <= v->sb.reserved_blocks + v->sb.hard_min_blocks + need) {
            fprintf(stderr, "[create] ENOSPC atomic: need %llu free %llu\n",
                    (unsigned long long)need,
                    (unsigned long long)v->free_blocks);
            free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
    }

    for (i = 0; i < ast_entries; i++) {
        const uint8_t *src = data + (size_t)i * SEGMENT_SIZE;
        size_t slen = (i + 1 == ast_entries) ? len - (size_t)i * SEGMENT_SIZE : SEGMENT_SIZE;
        int cbound = LZ4_compressBound((int)slen);
        /* one block of slack past the payload: the segment goes out as whole
           blocks, so write_segment_blocks zeroes up to the block boundary */
        uint8_t *cbuf = (uint8_t *)malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        uint8_t hdr[8];
        uint64_t pba, phys_blocks; int seg_zone;
        uint32_t csize, seg_crc;

        if (!cbuf) { free(entries); free(seg_lz4); free(seg_csize); return 0; }
        if (v->profile == INVFS_PROFILE_TURBO) {
            /* WP19 turbo: store verbatim (segment-aligned whole blocks,
             * same framing) -- skip even the LZ4 attempt */
            csize = 0;
        } else {
            csize = (uint32_t)LZ4_compress_default((const char *)src,
                                                   (char *)(cbuf + 8),
                                                   (int)slen, cbound);
        }
        if (csize == 0 || csize >= slen) {
            /* store raw */
            csize = (uint32_t)slen;
            memcpy(cbuf + 8, src, slen);
            seg_lz4[i] = 0;
        } else {
            seg_lz4[i] = 1;
        }
        seg_csize[i] = csize;
        seg_crc = invfs_crc32c(cbuf + 8, csize);
        if (getenv("INVFS_DEBUG"))
            printf("[create_file] seg %zu: slen=%zu csize=%u %s\n",
                   i, slen, csize, seg_lz4[i] ? "(lz4)" : "(raw)");

        hdr[0] = (uint8_t)(csize & 0xFF);
        hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
        hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
        hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
        hdr[4] = (uint8_t)(seg_crc & 0xFF);
        hdr[5] = (uint8_t)((seg_crc >> 8) & 0xFF);
        hdr[6] = (uint8_t)((seg_crc >> 16) & 0xFF);
        hdr[7] = (uint8_t)((seg_crc >> 24) & 0xFF);
        memcpy(cbuf, hdr, 8);

        /* allocate physical blocks and write segment */
        phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_raw_or_shadow(v, phys_blocks, &seg_zone);
        if (pba == 0) {
            fprintf(stderr, "[create] ENOSPC seg %zu\n", i);
            /* reclaim already-written segments so ENOSPC leaves no
             * orphans (no inode record -> blocks would leak until fsck) */
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
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
        if (write_segment_blocks(v, pba, cbuf, (size_t)csize + 8,
                                 phys_blocks) != 0) {
            fprintf(stderr, "[create] write fail seg %zu pba=%llu phys=%llu%s%s\n",
                    i, (unsigned long long)pba, (unsigned long long)phys_blocks,
#ifdef _WIN32
                    " err=", "");
            fprintf(stderr, "%lu", (unsigned long)GetLastError());
#else
                    "", "");
#endif
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
        free(cbuf);

        /* L2P: segment index -> physical start */
        if (vol_map(v, inode_id, (uint64_t)i, pba, (uint32_t)phys_blocks) != 0) {
            fprintf(stderr, "[create] L2P fail seg %zu\n", i);
            free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
        /* WP19: the new entry is born write-hot 1 and read-cold
         * (INVFS_HEAT_INIT may pre-warm the read side) */
        {
            invfs_l2p_entry *ne = &v->l2p[v->l2p_count - 1];
            l2p_set_rheat(ne, v->heat_init);
            ne->pad[2] = 1;
        }

        entries[i].file_offset = (uint64_t)i * SEGMENT_SIZE;
        entries[i].length = slen;
        entries[i].zone = (uint32_t)seg_zone;
        entries[i].algo = seg_lz4[i] ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
        entries[i].block_id = (uint32_t)i;   /* segment index (L2P lba) */
        entries[i].block_offset = 0;
    }
    free(seg_lz4);
    free(seg_csize);

    /* 2. AST recipe header */
    memset(&ast_h, 0, sizeof(ast_h));
    ast_h.version = 1;
    ast_h.file_size = (uint32_t)len;
    ast_h.num_blocks = (uint16_t)ast_entries;
    ast_h.num_children = 0;

    /* 3. assemble record: header + name + ast_header + entries */
    rec_size = sizeof(invfs_inode_rec) + sizeof(ast_h) + ast_entries * sizeof(invfs_ast_block_entry);
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) { free(entries); return 0; }
    rec_h = (invfs_inode_rec *)rec;
    rec_h->magic = INODE_REC_MAGIC;
    rec_h->rec_len = (uint32_t)rec_size;
    rec_h->inode_id = inode_id;
    rec_h->file_size = len;
    rec_h->ctime = (uint64_t)time(NULL);
    rec_set_name(rec_h, name);

    memcpy(rec + sizeof(invfs_inode_rec), &ast_h, sizeof(ast_h));
    memcpy(rec + sizeof(invfs_inode_rec) + sizeof(ast_h),
           entries, ast_entries * sizeof(invfs_ast_block_entry));
    free(entries);

    crc_rec = invfs_crc32c(rec, rec_size);
    /* append record with trailing CRC32C (4 bytes) */
    if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) {
        fprintf(stderr, "inode area full\n");
        free(rec);
        return 0;
    }
    /* Maps first: block_id in the AST is a segment index, so this record is
       readable only if its L2P is already on disk (see vol_pre_record). */
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


/* Collect the block_ids of an inode's zone==TEXT AST entries (WP10 §7).
 * Those segments are shared PPMd batches owned by the internal "\x01tzb"
 * inode; the retiring inode holds only duplicate L2P mappings to them, so
 * the blocks must survive the retire. *out is malloc'd (NULL when 0). A
 * parse failure yields 0/NULL, which just disables the skip -- the same
 * behaviour the retire path always had for a record it cannot read. */
static size_t collect_text_lbas(invfs_volume *v, uint64_t inode_id,
                                uint32_t **out)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_recipe_header ah;
    const invfs_ast_block_entry *ents;
    uint32_t *ids = NULL;
    size_t n = 0, i;

    *out = NULL;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;
    if (rl >= sizeof(invfs_inode_rec) + sizeof(ah)) {
        memcpy(&ah, buf + sizeof(invfs_inode_rec), sizeof(ah));
        if (rl >= sizeof(invfs_inode_rec) + sizeof(ah) +
                  (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
            ids = (uint32_t *)malloc(ah.num_blocks
                                     ? ah.num_blocks * sizeof(uint32_t) : 1);
            if (ids) {
                ents = (const invfs_ast_block_entry *)
                       (buf + sizeof(invfs_inode_rec) + sizeof(ah));
                for (i = 0; i < ah.num_blocks; i++)
                    if (ents[i].zone == INVFS_ZONE_TEXT)
                        ids[n++] = ents[i].block_id;
            }
        }
    }
    free(buf);
    *out = ids;
    return n;
}


/*
 * Delete a file: free its data blocks, unmap L2P, append tombstone.
 * Returns 0 on success, -1 if not found.
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
static int vol_retire_inode(invfs_volume *v, uint64_t inode_id,
                            const char *name, int free_data)
{
    uint64_t pos, end;
    size_t i;

    if (inode_id == 0)
        return -1;
    if (vol_mark_dirty(v) != 0)
        return -1;

    /* Give the cache's budget back. Not a safety measure: inode ids come from
       a monotonic counter and no path rewrites a record in place, so a stale
       entry could never be served for the wrong content -- it would just hold
       memory for a file that no longer exists. Dropping the ghost matters more
       than dropping the data: a ghost for an id that can never be inserted
       again is history the adaptation would learn nothing from. */
    arc_invalidate(v->arc, inode_id);
    /* WP16b: the parsed-map cache keys on the NAME (the read path resolves
     * member siblings by name): retiring "x" drops any map cached for it,
     * and retiring "x!..." (a member/table/map sibling) drops the map of
     * the container it belongs to. */
    cpack_map_cache_invalidate(v, name);

    /* free all blocks mapped to this inode.
     * L2P length is the PHYSICAL block count of each segment (written at
     * map time), so no header reads here — safe against stale/reallocated
     * blocks. */
    if (free_data) {
        /* WP10 §7 (anti-PB7): never free blocks a zone==TEXT AST entry
         * names. The member holds only an L2P dup into the shared batch,
         * which belongs to the hidden owner inode; freeing here would punch
         * a hole in every other member. GC reclaims dead batches. The dup
         * mappings themselves are dropped with the inode below, as usual. */
        uint32_t *text_lbas = NULL;
        size_t n_text = collect_text_lbas(v, inode_id, &text_lbas), t, j;
        /* PB7 (dedupe sharers): a segment merged by vol_sweep_dedupe is
         * mapped under EVERY sharer; freeing it with one retiring inode
         * leaves the survivors dangling (fsck "missing"). Skip any pba
         * another live MAP entry still references. Two pba-indexed bitmaps
         * make the check O(l2p) once per retire instead of O(own x l2p);
         * the second bitmap also stops double-frees when a file's own
         * entries share a pba (same-file dedupe). Costs 2 x total_blocks/8
         * bytes per delete. */
        uint8_t *refd = calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
        uint8_t *done = calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
        if (refd) {
            for (j = 0; j < v->l2p_count; j++) {
                const invfs_l2p_entry *o = &v->l2p[j];
                uint64_t b, bend;
                if (o->type != INVFS_JRN_MAP || o->inode == inode_id)
                    continue;
                if (o->pba >= v->sb.total_blocks) continue;
                bend = o->pba + o->length;
                if (bend > v->sb.total_blocks) bend = v->sb.total_blocks;
                for (b = o->pba; b < bend; b++)
                    refd[b >> 3] |= (uint8_t)(1u << (b & 7));
            }
        }
        for (i = 0; i < v->l2p_count; i++) {
            const invfs_l2p_entry *e = &v->l2p[i];
            if (e->type == INVFS_JRN_MAP && e->inode == inode_id) {
                uint64_t nblk = e->length, b, bend;
                int shared = 0;
                for (t = 0; t < n_text; t++)
                    if (text_lbas[t] == e->lba) { shared = 1; break; }
                if (shared) continue;
                if (nblk == 0 || e->pba >= v->sb.total_blocks ||
                    nblk > v->sb.total_blocks - e->pba)
                    continue;  /* stale entry — never free out of bounds */
                bend = e->pba + nblk;
                shared = 0;
                for (b = e->pba; b < bend; b++) {
                    if (refd && (refd[b >> 3] & (1u << (b & 7)))) {
                        shared = 1; break;   /* another live inode maps it */
                    }
                    if (done && (done[b >> 3] & (1u << (b & 7)))) {
                        shared = 1; break;   /* already freed this retire */
                    }
                }
                if (shared) continue;
                vol_free_blocks(v, e->pba, nblk);
                if (done)
                    for (b = e->pba; b < bend; b++)
                        done[b >> 3] |= (uint8_t)(1u << (b & 7));
            }
        }
        free(refd);
        free(done);
        free(text_lbas);
    }
    /* rewrite L2P in-memory: drop this inode's maps */
    {
        size_t w = 0;
        for (i = 0; i < v->l2p_count; i++) {
            if (v->l2p[i].inode == inode_id) {
                if (w < v->l2p_dirty) v->l2p_dirty = w;
                continue;
            }
            if (w != i) v->l2p[w] = v->l2p[i];
            w++;
        }
        v->l2p_count = w;
        if (v->l2p_dirty > v->l2p_count) v->l2p_dirty = v->l2p_count;
    }

    /* append tombstone record */
    pos = v->inode_area_pos;
    end = v->inode_area_end;
    if (pos + sizeof(invfs_inode_rec) + 4 > end)
        return -1;
    {
        invfs_inode_rec rec;
        uint32_t crc;
        memset(&rec, 0, sizeof(rec));
        rec.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
        rec.rec_len = (uint32_t)sizeof(invfs_inode_rec);
        rec.inode_id = inode_id;
        rec.file_size = 0;
        rec_set_name(&rec, name);
        crc = invfs_crc32c(&rec, sizeof(rec));
        if (io_seek(&v->io, pos) != 0 ||
            io_write(&v->io, &rec, sizeof(rec)) != 0 ||
            io_write(&v->io, &crc, 4) != 0)
            return -1;
        v->inode_area_pos = pos + sizeof(rec) + 4;
        idx_del(v, name, strlen(name), inode_id);
    }
    return 0;
}


int vol_delete_inode(invfs_volume *v, uint64_t inode_id, const char *name)
{
    return vol_retire_inode(v, inode_id, name, 1);
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
 * scan would walk into records it had just written. */
int vol_delete_siblings(invfs_volume *v, const char *name)
{
    char (*names)[256] = NULL;
    size_t n = 0, cap = 0, nlen = strlen(name), i;
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        char nm[257];
        size_t nl;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &h, sizeof h) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(invfs_inode_rec) ||
            h.rec_len > INVFS_MAX_REC_LEN) break;
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic != INODE_REC_MAGIC) continue;
        nl = h.name_len < 256 ? h.name_len : 256;
        memcpy(nm, h.name, nl);
        nm[nl] = 0;
        if (nl <= nlen + 1 || strncmp(nm, name, nlen) != 0 || nm[nlen] != '!')
            continue;
        if (vol_find(v, nm) == 0) continue;      /* already superseded */
        for (i = 0; i < n; i++)
            if (strcmp(names[i], nm) == 0) break;
        if (i < n) continue;                     /* older record, same name */
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            void *nn = realloc(names, ncap * sizeof(*names));
            if (!nn) break;                      /* purge what we gathered */
            names = (char (*)[256])nn;
            cap = ncap;
        }
        memcpy(names[n++], nm, nl + 1);
    }

    for (i = 0; i < n; i++)
        vol_delete_file(v, names[i]);
    free(names);
    return (int)n;
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
static size_t vol_ast_blob_len(const uint8_t *rec, size_t rec_len)
{
    invfs_ast_recipe_header h;
    size_t base = sizeof(invfs_inode_rec);
    size_t len;
    uint16_t i;
    const uint8_t *p, *end;
    if (rec_len < base + sizeof(h)) return (size_t)-1;
    memcpy(&h, rec + base, sizeof(h));
    len = sizeof(h) + (size_t)h.num_blocks * sizeof(invfs_ast_block_entry);
    if (len > rec_len - base) return (size_t)-1;
    p = rec + base + len;
    end = rec + rec_len;
    for (i = 0; i < h.num_children; i++) {
        uint16_t nl;
        if ((size_t)(end - p) < 2) return (size_t)-1;
        memcpy(&nl, p, 2); p += 2;
        /* wire child = [u16 nlen][name][u16 method][4x u32] */
        if (nl > MAX_AST_CHILD_NAME || (size_t)(end - p) < nl + 20)
            return (size_t)-1;
        p += nl + 20;
    }
    return (size_t)(p - (rec + base));
}


/* parse one xattr TLV at p; returns bytes consumed or 0 on corruption */
static size_t xattr_tlv_size(const uint8_t *p, size_t avail)
{
    uint16_t nl, vl;
    if (avail < 4) return 0;
    memcpy(&nl, p, 2);
    memcpy(&vl, p + 2 + nl, 2);
    if ((size_t)2 + nl + 2 + vl > avail || nl == 0) return 0;
    return (size_t)2 + nl + 2 + vl;
}


static int meta_parse_ext(const uint8_t *p, size_t n,
                          invfs_meta_pub *out,
                          uint8_t **xattrs_out, size_t *xlen_out)
{
    invfs_meta_ext_hdr h;
    if (n < sizeof(h)) return -1;
    memcpy(&h, p, sizeof(h));
    if (h.magic != INVFS_META_MAGIC || h.version != 2 ||
        h.ext_len > n || h.target_len >= sizeof(out->target))
        return -1;
    if ((size_t)sizeof(h) + h.target_len > h.ext_len) return -1;
    memset(out, 0, sizeof(*out));
    out->type = h.type;
    out->mode = h.mode;
    out->uid = h.uid;
    out->gid = h.gid;
    out->mtime = h.mtime;
    out->atime = h.atime;
    out->nlink = h.nlink;
    out->rdev = h.rdev;
    if (h.target_len) {
        memcpy(out->target, p + sizeof(h), h.target_len);
        out->target[h.target_len] = 0;
    } else {
        out->target[0] = 0;
    }
    if (xattrs_out) {
        *xattrs_out = (uint8_t *)(p + sizeof(h) + h.target_len);
        *xlen_out = h.ext_len - sizeof(h) - h.target_len;
    }
    return 0;
}


/* locate the ext inside a raw record; NULL when absent (v1 record) */
const uint8_t *meta_locate_ext(const uint8_t *rec, size_t rec_len,
                                      size_t *ext_len_out)
{
    size_t ast = vol_ast_blob_len(rec, rec_len);
    size_t off;
    invfs_meta_ext_hdr h;
    if (ast == (size_t)-1) return NULL;
    off = sizeof(invfs_inode_rec) + ast;
    if (rec_len < off + sizeof(h)) return NULL;
    memcpy(&h, rec + off, sizeof(h));
    if (h.magic != INVFS_META_MAGIC || h.ext_len > rec_len - off)
        return NULL;
    *ext_len_out = h.ext_len;
    return rec + off;
}


/* read the latest live record for an inode id; returns malloc'd buffer and
 * optionally its name/position. Walks forward from the index hint so stale
 * hints degrade to a full-area scan instead of wrong answers. */
int meta_read_record_by_id(invfs_volume *v, uint64_t inode_id,
                                  uint8_t **buf_out, uint32_t *rl_out,
                                  char *name_out, size_t name_cap,
                                  uint64_t *pos_out)
{
    uint64_t p, hint;

    hint = idx_get_id(v, inode_id);
    p = vol_inode_area_start(v);
    if (hint >= p && hint + sizeof(invfs_inode_rec) <= v->inode_area_pos)
        p = hint;
    /* vol_inode_next returns the NEXT scan position; the record it described
     * sits at p - rl - 4. Same pattern as vol_get_children. */
    while ((p = vol_inode_next(v, p, NULL, NULL, NULL, NULL, 0, rl_out)) != 0) {
        invfs_inode_rec rh;
        uint8_t *buf;
        if (vol_read_raw(v, p - *rl_out - 4, &rh, sizeof(rh)) != 0)
            break;
        if (rh.magic == TOMBSTONE_MAGIC) continue;
        if (rh.inode_id != inode_id) continue;
        buf = (uint8_t *)malloc(*rl_out);
        if (!buf) return -1;
        if (vol_read_raw(v, p - *rl_out - 4, buf, *rl_out) != 0) {
            free(buf);
            return -1;
        }
        if (name_out && name_cap) {
            size_t nl = rh.name_len < name_cap - 1 ? rh.name_len : name_cap - 1;
            memcpy(name_out, rh.name, nl);
            name_out[nl] = 0;
        }
        if (pos_out) *pos_out = p - *rl_out - 4;
        *buf_out = buf;
        return 0;
    }
    return -1;
}


static size_t meta_serialize(const invfs_meta_pub *m,
                             const uint8_t *xattrs, size_t xlen,
                             uint8_t *dst)
{
    invfs_meta_ext_hdr h;
    size_t tlen = m ? strlen(m->target) : 0;
    uint8_t *d = dst;
    memset(&h, 0, sizeof(h));
    h.magic = INVFS_META_MAGIC;
    h.version = 2;
    h.ext_len = (uint16_t)(sizeof(h) + tlen + xlen);
    if (m) {
        h.type = m->type;
        h.mode = m->mode;
        h.uid = m->uid;
        h.gid = m->gid;
        h.mtime = m->mtime;
        h.atime = m->atime;
        h.nlink = m->nlink;
        h.rdev = m->rdev;
        h.target_len = (uint16_t)tlen;
    }
    memcpy(d, &h, sizeof(h)); d += sizeof(h);
    if (tlen) { memcpy(d, m->target, tlen); d += tlen; }
    if (xlen) { memcpy(d, xattrs, xlen); d += xlen; }
    return (size_t)(d - dst);
}


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
    uint8_t *buf = NULL;
    uint32_t rl;
    const uint8_t *ext;
    size_t elen = 0;
    if (!out) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    ext = meta_locate_ext(buf, rl, &elen);
    free(buf);
    if (!ext) return -1;
    return meta_parse_ext(ext, elen, out, NULL, NULL);
}


/* append [INOD(same id, updated ext)][DELT(position-kill)] as one write */
static uint64_t meta_rewrite(invfs_volume *v, uint64_t inode_id,
                             const invfs_meta_pub *newpub,   /* NULL=keep */
                             const uint8_t *newx, size_t newxlen, /* NULL=keep */
                             uint64_t *new_pos_out)
{
    uint8_t *oldbuf = NULL, *nurec = NULL, *combo = NULL;
    uint32_t rl;
    char name[256];
    uint64_t old_pos = 0;
    invfs_inode_rec *oh, *nh;
    size_t ast, elen = 0, new_ext_size, nu_len, total, off;
    const uint8_t *extp;
    uint8_t *xattrs = NULL;
    size_t xlen = 0;
    invfs_meta_pub cur;
    invfs_inode_rec tomb;
    uint32_t crc_nu, crc_tb;

    if (meta_read_record_by_id(v, inode_id, &oldbuf, &rl, name, sizeof(name),
                               &old_pos) != 0)
        return 0;
    oh = (invfs_inode_rec *)oldbuf;
    ast = vol_ast_blob_len(oldbuf, rl);
    if (ast == (size_t)-1) { free(oldbuf); return 0; }

    extp = meta_locate_ext(oldbuf, rl, &elen);
    if (extp && meta_parse_ext(extp, elen, &cur, &xattrs, &xlen) != 0)
        extp = NULL;
    if (!extp) {
        meta_pub_from_hdr_defaults(&cur, oh->name_len &&
                                   name[oh->name_len - 1] == '/'
                                   ? INVFS_ITYP_DIR : INVFS_ITYP_REG);
        xattrs = NULL; xlen = 0;
    }

    {
        static const uint8_t no_x[1] = { 0 };
        if (!newpub) newpub = &cur;
        if (!newx) { newx = xattrs ? xattrs : no_x; newxlen = xattrs ? xlen : 0; }
        new_ext_size = sizeof(invfs_meta_ext_hdr) +
                       strlen(newpub->target) + newxlen;
        if (new_ext_size > INVFS_META_SLACK) { free(oldbuf); return 0; }
        nu_len = sizeof(invfs_inode_rec) + ast + new_ext_size;
        total = nu_len + 4 + sizeof(invfs_inode_rec) + 4;
        combo = (uint8_t *)calloc(1, total);
        if (!combo) { free(oldbuf); return 0; }

        nh = (invfs_inode_rec *)combo;
        memcpy(nh, oh, sizeof(*oh));
        nh->rec_len = (uint32_t)nu_len;   /* ext grows the record */
        memcpy(combo + sizeof(invfs_inode_rec),
               oldbuf + sizeof(invfs_inode_rec), ast);
        meta_serialize(newpub, newx, newxlen,
                       combo + sizeof(invfs_inode_rec) + ast);
        crc_nu = invfs_crc32c(combo, nu_len);
        memcpy(combo + nu_len, &crc_nu, 4);

        memset(&tomb, 0, sizeof(tomb));
        tomb.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
        tomb.rec_len = (uint32_t)sizeof(tomb);
        tomb.inode_id = inode_id;
        tomb.file_size = old_pos;          /* position kill (v2) */
        tomb.name_len = oh->name_len;
        memcpy(tomb.name, oh->name, sizeof(tomb.name));
        crc_tb = invfs_crc32c((uint8_t *)&tomb, sizeof(tomb));
        off = nu_len + 4;
        memcpy(combo + off, &tomb, sizeof(tomb));
        memcpy(combo + off + sizeof(tomb), &crc_tb, 4);
    }
    free(oldbuf);

    if (v->inode_area_pos + total > v->inode_area_end) { free(combo); return 0; }
    if (vol_mark_dirty(v) != 0) { free(combo); return 0; }
    {
        uint64_t rec_start = v->inode_area_pos;
        if (io_seek(&v->io, rec_start) != 0 ||
            io_write(&v->io, combo, total) != 0) {
            free(combo);
            return 0;
        }
        if (new_pos_out) *new_pos_out = rec_start;
        v->inode_area_pos = rec_start + total;
        idx_put(v, name, strlen(name), inode_id, rec_start,
                nh->file_size, nh->ctime);
        idx_put_id(v, inode_id, rec_start);
    }
    free(combo);
    return inode_id;
}


uint64_t vol_apply_meta(invfs_volume *v, const char *name,
                        const invfs_meta_pub *meta)
{
    uint64_t id;
    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    id = vol_find(v, name);
    if (id == 0) return 0;
    return meta_rewrite(v, id, meta, NULL, 0, NULL);
}


uint64_t vol_create_symlink(invfs_volume *v, const char *name,
                            const char *target)
{
    invfs_meta_pub m;
    size_t tl;
    uint64_t nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    if (name_too_long(name)) return 0;
    tl = strlen(target);
    if (tl == 0 || tl >= INVFS_META_TARGET_MAX) return 0;

    nid = vol_create_file(v, name, NULL, 0);
    if (nid == 0) return 0;
    meta_pub_from_hdr_defaults(&m, INVFS_ITYP_LNK);
    m.mode = 0777;
    memcpy(m.target, target, tl + 1);
    m.mtime = (int64_t)time(NULL);
    if (meta_rewrite(v, nid, &m, NULL, 0, NULL) == 0) {
        vol_delete_file(v, name);      /* roll back the empty placeholder */
        return 0;
    }
    return nid;
}


uint64_t vol_create_special(invfs_volume *v, const char *name,
                            uint8_t type, uint16_t mode, uint64_t rdev)
{
    invfs_meta_pub m;
    uint64_t nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    if (name_too_long(name)) return 0;
    if (type != INVFS_ITYP_FIFO && type != INVFS_ITYP_SOCK &&
        type != INVFS_ITYP_CHR && type != INVFS_ITYP_BLK)
        return 0;

    nid = vol_create_file(v, name, NULL, 0);
    if (nid == 0) return 0;
    meta_pub_from_hdr_defaults(&m, type);
    m.mode = mode;
    m.rdev = rdev;
    m.mtime = (int64_t)time(NULL);
    if (meta_rewrite(v, nid, &m, NULL, 0, NULL) == 0) {
        vol_delete_file(v, name);
        return 0;
    }
    return nid;
}


/* ---- xattr TLV helpers ---- */

int vol_get_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  void *val, size_t *vlen)
{
    uint8_t *buf = NULL, *x = NULL;
    uint32_t rl;
    size_t xl = 0, nlen = strlen(xn), rem, got = 0;
    const uint8_t *p;
    if (!vlen) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (!meta_locate_ext(buf, rl, &xl)) { free(buf); return -1; }
    /* re-walk via parse to get the TLV slice */
    {
        size_t ast = vol_ast_blob_len(buf, rl);
        const uint8_t *ext = buf + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) != 0) {
            free(buf);
            return -1;
        }
    }
    p = x; rem = xl;
    while (rem > 0) {
        size_t tsz = xattr_tlv_size(p, rem);
        uint16_t nl, vl;
        if (tsz == 0) break;
        memcpy(&nl, p, 2); memcpy(&vl, p + 2 + nl, 2);
        if (nl == nlen && memcmp(p + 2, xn, nlen) == 0) {
            got = vl;
            if (*vlen == 0) { *vlen = vl; free(buf); return 0; }
            if (*vlen < vl) { free(buf); return -2; }   /* ERANGE-ish */
            memcpy(val, p + 2 + nl + 2, vl);
            *vlen = vl;
            free(buf);
            return 0;
        }
        p += tsz; rem -= tsz;
    }
    free(buf);
    return -1;   /* ENODATA */
}


int vol_set_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  const void *val, size_t vlen)
{
    uint8_t *buf = NULL, *nb = NULL;
    uint32_t rl;
    size_t xl = 0, nlen = strlen(xn), rem;
    uint8_t *x = NULL;
    const uint8_t *p;
    size_t cap = INVFS_META_XATTR_MAX, used = 0, tsz;
    char name[256];
    int rc = -1;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    if (nlen == 0 || nlen > 255) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, name, sizeof(name),
                               NULL) != 0)
        return -1;
    nb = (uint8_t *)calloc(1, cap);
    if (!nb) { free(buf); return -1; }

    if (meta_locate_ext(buf, rl, &xl)) {
        size_t ast = vol_ast_blob_len(buf, rl);
        const uint8_t *ext = buf + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) == 0) {
            /* copy existing TLVs except the one being replaced */
            p = x; rem = xl;
            while (rem > 0) {
                uint16_t nl;
                tsz = xattr_tlv_size(p, rem);
                if (tsz == 0) break;
                memcpy(&nl, p, 2);
                if (!(nl == nlen && memcmp(p + 2, xn, nlen) == 0)) {
                    if (used + tsz > cap) goto done;
                    memcpy(nb + used, p, tsz);
                    used += tsz;
                }
                p += tsz; rem -= tsz;
            }
        }
    }
    tsz = 2 + nlen + 2 + vlen;
    if (used + tsz > cap) { rc = -2; goto done; }
    {
        uint16_t nl = (uint16_t)nlen, vl = (uint16_t)vlen;
        memcpy(nb + used, &nl, 2);
        memcpy(nb + used + 2, xn, nlen);
        memcpy(nb + used + 2 + nlen, &vl, 2);
        if (vlen) memcpy(nb + used + 2 + nlen + 2, val, vlen);
    }
    used += tsz;
    rc = meta_rewrite(v, inode_id, NULL, nb, used, NULL) ? 0 : -1;
done:
    free(nb);
    free(buf);
    return rc;
}


int vol_remove_xattr(invfs_volume *v, uint64_t inode_id, const char *xn)
{
    uint8_t *buf = NULL, *nb = NULL;
    uint32_t rl;
    size_t xl = 0, nlen = strlen(xn), rem, used = 0, cap = INVFS_META_XATTR_MAX;
    uint8_t *x = NULL;
    const uint8_t *p;
    char name[256];
    int found = 0, rc;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, name, sizeof(name),
                               NULL) != 0)
        return -1;
    if (!meta_locate_ext(buf, rl, &xl)) { free(buf); return -1; }
    nb = (uint8_t *)calloc(1, cap);
    if (!nb) { free(buf); return -1; }
    {
        size_t ast = vol_ast_blob_len(buf, rl);
        const uint8_t *ext = buf + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) != 0) {
            free(nb); free(buf); return -1;
        }
    }
    p = x; rem = xl;
    while (rem > 0) {
        uint16_t nl;
        size_t tsz = xattr_tlv_size(p, rem);
        if (tsz == 0) break;
        memcpy(&nl, p, 2);
        if (nl == nlen && memcmp(p + 2, xn, nlen) == 0) {
            found = 1;
        } else {
            if (used + tsz > cap) break;
            memcpy(nb + used, p, tsz);
            used += tsz;
        }
        p += tsz; rem -= tsz;
    }
    if (!found) { free(nb); free(buf); return -1; }
    rc = meta_rewrite(v, inode_id, NULL, nb, used, NULL) ? 0 : -1;
    free(nb);
    free(buf);
    return rc;
}


int vol_list_xattr(invfs_volume *v, uint64_t inode_id,
                   char *buf, size_t bcap)
{
    uint8_t *rb = NULL, *x = NULL;
    uint32_t rl;
    size_t xl = 0, rem, used = 0;
    const uint8_t *p;
    if (meta_read_record_by_id(v, inode_id, &rb, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (!meta_locate_ext(rb, rl, &xl)) { free(rb); return 0; }  /* none */
    {
        size_t ast = vol_ast_blob_len(rb, rl);
        const uint8_t *ext = rb + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) != 0) {
            free(rb);
            return -1;
        }
    }
    p = x; rem = xl;
    while (rem > 0) {
        uint16_t nl;
        size_t tsz = xattr_tlv_size(p, rem);
        if (tsz == 0) break;
        memcpy(&nl, p, 2);
        if (buf) {
            if (used + nl + 1 > bcap) { free(rb); return -2; }
            memcpy(buf + used, p + 2, nl);
            buf[used + nl] = 0;
        }
        used += nl + 1;
        p += tsz; rem -= tsz;
    }
    free(rb);
    return (int)used;
}


/* ---- WP10 storage-class flag ("invfs.class" xattr, WP10 §2) ---- */

int vol_get_class(invfs_volume *v, uint64_t inode_id,
                  uint8_t *cls, uint8_t *algo, uint16_t *gen)
{
    invfs_class_tlv tlv;
    size_t vlen = sizeof(tlv);
    if (vol_get_xattr(v, inode_id, INVFS_XATTR_CLASS, &tlv, &vlen) != 0 ||
        vlen != sizeof(tlv))
        return 1;   /* absent (a malformed value reads as unclassified) */
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




/* Does this record own "name!..." siblings that must die with it? A
 * ZIP-style container lists AST children; the extraction containers
 * (TARR/GZR/PNGR/FLACR/EXER) carry num_children == 0 but keep their
 * payload in sibling inodes ("name!partN", "name!recipe", "name!jxl",
 * "name!coverN", "name!exrN") the read path resolves by name -- deleting
 * only the anchor strands them as live records nothing reaches (verified:
 * a TAR's parts survived vol_unlink). The sibling walk is O(area), so
 * plain files skip it.
 * 1 = siblings possible (unknown record -> 1: scan conservatively). */
int record_owns_siblings(const uint8_t *rec, uint32_t rl)
{
    invfs_ast_recipe_header ah;
    size_t base = sizeof(invfs_inode_rec);
    uint32_t fl;

    if (!rec || rl < base + sizeof(ah) + sizeof(invfs_ast_block_entry))
        return 1;
    memcpy(&ah, rec + base, sizeof(ah));
    if (ah.num_children != 0)
        return 1;
    if (ah.num_blocks == 0)
        return 0;
    memcpy(&fl, rec + base + sizeof(ah) + 16, 4);   /* zone:2 | algo:6 LSB */
    {
        uint32_t algo = (fl >> 2) & 0x3F;
        switch (algo) {
        case INVFS_ALGO_TARR:
        case INVFS_ALGO_GZR:
        case INVFS_ALGO_PNGR:
        case INVFS_ALGO_FLACR:
        case INVFS_ALGO_EXER:
            return 1;
        }
        /* WP16a: a container codecpack's recipe record owns "!mbrNNNN"
         * siblings. An algo this build cannot resolve (the pack is not
         * loaded right now) gets the conservative answer: the scan costs
         * one area walk and can only find what is there -- the codec-pack
         * case (raw_image et al) simply has no siblings to find. */
        {
            const invfs_codec *pc = invfs_codec_by_algo(algo);
            const invfs_pack_def *pd;
            if (!pc) return 1;
            pd = invfs_codec_pack_def(pc);
            if (pd && pd->is_container) return 1;
        }
    }
    return 0;
}


/* test-only export of the static parser */
int meta_read_record_by_id_p(invfs_volume *v, uint64_t id, uint8_t **buf, uint32_t *rl)
{ return meta_read_record_by_id(v, id, buf, rl, NULL, 0, NULL); }
