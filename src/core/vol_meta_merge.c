/* vol_meta_merge.c — WP30: dynamic metadata extent management.
 * Mapper table, MET0 persistence, append-position resolution, and owner
 * extent allocation. Consolidation (extent shrink/merge) was retired in
 * WP-M21 (v3 mapper is pre-allocated at mkfs; per-extent reclaim is a
 * read-friendly fold that lives in vol_fold.c / vol_reclaim.c). */

#include "volume_internal.h"

/* ---- mapper table accessors ------------------------------------------- */

/* Read the mapper table from disk into v->meta_mapper if not already cached.
 * v0.3.0+: mapper is pre-allocated at mkfs at sb.meta_mapper_pba.
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
    v->meta_mapper_n = (size_t)v->met0.extent_count;  /* entries 0..extent_count-1 valid */
    return 0;
}

/* Persist the mapper table to disk. Returns 0 on success, -1 on error. */
int meta_mapper_flush(invfs_volume *v)
{
    if (!v->meta_mapper || v->sb.meta_mapper_pba == 0) return 0;
    uint64_t bytes = INVFS_META_EXT_BLOCKS * INVFS_BLOCK_SIZE;
#ifdef INVFS_DEBUG_META_EXTENTS
    fprintf(stderr, "[flush.mapper] write at pba=%llu bytes=%llu\n",
            (unsigned long long)v->sb.meta_mapper_pba, (unsigned long long)bytes);
#endif
    if (io_seek(&v->io, v->sb.meta_mapper_pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, v->meta_mapper, (size_t)bytes) != 0)
        return -1;
    return 0;
}

/* Get mapper entry at index i — read-locked */
uint64_t meta_mapper_get(const invfs_volume *v, size_t i)
{
    if (!v->meta_mapper || i >= v->meta_mapper_n) return 0;
    pthread_rwlock_rdlock(&((invfs_volume *)v)->meta_lock);
    uint64_t val = v->meta_mapper[i];
    pthread_rwlock_unlock(&((invfs_volume *)v)->meta_lock);
    return val;
}

/* Set mapper entry at index i — caller must hold meta_lock write-locked */
void meta_mapper_set(invfs_volume *v, size_t i, uint64_t entry)
{
    if (!v->meta_mapper || i >= v->meta_mapper_n) return;
    v->meta_mapper[i] = entry;
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
    invfs_met0 m = v->met0;
    m.crc32c = 0;
    m.crc32c = meta_met0_crc(&m);
#ifdef INVFS_DEBUG_META_EXTENTS
    fprintf(stderr, "[flush.met0] write ext_count=%llu active_extent=%llu active_offset=%llu\n",
            (unsigned long long)m.extent_count, (unsigned long long)m.active_extent,
            (unsigned long long)m.active_offset);
#endif
    if (io_seek(&v->io, INVFS_MET0_OFF) != 0 ||
        io_write(&v->io, &m, sizeof(m)) != 0)
        return -1;
    return 0;
}

/* Get the current append position for a record of rec_size bytes.
 * v0.3.0+: uses the met0-based dynamic extent system (mandatory).
 * Returns 0 on success, -1 on error, -2 if ENOSPC.
 * Sets *pba_out to absolute byte PBA, *offset_out to byte offset within extent. */
