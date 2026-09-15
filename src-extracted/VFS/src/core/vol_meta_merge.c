/* vol_meta_merge.c — WP30 Phase 5: metadata extent merge/consolidation.
 * Split from volume.c. */

#include "volume_internal.h"

/* ---- WP30: metadata extent WAL helpers -------------------------------- */

static uint32_t meta_wal_crc(const invfs_meta_wal *w)
{
    return invfs_crc32c(w, offsetof(invfs_meta_wal, crc));
}

int meta_wal_push(invfs_volume *v, const invfs_meta_wal *w)
{
    invfs_meta_wal e = *w;
    e.crc = 0;
    e.crc = (uint16_t)invfs_crc32c(&e, offsetof(invfs_meta_wal, crc));
    return jrn_push_meta_op(v, &e);
}

static int meta_wal_shrink(invfs_volume *v, uint16_t ext_idx,
                           uint64_t new_pba, uint8_t new_class)
{
    invfs_meta_wal w;
    memset(&w, 0, sizeof w);
    w.type = INVFS_JRN_META_SHRINK;
    w.ext_idx = ext_idx;
    w.pba = new_pba;
    w.size_class = new_class;
    return meta_wal_push(v, &w);
}

static int meta_wal_merge(invfs_volume *v, uint16_t tgt_idx,
                          uint16_t src_idx, uint64_t new_pba, uint8_t new_class)
{
    invfs_meta_wal w;
    memset(&w, 0, sizeof w);
    w.type = INVFS_JRN_META_MERGE;
    w.ext_idx = tgt_idx;
    w.aux = (uint8_t)src_idx;
    w.pba = new_pba;
    w.size_class = new_class;
    return meta_wal_push(v, &w);
}

static int meta_wal_free(invfs_volume *v, uint16_t ext_idx)
{
    invfs_meta_wal w;
    memset(&w, 0, sizeof w);
    w.type = INVFS_JRN_META_FREE;
    w.ext_idx = ext_idx;
    return meta_wal_push(v, &w);
}

/* ---- metadata extent size helpers ------------------------------------- */

static uint64_t extent_bytes(uint8_t size_class)
{
    return 65536ULL << size_class;
}

static uint32_t extent_blocks(uint8_t size_class)
{
    return (uint32_t)((extent_bytes(size_class) + INVFS_BLOCK_SIZE - 1) /
                       INVFS_BLOCK_SIZE);
}

/* ---- mapper table accessors ------------------------------------------- */

/* Read the mapper table from disk into v->meta_mapper if not already cached.
 * Returns 0 on success, -1 on error. */
int meta_mapper_load(invfs_volume *v)
{
    if (v->meta_mapper) return 0;
    if (!v->met0_present) return 0;
    if (v->sb.meta_mapper_pba == 0) return 0;

    uint64_t blocks = INVFS_META_EXT_BLOCKS;
    uint64_t bytes = blocks * INVFS_BLOCK_SIZE;
    uint64_t *buf = (uint64_t *)malloc((size_t)bytes);
    if (!buf) return -1;

    if (io_seek(&v->io, v->sb.meta_mapper_pba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, (size_t)bytes) != 0) {
        free(buf);
        return -1;
    }

    v->meta_mapper = buf;
    v->meta_mapper_n = INVFS_META_EXT_ENTRIES;
    return 0;
}

/* Persist the mapper table to disk. Returns 0 on success, -1 on error. */
int meta_mapper_flush(invfs_volume *v)
{
    if (!v->meta_mapper || v->sb.meta_mapper_pba == 0) return 0;
    uint64_t bytes = INVFS_META_EXT_BLOCKS * INVFS_BLOCK_SIZE;
    if (io_seek(&v->io, v->sb.meta_mapper_pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, v->meta_mapper, (size_t)bytes) != 0)
        return -1;
    return 0;
}

/* Get mapper entry at index i */
uint64_t meta_mapper_get(const invfs_volume *v, size_t i)
{
    if (!v->meta_mapper || i >= v->meta_mapper_n) return 0;
    return v->meta_mapper[i];
}

/* Set mapper entry at index i */
void meta_mapper_set(invfs_volume *v, size_t i, uint64_t entry)
{
    if (!v->meta_mapper || i >= v->meta_mapper_n) return;
    v->meta_mapper[i] = entry;
}

