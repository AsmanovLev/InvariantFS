/*
 * volume.c — InvariantFS volume access layer
 *
 *   vol_open / vol_close
 *   vol_alloc  (bitmap free-list cursor)
 *   vol_write_raw (allocate blocks in RAW zone + write)
 *   vol_map    (L2P journal MAP entry + in-memory table)
 *   vol_read_block (read raw bytes at pba)
 *   inode area: append-only records (name + AST recipe)
 *
 * Metadata zone layout:
 *   [0 .. bitmap_blocks):            block bitmap
 *   [bitmap_blocks .. +journal):     L2P journal (append-only)
 *   [bitmap_blocks+journal .. end):  inode/AST area (append-only)
 */

#include "volume_internal.h"


static const uint64_t JOURNAL_BLOCKS = INVFS_JOURNAL_BLOCKS;


/* ---- name index ---------------------------------------------------- */

uint64_t idx_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a 64 */
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}


static void idx_clear(invfs_volume *v)
{
    size_t i;
    if (v->nbuck) {
        for (i = 0; i <= v->nmask; i++) {
            name_index_entry *e = v->nbuck[i];
            while (e) { name_index_entry *nx = e->next; free(e); e = nx; }
        }
        free(v->nbuck);
    }
    if (v->dbuck) {
        for (i = 0; i <= v->dmask; i++) {
            dir_index_entry *e = v->dbuck[i];
            while (e) { dir_index_entry *nx = e->next; free(e); e = nx; }
        }
        free(v->dbuck);
    }
    if (v->ibuck) {
        for (i = 0; i <= v->imask; i++) {
            id_index_entry *e = v->ibuck[i];
            while (e) { id_index_entry *nx = e->next; free(e); e = nx; }
        }
        free(v->ibuck);
    }
    v->nbuck = NULL; v->dbuck = NULL; v->ibuck = NULL;
    v->nmask = v->dmask = v->imask = 0;
    v->ncount = v->dcount = v->icount = 0;
}


/* Grow to keep the load factor near 1. Failure is not fatal: the table
   simply stays smaller and lookups get longer chains. */