int meta_get_append_pos(invfs_volume *v, uint64_t rec_size,
                        uint64_t *pba_out, uint64_t *offset_out)
{
    *pba_out = 0;
    *offset_out = 0;

    /* Hold exclusive lock for the full write-path duration */
    pthread_rwlock_wrlock(&v->meta_lock);

    /* Direct mapper access (no meta_mapper_get — caller holds write lock).
     * meta_mapper_get uses a read-lock which would deadlock here. */
    uint64_t extent_idx = v->met0.active_extent;
    uint64_t offset = v->met0.active_offset;

#ifdef INVFS_DEBUG_META_EXTENTS
    fprintf(stderr, "[mga] in: extent_idx=%llu off=%llu mapper_n=%zu ec=%llu\n",
            (unsigned long long)extent_idx, (unsigned long long)offset,
            v->meta_mapper_n, (unsigned long long)v->met0.extent_count);
#endif

    uint64_t entry = (v->meta_mapper && extent_idx < v->meta_mapper_n)
                     ? v->meta_mapper[extent_idx] : 0;

    /* Lazy allocation of extent 0 on first write */
    if (!entry && extent_idx == 0 && v->met0.extent_count == 0) {
        uint64_t new_idx = alloc_meta_extent(v, INVFS_META_EXT_MIN_SIZE_CLASS);
        if (new_idx == 0) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
        v->met0.extent_count = 1;
        /* mapper_n must cover the new entry -- meta_mapper_load sets it
         * from extent_count ONCE at open; lazy alloc must grow it. */
        if (v->meta_mapper_n < (size_t)v->met0.extent_count)
            v->meta_mapper_n = (size_t)v->met0.extent_count;
        entry = (v->meta_mapper && extent_idx < v->meta_mapper_n)
                ? v->meta_mapper[extent_idx] : 0;
        if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
        meta_mapper_flush(v);
        meta_met0_persist(v);
    } else if (!entry) {
        if (extent_idx >= v->met0.extent_count && v->met0.extent_count > 0) {
            extent_idx = v->met0.extent_count - 1;
            v->met0.active_extent = extent_idx;
            offset = v->met0.active_offset;
            entry = (v->meta_mapper && extent_idx < v->meta_mapper_n)
                    ? v->meta_mapper[extent_idx] : 0;
            if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
        } else {
            pthread_rwlock_unlock(&v->meta_lock); return -1;
        }
    }

    uint64_t extent_size = invfs_meta_ext_size(entry);
    uint64_t pba = invfs_meta_ext_pba(entry);
#ifdef INVFS_DEBUG_META_EXTENTS
    fprintf(stderr, "[mga] post: pba=%llu esz=%llu off=%llu rec_size=%llu\n",
            (unsigned long long)pba, (unsigned long long)extent_size,
            (unsigned long long)offset, (unsigned long long)rec_size);
#endif

    if (offset + rec_size > extent_size) {
        /* Need a new extent. Size it so it can hold rec_size. */
        uint8_t size_class = INVFS_META_EXT_MIN_SIZE_CLASS;
        while (size_class < INVFS_META_EXT_SIZE_CLASS_MAX &&
               (65536ULL << size_class) < rec_size)
            size_class++;

        uint64_t new_idx = alloc_meta_extent(v, size_class);
        if (new_idx == 0) {
            /* Try to extend the current extent first */
            uint8_t cur_class = invfs_meta_ext_class(entry);
            if (cur_class < INVFS_META_EXT_SIZE_CLASS_MAX) {
                uint8_t try_class = cur_class + 1;
                while (try_class <= INVFS_META_EXT_SIZE_CLASS_MAX) {
                    if (extend_meta_extent(v, extent_idx, try_class) == 1) {
                        entry = (v->meta_mapper && extent_idx < v->meta_mapper_n)
                                ? v->meta_mapper[extent_idx] : 0;
                        extent_size = invfs_meta_ext_size(entry);
                        pba = invfs_meta_ext_pba(entry);
                        meta_mapper_flush(v);
                        break;
                    }
                    try_class++;
                }
            }
            if (offset + rec_size > extent_size) {
#ifdef INVFS_DEBUG_META_EXTENTS
                fprintf(stderr, "[mga] alloc failed AND no ext worked, ENOSPC\n");
#endif
                pthread_rwlock_unlock(&v->meta_lock); return -2;  /* ENOSPC */
            }
        } else {
#ifdef INVFS_DEBUG_META_EXTENTS
            fprintf(stderr, "[mga] alloc_meta_extent returned 0, trying extend\n");
#endif
            /* New extent allocated */
            extent_idx = new_idx - 1;  /* alloc_meta_extent returns 1-based */
            v->met0.active_extent = extent_idx;
            v->met0.active_offset = 0;
            v->met0.extent_count++;
            /* grow mapper_n so meta_mapper_get sees the new entry */
            if (v->meta_mapper_n < (size_t)v->met0.extent_count)
                v->meta_mapper_n = (size_t)v->met0.extent_count;

            entry = (v->meta_mapper && extent_idx < v->meta_mapper_n)
                    ? v->meta_mapper[extent_idx] : 0;
            if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
            pba = invfs_meta_ext_pba(entry);
            extent_size = invfs_meta_ext_size(entry);
            offset = 0;

            /* if the new extent is still too small for this record (rare:
             * allocator might have given us a smaller class because the
             * requested one ran out of contiguous space), try to extend. */
            while (extent_size < rec_size &&
                   invfs_meta_ext_class(entry) < INVFS_META_EXT_SIZE_CLASS_MAX) {
                uint8_t cur = invfs_meta_ext_class(entry);
                if (extend_meta_extent(v, extent_idx, cur + 1) != 1) break;
                entry = (v->meta_mapper && extent_idx < v->meta_mapper_n)
                        ? v->meta_mapper[extent_idx] : 0;
                extent_size = invfs_meta_ext_size(entry);
            }
            if (extent_size < rec_size) {
                pthread_rwlock_unlock(&v->meta_lock); return -2;  /* ENOSPC */
            }

            /* Persist mapper + MET0 so the new extent survives reopen. */
            meta_mapper_flush(v);
            if (meta_met0_persist(v) != 0) {
                pthread_rwlock_unlock(&v->meta_lock); return -1;
            }
        }
    }

    *pba_out = pba * INVFS_BLOCK_SIZE;
    *offset_out = offset;
    pthread_rwlock_unlock(&v->meta_lock);
    return 0;
}