/* ---- extent occupancy analysis ---------------------------------------- */

/* Count live records in an extent by scanning the inode area for records
 * whose AST entries reference blocks within [pba, pba+blocks).
 * Returns count of live entries referencing this extent. */
static uint32_t extent_live_count(invfs_volume *v, uint64_t pba,
                                  uint64_t blocks)
{
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    uint32_t count = 0;

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec rh;
        uint8_t *rb = NULL;
        uint32_t stored, calc;

        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rh, sizeof(rh)) != 0)
            break;
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC)
            break;
        if (rh.rec_len < sizeof(rh) || rh.rec_len > INVFS_MAX_REC_LEN ||
            pos + rh.rec_len + 4 > end) break;

        rb = (uint8_t *)malloc((size_t)rh.rec_len + 4);
        if (!rb) break;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rb, (size_t)rh.rec_len + 4) != 0) {
            free(rb);
            break;
        }
        memcpy(&stored, rb + rh.rec_len, 4);
        calc = invfs_crc32c(rb, rh.rec_len);
        if (calc != stored) { free(rb); pos += (uint64_t)rh.rec_len + 4; continue; }
        pos += (uint64_t)rh.rec_len + 4;

        if (rh.magic != INODE_REC_MAGIC) { free(rb); continue; }

        /* check if this record's AST entries reference blocks in this extent */
        {
            size_t base = sizeof(invfs_inode_rec);
            invfs_ast_hdr ah;
            if (rh.rec_len >= base + INVFS_AST_HDR_V1_LEN &&
                invfs_ast_hdr_parse(rb + base, rh.rec_len - base, &ah) == 0 &&
                rh.rec_len >= base + ah.hdr_len +
                              (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
                const uint8_t *ep = rb + base + ah.hdr_len;
                uint32_t pi;
                for (pi = 0; pi < ah.num_blocks; pi++) {
                    uint64_t epba;
                    memcpy(&epba, ep + pi * sizeof(invfs_ast_block_entry) + 24, 8);
                    if (epba >= pba && epba < pba + blocks) {
                        count++;
                        break;  /* one reference per record is enough */
                    }
                }
            }
        }
        free(rb);
    }
    return count;
}

/* Estimate dead record fraction in an extent.
 * Returns fraction * 1000 (e.g., 250 = 25.0% dead). */
static uint32_t extent_dead_fraction(invfs_volume *v, uint64_t pba,
                                     uint64_t blocks)
{
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    uint32_t total = 0, dead = 0;

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec rh;
        uint8_t *rb = NULL;
        uint32_t stored, calc;
        int is_live = 0;

        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rh, sizeof(rh)) != 0)
            break;
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC)
            break;
        if (rh.rec_len < sizeof(rh) || rh.rec_len > INVFS_MAX_REC_LEN ||
            pos + rh.rec_len + 4 > end) break;

        rb = (uint8_t *)malloc((size_t)rh.rec_len + 4);
        if (!rb) break;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rb, (size_t)rh.rec_len + 4) != 0) {
            free(rb);
            break;
        }
        memcpy(&stored, rb + rh.rec_len, 4);
        calc = invfs_crc32c(rb, rh.rec_len);
        if (calc != stored) { free(rb); pos += (uint64_t)rh.rec_len + 4; continue; }
        pos += (uint64_t)rh.rec_len + 4;

        if (rh.magic == TOMBSTONE_MAGIC) {
            dead++;
            total++;
        } else if (rh.magic == INODE_REC_MAGIC) {
            total++;
            /* check if live version */
            uint64_t ip = idx_get_id(v, rh.inode_id);
            uint64_t expected_pos = pos - ((uint64_t)rh.rec_len + 4);
            if (vol_find(v, rh.name) == rh.inode_id &&
                ip && ip == expected_pos) {
                is_live = 1;
            }
            if (!is_live) dead++;
        }
        free(rb);
    }
    if (total == 0) return 0;
    return (dead * 1000) / total;
}

/* ---- extent shrink ---------------------------------------------------- */

/* In-place shrink: read extent's records, drop dead ones, rewrite at same
 * location with smaller size_class, free excess blocks, log META_SHRINK.
 * Returns 0 on success, <0 on error, >0 if shrink not worthwhile. */