static void idx_grow_names(invfs_volume *v)
{
    size_t ncap = (v->nmask + 1) * 2, i;
    name_index_entry **nb = (name_index_entry **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= v->nmask; i++) {
        name_index_entry *e = v->nbuck[i];
        while (e) {
            name_index_entry *nx = e->next;
            size_t b = (size_t)(idx_hash(e->name, e->nlen) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(v->nbuck);
    v->nbuck = nb;
    v->nmask = ncap - 1;
}


static void idx_grow_dirs(invfs_volume *v)
{
    size_t ncap = (v->dmask + 1) * 2, i;
    dir_index_entry **nb = (dir_index_entry **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= v->dmask; i++) {
        dir_index_entry *e = v->dbuck[i];
        while (e) {
            dir_index_entry *nx = e->next;
            size_t b = (size_t)(idx_hash(e->name, e->nlen) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(v->dbuck);
    v->dbuck = nb;
    v->dmask = ncap - 1;
}


static int idx_init(invfs_volume *v)
{
    idx_clear(v);
    v->nbuck = (name_index_entry **)calloc(1024, sizeof *v->nbuck);
    v->dbuck = (dir_index_entry **)calloc(256, sizeof *v->dbuck);
    v->ibuck = (id_index_entry **)calloc(1024, sizeof *v->ibuck);
    if (!v->nbuck || !v->dbuck || !v->ibuck) { idx_clear(v); return -1; }
    v->nmask = 1023;
    v->dmask = 255;
    v->imask = 1023;
    return 0;
}


/* 64-bit mix (splitmix64 finalizer): inode ids are sequential, so the low
   bits alone would pile every id into one bucket after a grow */
static uint64_t idx_mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}


static void idx_grow_ids(invfs_volume *v)
{
    size_t ncap = (v->imask + 1) * 2, i;
    id_index_entry **nb = (id_index_entry **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= v->imask; i++) {
        id_index_entry *e = v->ibuck[i];
        while (e) {
            id_index_entry *nx = e->next;
            size_t b = (size_t)(idx_mix(e->id) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(v->ibuck);
    v->ibuck = nb;
    v->imask = ncap - 1;
}


/* Remember where inode `id` lives. The last record for an id wins, matching
   the old scan, which kept walking and overwrote rec_pos on every hit. */
void idx_put_id(invfs_volume *v, uint64_t id, uint64_t pos)
{
    size_t b;
    id_index_entry *e;
    if (!v->ibuck) return;
    b = (size_t)(idx_mix(id) & v->imask);
    for (e = v->ibuck[b]; e; e = e->next)
        if (e->id == id) { e->pos = pos; return; }
    e = (id_index_entry *)malloc(sizeof *e);
    if (!e) return;
    e->id = id;
    e->pos = pos;
    e->next = v->ibuck[b];
    v->ibuck[b] = e;
    v->icount++;
    if (v->icount > v->imask + 1) idx_grow_ids(v);
}


/* 0 = unknown; callers fall back to a scan */
uint64_t idx_get_id(invfs_volume *v, uint64_t id)
{
    size_t b;
    const id_index_entry *e;
    if (!v->ibuck) return 0;
    b = (size_t)(idx_mix(id) & v->imask);
    for (e = v->ibuck[b]; e; e = e->next)
        if (e->id == id) return e->pos;
    return 0;
}


/* Shared-id bookkeeping (WP22c/F2): the rename fast path hardlinks the
 * copy onto the old id, so two live records can resolve through one id's
 * L2P mappings, and a torn drop can leave such a pair behind too. Tracked
 * from the name index's own updates, so the count is exactly "live names
 * pointing at this id". The retire path reads it before dropping maps. */
static void idx_ref_id(invfs_volume *v, uint64_t id, int delta)
{
    size_t b;
    id_index_entry *e;
    if (!v->ibuck) return;
    b = (size_t)(idx_mix(id) & v->imask);
    for (e = v->ibuck[b]; e; e = e->next)
        if (e->id == id) {
            if (delta > 0) e->live++;
            else if (e->live) e->live--;
            return;
        }
    if (delta <= 0) return;
    /* no entry yet (the record's idx_put_id lands right after): create a
     * hint-less one -- pos 0 reads as "unknown" and callers fall back to
     * a scan, exactly as if the entry did not exist */
    e = (id_index_entry *)calloc(1, sizeof *e);
    if (!e) return;
    e->id = id;
    e->live = 1;
    e->next = v->ibuck[b];
    v->ibuck[b] = e;
    v->icount++;
    if (v->icount > v->imask + 1) idx_grow_ids(v);
}


uint32_t idx_id_live(const invfs_volume *v, uint64_t id)
{
    size_t b;
    const id_index_entry *e;
    if (!v->ibuck) return 0;
    b = (size_t)(idx_mix(id) & v->imask);
    for (e = v->ibuck[b]; e; e = e->next)
        if (e->id == id) return e->live;
    return 0;
}


/* add `delta` to the live count of every directory prefix of `name`:
   "a/b/c.txt" bumps "a/" and "a/b/"; the anchor "a/" bumps "a/" itself,
   which is what keeps an empty directory visible */
void idx_bump_dirs(invfs_volume *v, const char *name, size_t nlen,
                          int delta)
{
    size_t i;
    if (!v->dbuck) return;
    for (i = 0; i < nlen; i++) {
        size_t plen, b;
        dir_index_entry *e, **pp;
        if (name[i] != '/') continue;
        plen = i + 1;                     /* prefix includes the '/' */
        b = (size_t)(idx_hash(name, plen) & v->dmask);
        pp = &v->dbuck[b];
        for (e = *pp; e; pp = &e->next, e = e->next)
            if (e->nlen == plen && memcmp(e->name, name, plen) == 0) break;
        if (e) {
            if (delta < 0) {
                if (e->count) e->count--;
                if (e->count == 0) { *pp = e->next; free(e); v->dcount--; }
            } else {
                e->count++;
            }
            continue;
        }
        if (delta < 0) continue;          /* nothing to decrement */
        e = (dir_index_entry *)malloc(sizeof *e + plen);
        if (!e) continue;
        memcpy(e->name, name, plen);
        e->name[plen] = 0;
        e->nlen = (uint32_t)plen;
        e->count = 1;
        e->next = v->dbuck[b];
        v->dbuck[b] = e;
        v->dcount++;
        if (v->dcount > v->dmask + 1) idx_grow_dirs(v);
    }
}


/* record `name` as live under `id`. Replacing an existing name keeps the
   directory counts untouched -- it is still one live name. */
void idx_put(invfs_volume *v, const char *name, size_t nlen,
                    uint64_t id, uint64_t pos, uint64_t size, uint64_t ctime)
{
    size_t b;
    name_index_entry *e;
    if (!v->nbuck || nlen == 0 || nlen > 255) return;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    for (e = v->nbuck[b]; e; e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) {
            /* update of an existing name: only logical size moves */
            v->hot.logical_bytes += size;
            v->hot.logical_bytes -= e->size;
            if (e->inode_id != id) {   /* replacement under a new id */
                idx_ref_id(v, e->inode_id, -1);
                idx_ref_id(v, id, +1);
            }
            e->inode_id = id;
            e->pos = pos;
            e->size = size;
            e->ctime = ctime;
            return;
        }
    /* brand-new name: population counter (dir anchors end with '/') */
    if (nlen && name[nlen - 1] == '/') v->hot.dirs++;
    else { v->hot.files++; v->hot.logical_bytes += size; }
    e = (name_index_entry *)malloc(sizeof *e + nlen);
    if (!e) return;
    memcpy(e->name, name, nlen);
    e->name[nlen] = 0;
    e->nlen = (uint32_t)nlen;
    e->inode_id = id;
    e->pos = pos;
    e->size = size;
    e->ctime = ctime;
    e->next = v->nbuck[b];
    v->nbuck[b] = e;
    v->ncount++;
    idx_ref_id(v, id, +1);
    idx_bump_dirs(v, name, nlen, +1);
    if (v->ncount > v->nmask + 1) idx_grow_names(v);
}


/* A tombstone kills only ITS version of the name: the sweep appends the
   replacement record BEFORE the tombstone for the old inode, so a blind
   delete-by-name would drop the newer file. Mirrors the old vol_find scan. */
void idx_del(invfs_volume *v, const char *name, size_t nlen,
                    uint64_t id)
{
    size_t b;
    name_index_entry *e, **pp;
    if (!v->nbuck || nlen == 0 || nlen > 255) return;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    pp = &v->nbuck[b];
    for (e = *pp; e; pp = &e->next, e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) break;
    if (!e || e->inode_id != id) return;
    *pp = e->next;
    if (nlen && name[nlen - 1] == '/') v->hot.dirs--;
    else { v->hot.files--; v->hot.logical_bytes -= e->size; }
    idx_ref_id(v, e->inode_id, -1);
    free(e);
    v->ncount--;
    idx_bump_dirs(v, name, nlen, -1);
}


/* v2 position kill: remove the entry whose record lives exactly at `pos`,
 * whatever its id. Same-id metadata rewrites chain versions under one name,
 * so a plain id match would kill the NEWEST version instead of the one the
 * tombstone names. Falls back to nothing -- callers keep the id path for
 * legacy (file_size==0) tombstones. */
void idx_del_at(invfs_volume *v, const char *name, size_t nlen,
                       uint64_t pos)
{
    size_t b;
    name_index_entry *e, **pp;
    if (!v->nbuck || nlen == 0 || nlen > 255 || pos == 0) return;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    pp = &v->nbuck[b];
    for (e = *pp; e; pp = &e->next, e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0 &&
            e->pos == pos)
            break;
    if (!e) return;
    *pp = e->next;
    if (nlen && name[nlen - 1] == '/') v->hot.dirs--;
    else { v->hot.files--; v->hot.logical_bytes -= e->size; }
    idx_ref_id(v, e->inode_id, -1);
    free(e);
    v->ncount--;
    idx_bump_dirs(v, name, nlen, -1);
}

const name_index_entry *idx_get(invfs_volume *v, const char *name,
                                       size_t nlen)
{
    size_t b;
    const name_index_entry *e;
    if (!v->nbuck || nlen == 0 || nlen > 255) return NULL;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    for (e = v->nbuck[b]; e; e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) return e;
    return NULL;
}

uint64_t idx_dir_count(invfs_volume *v, const char *pre, size_t plen)
{
    size_t b;
    const dir_index_entry *e;
    if (!v->dbuck || plen == 0) return 0;
    b = (size_t)(idx_hash(pre, plen) & v->dmask);
    for (e = v->dbuck[b]; e; e = e->next)
        if (e->nlen == plen && memcmp(e->name, pre, plen) == 0)
            return e->count;
    return 0;
}

int idx_dir_live(invfs_volume *v, const char *pre, size_t plen)
{
    return idx_dir_count(v, pre, plen) != 0;
}


/* Widen the dirty byte range to cover the byte holding bit i, so vol_flush
 * can write just that slice instead of the whole bitmap. */
static void bm_dirty(invfs_volume *v, uint64_t i)
{
    uint64_t byte = i / 8;
    if (v->bm_lo > v->bm_hi) { v->bm_lo = byte; v->bm_hi = byte + 1; return; }
    if (byte < v->bm_lo) v->bm_lo = byte;
    if (byte + 1 > v->bm_hi) v->bm_hi = byte + 1;
}


/* CRC convention: over the 24-byte descriptor with the crc32c field
 * itself read as zero (i.e. the bytes 0x100..0x117 of block 0). */
static uint32_t rdp0_crc(const invfs_rdp0 *rd)
{
    invfs_rdp0 t = *rd;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


/* Persist (rd != NULL) or clear (rd == NULL) the RDP0 descriptor by
 * read-modify-write of the whole block 0. vol_write_sb only ever writes
 * the 144-byte struct at offset 0, so the reserved tail survives state
 * flips; on a raw device the full-block write is what the alignment
 * demands anyway. The in-memory copy follows the disk state. */
int vol_write_rdp0(invfs_volume *v, const invfs_rdp0 *rd)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    if (io_seek(&v->io, 0) != 0 ||
        io_read(&v->io, blk, sizeof blk) != 0)
        return -1;
    if (rd) {
        invfs_rdp0 t = *rd;
        memcpy(t.magic, "RDP0", 4);
        t.pad = 0;
        t.crc32c = 0;
        t.crc32c = rdp0_crc(&t);
        memcpy(blk + INVFS_RDP0_OFF, &t, sizeof t);
        v->rd = t;
        v->rd_present = 1;
    } else {
        memset(blk + INVFS_RDP0_OFF, 0, sizeof(invfs_rdp0));
        memset(&v->rd, 0, sizeof v->rd);
        v->rd_present = 0;
    }
    if (io_seek(&v->io, 0) != 0 ||
        io_write(&v->io, blk, sizeof blk) != 0)
        return -1;
    return 0;
}


/* Replay the L2P journal from disk into the in-memory table and reseed the
 * WP19 hot summaries; sets v->journal_pos to the end of the valid prefix.
 * vol_open runs this once; the WP21 rollback runs it again after restoring
 * the checkpoint's staged journal bytes. The table allocation grows but
 * never shrinks. Returns 0, -1 on allocation failure. */
int l2p_replay(invfs_volume *v)
{
    uint64_t jp = v->journal_start * INVFS_BLOCK_SIZE;
    uint64_t jend = (v->journal_start + JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
    uint64_t replayed = 0;
    size_t i;

    v->l2p_count = 0;
    v->l2p_dirty = 0;
    while (jp + sizeof(invfs_l2p_entry) <= jend) {
        invfs_l2p_entry e;
        if (io_seek(&v->io, jp) != 0 ||
            io_read(&v->io, &e, sizeof(e)) != 0)
            break;
        if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc)
            break;  /* end of valid journal */
        if (e.type == INVFS_JRN_MAP) {
            if (v->l2p_count == v->l2p_cap) {
                v->l2p_cap = v->l2p_cap ? v->l2p_cap * 2 : 256;
                v->l2p = (invfs_l2p_entry *)realloc(v->l2p,
                                v->l2p_cap * sizeof(invfs_l2p_entry));
                if (!v->l2p) return -1;
            }
            v->l2p[v->l2p_count++] = e;
            replayed++;
        } else if (e.type == INVFS_JRN_UNMAP) {
            l2p_remove(v, e.inode, e.lba);
        }
        jp += sizeof(e);
    }
    v->journal_pos = jp;
    if (getenv("INVFS_DEBUG"))
        printf("[l2p_replay] replayed %llu L2P entries, journal_pos=%llu\n",
               (unsigned long long)replayed,
               (unsigned long long)v->journal_pos);
    /* WP19: seed the hot summaries from the persisted heat, so a sweep
     * right after a mount sees crossings that happened before it */
    v->heat_any_rhot = 0;
    v->heat_any_whot = 0;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type != INVFS_JRN_MAP) continue;
        if (l2p_rheat(e) >= INVFS_HEAT_HOT) v->heat_any_rhot = 1;
        if (e->pad[2] >= INVFS_WHEAT_HOT) v->heat_any_whot = 1;
    }
    return 0;
}


invfs_volume *vol_open(const char *path, int *err)
{
    char devbuf[64];
    const char *real;
    int rc;
    invfs_volume *v = (invfs_volume *)calloc(1, sizeof(invfs_volume));
    if (!v) { *err = -1; return NULL; }

    /* "W:" is the shorthand a user types; CreateFileW needs "\\.\W:". Store
       the normalized form, so diagnostics name what was actually opened. */
    real = blkio_normalize(path, devbuf, sizeof devbuf);
    v->path = strdup(real);

    /* A device is taken exclusively -- locked and dismounted. Two processes
       each holding their own in-memory bitmap and L2P would corrupt the
       volume between them, which is why an image file is opened without
       FILE_SHARE_WRITE too. It also stops Windows from mounting whatever
       filesystem it believes is there and writing that filesystem's metadata
       over ours. */
    rc = blkio_open(&v->io, real,
                    blkio_looks_like_device(real) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "vol_open: %s: %s\n", real, blkio_strerror(rc));
        *err = -2;
        free(v->path);
        free(v);
        return NULL;
    }
#ifndef _WIN32
    /* POSIX twin of the Windows no-share open above: a lingering FUSE daemon
     * finishing its drain and an offline tool (invf-cp/invf-sweep) writing
     * the same image corrupt it between their in-memory bitmaps/L2P (seen
     * in the wild: stale-position record appends clobbering fresh records).
     * LOCK_NB: fail loudly instead of waiting. Released by close(). */
    if (flock(v->io.fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "vol_open: %s: image is in use by another process\n",
                real);
        *err = -2;
        goto fail;
    }
#endif
    if (io_seek(&v->io, 0) != 0 ||
        io_read(&v->io, &v->sb, sizeof(v->sb)) != 0) { *err = -3; goto fail; }
    if (memcmp(v->sb.magic, INVFS_MAGIC, 8) != 0) { *err = -4; goto fail; }
    if (invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum)) != v->sb.checksum)
        { *err = -5; goto fail; }

    /* WP20b: second block-0 read for the RDP0 redundancy descriptor at
     * 0x100 (past the 144-byte superblock struct; pre-WP20b images carry
     * zeros there -> absent). Valid magic+crc loads the persisted
     * redundancy config; anything else leaves the layer-1 default. */
    v->seal_k1 = SEAL_STRIPE_K;
    {
        invfs_rdp0 rd;
        if (io_seek(&v->io, INVFS_RDP0_OFF) == 0 &&
            io_read(&v->io, &rd, sizeof rd) == 0 &&
            memcmp(rd.magic, "RDP0", 4) == 0) {
            if (rdp0_crc(&rd) == rd.crc32c) {
                v->rd = rd;
                v->rd_present = 1;
            } else {
                fprintf(stderr, "vol_open: RDP0 descriptor CRC mismatch; "
                                "redundancy config ignored\n");
            }
        }
    }
    if (v->rd_present) {
        int l2_ok = (v->rd.l2_algo == INVFS_RDP0_L2_RS_VM ||
                     v->rd.l2_algo == INVFS_RDP0_L2_RS_CAUCHY) &&
                    v->rd.k2 == SEAL2_K &&
                    v->rd.m2 >= SEAL2_M2_MIN && v->rd.m2 <= SEAL2_M2_MAX;
        if (v->rd.l1_algo == INVFS_RDP0_L1_XOR &&
            v->rd.k1 >= 8 && v->rd.k1 <= 128)
            v->seal_k1 = v->rd.k1;
        if (v->rd.l2_algo && !l2_ok) {
            fprintf(stderr, "vol_open: unsupported RDP0 layer-2 shape "
                    "(algo %u, k2 %u, m2 %u); layer 2 ignored\n",
                    (unsigned)v->rd.l2_algo, (unsigned)v->rd.k2,
                    (unsigned)v->rd.m2);
            v->rd.l2_algo = 0;
        }
    }

    /* WP18: an armed resize (RSZ0) takes precedence over everything below --
     * the metadata the current superblock describes may already be partly
     * overwritten, so the roll-forward runs before the bitmap/journal/inode
     * reads. A committed-but-uncleared descriptor is just swept up. */
    {
        invfs_rsz0 rz;
        if (io_seek(&v->io, INVFS_RSZ0_OFF) == 0 &&
            io_read(&v->io, &rz, sizeof rz) == 0 &&
            memcmp(rz.magic, "RSZ0", 4) == 0) {
            if (rsz0_crc(&rz) != rz.crc32c || !rsz0_sane(&rz)) {
                /* torn/desc corrupt: the apply it armed never started (the
                 * arm precedes it), so the old metadata is intact -- ignore
                 * the descriptor and let the RECOVERY path below decide */
                fprintf(stderr, "vol_open: ignoring a corrupt RSZ0 resize "
                                "descriptor\n");
            } else if (v->sb.total_blocks == rz.old_total) {
                if (vol_rsz0_apply(v, &rz) != 0) {
                    fprintf(stderr, "vol_open: resize roll-forward failed; "
                                    "volume left for invf-fsck/retry\n");
                    *err = -10; goto fail;
                }
                fprintf(stderr, "vol_open: completed an interrupted resize "
                        "(%llu -> %llu blocks)\n",
                        (unsigned long long)rz.old_total,
                        (unsigned long long)v->sb.total_blocks);
            } else if (v->sb.total_blocks == rz.new_sb.total_blocks) {
                /* committed, only the descriptor clear was lost */
                uint8_t z[sizeof(invfs_rsz0)];
                memset(z, 0, sizeof z);
                if (io_seek(&v->io, INVFS_RSZ0_OFF) == 0)
                    io_write(&v->io, z, sizeof z);
            } else {
                fprintf(stderr, "vol_open: RSZ0 resize descriptor matches "
                                "neither the current nor its target size; "
                                "ignored\n");
            }
        }
    }

    /* WP21: the CKP0 sweep-checkpoint descriptor at 0x220 (past the RSZ0
     * block above, so an applied resize has already cleared it). Valid
     * magic+crc loads the checkpoint; anything else reads as absent. A CRC
     * mismatch is a torn arm/clear -- treated as absent rather than fatal:
     * the retention registry, if one was ever written, is reclaimed by the
     * next sweep's defensive realize (owner present, CKP0 absent). */
    {
        invfs_ckp0 ck;
        if (io_seek(&v->io, INVFS_CKP0_OFF) == 0 &&
            io_read(&v->io, &ck, sizeof ck) == 0 &&
            memcmp(ck.magic, "CKP0", 4) == 0) {
            if (ckp0_crc(&ck) == ck.crc32c) {
                v->ck = ck;
                v->ck_present = 1;
                v->ck_prev_seq = ck.sweep_seq;
                if (getenv("INVFS_DEBUG"))
                    printf("[vol_open] checkpoint #%llu live (swept at %llu)\n",
                           (unsigned long long)ck.sweep_seq,
                           (unsigned long long)ck.time_unix);
            } else {
                fprintf(stderr, "vol_open: CKP0 checkpoint descriptor CRC "
                                "mismatch; checkpoint ignored\n");
            }
        }
    }

    v->bitmap_blocks = (v->sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    v->bitmap = (uint8_t *)calloc(1, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE);
    if (!v->bitmap) { *err = -6; goto fail; }
    if (io_seek(&v->io, v->sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, v->bitmap, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE) != 0)
        { *err = -7; goto fail; }

    v->journal_start = v->sb.metadata_zone_start + v->bitmap_blocks;
    v->journal_pos = v->journal_start * INVFS_BLOCK_SIZE;
    v->inode_area_start = v->journal_start + JOURNAL_BLOCKS;
    v->inode_area_pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    v->inode_area_end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
                        * INVFS_BLOCK_SIZE;

    /* scan existing inode records: find end of area + max inode id + name index */
    {
        uint64_t p = v->inode_area_pos;
        uint64_t found = 0;
        if (idx_init(v) != 0) { *err = -6; goto fail; }
        while (p + sizeof(invfs_inode_rec) <= v->inode_area_end) {
            invfs_inode_rec rec_h;
            if (io_seek(&v->io, p) != 0 ||
                io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
                break;
            if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
                break;  /* end of records */
            if (rec_h.rec_len < sizeof(invfs_inode_rec) ||
                rec_h.rec_len > INVFS_MAX_REC_LEN ||
                p + rec_h.rec_len + 4 > v->inode_area_end) {
                v->scan_anomalies++;
                break;  /* corrupted tail — stop */
            }
            /* torn-write protection: verify trailing CRC32C; a record
             * whose CRC fails is a half-written append (crash/SIGPIPE),
             * NOT a valid boundary — stop here so later tools never
             * step into garbage. */
            {
                uint8_t *rb = (uint8_t *)malloc((size_t)rec_h.rec_len + 4);
                uint32_t crc_stored, crc_calc;
                if (!rb) break;
                if (io_seek(&v->io, p) != 0 ||
                    io_read(&v->io, rb, (size_t)rec_h.rec_len + 4) != 0) {
                    free(rb); break;
                }
                memcpy(&crc_stored, rb + rec_h.rec_len, 4);
                crc_calc = invfs_crc32c(rb, rec_h.rec_len);
                free(rb);
                if (crc_calc != crc_stored) {
                    fprintf(stderr, "vol_open: corrupt inode record at %llu, "
                            "skipping (rec_len=%u)\n",
                            (unsigned long long)p,
                            (unsigned)rec_h.rec_len);
                    v->scan_anomalies++;
                    /* skip the corrupt record and keep scanning so files
                     * AFTER it stay visible; run fsck -f to clean up */
                    p += (uint64_t)rec_h.rec_len + 4;
                    continue;
                }
            }
            if (rec_h.magic == INODE_REC_MAGIC &&
                rec_h.inode_id >= v->next_inode_id)
                v->next_inode_id = rec_h.inode_id + 1;
            /* feed the name index from this same pass: the record is read and
               CRC-verified here, so the index inherits the exact same
               skip-the-corrupt-record semantics for free. A separate pass
               would stop at the first bad record and hide every name after
               it. Last record for a name wins, so replaying the area in
               order lands on the same answer the old linear scan gave. */
            {
                size_t nl = rec_h.name_len < sizeof(rec_h.name)
                          ? rec_h.name_len : sizeof(rec_h.name) - 1;
                if (rec_h.magic == INODE_REC_MAGIC) {
                    idx_put(v, rec_h.name, nl, rec_h.inode_id, p,
                            rec_h.file_size, rec_h.ctime);
                    idx_put_id(v, rec_h.inode_id, p);
                } else {
                    /* v2 tombstones carry the killed record's byte offset in
                     * the unused file_size field; 0 keeps legacy semantics */
                    if (rec_h.file_size != 0)
                        idx_del_at(v, rec_h.name, nl, rec_h.file_size);
                    else
                        idx_del(v, rec_h.name, nl, rec_h.inode_id);
                }
            }
            p += rec_h.rec_len + 4;
            found++;
        }
        v->inode_area_pos = p;
        /* everything the scan just walked is on the device already: the
         * first barrier anchor (WP22c/F1) */
        v->inode_area_durable = p;
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] scanned %llu inode recs, next_inode=%llu, area_pos=%llu, "
                   "index=%llu names/%llu dirs\n",
                   (unsigned long long)found, (unsigned long long)v->next_inode_id,
                   (unsigned long long)v->inode_area_pos,
                   (unsigned long long)v->ncount, (unsigned long long)v->dcount);
    }

    /* replay L2P journal: rebuild in-memory table, continue at end */
    if (l2p_replay(v) != 0) { *err = -9; goto fail; }

    v->alloc_cursor = v->sb.raw_zone_start;
    if (v->next_inode_id == 0)
        v->next_inode_id = 1;
    /* ENOSPC defaults for images created before the policy fields */
    if (v->sb.reserved_blocks == 0)
        v->sb.reserved_blocks = (uint32_t)(v->sb.total_blocks / 128 + 64);
    if (v->sb.hard_min_blocks == 0)
        v->sb.hard_min_blocks = (uint32_t)(v->sb.total_blocks / 1024 + 16);
    v->free_blocks = vol_count_free(v);
    alloc_state_reset(v);
    /* H5: a volume whose latch persisted in the superblock re-evaluates it
     * at open: space freed while it was offline (fsck reclaim, a resize,
     * a delete in a session that never flushed the flag clear) must not
     * keep it read-only. In RAM only -- persisted by the first flush. */
    vol_readonly_unlatch(v);

    /* Reconstructed-content cache.
     *
     * INVFS_ARC_BYTES sets the budget; 0 disables the cache outright, which is
     * how the tests measure what it is worth and how a memory-tight host opts
     * out. The default is deliberately modest -- this is a userspace FS process
     * that already buffers whole files for writes, and a cache that competes
     * with that is a worse trade than a slower read.
     *
     * K/M/G suffixes are accepted, and anything unparseable falls back to the
     * default with a complaint. Both matter because the failure was silent and
     * looked like success: "256M" parsed as 256 *bytes*, which leaves the cache
     * switched on and refusing every entry over 128 bytes, and a typo parsed as
     * 0, which switches it off. Either way reads got slow and nothing said why. */
    {
        const char *ab = getenv("INVFS_ARC_BYTES");
        size_t budget = 256u << 20;      /* 256 MB */
        if (ab) {
            char *endp = NULL;
            unsigned long long want = strtoull(ab, &endp, 10);
            unsigned long long mult = 1;
            int ok = (endp != ab);
            if (ok) {
                while (*endp == ' ' || *endp == '\t') endp++;
                switch (*endp) {
                    case 'k': case 'K': mult = 1024ull; endp++; break;
                    case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
                    case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
                    default: break;
                }
                if (*endp == 'b' || *endp == 'B') endp++;
                while (*endp == ' ' || *endp == '\t') endp++;
                if (*endp != '\0') ok = 0;
                if (want > (unsigned long long)SIZE_MAX / mult) ok = 0;
            }
            if (ok) {
                budget = (size_t)(want * mult);
            } else {
                fprintf(stderr, "[vol] INVFS_ARC_BYTES=\"%s\" is not a size; "
                                "using the default %llu bytes\n",
                        ab, (unsigned long long)budget);
            }
        }
        v->arc = arc_create(budget);     /* NULL == disabled, callers cope */
        v->arc_budget = (uint64_t)budget;
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] content cache: %s (%llu bytes)\n",
                   v->arc ? "on" : "off", (unsigned long long)budget);
    }

    /* WP16b codec profile: INVFS_PROFILE = fast|balanced|dense|archive,
     * default balanced. v1 effects: the generic sweep's ZSTD level, and the
     * effective name is published back to the environment so every pack
     * subprocess inherits it (the tools hold one volume per process, so the
     * env IS the volume's context; setenv around each pack exec would be
     * equivalent -- the sweep is single-threaded). An unknown value keeps
     * the default with a complaint (the INVFS_ARC_BYTES convention). */
    {
        const char *pf = getenv("INVFS_PROFILE");
        int p = INVFS_PROFILE_BALANCED;
        if (pf && *pf) {
            int w = invfs_profile_parse(pf);
            if (w < 0)
                fprintf(stderr, "[vol] INVFS_PROFILE=\"%s\" is not a profile; "
                                "using balanced\n", pf);
            else
                p = w;
        }
        v->profile = (uint8_t)p;
#ifdef _WIN32
        {
            /* _putenv does not copy its argument: the string must outlive
             * the volume (static storage). */
            static char pfbuf[64];
            snprintf(pfbuf, sizeof pfbuf, "INVFS_PROFILE=%s",
                     invfs_profile_name(p));
            _putenv(pfbuf);
        }
#else
        setenv("INVFS_PROFILE", invfs_profile_name(p), 1);
#endif
        if (getenv("INVFS_DEBUG")) {
            int ga = invfs_profile_generic_algo(p);
            if (ga == INVFS_ALGO_ZSTD)
                printf("[vol_open] profile: %s (generic zstd level %d)\n",
                       invfs_profile_name(p), invfs_profile_zstd_level(p));
            else
                printf("[vol_open] profile: %s (generic %s)\n",
                       invfs_profile_name(p),
                       ga == INVFS_ALGO_LZ4 ? "lz4" : "verbatim");
        }
    }

    /* WP19: INVFS_HEAT_INIT seeds the read-heat of entries created from
     * now on (import/mkfs-time friendliness: pre-warm files you already
     * know are hot). u16, default 0; the ARC_BYTES complaint convention. */
    {
        const char *hi = getenv("INVFS_HEAT_INIT");
        if (hi && *hi) {
            char *endp = NULL;
            unsigned long want = strtoul(hi, &endp, 10);
            if (endp == hi || *endp != '\0' || want > 0xFFFFul) {
                fprintf(stderr, "[vol] INVFS_HEAT_INIT=\"%s\" is not a u16; "
                                "using 0\n", hi);
            } else {
                v->heat_init = (uint16_t)want;
            }
        }
    }
    /* WP22c test hook: see sync_fail_at (volume_internal.h). Parsed once
     * here like the other env knobs; fires once per process. */
    {
        const char *sf = getenv("INVFS_SYNC_FAIL_AT");
        if (sf && *sf) {
            char *endp = NULL;
            unsigned long long n = strtoull(sf, &endp, 10);
            if (endp != sf && *endp == '\0' && n > 0)
                v->sync_fail_at = n;
        }
    }
    /* WP20b: a live descriptor means a seal config exists -- start the
     * dirty bitmap (all-ones: the first reseal of a session is a full
     * pass, what happened while unmounted is unknowable). */
    if (v->rd_present && (v->rd.l1_algo || v->rd.l2_algo))
        seal_dirty_reset(v);
    *err = 0;
    /* Unclean shutdown. Reading always works -- that is how you find out
     * what survived. For WRITES the old behavior was a hard latch: every
     * mount after an unclean stop stayed read-only until a manual
     * `invf-fsck -f`, which made any ungraceful kill (openrc shutdown
     * storms, OOM, host crash) boot-blocking for root-FS duty.
     * Auto-recovery instead: if the full-record scan just completed with
     * ZERO anomalies (every CRC verified, no torn tail), replay already
     * rebuilt the exact on-disk truth and nothing was lost -- clear DIRTY
     * and continue read-write. Any real damage keeps the conservative
     * manual-fsck path. INVFS_AUTO_RECOVER=0 opts out. */
    if (v->sb.state != INVFS_STATE_CLEAN) {
        const char *ar = getenv("INVFS_AUTO_RECOVER");
        int ro_flag = (v->sb.vol_flags & VOLF_READONLY) != 0;
        if (!ro_flag && v->scan_anomalies == 0 && (!ar || strcmp(ar, "0") != 0)) {
            v->sb.state = INVFS_STATE_CLEAN;
            if (vol_write_sb(v) == 0)
                fprintf(stderr, "vol_open: %s not closed cleanly but scan is "
                                "anomaly-free; recovered to CLEAN (rw)\n",
                        real);
            else
                v->needs_recovery = 1;
        } else {
            fprintf(stderr,
                    "vol_open: %s was not closed cleanly (state=0x%02X%s); "
                    "read-only until recovery. Run `invf-fsck -f %s`.\n",
                    real, (unsigned)v->sb.state,
                    v->scan_anomalies ? ", damaged records" : "",
                    path);
            v->needs_recovery = 1;
        }
    }
    return v;
fail:
    io_close(&v->io);
    if (v->bitmap) free(v->bitmap);
    free(v->path);
    free(v);
    return NULL;
}


void vol_close(invfs_volume *v)
{
    if (!v) return;
    /* Close is the only place that can honestly write CLEAN, and it can only
       do so after the maps are down. There was no flush here at all: every
       tool that mutated the volume had to remember to call vol_flush itself,
       and forgetting cost the whole run silently. Only a session that
       actually dirtied the volume writes anything, so invf-ls and invf-cat
       stay read-only. */
    if (v->dirty) {
        if (v->needs_recovery) {
            /* WP22c: an io error latched this session. Do NOT run the
             * usual final flush: the in-RAM journal/bitmap may reflect
             * mutations whose records never reached the device (the
             * re-anchored tail), and persisting them over the
             * last-barriered state would invent exactly the F2 mismatch
             * (a live record whose map is gone). Leave the device at the
             * last successful barrier; the next mount recovers. */
            fprintf(stderr, "vol_close: an io error was latched this "
                    "session; the final flush is skipped and the volume "
                    "stays dirty for recovery at the next mount\n");
        } else if (vol_flush(v) == 0) {
            v->sb.state = INVFS_STATE_CLEAN;
            if (vol_write_sb(v) != 0)
                fprintf(stderr, "vol_close: could not mark volume clean; "
                                "next mount will recover\n");
        } else {
            fprintf(stderr, "vol_close: final flush failed; volume stays "
                            "dirty and will be recovered at next mount\n");
        }
        /* A buffered image file needs a real barrier for power-loss safety;
           a device is already write-through. Off by default because the
           barrier costs a full device cache flush and the ordering above is
           what makes process death survivable either way. */
        if (getenv("INVFS_FSYNC"))
            blkio_flush(&v->io);
    }
    io_close(&v->io);
    idx_clear(v);
    arc_destroy(v->arc);
    cpack_map_cache_reset(v);
    free(v->heat_seen);
    free(v->seal_dirty);
    free(v->retmap);
    free(v->tz);
    free(v->bz);
    free(v->bitmap);
    free(v->l2p);
    free(v->path);
    free(v);
}


void vol_arc_stats(invfs_volume *v, invfs_arc_stats *out)
{
    arc_stats(v ? v->arc : NULL, out);
}


/* ---- WP10: memory policy (sweep-time admission only) ---- */

#define INVFS_DEC_MEM_DEFAULT (512ull << 20)   /* 512 MiB (WP10 §6) */


/* Recreate the content cache with a new budget (the arc has no resize).
 * 0 keeps the current one, so INVFS_ARC_BYTES at vol_open stays the
 * fallback when no explicit budget was set. */
void vol_set_arc_budget(invfs_volume *v, uint64_t bytes)
{
    if (!v || bytes == 0) return;
    arc_destroy(v->arc);
    v->arc = arc_create((size_t)bytes);   /* NULL == disabled, callers cope */
    v->arc_budget = bytes;
}


void vol_set_dec_mem_limit(invfs_volume *v, uint64_t bytes)
{
    if (v) v->dec_mem_limit = bytes;      /* 0 = default */
}


uint64_t vol_get_dec_mem_limit(invfs_volume *v)
{
    return (v && v->dec_mem_limit) ? v->dec_mem_limit : INVFS_DEC_MEM_DEFAULT;
}


unsigned vol_get_profile(const invfs_volume *v)
{
    return v ? v->profile : INVFS_PROFILE_BALANCED;
}

/* Persist the superblock. The checksum covers bytes 0..0x7B, and `state`
   lives at 0x18 -- inside that range -- so it has to be recomputed here.
   It was not, which was harmless only for as long as nothing inside the
   checksummed range ever changed: the ENOSPC policy fields and the READONLY
   flag sit at 0x80 and beyond deliberately. The moment `state` starts moving
   (which is the whole point of crash detection) a stale checksum turns the
   volume unopenable -- vol_open rejects it with err -5. */
int vol_write_sb(invfs_volume *v)
{
    v->sb.checksum = invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum));
    if (io_seek(&v->io, 0) != 0 ||
        io_write(&v->io, &v->sb, sizeof(v->sb)) != 0)
        return -1;
    return 0;
}