/* WP52: dedicated append for the large, monotonically-growing owner records
 * (\x01rawm / \x01tier0 / \x01parity* / the text-zone owner).
 *
 * The shared append cursor (met0.active_extent/active_offset) is the FILE
 * record stream. Routing an owner rewrite through it mixed two very
 * different lifetimes: on every flush the owner record was re-appended,
 * and because it immediately filled whatever extent the cursor pointed at,
 * the next record (and the next flush) kept allocating fresh extents. On
 * the 30k-file bigvol import that consumed the whole shadow zone (the WP47
 * regression). The owner record belongs in its own extent: allocate one
 * sized for the WHOLE record, write it there, and leave the file-record
 * cursor exactly where it was, so ordinary record appends keep packing and
 * flushes stay on the journal watermark instead of firing per record.
 *
 * The allocated extent is registered in the mapper (so vol_records_walk and
 * the open scan see the record) and MET0 is persisted. *pba_out is the
 * absolute byte offset of the extent, *offset_out is 0. Returns 0 on
 * success, -1 on error, -2 on ENOSPC. Flush-safe: alloc_meta_extent and the
 * persistence helpers are ordinary io_writes; nothing here recurses into
 * vol_flush. */
int meta_get_owner_append_pos(invfs_volume *v, uint64_t rec_size,
                              uint64_t *ext_slot, uint64_t *pba_out,
                              uint64_t *offset_out)
{
    *pba_out = 0;
    *offset_out = 0;
    if (!(v->met0_present && v->meta_mapper))
        return meta_get_append_pos(v, rec_size, pba_out, offset_out);

    pthread_rwlock_wrlock(&v->meta_lock);

    /* Reuse the owner's dedicated extent while the whole record still
     * fits. When the owner has grown past it, EXTEND it in place by size
     * class (consuming only the adjacent free delta) rather than moving
     * to a fresh extent: the old extent is never abandoned, and -- just as
     * important -- no mapper slot is freed, because a zeroed slot in the
     * middle of the table truncates the open scan / vol_inode_next walks
     * (they stop at the first !entry). One extending extent per owner. */
    uint64_t extent_idx = *ext_slot;   /* 1-based; 0 = none */
    uint64_t entry = 0;
    int have = 0;
    if (extent_idx && extent_idx <= v->meta_mapper_n) {
        entry = v->meta_mapper[extent_idx - 1];
        have = entry != 0;
    }
    if (!have) {
        extent_idx = 0;
        entry = 0;
    }

    if (!have) {
        uint8_t size_class = INVFS_META_EXT_MIN_SIZE_CLASS;
        while (size_class < INVFS_META_EXT_SIZE_CLASS_MAX &&
               (65536ULL << size_class) < rec_size)
            size_class++;
        uint64_t new_idx = alloc_meta_extent(v, size_class);
        if (new_idx == 0) {
            pthread_rwlock_unlock(&v->meta_lock);
            return -2;   /* ENOSPC: no shadow run / mapper slot */
        }
        extent_idx = new_idx;   /* alloc_meta_extent is 1-based */
        v->meta_mapper_n = v->meta_mapper_n < (size_t)extent_idx
                         ? (size_t)extent_idx : v->meta_mapper_n;
        if (v->met0.extent_count < extent_idx)
            v->met0.extent_count = extent_idx;
        entry = v->meta_mapper[extent_idx - 1];
        if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
    }

    uint64_t extent_size = invfs_meta_ext_size(entry);
    /* the allocator can hand back a smaller class than requested when the
     * requested one ran out of contiguous space: grow in place until the
     * whole record fits, else fail rather than write past the extent */
    while (extent_size < rec_size &&
           invfs_meta_ext_class(entry) < INVFS_META_EXT_SIZE_CLASS_MAX) {
        uint8_t cur = invfs_meta_ext_class(entry);
        if (extend_meta_extent(v, extent_idx - 1, cur + 1) != 1) break;
        entry = (extent_idx <= v->meta_mapper_n)
              ? v->meta_mapper[extent_idx - 1] : 0;
        if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
        extent_size = invfs_meta_ext_size(entry);
    }
    if (extent_size < rec_size) {
        /* Could not make room in the owner's own extent (no adjacent free
         * run): allocate a fresh, dedicated extent sized for the whole
         * record. The old extent is left in place (its slot stays valid,
         * so scans are not truncated); it is small next to the record and
         * the abandoned case is rare. */
        uint8_t sc = INVFS_META_EXT_MIN_SIZE_CLASS;
        while (sc < INVFS_META_EXT_SIZE_CLASS_MAX &&
               (65536ULL << sc) < rec_size)
            sc++;
        uint64_t ni = alloc_meta_extent(v, sc);
        if (ni == 0) {
            pthread_rwlock_unlock(&v->meta_lock);
            return -2;   /* ENOSPC */
        }
        extent_idx = ni;
        v->meta_mapper_n = v->meta_mapper_n < (size_t)extent_idx
                         ? (size_t)extent_idx : v->meta_mapper_n;
        if (v->met0.extent_count < extent_idx)
            v->met0.extent_count = extent_idx;
        entry = v->meta_mapper[extent_idx - 1];
        if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
        while (invfs_meta_ext_size(entry) < rec_size &&
               invfs_meta_ext_class(entry) < INVFS_META_EXT_SIZE_CLASS_MAX) {
            uint8_t cur = invfs_meta_ext_class(entry);
            if (extend_meta_extent(v, extent_idx - 1, cur + 1) != 1) break;
            entry = (extent_idx <= v->meta_mapper_n)
                  ? v->meta_mapper[extent_idx - 1] : 0;
            if (!entry) { pthread_rwlock_unlock(&v->meta_lock); return -1; }
        }
        if (invfs_meta_ext_size(entry) < rec_size) {
            pthread_rwlock_unlock(&v->meta_lock);
            return -2;   /* ENOSPC */
        }
    }

    meta_mapper_flush(v);
    if (meta_met0_persist(v) != 0) {
        pthread_rwlock_unlock(&v->meta_lock);
        return -1;
    }

    *ext_slot = extent_idx;
    *pba_out = invfs_meta_ext_pba(entry) * INVFS_BLOCK_SIZE;
    *offset_out = 0;
    pthread_rwlock_unlock(&v->meta_lock);
    return 0;
}