int vol_meta_extent_shrink(invfs_volume *v, uint16_t ext_idx)
{
    uint64_t entry = meta_mapper_get(v, ext_idx);
    if (!entry) return 1;

    uint64_t pba = invfs_meta_ext_pba(entry);
    uint8_t cur_class = invfs_meta_ext_class(entry);
    uint64_t cur_size = invfs_meta_ext_size(entry);
    uint32_t cur_blocks = (uint32_t)(cur_size / INVFS_BLOCK_SIZE);

    if (cur_class <= INVFS_META_EXT_MIN_SIZE_CLASS) return 1;

    /* scan for records referencing this extent and count live vs dead */
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    uint32_t live_count = 0, total_count = 0;
    uint64_t live_bytes = 0;

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec rh;
        uint8_t *rb = NULL;
        uint32_t stored, calc;

        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rh, sizeof(rh)) != 0)
            break;
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC)
            break;
        if (rh.rec_len < sizeof(rh) || rh.rec_len > INVFS_MAX_REC_LEN ||
            pos + rh.rec_len + 4 > end) break;

        rb = (uint8_t *)malloc((size_t)rh.rec_len + 4);
        if (!rb) break;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rb, (size_t)rh.rec_len + 4) != 0) {
            free(rb);
            break;
        }
        memcpy(&stored, rb + rh.rec_len, 4);
        calc = invfs_crc32c(rb, rh.rec_len);
        if (calc != stored) { free(rb); pos += (uint64_t)rh.rec_len + 4; continue; }
        pos += (uint64_t)rh.rec_len + 4;

        if (rh.magic == TOMBSTONE_MAGIC) {
            total_count++;
            continue;
        }
        if (rh.magic != INODE_REC_MAGIC) { free(rb); continue; }

        /* check if live version of this record */
        uint64_t ip = idx_get_id(v, rh.inode_id);
        uint64_t expected_pos = pos - ((uint64_t)rh.rec_len + 4);
        int is_live = (vol_find(v, rh.name) == rh.inode_id &&
                       ip && ip == expected_pos);

        /* check if record references blocks in this extent */
        {
            size_t base = sizeof(invfs_inode_rec);
            invfs_ast_hdr ah;
            int refs_extent = 0;
            if (rh.rec_len >= base + INVFS_AST_HDR_V1_LEN &&
                invfs_ast_hdr_parse(rb + base, rh.rec_len - base, &ah) == 0 &&
                rh.rec_len >= base + ah.hdr_len +
                              (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
                const uint8_t *ep = rb + base + ah.hdr_len;
                uint32_t pi;
                for (pi = 0; pi < ah.num_blocks; pi++) {
                    uint64_t epba;
                    memcpy(&epba, ep + pi * sizeof(invfs_ast_block_entry) + 24, 8);
                    if (epba >= pba && epba < pba + cur_blocks) {
                        refs_extent = 1;
                        break;
                    }
                }
            }
            if (refs_extent) {
                total_count++;
                if (is_live) {
                    live_count++;
                    live_bytes += rh.rec_len + 4;
                }
            }
        }
        free(rb);
    }

    /* need at least 25% space savings to be worthwhile */
    if (live_bytes >= cur_size / 4) return 1;

    /* find the new size class that fits the live data */
    uint8_t new_class = INVFS_META_EXT_MIN_SIZE_CLASS;
    uint64_t new_size = extent_bytes(new_class);
    while (new_size < live_bytes && new_class < cur_class - 1) {
        new_class++;
        new_size = extent_bytes(new_class);
    }

    if (new_class >= cur_class) return 1;

    uint32_t new_blocks = extent_blocks(new_class);
    uint32_t excess_blocks = cur_blocks - new_blocks;

    /* log WAL first: META_SHRINK before freeing blocks */
    if (meta_wal_shrink(v, ext_idx, pba, new_class) != 0) return -1;

    /* free excess blocks */
    if (excess_blocks > 0) {
        v->retain_release = 1;
        vol_free_blocks(v, pba + new_blocks, excess_blocks);
        v->retain_release = 0;
    }

    /* update mapper entry */
    uint64_t new_entry = invfs_meta_ext_encode(pba, new_class);
    meta_mapper_set(v, ext_idx, new_entry);

    return 0;
}