int vol_flush(invfs_volume *v)
{
    /* persist superblock (state / ENOSPC policy fields / READONLY flag) */
    if (vol_write_sb(v) != 0) {
        vol_io_error_latch(v, "superblock write");
        return -1;
    }
    /* Persist only the part of the bitmap that changed. Dokan flushes on
     * every file close and the device is unbuffered write-through, so the
     * old unconditional full-bitmap write cost a synchronous 480 KB per
     * close on this volume -- about 250 ms on flash, independent of how
     * many bytes the file actually had. A file touches a handful of
     * bitmap bytes; write those. */
    {
        uint64_t bm_bytes = (uint64_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
        uint64_t base = v->sb.metadata_zone_start * INVFS_BLOCK_SIZE;
        if (v->bm_lo <= v->bm_hi) {
            uint64_t lo = v->bm_lo, hi = v->bm_hi;
            if (hi > bm_bytes) hi = bm_bytes;
            /* round out to INVFS_BLOCK_SIZE so the unbuffered path writes
               whole blocks and never read-modify-writes a partial one */
            lo -= lo % INVFS_BLOCK_SIZE;
            hi = ((hi + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE) * INVFS_BLOCK_SIZE;
            if (hi > bm_bytes) hi = bm_bytes;
            if (hi > lo) {
                if (io_seek(&v->io, base + lo) != 0 ||
                    io_write(&v->io, v->bitmap + lo, (size_t)(hi - lo)) != 0) {
                    vol_io_error_latch(v, "bitmap write");
                    return -1;
                }
            }
            v->bm_lo = 1; v->bm_hi = 0;   /* clean */
        }
    }
    /* Compact L2P journal from the in-memory table (source of truth after
     * replay + ops). Keeps the journal bounded across many sweep/cp cycles
     * and drops stale MAP entries of tombstoned inodes (vol_delete_inode
     * removes them in-memory without journaling).
     *
     * Only the suffix from l2p_dirty is rewritten. The old code re-seeked
     * and re-wrote every entry on every flush, and Dokan flushes on each
     * file close, so writing n files cost n^2/2 seek+write pairs -- 20 ms
     * per file at 1600 files, all of it in this loop. Appending files now
     * writes only the new entries. */
    {
        uint64_t jstart = v->journal_start * INVFS_BLOCK_SIZE;
        uint64_t jend = (v->journal_start + JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
        size_t esz = sizeof(invfs_l2p_entry);
        size_t from = v->l2p_dirty < v->l2p_count ? v->l2p_dirty : v->l2p_count;
        size_t n = v->l2p_count - from;
        uint64_t jp = jstart + (uint64_t)from * esz;

        /* +1 for the terminator that stops replay */
        if (jp + (uint64_t)(n + 1) * esz > jend)
            return -1;   /* journal full */

        if (n) {
            invfs_l2p_entry *buf = (invfs_l2p_entry *)malloc(n * esz);
            size_t i;
            if (!buf) return -1;
            for (i = 0; i < n; i++) {
                buf[i] = v->l2p[from + i];
                buf[i].crc = invfs_crc32c(&buf[i], offsetof(invfs_l2p_entry, crc));
            }
            if (io_seek(&v->io, jp) != 0 ||
                io_write(&v->io, buf, n * esz) != 0) {
                free(buf);
                vol_io_error_latch(v, "journal write");
                return -1;
            }
            free(buf);
            jp += (uint64_t)n * esz;
        }
        /* Terminator: replay stops at the first entry whose CRC does not
           check out. Without it a journal that SHRANK (after a delete) left
           the previous, still-valid tail behind, and the next mount replayed
           mappings for blocks that had already been freed. */
        {
            invfs_l2p_entry z;
            memset(&z, 0, sizeof z);
            z.crc = ~invfs_crc32c(&z, offsetof(invfs_l2p_entry, crc));
            if (io_seek(&v->io, jp) != 0 ||
                io_write(&v->io, &z, esz) != 0) {
                vol_io_error_latch(v, "journal terminator write");
                return -1;
            }
        }
        v->journal_pos = jp;
        v->l2p_dirty = v->l2p_count;
    }
    return 0;
}


/* fsync/fdatasync entry point (WP4ab): everything vol_flush persists
 * (superblock, dirty bitmap range, journal suffix + terminator) becomes
 * durable against power loss, not just process death. vol_write_commit
 * has already pushed the data blocks themselves with io_write, so one
 * barrier at the end covers the whole pending state.
 *
 * WP22c/F1: the barrier is also the only place a buffered backing store
 * reports a write error (a pwrite lands in the page cache and returns
 * success; the dm-flakey error window kills the dirty pages in writeback
 * and surfaces EIO here). A failed barrier therefore means the append
 * cursors may already sit past bytes that will never reach the device:
 * latch the volume and re-anchor the tail (vol_io_error_latch). Only a
 * successful barrier moves inode_area_durable. */
int vol_sync(invfs_volume *v)
{
    if (!v) return -1;
    if (vol_flush(v) != 0) return -1;   /* flush latches its own failures */
#ifndef _WIN32
    /* WP22c test hook (tools/test-flushfail.sh): the Nth vol_sync of the
     * process simulates the error window -- the un-barriered inode-area
     * tail dies in "writeback" (zeroed on the image) and the barrier
     * reports EIO. */
    if (v->sync_fail_at && --v->sync_fail_at == 0) {
        if (v->inode_area_pos > v->inode_area_durable) {
            static const uint8_t z[INVFS_BLOCK_SIZE];
            uint64_t p = v->inode_area_durable;
            while (p < v->inode_area_pos) {
                size_t n = (size_t)(v->inode_area_pos - p);
                if (n > sizeof z) n = sizeof z;
                if (io_seek(&v->io, p) != 0 || io_write(&v->io, z, n) != 0)
                    break;   /* the device is erroring anyway: latch below */
                p += n;
            }
        }
        vol_io_error_latch(v, "sync (INVFS_SYNC_FAIL_AT)");
        return -1;
    }
#endif
    if (blkio_flush(&v->io) != 0) {
        vol_io_error_latch(v, "sync");
        return -1;
    }
    v->inode_area_durable = v->inode_area_pos;
    return 0;
}


/* allocate n consecutive free blocks in a zone; returns start block or 0 */
uint64_t alloc_blocks(invfs_volume *v, uint64_t zone_start, uint64_t zone_len,
                             uint64_t n, int use_reserve)
{
    uint64_t zone_end = zone_start + zone_len;
    uint64_t i, count = 0, start = 0;
    /* Cursor and free count belong to the zone being scanned, not to the
     * volume: with one shared cursor every spill into SHADOW rewound the
     * next RAW attempt to the zone head, so a full RAW zone was rescanned
     * end to end for every 64 KB segment. */
    uint64_t *cursor, *zone_free, *fail_run;

    if (zone_start == v->sb.shadow_zone_start) {
        cursor = &v->shadow_cursor; zone_free = &v->shadow_free;
        fail_run = &v->shadow_fail_run;
    } else {
        cursor = &v->raw_cursor;    zone_free = &v->raw_free;
        fail_run = &v->raw_fail_run;
    }

    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;

    /* Skip the scan when this zone cannot satisfy n. Either it has too few
     * free blocks outright, or a previous scan proved no run this long
     * exists. This is what turns a hopeless RAW retry from 786k bitmap
     * probes into one compare -- per 64 KB segment, so ~100M probes per
     * 8 MB file before this check existed. */
    if (*zone_free < n)
        return 0;
    if (*fail_run && n >= *fail_run)
        return 0;

    /* ENOSPC policy: ordinary writes must leave the reserve + hard-min
     * untouched; sweep/transcodes (use_reserve) may drain the reserve but
     * never the hard-min floor. Hitting the floor flips the volume to
     * READONLY so applications get a clean ENOSPC/EROFS instead of data
     * loss (fsck can then reclaim orphaned blocks). H5: VOLF_RO_SPACE
     * marks it as the SPACE latch (not an operator hold) so
     * vol_readonly_unlatch may release it when space comes back. */
    {
        uint64_t guard = (use_reserve ? 0 : v->sb.reserved_blocks)
                         + v->sb.hard_min_blocks;
        if (v->free_blocks <= guard) {
            if (v->free_blocks <= v->sb.hard_min_blocks &&
                !(v->sb.vol_flags & VOLF_READONLY)) {
                v->sb.vol_flags |= VOLF_READONLY | VOLF_RO_SPACE;
                fprintf(stderr, "[alloc] READONLY: free=%llu hard_min=%u\n",
                        (unsigned long long)v->free_blocks,
                        (unsigned)v->sb.hard_min_blocks);
            }
            return 0;  /* ENOSPC */
        }
    }

    i = *cursor;
    if (i < zone_start || i >= zone_end)
        i = zone_start;

    for (count = 0; count < zone_len; count++) {
        if (i >= zone_end) { i = zone_start; start = 0; }
        if (!bit_get(v->bitmap, i)) {
            if (start == 0) start = i;
            if (i - start + 1 == n) {
                uint64_t k;
                for (k = start; k <= i; k++) bit_set(v->bitmap, k);
                bm_dirty(v, start);
                bm_dirty(v, i);
                /* WP20b: fresh shadow content invalidates its stripes'
                 * parity (the block's old content was zero-as-absent) */
                if (zone_start == v->sb.shadow_zone_start)
                    seal_dirty_mark(v, start, n);
                *cursor = (i + 1 < zone_end) ? i + 1 : zone_start;
                v->free_blocks -= n;
                *zone_free -= n;
                return start;
            }
        } else {
            start = 0;
        }
        i++;
    }
    /* Scanned the whole zone without a run of n: the space is there but too
     * fragmented. Record the smallest run size known to fail so later, larger
     * requests skip the scan. Any free() in this zone clears the hint. */
    if (*fail_run == 0 || n < *fail_run)
        *fail_run = n;
    return 0;  /* ENOSPC */
}

uint64_t alloc_raw_or_shadow(invfs_volume *v, uint64_t nblocks, int *zone_out)
{
    uint64_t pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks,
                                nblocks, 0);
    if (pba == 0) {
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           nblocks, 0);
        if (pba) *zone_out = INVFS_ZONE_BINARY;
    } else {
        *zone_out = INVFS_ZONE_RAW;
    }
    return pba;
}


/* write raw data to RAW zone, returns first pba (0 on error) */
uint64_t vol_write_raw(invfs_volume *v, const uint8_t *data, size_t len)
{
    uint64_t nblocks = (len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    uint64_t pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks, nblocks, 0);
    if (pba == 0)   /* RAW exhausted -> spill into SHADOW (still uncompressed) */
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           nblocks, 0);
    if (pba == 0) return 0;
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, data, (size_t)nblocks * INVFS_BLOCK_SIZE) != 0)
        return 0;
    return pba;
}


/* L2P MAP entry: inode logical block -> physical block */
int vol_map(invfs_volume *v, uint64_t inode, uint64_t lba, uint64_t pba, uint32_t length)
{
    invfs_l2p_entry e;
    memset(&e, 0, sizeof(e));
    e.type = INVFS_JRN_MAP;
    e.inode = inode;
    e.lba = lba;
    e.pba = pba;
    e.length = length;
    e.crc = invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc));

    /* In-memory only. vol_flush is the single writer of the journal: it
     * rewrites the dirty suffix of this table and re-stamps the terminator.
     *
     * This used to seek and write the 64-byte record here, on every segment.
     * On an image that is a buffered 64-byte write and invisible; on a raw
     * device opened NO_BUFFERING|WRITE_THROUGH it is a read-modify-write of
     * a 4 KB block plus a synchronous flush to flash -- ~80 ms. Files are
     * split into 64 KB segments, so an 8 MB file paid ~128 of them (~11 s)
     * and an 18 MB file ~20 s, all inside vol_create_file under the Dokan
     * write lock. That is what pushed a callback past opt.Timeout and made
     * the mount die mid-copy with no error, and why a fully compressible
     * 8 MB file cost the same as an incompressible one: the price was per
     * segment, not per byte reaching the device.
     *
     * Dropping the write also fixes a correctness hole. The record went in
     * ON TOP of the terminator without writing a new one, so a crash before
     * the next flush left replay free to run past the end of the live
     * entries into whatever stale, still-CRC-valid records the journal held
     * from an earlier, longer life -- exactly what the terminator exists to
     * prevent. journal_pos is recomputed by vol_flush; nothing reads it in
     * between except stat.c, which reports on a freshly opened volume. */
    if ((uint64_t)(v->l2p_count + 2) * sizeof(e) >
        (uint64_t)JOURNAL_BLOCKS * INVFS_BLOCK_SIZE) {
        fprintf(stderr, "L2P journal full\n");
        return -1;
    }

    /* in-memory */
    if (v->l2p_count == v->l2p_cap) {
        v->l2p_cap = v->l2p_cap ? v->l2p_cap * 2 : 256;
        v->l2p = (invfs_l2p_entry *)realloc(v->l2p, v->l2p_cap * sizeof(invfs_l2p_entry));
        if (!v->l2p) return -1;
    }
    v->l2p[v->l2p_count++] = e;
    return 0;
}