/* ---- extent packing/merge --------------------------------------------- */

/* Find the best target extent for merge (one with sufficient free tail space).
 * Returns mapper index of best target, or -1 if none found. */
static int find_merge_target(invfs_volume *v, uint64_t need_blocks,
                             uint16_t skip_idx)
{
    int best = -1;
    uint64_t best_free = 0;

    for (size_t i = 0; i < v->meta_mapper_n; i++) {
        if (i == skip_idx) continue;
        uint64_t entry = meta_mapper_get(v, i);
        if (!entry) continue;

        uint64_t pba = invfs_meta_ext_pba(entry);
        uint8_t sc = invfs_meta_ext_class(entry);
        uint64_t sz = invfs_meta_ext_size(sc);
        uint32_t blocks = (uint32_t)(sz / INVFS_BLOCK_SIZE);

        /* estimate used space in this extent */
        uint32_t live = extent_live_count(v, pba, blocks);
        uint32_t used_blocks = (live + 7) / 8;  /* rough estimate: avg 8 records per block */
        if (used_blocks >= blocks) continue;

        uint32_t free_blocks = blocks - used_blocks;
        if (free_blocks >= need_blocks && free_blocks > best_free) {
            best_free = free_blocks;
            best = (int)i;
        }
    }
    return best;
}

/* Pack one extent into another's free space.
 * src_idx -> target_idx. Source is freed after copy.
 * Returns 0 on success, <0 on error. */