/* in-memory L2P lookup: inode + lba -> pba and phys length (or 0) */
uint64_t vol_lookup(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    uint64_t pba = 0, len = 0;
    vol_lookup_entry(v, inode, lba, &pba, &len);
    return pba;
}


int vol_lookup_entry(invfs_volume *v, uint64_t inode, uint64_t lba,
                     uint64_t *pba_out, uint64_t *len_out)
{
    /* newest wins: scan from the end (L2P is append-only journal) */
    size_t i;
    for (i = v->l2p_count; i-- > 0; ) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba) {
            *pba_out = e->pba;
            *len_out = e->length;
            return 0;
        }
    }
    return -1;
}


/* remove all mappings for (inode, lba) from the in-memory table */
void l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    size_t i, w = 0;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba) {
            /* everything from here shifts down, so the on-disk suffix
               starting at w is now stale */
            if (w < v->l2p_dirty) v->l2p_dirty = w;
            continue;  /* drop */
        }
        if (w != i) v->l2p[w] = v->l2p[i];
        w++;
    }
    v->l2p_count = w;
    if (v->l2p_dirty > v->l2p_count) v->l2p_dirty = v->l2p_count;
}


/* public wrapper (used by dedupe pass) */
void vol_l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    l2p_remove(v, inode, lba);
}


/* read one block worth of raw bytes at pba */
int vol_read_block(invfs_volume *v, uint64_t pba, void *buf)
{
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, INVFS_BLOCK_SIZE) != 0)
        return -1;
    return 0;
}


/* Write one segment as whole blocks.
 *
 * phys_blocks is ceil(payload / block size) and alloc_blocks hands over that
 * many blocks exclusively, so the slack at the end of the last block belongs
 * to this segment and to nothing else. Writing only the payload left that
 * block partially covered, which forced the block layer to read it back first
 * to preserve bytes that were never anyone's data. On flash that read costs
 * about what the write costs: a full-tree copy issued 103418 reads against
 * 103466 writes, very nearly one wasted read per segment. Padding to the block
 * boundary makes the transfer aligned at both ends, so it needs no read at
 * all, and zeroing the slack beats leaving whatever the block held before.
 *
 * buf must have room for phys_blocks * INVFS_BLOCK_SIZE bytes.
 */
int write_segment_blocks(invfs_volume *v, uint64_t pba, uint8_t *buf,
                                size_t payload, uint64_t phys_blocks)
{
    size_t span = (size_t)phys_blocks * INVFS_BLOCK_SIZE;
    if (span > payload)
        memset(buf + payload, 0, span - payload);
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, buf, span) != 0)
        return -1;
    /* WP20b: overwriting occupied shadow blocks dirties their stripes */
    seal_dirty_mark(v, pba, phys_blocks);
    return 0;
}