int vol_meta_extent_merge(invfs_volume *v, uint16_t src_idx, uint16_t tgt_idx)
{
    uint64_t src_entry = meta_mapper_get(v, src_idx);
    uint64_t tgt_entry = meta_mapper_get(v, tgt_idx);
    if (!src_entry || !tgt_entry) return -1;

    uint64_t src_pba = invfs_meta_ext_pba(src_entry);
    uint8_t src_class = invfs_meta_ext_class(src_entry);
    uint64_t src_size = invfs_meta_ext_size(src_entry);
    uint32_t src_blocks = (uint32_t)(src_size / INVFS_BLOCK_SIZE);

    uint64_t tgt_pba = invfs_meta_ext_pba(tgt_entry);
    uint8_t tgt_class = invfs_meta_ext_class(tgt_entry);
    uint64_t tgt_size = invfs_meta_ext_size(tgt_class);
    uint32_t tgt_blocks = (uint32_t)(tgt_size / INVFS_BLOCK_SIZE);

    /* find where target's free space starts (approximate: assume 75% utilization) */
    uint32_t live_in_tgt = extent_live_count(v, tgt_pba, tgt_blocks);
    uint32_t tgt_used_blocks = (live_in_tgt * (sizeof(invfs_inode_rec) + 4) +
                                 INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    if (tgt_used_blocks >= tgt_blocks) return -1;

    uint32_t tgt_free_start = tgt_used_blocks;
    uint32_t tgt_free_blocks = tgt_blocks - tgt_free_start;

    if (src_blocks > tgt_free_blocks) return -1;

    /* copy source extent data to target's free space */
    uint64_t copy_dst = (tgt_pba + tgt_free_start) * INVFS_BLOCK_SIZE;
    uint64_t copy_src = src_pba * INVFS_BLOCK_SIZE;
    uint64_t copy_bytes = src_size;

    uint8_t *buf = (uint8_t *)malloc((size_t)copy_bytes);
    if (!buf) return -1;

    if (io_seek(&v->io, (off_t)copy_src) != 0 ||
        io_read(&v->io, buf, (size_t)copy_bytes) != 0) {
        free(buf);
        return -1;
    }

    if (io_seek(&v->io, (off_t)copy_dst) != 0 ||
        io_write(&v->io, buf, (size_t)copy_bytes) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    /* log WAL: META_MERGE before freeing source */
    if (meta_wal_merge(v, tgt_idx, src_idx, tgt_pba + tgt_free_start,
                       tgt_class) != 0) return -1;

    /* free source extent blocks */
    v->retain_release = 1;
    vol_free_blocks(v, src_pba, src_blocks);
    v->retain_release = 0;

    /* mark source mapper entry as free */
    meta_mapper_set(v, src_idx, 0);

    return 0;
}

/* ---- merge scheduling ------------------------------------------------- */

/* WP30 merge thresholds (can be overridden by env) */
#ifndef INVFS_META_MERGE_FOOTPRINT_PCT
#define INVFS_META_MERGE_FOOTPRINT_PCT 70
#endif
#ifndef INVFS_META_MERGE_EXTENT_COUNT_MAX
#define INVFS_META_MERGE_EXTENT_COUNT_MAX 128
#endif
#ifndef INVFS_META_MERGE_DEAD_FRACTION_PCT
#define INVFS_META_MERGE_DEAD_FRACTION_PCT 30
#endif

int vol_meta_merge_needed(invfs_volume *v)
{
    if (!v->met0_present || !v->meta_mapper) return 0;

    uint64_t used_blocks = 0, total_blocks = 0;
    uint32_t extent_count = 0;
    uint64_t dead_records = 0, total_records = 0;

    for (size_t i = 0; i < v->meta_mapper_n; i++) {
        uint64_t entry = meta_mapper_get(v, i);
        if (!entry) continue;
        extent_count++;
        uint64_t pba = invfs_meta_ext_pba(entry);
        uint8_t sc = invfs_meta_ext_class(entry);
        uint64_t sz = invfs_meta_ext_size(sc);
        uint32_t blocks = (uint32_t)(sz / INVFS_BLOCK_SIZE);
        total_blocks += blocks;

        uint32_t live = extent_live_count(v, pba, blocks);
        used_blocks += blocks;
        uint32_t dead = extent_dead_fraction(v, pba, blocks);
        dead_records += (uint64_t)dead * blocks;
        total_records += blocks;
    }

    /* check thresholds */
    if (total_blocks > 0) {
        uint64_t footprint_pct = (used_blocks * 100) / total_blocks;
        if (footprint_pct > INVFS_META_MERGE_FOOTPRINT_PCT) return 1;
    }

    if (extent_count > INVFS_META_MERGE_EXTENT_COUNT_MAX) return 1;

    if (total_records > 0) {
        uint64_t dead_pct = (dead_records * 100) / total_records;
        if (dead_pct > INVFS_META_MERGE_DEAD_FRACTION_PCT) return 1;
    }

    return 0;
}

/* Run one incremental merge step (one pair of extents).
 * Returns 0 if merge was performed, 1 if nothing to merge, <0 on error. */
int vol_meta_merge_step(invfs_volume *v)
{
    if (!v->met0_present || !v->meta_mapper) return 1;

    /* find the most sparse extent (highest dead fraction) */
    int sparse_idx = -1;
    uint32_t max_dead = 0;
    uint64_t sparse_pba = 0, sparse_blocks = 0;

    for (size_t i = 0; i < v->meta_mapper_n; i++) {
        uint64_t entry = meta_mapper_get(v, i);
        if (!entry) continue;

        uint64_t pba = invfs_meta_ext_pba(entry);
        uint8_t sc = invfs_meta_ext_class(entry);
        uint64_t sz = invfs_meta_ext_size(sc);
        uint32_t blocks = (uint32_t)(sz / INVFS_BLOCK_SIZE);

        uint32_t dead = extent_dead_fraction(v, pba, blocks);
        if (dead > max_dead) {
            max_dead = dead;
            sparse_idx = (int)i;
            sparse_pba = pba;
            sparse_blocks = blocks;
        }
    }

    if (sparse_idx < 0 || max_dead < 100) return 1;  /* < 10% dead */

    /* try to shrink this sparse extent first */
    if (vol_meta_extent_shrink(v, (uint16_t)sparse_idx) == 0) {
        return 0;  /* shrink happened */
    }

    /* find a target extent with free space for packing */
    int tgt_idx = find_merge_target(v, sparse_blocks, (uint16_t)sparse_idx);
    if (tgt_idx < 0) return 1;  /* no suitable target */

    return vol_meta_extent_merge(v, (uint16_t)sparse_idx, (uint16_t)tgt_idx);
}

/* Run the merge phase: incremental merge until thresholds satisfied or
 * nothing left to merge. Called after sweep walk and before second checkpoint. */
int vol_meta_merge_run(invfs_volume *v)
{
    if (!vol_meta_merge_needed(v)) return 0;

    int rc;
    uint32_t iterations = 0;
    const uint32_t max_iterations = 16;  /* cap per sweep run */

    while (iterations < max_iterations && vol_meta_merge_needed(v)) {
        rc = vol_meta_merge_step(v);
        if (rc < 0) return rc;
        if (rc > 0) break;  /* nothing more to merge */
        iterations++;
    }

    /* persist mapper changes */
    return meta_mapper_flush(v);
}

/* ---- WP30 Phase 3: dynamic metadata extent append path -------------- */

uint32_t meta_met0_crc(const invfs_met0 *m)
{
    return invfs_crc32c(m, offsetof(invfs_met0, crc32c));
}

/* Persist the MET0 descriptor to block 0 at INVFS_MET0_OFF.
 * Returns 0 on success, -1 on error. */
int meta_met0_persist(invfs_volume *v)
{
    if (!(v->sb.vol_flags & VOLF_META_DYN) || !v->met0_present)
        return 0;

    invfs_met0 m = v->met0;
    m.crc32c = 0;
    m.crc32c = meta_met0_crc(&m);

    if (io_seek(&v->io, INVFS_MET0_OFF) != 0 ||
        io_write(&v->io, &m, sizeof(m)) != 0)
        return -1;
    return 0;
}

/* Get the current append position for a record of rec_size bytes.
 * When VOLF_META_DYN is set, uses the met0-based dynamic extent system.
 * Returns 0 on success, -1 on error, -2 if ENOSPC.
 * Sets *pba_out to absolute byte PBA, *offset_out to byte offset within extent. */
int meta_get_append_pos(invfs_volume *v, uint64_t rec_size,
                        uint64_t *pba_out, uint64_t *offset_out)
{
    *pba_out = 0;
    *offset_out = 0;

    /* Legacy path: use flat inode_area_pos */
    if (!(v->sb.vol_flags & VOLF_META_DYN) || !v->met0_present) {
        if (v->inode_area_pos + rec_size > v->inode_area_end)
            return -2;
        *pba_out = v->inode_area_pos;
        *offset_out = 0;
        return 0;
    }

    /* Dynamic extent path */
    uint64_t extent_idx = v->met0.active_extent;
    uint64_t offset = v->met0.active_offset;

    /* Check if current extent has room */
    uint64_t entry = meta_mapper_get(v, (size_t)extent_idx);
    if (!entry) {
        fprintf(stderr, "meta_get_append_pos: active extent %llu has no mapper entry\n",
                (unsigned long long)extent_idx);
        return -1;
    }

    uint64_t extent_size = invfs_meta_ext_size(entry);
    uint64_t pba = invfs_meta_ext_pba(entry);

    if (offset + rec_size > extent_size) {
        /* Need a new extent */
        uint8_t size_class = INVFS_META_EXT_MIN_SIZE_CLASS;
        uint64_t new_idx = alloc_meta_extent(v, size_class);
        if (new_idx == 0) {
            /* Try to extend the current extent first */
            uint8_t cur_class = invfs_meta_ext_class(entry);
            if (cur_class < INVFS_META_EXT_SIZE_CLASS_MAX) {
                uint8_t try_class = cur_class + 1;
                while (try_class <= INVFS_META_EXT_SIZE_CLASS_MAX) {
                    if (extend_meta_extent(v, extent_idx, try_class) == 1) {
                        entry = meta_mapper_get(v, (size_t)extent_idx);
                        extent_size = invfs_meta_ext_size(entry);
                        pba = invfs_meta_ext_pba(entry);
                        break;
                    }
                    try_class++;
                }
            }
            if (offset + rec_size > extent_size) {
                return -2;  /* ENOSPC */
            }
        } else {
            /* New extent allocated */
            extent_idx = new_idx - 1;  /* alloc_meta_extent returns 1-based */
            v->met0.active_extent = extent_idx;
            v->met0.active_offset = 0;
            v->met0.extent_count++;

            entry = meta_mapper_get(v, (size_t)extent_idx);
            if (!entry) return -1;
            pba = invfs_meta_ext_pba(entry);
            extent_size = invfs_meta_ext_size(entry);
            offset = 0;

            /* Persist MET0 after switching to a new extent */
            if (meta_met0_persist(v) != 0)
                return -1;
        }
    }

    *pba_out = pba * INVFS_BLOCK_SIZE;
    *offset_out = offset;
    return 0;
}