const invfs_superblock *vol_sb(invfs_volume *v)
{
    return &v->sb;
}


const uint8_t *vol_bitmap(invfs_volume *v, uint64_t *blocks_out)
{
    if (blocks_out) *blocks_out = v->sb.total_blocks;
    return v->bitmap;
}


const invfs_l2p_entry *vol_l2p(invfs_volume *v, size_t *count_out)
{
    if (count_out) *count_out = v->l2p_count;
    return v->l2p;
}


uint64_t vol_inode_area_pos(invfs_volume *v) { return v->inode_area_pos; }

uint64_t vol_inode_area_start(invfs_volume *v) { return v->inode_area_start * INVFS_BLOCK_SIZE; }

uint64_t vol_inode_area_end(invfs_volume *v) { return v->inode_area_end; }

uint64_t vol_journal_pos(invfs_volume *v)   { return v->journal_pos; }


/* Bytes left for new inode records. The area is append-only, so this is what
   stands between the volume and "create silently returns 0": callers that can
   still report an error to the application should check it before accepting
   data, not after. A record is sizeof(invfs_inode_rec) + the AST recipe, so
   this is an upper bound on what will fit, not a file count. */
uint64_t vol_inode_area_free(invfs_volume *v)
{
    return v->inode_area_pos < v->inode_area_end
         ? v->inode_area_end - v->inode_area_pos : 0;
}


uint64_t vol_inode_next(invfs_volume *v, uint64_t pos, uint32_t *magic_out,
                        uint64_t *inode_out, uint64_t *size_out,
                        char *name_out, size_t name_cap, uint32_t *rec_len_out)
{
    invfs_inode_rec rec_h;
    /* scan only the CRC-validated extent (vol_open truncated at the first
     * torn/corrupt record); never step into garbage past area_pos */
    uint64_t end = v->inode_area_pos;
    while (pos + sizeof(rec_h) <= end) {
        if (vol_read_raw(v, pos, &rec_h, sizeof(rec_h)) != 0)
            break;
        if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
            break;  /* end of valid records */
        if (rec_h.rec_len < sizeof(rec_h) || rec_h.rec_len > INVFS_MAX_REC_LEN)
            break;
        if (magic_out)  *magic_out = rec_h.magic;
        if (inode_out)  *inode_out = rec_h.inode_id;
        if (size_out)   *size_out = rec_h.file_size;
        if (rec_len_out) *rec_len_out = rec_h.rec_len;
        if (name_out && name_cap) {
            /* name_len comes off disk, so clamp against the field it indexes
               as well as the caller's buffer: name is the last member of the
               record header, and a caller with a buffer bigger than the field
               would otherwise let a corrupt length read past it. */
            size_t n = rec_h.name_len;
            if (n > INVFS_MAX_NAME) n = INVFS_MAX_NAME;
            if (n > name_cap - 1) n = name_cap - 1;
            memcpy(name_out, rec_h.name, n);  /* name lives inside rec */
            name_out[n] = 0;
        }
        return pos + rec_h.rec_len + 4;  /* +4: trailing CRC32C */
    }
    return 0;
}


/* raw byte-range read at absolute volume offset (for tools) */
int vol_read_raw(invfs_volume *v, uint64_t offset, void *buf, size_t len)
{
    if (io_seek(&v->io, offset) != 0 || io_read(&v->io, buf, len) != 0)
        return -1;
    return 0;
}


/* free n physical blocks (clear bitmap bits), bounded by volume size */
/* ENOSPC policy helpers */
int vol_write_enabled(invfs_volume *v)
{
    /* A volume awaiting recovery is read-only for the same reason a
       READONLY-flagged one is: the callers that check this are the ones that
       would otherwise append records, and appending onto maps that were never
       finished is how a single crash becomes two. */
    if (v->needs_recovery) return 0;
    return !(v->sb.vol_flags & VOLF_READONLY);
}


/* flip the READONLY flag; persists on next vol_flush (caller flushes).
 * An operator hold (ro=1) is VOLF_READONLY ALONE -- never auto-released
 * (vol_readonly_unlatch only releases the space latch, VOLF_RO_SPACE).
 * A manual release (ro=0) clears both bits: it overrides either hold. */
void vol_set_readonly(invfs_volume *v, int ro)
{
    if (ro)
        v->sb.vol_flags |= VOLF_READONLY;
    else
        v->sb.vol_flags &= ~(VOLF_READONLY | VOLF_RO_SPACE);
}

/* H5: the hard_min READONLY latch has a return path. alloc_blocks sets
 * VOLF_READONLY|VOLF_RO_SPACE when the free count hits the floor; before
 * WP22a nothing ever cleared the flag (vol_set_readonly(v, 0) had zero
 * callers), so a volume that once touched the floor stayed read-only
 * forever -- including across remounts (the flag persists in the
 * superblock) and even after deletes freed half the volume. A SPACE latch
 * now auto-releases with hysteresis: when free space climbs back above
 * hard_min + 2% of the volume, both bits drop (logged; the next
 * vol_flush's superblock write persists it). The band keeps a workload
 * hovering at the trigger from flapping the flag. An operator hold
 * (VOLF_READONLY alone) is never auto-released. Runs from vol_free_blocks
 * (every real free), from the fsck bitmap rebuild, and at vol_open (a
 * volume freed while offline opens RW). */
void vol_readonly_unlatch(invfs_volume *v)
{
    uint64_t watermark = (uint64_t)v->sb.hard_min_blocks +
                         v->sb.total_blocks / 50;
    if ((v->sb.vol_flags & (VOLF_READONLY | VOLF_RO_SPACE)) ==
            (VOLF_READONLY | VOLF_RO_SPACE) &&
        v->free_blocks > watermark) {
        v->sb.vol_flags &= ~(VOLF_READONLY | VOLF_RO_SPACE);
        fprintf(stderr, "[alloc] RW again: free=%llu above hard_min+2%% "
                "(hard_min=%u); READONLY latch released\n",
                (unsigned long long)v->free_blocks,
                (unsigned)v->sb.hard_min_blocks);
    }
}

uint64_t vol_free_blocks_cached(invfs_volume *v)
{
    return v->free_blocks;
}


/* blocks that ordinary writes must leave untouched (reserve + floor) */
uint64_t vol_write_guard(invfs_volume *v)
{
    return (uint64_t)v->sb.reserved_blocks + v->sb.hard_min_blocks;
}


void vol_free_blocks(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    uint64_t i;
    uint64_t end = pba + nblocks;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    /* WP21: while a checkpoint-armed sweep runs, NOTHING is freed. The
     * blocks stay allocated in the bitmap (that is what bars their reuse
     * for the rest of the run -- the rollback fidelity guarantee) and are
     * recorded in retmap, the realize-time registry list. Content does not
     * change, so seal stripes stay valid and the free counters stay honest
     * (the blocks are NOT free). Idempotent per block, so the PB7
     * shared-pba cases mark twice without consequence. */
    if (v->retain) {
        if (v->retmap)
            for (i = pba; i < end; i++)
                bit_set(v->retmap, i);
        return;
    }
    for (i = pba; i < end; i++)
        bit_clr(v->bitmap, i);
    if (end > pba) { bm_dirty(v, pba); bm_dirty(v, end - 1); }
    /* WP20b: a freed shadow block changes its stripes' membership */
    seal_dirty_mark(v, pba, end - pba);
    v->free_blocks += nblocks;

    /* Give the freed space back to the zone it came from, and drop the
     * "no run this long exists" hint: a fresh hole may satisfy a request
     * that the last full scan rejected. Rewind the cursor so the scan
     * actually reaches the hole instead of walking past it. */
    if (pba >= v->sb.shadow_zone_start) {
        v->shadow_free += nblocks;
        v->shadow_fail_run = 0;
        if (pba < v->shadow_cursor) v->shadow_cursor = pba;
    } else if (pba >= v->sb.raw_zone_start) {
        v->raw_free += nblocks;
        v->raw_fail_run = 0;
        if (pba < v->raw_cursor) v->raw_cursor = pba;
    }
    /* H5: freeing is the way out of the space latch -- re-evaluate */
    vol_readonly_unlatch(v);
}



uint64_t vol_count_free(invfs_volume *v)
{
    uint64_t i, free = 0;
    for (i = 0; i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i))
            free++;
    return free;
}


/* Free blocks inside one zone. Called once per mount (and after fsck
 * rewrites the bitmap) to seed the per-zone counters that alloc_blocks
 * then maintains incrementally. */
static uint64_t zone_count_free(invfs_volume *v, uint64_t start, uint64_t len)
{
    uint64_t i, free = 0;
    for (i = start; i < start + len && i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i))
            free++;
    return free;
}

void alloc_state_reset(invfs_volume *v)
{
    v->raw_cursor    = v->sb.raw_zone_start;
    v->shadow_cursor = v->sb.shadow_zone_start;
    v->raw_free    = zone_count_free(v, v->sb.raw_zone_start,
                                    v->sb.raw_zone_blocks);
    v->shadow_free = zone_count_free(v, v->sb.shadow_zone_start,
                                    v->sb.shadow_zone_blocks);
    v->raw_fail_run = v->shadow_fail_run = 0;
    v->bm_lo = 1; v->bm_hi = 0;   /* on-disk bitmap matches memory */
}
