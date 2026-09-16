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


/* ---- WP22d: crash-atomic journal (append-only log + slot compaction) --
 * The on-disk rules live with INVFS_JRN_MAGIC in invarifs.h. In short:
 * the journal area is two slots; the active one holds a CRC-chained log
 * (a compacted image followed by appended ops); a flush APPENDS the
 * pending ops and never rewrites durable entries; when the log cannot
 * hold them (or fsck rebuilt the table, or a legacy volume migrates) the
 * whole table is imaged into the inactive slot, barriered, and the
 * superblock selector flipped (barriered again). Replay picks the
 * highest-sequence CRC-valid slot; with none valid and pad2 == 0 the area
 * is a legacy flat log and replays by the pre-WP22d rule. */

/* chain seed of a slot: crc over the header fields preceding image_crc
 * (everything the image bytes cannot influence -- the image's own chained
 * entry crcs ride inside image_crc) */
static uint32_t jrn_seed(const invfs_jrn_hdr *h)
{
    return invfs_crc32c(h, offsetof(invfs_jrn_hdr, image_crc));
}


/* chained entry crc: crc32c over entry[0..offsetof(crc)) continuing from
 * prev (the previous entry's stored crc; the slot seed for the first) */
static uint32_t jrn_chain(uint32_t prev, const invfs_l2p_entry *e)
{
    return invfs_crc32c_update(prev, e, offsetof(invfs_l2p_entry, crc));
}


/* absolute byte offset of slot `slot`'s header block */
uint64_t jrn_slot_base(const invfs_volume *v, uint32_t slot)
{
    return (v->journal_start + (uint64_t)slot * INVFS_JRN_SLOT_BLOCKS)
           * INVFS_BLOCK_SIZE;
}


/* read + validate a slot header; 0 = valid (out filled) */
static int jrn_read_hdr(invfs_volume *v, uint32_t slot, invfs_jrn_hdr *out)
{
    invfs_jrn_hdr h;
    uint64_t off = jrn_slot_base(v, slot);
    if (io_seek(&v->io, off) != 0 || io_read(&v->io, &h, sizeof h) != 0)
        return -1;
    if (memcmp(h.magic, INVFS_JRN_MAGIC, 4) != 0 ||
        h.version != INVFS_JRN_VERSION)
        return -1;
    if (h.image_bytes % sizeof(invfs_l2p_entry) != 0 ||
        h.image_bytes >
            (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1) * INVFS_BLOCK_SIZE)
        return -1;
    if (invfs_crc32c(&h, offsetof(invfs_jrn_hdr, crc32c)) != h.crc32c)
        return -1;
    *out = h;
    return 0;
}


/* apply one journaled entry to the in-memory table (replay path; no op
 * journaling). Returns -1 on allocation failure. */
int l2p_apply(invfs_volume *v, const invfs_l2p_entry *e)
{
    if (e->type == INVFS_JRN_MAP) {
        if (v->l2p_count == v->l2p_cap) {
            v->l2p_cap = v->l2p_cap ? v->l2p_cap * 2 : 256;
            v->l2p = (invfs_l2p_entry *)realloc(v->l2p,
                            v->l2p_cap * sizeof(invfs_l2p_entry));
            if (!v->l2p) return -1;
        }
        v->l2p[v->l2p_count++] = *e;
        /* WP-L2Q: replay builds the session index incrementally */
        l2p_idx_put(v, e->inode, e->lba, (uint64_t)(v->l2p_count - 1));
    } else if (e->type == INVFS_JRN_UNMAP) {
        l2p_remove_mem(v, e->inode, e->lba);
    }
    return 0;
}


/* Replay one slot whose header validated: image (wholesale CRC) then the
 * chained log until the first chain break. -1 = io/alloc failure, +1 =
 * image torn (slot unusable, caller tries the other slot). */
static int l2p_replay_slot(invfs_volume *v, uint32_t slot,
                           const invfs_jrn_hdr *h)
{
    uint64_t base = jrn_slot_base(v, slot);
    uint64_t jend = base + (uint64_t)INVFS_JRN_SLOT_BLOCKS * INVFS_BLOCK_SIZE;
    uint64_t jp = base + INVFS_BLOCK_SIZE;
    uint32_t prev = jrn_seed(h);
    uint64_t replayed = 0;

    if (h->image_bytes) {
        uint8_t *img = (uint8_t *)malloc((size_t)h->image_bytes);
        size_t n = (size_t)h->image_bytes / sizeof(invfs_l2p_entry);
        size_t i;
        if (!img) return -1;
        if (io_seek(&v->io, jp) != 0 ||
            io_read(&v->io, img, (size_t)h->image_bytes) != 0 ||
            invfs_crc32c(img, (size_t)h->image_bytes) != h->image_crc) {
            free(img);
            return 1;   /* torn compaction: the other slot is the truth */
        }
        for (i = 0; i < n; i++) {
            const invfs_l2p_entry *e = (const invfs_l2p_entry *)
                (img + i * sizeof(invfs_l2p_entry));
            if (l2p_apply(v, e) != 0) { free(img); return -1; }
            prev = e->crc;
            replayed++;
        }
        free(img);
        jp += h->image_bytes;
    }
    while (jp + sizeof(invfs_l2p_entry) <= jend) {
        invfs_l2p_entry e;
        if (io_seek(&v->io, jp) != 0 ||
            io_read(&v->io, &e, sizeof(e)) != 0)
            break;
        if (jrn_chain(prev, &e) != e.crc)
            break;  /* end of the log (chain stops at holes/stale tails) */
        if (l2p_apply(v, &e) != 0) return -1;
        prev = e.crc;
        jp += sizeof(e);
        replayed++;
    }
    v->journal_pos = jp;
    v->j_last_crc = prev;
    v->j_slot = slot;
    v->j_seq = h->seq;
    v->j_slotted = 1;
    if (getenv("INVFS_DEBUG"))
        printf("[l2p_replay] slot %u seq %llu: replayed %llu L2P entries, "
               "journal_pos=%llu\n", slot, (unsigned long long)h->seq,
               (unsigned long long)replayed,
               (unsigned long long)v->journal_pos);
    return 0;
}


/* The pre-WP22d flat log: bare-CRC entries from the journal base, replay
 * stops at the first bad one (the terminator convention). */
static int l2p_replay_legacy(invfs_volume *v)
{
    uint64_t jp = v->journal_start * INVFS_BLOCK_SIZE;
    uint64_t jend = (v->journal_start + JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
    uint64_t replayed = 0;

    while (jp + sizeof(invfs_l2p_entry) <= jend) {
        invfs_l2p_entry e;
        if (io_seek(&v->io, jp) != 0 ||
            io_read(&v->io, &e, sizeof(e)) != 0)
            break;
        if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc)
            break;  /* end of valid journal */
        if (l2p_apply(v, &e) != 0) return -1;
        jp += sizeof(e);
        replayed++;
    }
    v->journal_pos = jp;
    if (getenv("INVFS_DEBUG"))
        printf("[l2p_replay] legacy: replayed %llu L2P entries, "
               "journal_pos=%llu\n", (unsigned long long)replayed,
               (unsigned long long)v->journal_pos);
    return 0;
}


/* WP27: the journal no longer carries heat (it moved into the records'
 * INO2 ext), so replay has no summaries to reseed. */


/* Replay the L2P journal from disk into the in-memory table and reseed the
 * WP19 hot summaries; sets v->journal_pos to the end of the valid prefix.
 * vol_open runs this once; the WP21 rollback runs it again after restoring
 * the checkpoint's staged journal bytes. The table allocation grows but
 * never shrinks. Returns 0, -1 on allocation failure. */
int l2p_replay(invfs_volume *v)
{
    invfs_jrn_hdr h[2];
    int ok[2];
    int pick = -1;

    v->l2p_count = 0;
    v->jops_n = 0;
    l2p_idx_reset(v);   /* WP-L2Q: the replay below re-seeds the index */
    ok[0] = jrn_read_hdr(v, 0, &h[0]) == 0;
    ok[1] = jrn_read_hdr(v, 1, &h[1]) == 0;
    if (ok[0] && ok[1]) {
        /* both valid: the higher sequence is the newer compaction; a tie
         * (a rollback restored the same bytes to both) follows pad2 */
        pick = h[0].seq != h[1].seq ? (h[0].seq > h[1].seq ? 0 : 1)
             : (v->sb.pad2 == INVFS_JSEL_SLOT1 ? 1 : 0);
    } else if (ok[0]) {
        pick = 0;
    } else if (ok[1]) {
        pick = 1;
    }
    if (pick >= 0) {
        /* the winner's image itself may be torn (dropped mid-compaction):
         * fall back to the other slot, which then holds the full pre-
         * compaction state */
        int other = pick ^ 1;
        int rc = l2p_replay_slot(v, (uint32_t)pick, &h[pick]);
        if (rc > 0 && ok[other]) {
            fprintf(stderr, "l2p_replay: slot %d image torn; falling back "
                    "to slot %d\n", pick, other);
            v->l2p_count = 0;
            l2p_idx_reset(v);
            rc = l2p_replay_slot(v, (uint32_t)other, &h[other]);
        }
        if (rc > 0) {
            /* No slot fully validates. A torn LEGACY migration (the flip
             * never landed, or it landed while the image was still torn)
             * leaves the flat log's start intact in slot 0's region --
             * migration images into slot 1 for exactly this reason. The
             * legacy walk is read-only and self-terminating, so try it
             * whenever slots have failed: it recovers the pre-flush state
             * of the migration window, and any other case (a genuinely
             * slotted volume whose slots both died) just walks zero
             * entries past the slot-0 header and lands in the loud
             * both-torn branch below. */
        v->l2p_count = 0;
        l2p_idx_reset(v);
        v->j_slotted = 0;
        if (l2p_replay_legacy(v) != 0) return -1;
        if (v->sb.pad2 == INVFS_JSEL_LEGACY || v->l2p_count > 0) {
            fprintf(stderr, "l2p_replay: no intact slot; recovered "
                    "%llu entr%s from the pre-migration flat log\n",
                    (unsigned long long)v->l2p_count,
                    v->l2p_count == 1 ? "y" : "ies");
            return 0;
        }
        /* both slots torn: no trustworthy mapping anywhere. Loud,
         * empty, and the next flush recompacts a clean slot. WP27: the
         * owner WAL is redundant with the owner records' AST pbas, so
         * this loses the WAL (seal stripe maps, batch GC maps) only;
         * fsck -f rebuilds it from the owner records. */
        fprintf(stderr, "l2p_replay: BOTH journal slots torn; "
                "starting with an empty WAL (run invf-fsck -f)\n");
        v->l2p_count = 0;
        v->j_slotted = 1;
        v->j_slot = 0;
        v->j_seq = h[0].seq > h[1].seq ? h[0].seq : h[1].seq;
        v->journal_pos = jrn_slot_base(v, 0) + INVFS_BLOCK_SIZE;
        v->j_last_crc = 0;
        v->j_compact = 1;
        v->scan_anomalies++;
        return 0;
    }
    if (rc != 0) return -1;
    } else {
        v->j_slotted = 0;
        if (l2p_replay_legacy(v) != 0) return -1;
    }
    return 0;
}


/* ==================== WP25: two-device mux ====================
 * The engine addresses the volume in the GLOBAL block space (the
 * concatenation of dev0 + dev1; the DEVT descriptor at block 0 carries
 * the per-device sizes). The mux is the only place that knows the split:
 *
 *   - [0, meta_end_bytes): the metadata span (superblock+descriptors,
 *     bitmap, journal, inode area). Mirrored on BOTH devices at the SAME
 *     local offsets. Writes are writethrough to both before the commit
 *     barrier; a dev0 write failure logs + drops dev0 for the rest of the
 *     session (its DEVT sync_seq then necessarily lags -> the next open
 *     resyncs it from dev1, newest state wins); a dev1 failure latches
 *     the volume (WP22c semantics: dev1 is the canonical store).
 *   - [meta_end, dev0_blocks*4K): dev0 data (the RAW zone + the tier
 *     arena). Read/write dev0; a failure is the caller's business (the
 *     read path fails over to the dev1 mirror, see seg_read_checked).
 *   - [dev0_blocks*4K, ...): dev1 data (the canonical shadow zone), local
 *     offset = global - dev0 end.
 *
 * Degraded mount (dev0 absent): metadata reads come from the dev1 mirror,
 * dev1 data reads work, anything needing dev0 fails (-1) so the engine's
 * failover paths (RAW mirror, tier copy skip) engage; writes are refused
 * upstream (read-only degraded mount).
 *
 * With ndev < 2 every call is a straight passthrough to dev0's blkio --
 * the single-device path is byte-identical to the pre-WP25 one. */

static uint64_t mux_dev0_bytes(const invfs_volume *v)
{
    return v->dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE;
}


int vmux_seek(invfs_volume *v, uint64_t off)
{
    v->mux_pos = off;
    return 0;
}


/* one non-straddling slice; vmux_read/vmux_write split at the routing
 * boundaries (metadata span end, dev0/dev1 boundary) */
static int vmux_pread1(invfs_volume *v, uint64_t off, void *buf, size_t len)
{
    if (off < v->meta_end_bytes) {
        /* metadata: dev0 primary (unless absent or session-stale), fail
         * over to the dev1 mirror on any io failure. The mirror sits at
         * the SAME local offset on dev1. A session-stale mirror
         * (dev_skip[1]: its DEVT did not check out at open) is NOT a
         * failover source -- serving known-old metadata could resurrect
         * superseded records; fail loudly instead. */
        if (v->io_open[0] && !v->dev_skip[0] &&
            blkio_pread(&v->io, off, buf, len) == 0)
            return 0;
        if (v->io_open[0] && !v->dev_skip[0] && !v->metaread_logged) {
            v->metaread_logged = 1;
            fprintf(stderr, "vol: metadata read failing over to the dev1 "
                    "mirror (dev0 io error)\n");
        }
        if (v->io_open[1] && !v->dev_skip[1])
            return blkio_pread(&v->io2, off, buf, len);
        return -1;
    }
    if (off < mux_dev0_bytes(v)) {
        /* dev0 data (RAW zone / tier arena) */
        if (!v->io_open[0] || v->dev_skip[0])
            return -1;   /* degraded or dead: the engine fails over */
        return blkio_pread(&v->io, off, buf, len);
    }
    /* dev1 data (canonical shadow) */
    if (!v->io_open[1] || v->dev_skip[1])
        return -1;
    return blkio_pread(&v->io2, off - mux_dev0_bytes(v), buf, len);
}


int vmux_read(invfs_volume *v, void *buf, size_t len)
{
    uint64_t off = v->mux_pos;
    uint8_t *out = (uint8_t *)buf;

    if (v->ndev < 2 && !v->degraded) {
        int rc = blkio_pread(&v->io, off, buf, len);
        if (rc == 0) v->mux_pos = off + len;
        return rc;
    }
    while (len) {
        size_t n = len;
        if (off < v->meta_end_bytes && off + n > v->meta_end_bytes)
            n = (size_t)(v->meta_end_bytes - off);
        if (off < mux_dev0_bytes(v) && off + n > mux_dev0_bytes(v))
            n = (size_t)(mux_dev0_bytes(v) - off);
        if (vmux_pread1(v, off, out, n) != 0)
            return -1;
        out += n;
        off += n;
        len -= n;
    }
    v->mux_pos = off;
    return 0;
}


static int vmux_pwrite1(invfs_volume *v, uint64_t off,
                        const void *buf, size_t len)
{
    if (off < v->meta_end_bytes) {
        /* metadata: writethrough mirror to BOTH devices (same local
         * offset). dev0 fail -> log + sticky skip + continue on dev1
         * (rule 3: not a full latch; the DEVT sync_seq bump in vol_flush
         * then necessarily misses dev0 -> staleness is detectable).
         * dev1 fail -> the canonical store is gone: latch (WP22c). */
        int wrote = 0;
        if (v->degraded) {
            fprintf(stderr, "vol: metadata write refused: DEGRADED mount "
                    "(dev0 absent), the volume is read-only\n");
            return -1;
        }
        if (v->io_open[0] && !v->dev_skip[0]) {
            if (blkio_pwrite(&v->io, off, buf, len) != 0) {
                v->dev_skip[0] = 1;
                fprintf(stderr, "vol: dev0 metadata write failed at %llu; "
                        "continuing on dev1 (dev0 will resync at next "
                        "open)\n", (unsigned long long)off);
            } else {
                wrote = 1;
            }
        }
        if (v->io_open[1] && !v->dev_skip[1]) {
            if (blkio_pwrite(&v->io2, off, buf, len) != 0) {
                vol_io_error_latch(v, "metadata mirror write (dev1)");
                return -1;
            }
            wrote = 1;
        }
        return wrote ? 0 : -1;
    }
    if (off < mux_dev0_bytes(v)) {
        if (!v->io_open[0] || v->dev_skip[0]) {
            /* WP25 degraded: the op needs dev0, which is absent -- fail
             * loudly (EIO) with the plain reason */
            if (v->degraded && !v->metaread_logged) {
                v->metaread_logged = 1;
                fprintf(stderr, "vol: write to device 0 refused: DEGRADED "
                        "mount (dev0 absent) -- EIO. Reattach dev0 for "
                        "read-write.\n");
            }
            return -1;
        }
        return blkio_pwrite(&v->io, off, buf, len);
    }
    if (!v->io_open[1] || v->dev_skip[1])
        return -1;
    return blkio_pwrite(&v->io2, off - mux_dev0_bytes(v), buf, len);
}


int vmux_write(invfs_volume *v, const void *buf, size_t len)
{
    uint64_t off = v->mux_pos;
    const uint8_t *in = (const uint8_t *)buf;

    if (v->ndev < 2 && !v->degraded) {
        int rc = blkio_pwrite(&v->io, off, buf, len);
        if (rc == 0) v->mux_pos = off + len;
        return rc;
    }
    while (len) {
        size_t n = len;
        if (off < v->meta_end_bytes && off + n > v->meta_end_bytes)
            n = (size_t)(v->meta_end_bytes - off);
        if (off < mux_dev0_bytes(v) && off + n > mux_dev0_bytes(v))
            n = (size_t)(mux_dev0_bytes(v) - off);
        if (vmux_pwrite1(v, off, in, n) != 0)
            return -1;
        in += n;
        off += n;
        len -= n;
    }
    v->mux_pos = off;
    return 0;
}


void vmux_close(invfs_volume *v)
{
    if (v->io_open[0]) { blkio_close(&v->io);  v->io_open[0] = 0; }
    if (v->io_open[1]) { blkio_close(&v->io2); v->io_open[1] = 0; }
}


/* CRC convention: over the descriptor with the crc32c field read as zero
 * (the RDP0 rule). */
uint32_t devt_crc(const invfs_devt *d)
{
    invfs_devt t = *d;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


/* Persist the in-memory DEVT by read-modify-write of block 0, mirrored by
 * the mux to every writable (non-skipped) device. */
int vol_write_devt(invfs_volume *v)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    invfs_devt t;
    if (v->ndev != 2) return 0;
    t = v->devt;
    memcpy(t.magic, "DEVT", 4);
    t.crc32c = 0;
    t.crc32c = devt_crc(&t);
    if (vmux_seek(v, 0) != 0 || vmux_read(v, blk, sizeof blk) != 0)
        return -1;
    memcpy(blk + INVFS_DEVT_OFF, &t, sizeof t);
    if (vmux_seek(v, 0) != 0 || vmux_write(v, blk, sizeof blk) != 0)
        return -1;
    v->devt = t;
    return 0;
}


/* Storage barrier over both present devices. 0 = all good; 1 = dev0's
 * barrier failed (dev0 is now session-skipped and provably stale -- the
 * DEVT bump here lands on dev1 only, so the divergence is visible at the
 * next open and triggers the resync); -1 = dev1 failed (the caller
 * latches: the canonical store may have lost acknowledged writes --
 * WP22c). */
int vmux_barrier(invfs_volume *v, const char *what)
{
    if (v->ndev < 2 && !v->degraded)
        return blkio_flush(&v->io) == 0 ? 0 : -1;
    if (v->io_open[0] && !v->dev_skip[0] && blkio_flush(&v->io) != 0) {
        v->dev_skip[0] = 1;
        fprintf(stderr, "vol: dev0 barrier failed (%s); continuing on "
                "dev1, dev0 is stale until the next open resyncs it\n",
                what ? what : "barrier");
        v->devt.sync_seq++;
        if (vol_write_devt(v) != 0)
            return -1;
    }
    if (v->io_open[1] && blkio_flush(&v->io2) != 0)
        return -1;
    return v->dev_skip[0] ? 1 : 0;
}


/* Metadata mirror resync (WP25 rule 5): open found one device's metadata
 * span older than the other's (sync_seq mismatch), or a write/barrier
 * failure session-skipped a device. The whole span [0, meta_end) is
 * copied from the newer device to the stale one (both hold it at
 * identical local offsets); the DEVT bump in vol_flush then marks them
 * equal. Newest state wins; the run is logged. */
static int mirror_resync(invfs_volume *v)
{
    int loser = v->resync_winner ^ 1;
    blkio *src = v->resync_winner ? &v->io2 : &v->io;
    blkio *dst = loser ? &v->io2 : &v->io;
    uint8_t *buf = (uint8_t *)malloc(BLKIO_BOUNCE);
    uint64_t off = 0, end = v->meta_end_bytes;

    if (!buf) return -1;
    fprintf(stderr, "vol: mirror resync: dev%d <- dev%d (%llu bytes of "
            "metadata; newest state wins)\n", loser, v->resync_winner,
            (unsigned long long)end);
    while (off < end) {
        size_t n = (size_t)((end - off) > BLKIO_BOUNCE ? BLKIO_BOUNCE
                                                       : end - off);
        if (blkio_pread(src, off, buf, n) != 0) {
            fprintf(stderr, "vol: mirror resync: read failed at %llu\n",
                    (unsigned long long)off);
            free(buf);
            return -1;
        }
        if (blkio_pwrite(dst, off, buf, n) != 0) {
            free(buf);
            fprintf(stderr, "vol: mirror resync: dev%d write failed\n",
                    loser);
            if (loser == 1) {
                /* the canonical store refused: WP22c territory */
                vol_io_error_latch(v, "mirror resync write (dev1)");
            }
            return -1;   /* a dev0 loser stays session-skipped */
        }
        off += n;
    }
    free(buf);
    if (blkio_flush(dst) != 0) {
        if (loser == 1)
            vol_io_error_latch(v, "mirror resync barrier (dev1)");
        return -1;
    }
    v->dev_skip[loser] = 0;
    v->resync_pending = 0;
    return 0;
}


int vol_ndev(const invfs_volume *v)     { return v ? v->ndev : 0; }
int vol_degraded(const invfs_volume *v) { return v && v->degraded; }
int vol_mirror_stale(const invfs_volume *v)
{
    return v && (v->resync_pending || v->dev_skip[0] || v->dev_skip[1]);
}


/* DEVT sanity: a descriptor that claims 2 devices must agree with the
 * superblock it sits next to. */
static int devt_sane(const invfs_devt *d, const invfs_superblock *sb)
{
    if (d->version != INVFS_DEVT_VERSION) return 0;
    if (d->dev_count != 2) return 0;
    if (!d->dev_blocks[0] || !d->dev_blocks[1]) return 0;
    if (d->dev_blocks[0] + d->dev_blocks[1] != sb->total_blocks) return 0;
    if (memcmp(d->vol_uuid, sb->uuid, 16) != 0) return 0;
    return 1;
}


/* Open + validate device 1 of a 2-device volume. The path comes from
 * INVFS_DEV1 (wins) or the DEVT path hint. Returns 0 with io2 open and
 * locked, or -1 (loud). */
static int wp25_open_dev1(invfs_volume *v, const char *hint)
{
    const char *d1 = getenv("INVFS_DEV1");
    invfs_devt d2;
    int rc;

    if (!d1 || !*d1) d1 = hint;
    if (!d1 || !*d1) {
        fprintf(stderr, "vol_open: %s: two-device volume, device 1 not "
                "given (set INVFS_DEV1)\n", v->path);
        return -1;
    }
    rc = blkio_open(&v->io2, d1,
                    blkio_looks_like_device(d1) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "vol_open: %s: device 1 %s: %s\n", v->path, d1,
                blkio_strerror(rc));
        return -1;
    }
    v->io_open[1] = 1;
#ifndef _WIN32
    if (flock(v->io2.fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "vol_open: %s: device 1 %s is in use by another "
                "process\n", v->path, d1);
        vmux_close(v);
        return -1;
    }
#endif
    v->path2 = strdup(d1);
    /* dev1 carries the canonical data: refuse to run without it */
    if (blkio_capacity(&v->io2) <
        v->devt.dev_blocks[1] * (uint64_t)INVFS_BLOCK_SIZE) {
        fprintf(stderr, "vol_open: %s: device 1 %s is smaller than the "
                "device table says (%llu blocks)\n", v->path, d1,
                (unsigned long long)v->devt.dev_blocks[1]);
        return -1;
    }
    if (blkio_pread(&v->io2, INVFS_DEVT_OFF, &d2, sizeof d2) != 0 ||
        memcmp(d2.magic, "DEVT", 4) != 0 || devt_crc(&d2) != d2.crc32c ||
        !devt_sane(&d2, &v->sb)) {
        /* no readable table on dev1: treat it as stale (dev0's table is
         * authoritative); the next flush resyncs dev1's metadata span
         * wholesale, which rewrites its block 0 */
        fprintf(stderr, "vol_open: %s: device 1 has no valid DEVT; "
                "treating it as stale\n", v->path);
        v->resync_pending = 1;
        v->resync_winner = 0;
        v->dev_skip[1] = 1;    /* metadata writes skip dev1 until resync */
        return 0;
    }
    if (d2.dev_blocks[0] != v->devt.dev_blocks[0] ||
        d2.dev_blocks[1] != v->devt.dev_blocks[1]) {
        fprintf(stderr, "vol_open: %s: device 1 DEVT disagrees with "
                "device 0 on the geometry; refusing to mount\n", v->path);
        return -1;
    }
    if (d2.sync_seq != v->devt.sync_seq) {
        if (d2.sync_seq > v->devt.sync_seq) {
            /* dev1 is newer: dev0 is the stale one */
            v->resync_pending = 1;
            v->resync_winner = 1;
            v->dev_skip[0] = 1;
            fprintf(stderr, "vol_open: metadata mirror divergence: dev0 "
                    "is stale (seq %llu < %llu); reads fail over to dev1, "
                    "resync at the next flush\n",
                    (unsigned long long)v->devt.sync_seq,
                    (unsigned long long)d2.sync_seq);
        } else {
            v->resync_pending = 1;
            v->resync_winner = 0;
            fprintf(stderr, "vol_open: metadata mirror divergence: dev1 "
                    "is stale (seq %llu < %llu); resync at the next "
                    "flush\n",
                    (unsigned long long)d2.sync_seq,
                    (unsigned long long)v->devt.sync_seq);
        }
    }
    return 0;
}


/* Degraded bootstrap (WP25 rule 5): the dev0 image is absent. With
 * INVFS_DEV1 set we open device 1 alone, read its block 0 directly, and
 * continue as a READ-ONLY volume serving every structure from the dev1
 * mirror. */
static int wp25_open_degraded(invfs_volume *v)
{
    const char *d1 = getenv("INVFS_DEV1");
    uint8_t blk[INVFS_BLOCK_SIZE];
    invfs_devt d2;
    int rc;

    if (!d1 || !*d1)
        return -1;
    rc = blkio_open(&v->io2, d1,
                    blkio_looks_like_device(d1) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "vol_open: degraded open: device 1 %s: %s\n",
                d1, blkio_strerror(rc));
        return -1;
    }
    v->io_open[1] = 1;
    if (blkio_pread(&v->io2, 0, blk, sizeof blk) != 0)
        goto bad;
    memcpy(&v->sb, blk, sizeof v->sb);
    if (memcmp(v->sb.magic, INVFS_MAGIC, 8) != 0 ||
        invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum)) !=
            v->sb.checksum)
        goto bad;
    if (!(v->sb.vol_flags & VOLF_ASTV2)) {
        fprintf(stderr, "vol_open: %s: format v1 volume (no VOLF_ASTV2): "
                "this build reads format v2 only. Convert it offline "
                "first: invf-migrate-v2 <dev0>\n", d1);
        goto bad;
    }
    memcpy(&d2, blk + INVFS_DEVT_OFF, sizeof d2);
    if (memcmp(d2.magic, "DEVT", 4) != 0 || devt_crc(&d2) != d2.crc32c ||
        !devt_sane(&d2, &v->sb)) {
        fprintf(stderr, "vol_open: degraded open: %s is not device 1 of a "
                "two-device InvariantFS volume (no valid DEVT)\n", d1);
        goto bad;
    }
    if (blkio_capacity(&v->io2) <
        d2.dev_blocks[1] * (uint64_t)INVFS_BLOCK_SIZE) {
        fprintf(stderr, "vol_open: degraded open: %s is smaller than the "
                "device table says\n", d1);
        goto bad;
    }
    v->path2 = strdup(d1);
    v->devt = d2;
    v->devt_present = 1;
    v->ndev = 2;
    v->dev0_present = 0;
    v->degraded = 1;
    v->dev0_blocks = d2.dev_blocks[0];
    v->dev1_blocks = d2.dev_blocks[1];
    v->meta_end_bytes = (v->sb.metadata_zone_start +
                         v->sb.metadata_zone_blocks) *
                        (uint64_t)INVFS_BLOCK_SIZE;
    v->raw_mirror = (d2.flags & INVFS_DEVTF_RAW_MIRROR) != 0;
    v->arena_start = v->sb.raw_zone_start + v->sb.raw_zone_blocks;
    v->arena_blocks = v->dev0_blocks > v->arena_start
                    ? v->dev0_blocks - v->arena_start : 0;
    return 0;
bad:
    vmux_close(v);
    return -1;
}


/* WP24-lite: vol_open_inner(path, at_ckpt, ckpt_seq, err).
 * at_ckpt == 0 is the ordinary open (vol_open). at_ckpt != 0 asks for the
 * read-only time-travel view at the live CKP0 sweep checkpoint
 * (vol_open_at): the journal is replayed from the checkpoint's STAGED
 * prefix (never the live, post-sweep slots) and the inode-area scan stops
 * at the checkpoint's append pointer, so the in-memory view is exactly the
 * sweep-start state; ckpt_seq != 0 pins the expected sweep sequence
 * (K=1 contract: only the one live checkpoint exists). */
static invfs_volume *vol_open_inner(const char *path, int at_ckpt,
                                    uint64_t ckpt_seq, int *err)
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
    v->dev0_present = 1;

    /* A device is taken exclusively -- locked and dismounted. Two processes
       each holding their own in-memory bitmap and L2P would corrupt the
       volume between them, which is why an image file is opened without
       FILE_SHARE_WRITE too. It also stops Windows from mounting whatever
       filesystem it believes is there and writing that filesystem's metadata
       over ours. */
    rc = blkio_open(&v->io, real,
                    blkio_looks_like_device(real) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        /* WP25: with INVFS_DEV1 set a missing dev0 is the DEGRADED leg:
         * open device 1 alone and serve read-only from the mirror. */
        if (wp25_open_degraded(v) != 0) {
            fprintf(stderr, "vol_open: %s: %s\n", real, blkio_strerror(rc));
            *err = -2;
            free(v->path);
            free(v);
            return NULL;
        }
        fprintf(stderr, "vol_open: DEGRADED: %s: device 0 absent; serving "
                "READ-ONLY from the dev1 mirror %s (reattach dev0 for "
                "read-write)\n", real, v->path2);
    } else {
        v->io_open[0] = 1;
    }
#ifndef _WIN32
    /* POSIX twin of the Windows no-share open above: a lingering FUSE daemon
     * finishing its drain and an offline tool (invf-cp/invf-sweep) writing
     * the same image corrupt it between their in-memory bitmaps/L2P (seen
     * in the wild: stale-position record appends clobbering fresh records).
     * LOCK_NB: fail loudly instead of waiting. Released by close(). */
    if ((v->io_open[0] && flock(v->io.fd, LOCK_EX | LOCK_NB) != 0) ||
        (v->io_open[1] && flock(v->io2.fd, LOCK_EX | LOCK_NB) != 0)) {
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

    /* WP27: format v2 reads v2 only. A volume without VOLF_ASTV2 is format
     * v1: its records' AST entries are 24B and carry no physical addresses
     * (they live in the v1 L2P journal). There are deliberately NO dual
     * readers -- convert the volume offline instead. */
    if (!(v->sb.vol_flags & VOLF_ASTV2)) {
        fprintf(stderr,
                "vol_open: %s: format v1 volume (no VOLF_ASTV2): this build "
                "reads format v2 (INVFS_VERSION=%u) only.\n"
                "  Convert it offline first:  invf-migrate-v2 %s\n"
                "  (invf-migrate-v2 rewrites the records with resolved "
                "addresses, in place, crash-safe; run it on an unmounted, "
                "cleanly-closed volume)\n",
                real, INVFS_VERSION, real);
        *err = -12;
        goto fail;
    }

    /* WP25: the DEVT device table at 0x2A0 (block 0 reserved area, the
     * RDP0 convention: absent = zeros = single-device). When it names two
     * devices, open device 1 here so every structure read below (bitmap,
     * journal, inode area) can fail over to the mirror. dev1 holds the
     * canonical data: a volume that cannot reach dev1 is refused loudly
     * (the degraded leg covers dev0-absent only). */
    if (!v->devt_present) {
        invfs_devt dt;
        memset(&dt, 0, sizeof dt);
        if (io_seek(&v->io, INVFS_DEVT_OFF) == 0 &&
            io_read(&v->io, &dt, sizeof dt) == 0 &&
            memcmp(dt.magic, "DEVT", 4) == 0) {
            if (devt_crc(&dt) != dt.crc32c || dt.version != INVFS_DEVT_VERSION) {
                fprintf(stderr, "vol_open: %s: DEVT device table is torn; "
                        "cannot tell the device geometry -- refusing to "
                        "mount (reattach both devices / run invf-fsck)\n",
                        real);
                *err = -5; goto fail;
            }
            if (dt.dev_count == 2) {
                if (!devt_sane(&dt, &v->sb)) {
                    fprintf(stderr, "vol_open: %s: DEVT/superblock "
                                    "mismatch; refusing to mount\n", real);
                    *err = -5; goto fail;
                }
                v->devt = dt;
                v->devt_present = 1;
            } else if (dt.dev_count != 1) {
                fprintf(stderr, "vol_open: %s: DEVT dev_count %u "
                                "unsupported\n", real, dt.dev_count);
                *err = -5; goto fail;
            }
        }
        if (v->devt_present && v->devt.dev_count == 2) {
            v->ndev = 2;
            v->dev0_blocks = v->devt.dev_blocks[0];
            v->dev1_blocks = v->devt.dev_blocks[1];
            v->meta_end_bytes = (v->sb.metadata_zone_start +
                                 v->sb.metadata_zone_blocks) *
                                (uint64_t)INVFS_BLOCK_SIZE;
            v->raw_mirror = (v->devt.flags & INVFS_DEVTF_RAW_MIRROR) != 0;
            v->arena_start = v->sb.raw_zone_start + v->sb.raw_zone_blocks;
            v->arena_blocks = v->dev0_blocks > v->arena_start
                            ? v->dev0_blocks - v->arena_start : 0;
            if (blkio_capacity(&v->io) <
                v->dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE) {
                fprintf(stderr, "vol_open: %s: device 0 is smaller than "
                        "the device table says (%llu blocks)\n", real,
                        (unsigned long long)v->dev0_blocks);
                *err = -3; goto fail;
            }
            if (wp25_open_dev1(v, v->devt.dev1_hint) != 0) {
                fprintf(stderr, "vol_open: %s: device 1 (the canonical "
                        "data store) is required; refusing to mount. "
                        "Reattach it, or for a dev0-absent READ-ONLY "
                        "mount set INVFS_DEV1 and point the tool at the "
                        "missing dev0 path.\n", real);
                *err = -2; goto fail;
            }
        } else {
            v->ndev = 1;
            v->dev0_blocks = v->sb.total_blocks;
        }
    }

    /* WP30 Phase 3: load MET0 descriptor at 0x3A0.
     * v0.3.0+: dynamic metadata extents are mandatory.
     * format_version 0 = legacy volume (no dynamic extents support). */
    if (v->sb.format_version == 0) {
        fprintf(stderr, "vol_open: volume format version 0 (pre-v0.3.0) not supported; "
                        "dynamic metadata extents are mandatory\n");
        goto fail;
    }
    {
        invfs_met0 m0;
        if (io_seek(&v->io, INVFS_MET0_OFF) == 0 &&
            io_read(&v->io, &m0, sizeof(m0)) == 0 &&
            memcmp(m0.magic, "MET0", 4) == 0) {
            if (meta_met0_crc(&m0) == m0.crc32c && m0.version == 1) {
                v->met0 = m0;
                v->met0_present = 1;
                v->meta_active_extent = m0.active_extent;
                v->meta_active_offset = m0.active_offset;
            } else {
                fprintf(stderr, "vol_open: MET0 descriptor CRC/version "
                                "mismatch; volume may be corrupted\n");
                goto fail;
            }
        } else {
            fprintf(stderr, "vol_open: MET0 descriptor not found at offset 0x%llX\n",
                    (unsigned long long)INVFS_MET0_OFF);
            goto fail;
        }
    }

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
            if (rsz0_crc(&rz) != rz.crc32c || !rsz0_sane(v, &rz)) {
                /* torn/desc corrupt: the apply it armed never started (the
                 * arm precedes it), so the old metadata is intact -- ignore
                 * the descriptor and let the RECOVERY path below decide */
                fprintf(stderr, "vol_open: ignoring a corrupt RSZ0 resize "
                                "descriptor\n");
            } else if (v->degraded) {
                /* the roll-forward WRITES the metadata span; the dev1
                 * mirror alone cannot run it (the mirror would diverge
                 * from the half-moved dev0 state). Reattach dev0. */
                fprintf(stderr, "vol_open: %s: an interrupted resize is "
                        "pending and the volume is DEGRADED (dev0 absent); "
                        "reattach dev0 to finish the resize\n", real);
                *err = -10; goto fail;
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
    /* WP24-lite: a time-travel open requires the LIVE checkpoint -- the
     * retention registry (which keeps the post-checkpoint-freed blocks the
     * cut still references alive) exists exactly while CKP0 does. A
     * realized/absent checkpoint means the old segment versions may be
     * reused: refuse loudly, never serve a maybe-phantom view. */
    if (at_ckpt) {
        if (!v->ck_present) {
            fprintf(stderr, "vol_open_at: %s: no live sweep checkpoint "
                    "(nothing armed, or already realized)\n", real);
            *err = -11; goto fail;
        }
        if (ckpt_seq && ckpt_seq != v->ck.sweep_seq) {
            fprintf(stderr, "vol_open_at: %s: checkpoint #%llu requested, "
                    "but the live checkpoint is #%llu (K=1: only the live "
                    "one can be viewed)\n", real,
                    (unsigned long long)ckpt_seq,
                    (unsigned long long)v->ck.sweep_seq);
            *err = -11; goto fail;
        }
    }

    v->bitmap_blocks = (v->sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    v->bitmap = (uint8_t *)calloc(1, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE);
    if (!v->bitmap) { *err = -6; goto fail; }
    if (io_seek(&v->io, v->sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, v->bitmap, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE) != 0)
        { *err = -7; goto fail; }

    /* WP30 Phase 5: load metadata extent mapper table */
    if (meta_mapper_load(v) != 0) { *err = -7; goto fail; }

    v->journal_start = v->sb.metadata_zone_start + v->bitmap_blocks + INVFS_META_EXT_BLOCKS;
    v->journal_pos = v->journal_start * INVFS_BLOCK_SIZE;
    v->inode_area_start = v->journal_start + JOURNAL_BLOCKS;
    v->inode_area_pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    v->inode_area_end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
                        * INVFS_BLOCK_SIZE;

    /* replay the owner-WAL journal BEFORE the inode scan. WP27: file
     * records carry their own pbas and need no mapping; the WAL resolves
     * only the owner-referenced shapes (batches / seal parity / retention
     * ranges / device sidecars). WP24-lite: a time-travel open replays the
     * checkpoint's STAGED prefix instead (read-only, from memory). */
    if (at_ckpt) {
        int rrc = ckp_stage_replay(v);
        if (rrc != 0) {
            fprintf(stderr, "vol_open_at: %s: checkpoint #%llu staging "
                    "failed verification; the present is untouched\n",
                    real, (unsigned long long)v->ck.sweep_seq);
            *err = -11; goto fail;
        }
    } else if (l2p_replay(v) != 0) { *err = -9; goto fail; }

    /* scan existing inode records: find end of area + max inode id + name index */
    {
        uint64_t p = v->inode_area_pos;
        /* WP24-lite: at a time-travel open the scan (and with it the name
         * index and the WP22d consistent-cut fold) stops at the
         * checkpoint's append pointer: the post-checkpoint records,
         * tombstones and the retention registry itself stay invisible.
         * ckp_stage_replay already validated the pointer. */
        const uint64_t scan_end = at_ckpt ? v->ck.inode_area_pos
                                          : v->inode_area_end;
        uint64_t found = 0;
        scan_set ss = { NULL, 0, 0 };
        size_t si;

        if (idx_init(v) != 0) { *err = -6; goto fail; }
        while (p + sizeof(invfs_inode_rec) <= scan_end) {
            invfs_inode_rec rec_h;
            uint8_t *rb = NULL;
            if (io_seek(&v->io, p) != 0 ||
                io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
                break;
            if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
                break;  /* end of records */
            if (rec_h.rec_len < sizeof(invfs_inode_rec) ||
                rec_h.rec_len > INVFS_MAX_REC_LEN ||
                p + rec_h.rec_len + 4 > scan_end) {
                v->scan_anomalies++;
                break;  /* corrupted tail — stop */
            }
            /* torn-write protection: verify trailing CRC32C; a record
             * whose CRC fails is a half-written append (crash/SIGPIPE),
             * NOT a valid boundary — stop here so later tools never
             * step into garbage. */
            rb = (uint8_t *)malloc((size_t)rec_h.rec_len + 4);
            if (!rb) break;
            {
                uint32_t crc_stored, crc_calc;
                if (io_seek(&v->io, p) != 0 ||
                    io_read(&v->io, rb, (size_t)rec_h.rec_len + 4) != 0) {
                    free(rb); break;
                }
                memcpy(&crc_stored, rb + rec_h.rec_len, 4);
                crc_calc = invfs_crc32c(rb, rec_h.rec_len);
                if (crc_calc != crc_stored) {
                    fprintf(stderr, "vol_open: corrupt inode record at %llu, "
                            "skipping (rec_len=%u)\n",
                            (unsigned long long)p,
                            (unsigned)rec_h.rec_len);
                    v->scan_anomalies++;
                    free(rb);
                    /* skip the corrupt record and keep scanning so files
                     * AFTER it stay visible; run fsck -f to clean up */
                    p += (uint64_t)rec_h.rec_len + 4;
                    continue;
                }
            }
            if (rec_h.magic == INODE_REC_MAGIC &&
                rec_h.inode_id >= v->next_inode_id)
                v->next_inode_id = rec_h.inode_id + 1;
            /* Feed the scan set from this same pass: the record is read and
               CRC-verified here, so the set inherits the exact same
               skip-the-corrupt-record semantics for free. A separate pass
               would stop at the first bad record and hide every name after
               it. WP27: an INOD whose entries carry an invalid pba is
               BROKEN -- recorded as a version but never live; the name
               falls back to its newest valid version. A DELT still applies
               (position-kill), except the kill of a broken replacement's
               predecessor is skipped (torn retire: keep the fallback). */
            {
                size_t nl = rec_h.name_len < sizeof(rec_h.name)
                          ? rec_h.name_len : sizeof(rec_h.name) - 1;
                if (rec_h.magic == INODE_REC_MAGIC) {
                    uint64_t moff[4], mlen[4];
                    unsigned mn = 0;
                    /* a broken record is hidden from the live view; the
                     * loud log happens at fold time for the names that
                     * actually lose content (a properly retired old
                     * version is broken too -- and nothing was lost) */
                    uint64_t miss = rec_pba_miss(rb, rec_h.rec_len,
                                                 v->sb.total_blocks,
                                                 moff, mlen, &mn);
                    if (scanset_inod(&ss, rec_h.name, nl, rec_h.inode_id, p,
                                     rec_h.file_size, rec_h.ctime,
                                     miss) != 0) {
                        free(rb); scanset_free(&ss);
                        *err = -6; goto fail;
                    }
                } else {
                    scanset_delt(&ss, rec_h.name, nl, rec_h.inode_id,
                                 rec_h.file_size);
                }
            }
            free(rb);
            p += rec_h.rec_len + 4;
            found++;
        }
        v->inode_area_pos = p;
        /* everything the scan just walked is on the device already: the
         * first barrier anchor (WP22c/F1) */
        v->inode_area_durable = p;
        /* fold the consistent cut into the live index: per name, the newest
         * non-broken version; a name with none is absent (logged loudly) */
        for (si = 0; ss.buck && si <= ss.mask; si++) {
            scan_name *e;
            for (e = ss.buck[si]; e; e = e->next) {
                const scan_ver *lv = scanset_live(e);
                if (!lv) {
                    /* nvers == 0 is a properly deleted name -- silent.
                     * Only a name whose every version is broken is a
                     * genuine cut. */
                    if (e->nvers) {
                        fprintf(stderr, "vol_open: record cut: %s: no valid "
                                "version remains; the file is hidden (run "
                                "invf-fsck -f)\n", e->name);
                        v->open_cuts++;
                    }
                    continue;
                }
                if (e->vers[e->nvers - 1].broken) {
                    fprintf(stderr, "vol_open: record cut: %s: fell back to "
                            "record @%llu (newest version invalid)\n",
                            e->name, (unsigned long long)lv->pos);
                    v->open_cuts++;
                }
                idx_put(v, e->name, e->nlen, lv->id, lv->pos,
                        lv->size, lv->ctime);
                idx_put_id(v, lv->id, lv->pos);
            }
        }
        scanset_free(&ss);
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] scanned %llu inode recs, next_inode=%llu, area_pos=%llu, "
                   "index=%llu names/%llu dirs\n",
                   (unsigned long long)found, (unsigned long long)v->next_inode_id,
                   (unsigned long long)v->inode_area_pos,
                   (unsigned long long)v->ncount, (unsigned long long)v->dcount);
    }

    /* WP25: with the name/id indexes live, load the tier + RAW-mirror
     * indexes from their owner records (2-device volumes only; a degraded
     * mount needs the RAW mirror for reads). */
    if (v->ndev == 2) {
        v->tier_owner = vol_find(v, "\x01tier0");
        v->rawm_owner = vol_find(v, "\x01rawm");
        wp25_index_load(v);
    }

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
    /* WP22d/WP27: bitmap divergence guard. The bitmap is a CACHE of the
     * records + the owner WAL (fsck rebuilds it wholesale); the flush
     * writes the dirty bitmap range, the WAL append and the record appends
     * as separate units under one barrier, so a drop window can tear them
     * apart. The fsck -f rebuild does the full two-sided reconcile; at
     * open we do the cheap one-sided direction: every WAL-mapped block is
     * forced USED in the runtime bitmap, never cleared. A false positive
     * is a leak fsck reclaims; a false negative would be corruption.
     * Runs AFTER alloc_state_reset so the repaired range stays dirty for
     * the first flush. */
    {
        size_t bi;
        uint64_t fixed = 0;
        for (bi = 0; bi < v->l2p_count; bi++) {
            const invfs_l2p_entry *e = &v->l2p[bi];
            uint64_t b;
            if (e->type != INVFS_JRN_MAP) continue;
            for (b = e->pba; b < e->pba + e->length; b++) {
                if (b >= v->sb.total_blocks) break;
                if (!bit_get(v->bitmap, b)) {
                    bit_set(v->bitmap, b);
                    bm_dirty(v, b);
                    v->free_blocks--;
                    if (b >= v->sb.shadow_zone_start) v->shadow_free--;
                    else if (b >= v->sb.raw_zone_start) v->raw_free--;
                    fixed++;
                }
            }
        }
        if (fixed)
            fprintf(stderr, "vol_open: bitmap/journal divergence: %llu "
                    "mapped block%s were marked free; forced used\n",
                    (unsigned long long)fixed, fixed == 1 ? "" : "s");
    }
    /* WP27: and the record side of the same divergence: on a NOT-cleanly
     * closed volume the on-disk bitmap may predate landed records (the
     * record append is the commit point; the bitmap covering its pbas was
     * flushed first, but a drop window can take bitmap pages and spare the
     * record's). Reconcile one-sidedly: force every live record's segment
     * extent USED. Extents derive from the segments' framed headers
     * (seg_extent) -- one 8-byte read per segment, on the recovery path
     * only (a clean close flushed a consistent bitmap and skips this). A
     * segment whose header is unreadable is content-torn; its first block
     * is pinned and the file's reads fail loudly at CRC, exactly like
     * before -- fsck's content cut arbitrates. */
    if (v->sb.state != INVFS_STATE_CLEAN && !at_ckpt && v->nbuck) {
        uint64_t fixed = 0, pinned = 0;
        size_t b;
        for (b = 0; b <= v->nmask; b++) {
            const name_index_entry *ne;
            for (ne = v->nbuck[b]; ne; ne = ne->next) {
                uint8_t *rec = NULL;
                uint32_t rl = 0;
                invfs_ast_hdr ah;
                const invfs_ast_block_entry *ents;
                size_t base = sizeof(invfs_inode_rec);
                uint32_t i;
                if (meta_read_record_by_id(v, ne->inode_id, &rec, &rl,
                                           NULL, 0, NULL) != 0)
                    continue;
                if (rl < base + INVFS_AST_HDR_V1_LEN ||
                    invfs_ast_hdr_parse(rec + base, rl - base, &ah) != 0 ||
                    rl < base + ah.hdr_len +
                         (size_t)ah.num_blocks * sizeof(*ents)) {
                    free(rec);
                    continue;
                }
                ents = (const invfs_ast_block_entry *)(rec + base +
                                                       ah.hdr_len);
                for (i = 0; i < ah.num_blocks; i++) {
                    uint64_t pba = ents[i].pba, plen = 0, k, bend;
                    if (!pba || pba >= v->sb.total_blocks) continue;
                    /* "\x01reten*" registry entries carry BLOCKS in
                     * length (never read as files); everyone else's
                     * extent derives from the framed segment header */
                    if (ne->nlen >= 6 && ne->name[0] == 0x01 &&
                        memcmp(ne->name + 1, "reten", 5) == 0) {
                        plen = ents[i].length;
                    } else if (seg_extent(v, pba, NULL, &plen) != 0) {
                        plen = 1;   /* torn header: pin the first block */
                        pinned++;
                    }
                    if (!plen) continue;
                    bend = pba + plen;
                    if (bend > v->sb.total_blocks)
                        bend = v->sb.total_blocks;
                    for (k = pba; k < bend; k++) {
                        if (!bit_get(v->bitmap, k)) {
                            bit_set(v->bitmap, k);
                            bm_dirty(v, k);
                            v->free_blocks--;
                            if (k >= v->sb.shadow_zone_start)
                                v->shadow_free--;
                            else if (k >= v->sb.raw_zone_start)
                                v->raw_free--;
                            fixed++;
                        }
                    }
                }
                free(rec);
            }
        }
        if (fixed || pinned)
            fprintf(stderr, "vol_open: bitmap/record divergence: %llu "
                    "referenced block%s were marked free; forced used "
                    "(%llu torn segment header%s pinned by first block)\n",
                    (unsigned long long)fixed, fixed == 1 ? "" : "s",
                    (unsigned long long)pinned, pinned == 1 ? "" : "s");
    }
    /* H5: a volume whose latch persisted in the superblock re-evaluates it
     * at open: space freed while it was offline (fsck reclaim, a resize,
     * a delete in a session that never flushed the flag clear) must not
     * keep it read-only. In RAM only -- persisted by the first flush. */
    vol_readonly_unlatch(v);

    /* WP24-lite: the time-travel handle is read-only by construction. The
     * VOLF_READONLY latch (RAM only -- never flushed from this handle)
     * makes every vol_write_enabled caller answer EROFS; time_travel is
     * the engine-level backstop (vol_mark_dirty refuses loudly, vol_flush/
     * vol_sync/vol_close write nothing). Set BEFORE the auto-recovery
     * block below, so a dirty-at-open TT mount takes the conservative
     * no-write path (needs_recovery) instead of the sb-writing one. */
    if (at_ckpt) {
        vol_set_readonly(v, 1);
        v->time_travel = 1;
        fprintf(stderr, "vol_open_at: %s: read-only view at sweep "
                "checkpoint #%llu (inode area @%llu, journal @%llu); the "
                "live volume is untouched\n", real,
                (unsigned long long)v->ck.sweep_seq,
                (unsigned long long)v->ck.inode_area_pos,
                (unsigned long long)v->ck.journal_pos);
    }

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
    /* WP27: seed the heat summaries conservative-hot; the sweep's decay
     * pass recomputes the truth from the records' TLVs. */
    l2p_seed_heat(v);
    /* WP27: build the pba reference map at open: the map must count the
     * live set EXACTLY for the retire/dedupe free gates, and building it
     * from the records the open scan just CRC-validated is free I/O.
     * Building it lazily (the first retire) would lose records superseded
     * between open and that first use, and a retire's -1 on a
     * never-counted record would drive a live sharer's count to zero. */
    if (!v->time_travel)
        pba_ref_ensure(v);
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
        /* WP22d: a non-zero open_cuts means the consistent cut hid
         * records (drop-torn mappings). The live set is consistent, but
         * the losses are unreviewed: keep the conservative manual path
         * (fsck -f quarantines them) instead of silently cleaning up. */
        if (v->degraded) {
            fprintf(stderr, "vol_open: %s: DEGRADED read-only mount "
                    "(device 0 absent); the recorded state 0x%02X is "
                    "left untouched until reattach\n",
                    real, (unsigned)v->sb.state);
            v->needs_recovery = 1;
        } else if (!ro_flag && v->scan_anomalies == 0 && v->open_cuts == 0 &&
            (!ar || strcmp(ar, "0") != 0)) {
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
                    v->open_cuts ? ", l2p cuts hidden" : "",
                    path);
            v->needs_recovery = 1;
        }
    }
    if (v->degraded)
        v->needs_recovery = 1;   /* a degraded mount is always read-only */
    return v;
fail:
    io_close(&v->io);
    if (v->bitmap) free(v->bitmap);
    free(v->jops);
    free(v->mjops);
    free(v->meta_mapper);
    free(v->heat_tab);
    free(v->pba_ref);
    free(v->l2p);
    free(v->l2p_idx);
    free(v->tier);
    free(v->rawm);
    free(v->path2);
    free(v->path);
    free(v);
    return NULL;
}


invfs_volume *vol_open(const char *path, int *err)
{
    return vol_open_inner(path, 0, 0, err);
}


invfs_volume *vol_open_at(const char *path, uint64_t ckpt_seq, int *err)
{
    return vol_open_inner(path, 1, ckpt_seq, err);
}


int vol_time_travel(const invfs_volume *v)
{
    return v && v->time_travel;
}


void vol_close(invfs_volume *v)
{
    if (!v) return;
    /* WP27: read heat accrues per session in RAM and persists at the
     * sweep's decay pass (or an explicit vol_heat_persist) -- never here:
     * a close that appended record rewrites per read file would churn the
     * inode area on every read-only mount, and under a live checkpoint
     * (compaction barred) the churn can fill the area outright. */
    /* Close is the only place that can honestly write CLEAN, and it can only
       do so after the maps are down. There was no flush here at all: every
       tool that mutated the volume had to remember to call vol_flush itself,
       and forgetting cost the whole run silently. Only a session that
       actually dirtied the volume writes anything, so invf-ls and invf-cat
       stay read-only.
       WP24-lite: a time-travel handle skips the whole block -- it never
       dirtied the device (vol_mark_dirty refuses), so there is nothing to
       flush and the CLEAN mark is not this view's to write. */
    if (v->dirty && !v->time_travel) {
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
            vmux_barrier(v, "close");
    }
    io_close(&v->io);
    idx_clear(v);
    arc_destroy(v->arc);
    cpack_map_cache_reset(v);
    free(v->heat_tab);
    free(v->pba_ref);
    free(v->seal_dirty);
    free(v->retmap);
    free(v->tz);
    free(v->bz);
    free(v->bitmap);
    free(v->l2p);
    free(v->l2p_idx);
    free(v->jops);
    free(v->mjops);
    free(v->meta_mapper);
    free(v->tier);
    free(v->rawm);
    free(v->path2);
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


#ifndef _WIN32

/* WP22d test hook (tools/test-flushfail.sh / test-flakey.sh): die
 * mid-compaction -- after the image write ("image"), after the image
 * barrier ("barrier1"), or after the selector flip ("flip"). The next
 * mount must find either the old slot or the new one fully intact. */
static int jrn_abort_at(const char *stage)
{
    const char *a = getenv("INVFS_COMPACT_ABORT_AT");
    return a && strcmp(a, stage) == 0;
}

#endif


/* Queue one journal op for the next flush's append. The entry's crc is
 * restamped from the chain at write time; a MAP op's pad bytes are
 * re-read from the live table at write time (heat keeps moving in RAM
 * after the op was queued). */
int jrn_push_op(invfs_volume *v, const invfs_l2p_entry *e)
{
    if (v->jops_n == v->jops_cap) {
        size_t ncap = v->jops_cap ? v->jops_cap * 2 : 256;
        invfs_l2p_entry *nj =
            (invfs_l2p_entry *)realloc(v->jops, ncap * sizeof *nj);
        if (!nj) return -1;
        v->jops = nj;
        v->jops_cap = ncap;
    }
    v->jops[v->jops_n++] = *e;
    return 0;
}


/* WP30: queue one metadata extent WAL op for the next flush's append.
 * The entry's CRC16-CCITT is computed at push time. */
int jrn_push_meta_op(invfs_volume *v, const invfs_meta_wal *w)
{
    if (v->mjops_n == v->mjops_cap) {
        size_t ncap = v->mjops_cap ? v->mjops_cap * 2 : 64;
        invfs_meta_wal *nj = (invfs_meta_wal *)realloc(v->mjops, ncap * sizeof *nj);
        if (!nj) return -1;
        v->mjops = nj;
        v->mjops_cap = ncap;
    }
    v->mjops[v->mjops_n++] = *w;
    return 0;
}


/* Write the compacted image (the whole live table, chained) + slot header
 * into slot `target`. The header's crc lands in the same block write; the
 * caller barriers, then flips the selector. 0 = ok. */
static int jrn_write_image(invfs_volume *v, uint32_t target, uint64_t seq)
{
    uint64_t base = jrn_slot_base(v, target);
    uint64_t img_bytes = (uint64_t)v->l2p_count * sizeof(invfs_l2p_entry);
    invfs_jrn_hdr h;
    uint8_t blk[INVFS_BLOCK_SIZE];
    invfs_l2p_entry *buf = NULL;
    uint32_t prev;
    size_t i;

    memset(&h, 0, sizeof h);
    memcpy(h.magic, INVFS_JRN_MAGIC, 4);
    h.version = INVFS_JRN_VERSION;
    h.seq = seq;
    h.image_bytes = img_bytes;
    prev = jrn_seed(&h);
    if (img_bytes) {
        buf = (invfs_l2p_entry *)malloc((size_t)img_bytes);
        if (!buf) return -1;
        for (i = 0; i < v->l2p_count; i++) {
            buf[i] = v->l2p[i];
            buf[i].crc = jrn_chain(prev, &buf[i]);
            prev = buf[i].crc;
        }
        h.image_crc = invfs_crc32c(buf, (size_t)img_bytes);
    }
    h.crc32c = invfs_crc32c(&h, offsetof(invfs_jrn_hdr, crc32c));
    memset(blk, 0, sizeof blk);
    memcpy(blk, &h, sizeof h);
    if (io_seek(&v->io, base) != 0 ||
        io_write(&v->io, blk, sizeof blk) != 0) {
        free(buf);
        return -1;
    }
    if (img_bytes) {
        if (io_seek(&v->io, base + INVFS_BLOCK_SIZE) != 0 ||
            io_write(&v->io, buf, (size_t)img_bytes) != 0) {
            free(buf);
            return -1;
        }
    }
    /* the append point + chain state of the fresh slot */
    v->journal_pos = base + INVFS_BLOCK_SIZE + img_bytes;
    v->j_last_crc = prev;
    free(buf);
    return 0;
}


/* Atomic compaction: image the table into the inactive slot, barrier,
 * flip the superblock selector, barrier. A crash anywhere before the flip
 * leaves the old slot authoritative; a torn new slot is caught by
 * image_crc at the next replay and the old slot still answers.
 *
 * The LEGACY migration images into slot 1, never slot 0: the flat log it
 * replaces starts at the journal base = slot 0's header block, so imaging
 * slot 0 would destroy the log's BEGINNING -- the part replay reads first
 * -- and a drop or kill mid-migration would leave neither a valid slot
 * nor a replayable log. Imaging slot 1 leaves the log's start intact: a
 * prefix shorter than a slot survives whole, a longer one survives up to
 * the slot boundary (a valid prefix either way), and replay, seeing pad2
 * == LEGACY with no CRC-valid slot, falls back to it. */
static int jrn_compact(invfs_volume *v)
{
    uint32_t target = v->j_slotted ? (v->j_slot ^ 1) : 1;
    uint64_t seq = v->j_slotted ? v->j_seq + 1 : 1;

    if ((uint64_t)v->l2p_count * sizeof(invfs_l2p_entry) >
        (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1) * INVFS_BLOCK_SIZE)
        return 1;   /* the table outgrew a slot: only reachable for a
                     * legacy volume >466k live mappings -- caller stays
                     * legacy-append (documented limitation) */
#ifndef _WIN32
    {
        const char *fc = getenv("INVFS_JRN_FORCE_COMPACT");
        (void)fc;
    }
#endif
    if (jrn_write_image(v, target, seq) != 0) {
        vol_io_error_latch(v, "journal compaction image write");
        return -1;
    }
#ifndef _WIN32
    if (jrn_abort_at("image")) { kill(getpid(), SIGKILL); }
#endif
    if (vmux_barrier(v, "journal compaction barrier") < 0) {
        vol_io_error_latch(v, "journal compaction barrier");
        return -1;
    }
#ifndef _WIN32
    if (jrn_abort_at("barrier1")) { kill(getpid(), SIGKILL); }
#endif
    /* the flip: selector write is the commit point */
    v->sb.pad2 = target == 0 ? INVFS_JSEL_SLOT0 : INVFS_JSEL_SLOT1;
    if (vol_write_sb(v) != 0) {
        vol_io_error_latch(v, "journal slot selector write");
        return -1;
    }
    if (vmux_barrier(v, "journal slot flip barrier") < 0) {
        vol_io_error_latch(v, "journal slot flip barrier");
        return -1;
    }
#ifndef _WIN32
    if (jrn_abort_at("flip")) { kill(getpid(), SIGKILL); }
#endif
    v->j_slotted = 1;
    v->j_slot = target;
    v->j_seq = seq;
    v->jops_n = 0;         /* the image covers every pending op */
    v->j_compact = 0;
    return 0;
}


/* Append the pending ops at the active slot's log end.
 * One write, chained from j_last_crc; a drop window can only punch a hole
 * that replay stops at -- durable prefixes are never rewritten.
 * Also appends pending metadata extent WAL entries (mjops) after L2P entries. */
static int jrn_append_pending(invfs_volume *v)
{
    size_t n = v->jops_n, i;
    invfs_l2p_entry *buf;
    uint32_t prev;
    uint64_t jp;

    if (!n && !v->mjops_n) return 0;

    /* Append L2P entries first */
    if (n) {
        buf = (invfs_l2p_entry *)malloc(n * sizeof *buf);
        if (!buf) return -1;
        prev = v->j_last_crc;
        for (i = 0; i < n; i++) {
            buf[i] = v->jops[i];
            buf[i].crc = jrn_chain(prev, &buf[i]);
            prev = buf[i].crc;
        }
        jp = v->journal_pos;
        if (io_seek(&v->io, jp) != 0 ||
            io_write(&v->io, buf, n * sizeof *buf) != 0) {
            free(buf);
            vol_io_error_latch(v, "journal append");
            return -1;
        }
        free(buf);
        v->journal_pos = jp + n * sizeof *buf;
        v->j_last_crc = prev;
        v->jops_n = 0;
    }

    /* Append metadata extent WAL entries (mjops) */
    if (v->mjops_n) {
        size_t mn = v->mjops_n;
        uint8_t *mbuf = (uint8_t *)malloc(mn * sizeof(invfs_meta_wal));
        if (!mbuf) return -1;
        memcpy(mbuf, v->mjops, mn * sizeof(invfs_meta_wal));
        jp = v->journal_pos;
        if (io_seek(&v->io, jp) != 0 ||
            io_write(&v->io, mbuf, mn * sizeof(invfs_meta_wal)) != 0) {
            free(mbuf);
            vol_io_error_latch(v, "metadata WAL append");
            return -1;
        }
        free(mbuf);
        v->journal_pos = jp + mn * sizeof(invfs_meta_wal);
        v->mjops_n = 0;
    }
    return 0;
}


/* Legacy-mode append (only for a legacy volume whose live table outgrew a
 * slot -- everything else migrates): append at the legacy append point,
 * re-stamp the terminator, keep the pre-WP22d wire format. */
static int jrn_append_legacy(invfs_volume *v)
{
    uint64_t jend = (v->journal_start + JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
    size_t n = v->jops_n, i;
    invfs_l2p_entry *buf;
    uint64_t jp = v->journal_pos;

    if (!n) return 0;
    if (jp + (uint64_t)(n + 1) * sizeof(invfs_l2p_entry) > jend)
        return -1;   /* journal full */
    buf = (invfs_l2p_entry *)malloc(n * sizeof *buf);
    if (!buf) return -1;
    for (i = 0; i < n; i++) {
        buf[i] = v->jops[i];
        buf[i].crc = invfs_crc32c(&buf[i], offsetof(invfs_l2p_entry, crc));
    }
    if (io_seek(&v->io, jp) != 0 ||
        io_write(&v->io, buf, n * sizeof *buf) != 0) {
        free(buf);
        vol_io_error_latch(v, "journal write");
        return -1;
    }
    free(buf);
    jp += n * sizeof *buf;
    /* Terminator: replay stops at the first entry whose CRC does not
       check out. Without it a journal that SHRANK (after a delete) left
       the previous, still-valid tail behind, and the next mount replayed
       mappings for blocks that had already been freed. */
    {
        invfs_l2p_entry z;
        memset(&z, 0, sizeof z);
        z.crc = ~invfs_crc32c(&z, offsetof(invfs_l2p_entry, crc));
        if (io_seek(&v->io, jp) != 0 ||
            io_write(&v->io, &z, sizeof z) != 0) {
            vol_io_error_latch(v, "journal terminator write");
            return -1;
        }
    }
    v->journal_pos = jp;
    v->jops_n = 0;
    return 0;
}


/* The journal half of vol_flush: migrate legacy on first contact, compact
 * when the log cannot hold the pending ops (or a rebuild/bulk-heat forced
 * it), otherwise just append. */
static int jrn_flush(invfs_volume *v)
{
    uint64_t slot_end, need;
    int force = 0;
#ifndef _WIN32
    {
        const char *fc = getenv("INVFS_JRN_FORCE_COMPACT");
        force = fc && strcmp(fc, "0") != 0;
    }
#endif
    if (!v->j_slotted)
        return jrn_compact(v) < 0 ? -1 :
               (v->j_slotted ? 0 : jrn_append_legacy(v));
    need = (uint64_t)v->jops_n * sizeof(invfs_l2p_entry);
    slot_end = jrn_slot_base(v, v->j_slot) +
               (uint64_t)INVFS_JRN_SLOT_BLOCKS * INVFS_BLOCK_SIZE;
    if (v->j_compact || force ||
        v->journal_pos + need > slot_end)
        return jrn_compact(v) == 0 ? 0 : -1;
    return jrn_append_pending(v);
}


int vol_flush(invfs_volume *v)
{
    /* WP24-lite: a time-travel handle never persists. Every mutation was
     * already refused at vol_mark_dirty, so nothing is pending and the
     * flush contract is vacuously satisfied -- and the superblock write
     * below would otherwise land on the PRESENT volume's block 0. */
    if (v->time_travel) return 0;
    /* WP25: same for a degraded mount (dev0 absent): vol_mark_dirty
     * refused every mutation, so nothing is pending; a flush attempt
     * would only trip the mirror's read-only refusal. */
    if (v->degraded) return 0;
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
    /* WP22d: the journal is append-only within a slot (invarifs.h WP22d
     * note). The pre-WP22d flush rewrote the suffix from l2p_dirty and
     * re-stamped the terminator, so any delete/heat touch dragged the
     * rewrite frontier back over entries that were already durable -- and
     * a dm-flakey drop window mid-rewrite silently un-mapped fsync-
     * acknowledged files (F3). Now a flush appends the pending ops at the
     * log end, or compacts the whole table into the inactive slot
     * (double-buffered, selector flip after a barrier) when the log can't
     * hold them. */
    if (jrn_flush(v) != 0)
        return -1;   /* the journal paths latch their own failures */
    /* WP25: the rawm/tier owner records are rewritten here, AFTER the
     * journal carried their (re)maps (maps durable before the record that
     * names them -- the tz_seal rule). */
    if (v->ndev == 2 && !v->degraded && (v->rawm_dirty || v->tier_dirty)) {
        if (wp25_owner_sync(v) != 0) {
            vol_io_error_latch(v, "mirror/tier owner sync");
            return -1;
        }
    }
    /* WP25: the mirror staleness resync runs inside the first flush that
     * follows the open that detected it (newest state wins), then the
     * DEVT sync_seq bump certifies both devices carry this state. */
    if (v->ndev == 2 && !v->degraded) {
        if (v->resync_pending)
            mirror_resync(v);   /* failures keep the loser skipped */
        v->devt.sync_seq++;
        if (vol_write_devt(v) != 0) {
            vol_io_error_latch(v, "DEVT write");
            return -1;
        }
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
    /* WP24-lite: nothing of this handle's can be in flight (mutations are
     * refused), so the durability contract is already met without touching
     * the device. */
    if (v->time_travel) return 0;
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
    if (vmux_barrier(v, "sync") < 0) {
        vol_io_error_latch(v, "sync");
        return -1;
    }
    v->inode_area_durable = v->inode_area_pos;
    return 0;
}


/* allocate n consecutive free blocks in a zone; returns start block or 0.
 * type: INVFS_ALLOC_DATA (0) = data blocks, INVFS_ALLOC_META (1) = metadata blocks.
 * WP30: metadata allocations are tracked separately in meta_type_bitmap. */
uint64_t alloc_blocks(invfs_volume *v, uint64_t zone_start, uint64_t zone_len,
                             uint64_t n, int use_reserve, int type)
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
    } else if (v->ndev == 2 && v->arena_blocks &&
               zone_start == v->arena_start) {
        /* WP25: the dev0 tier arena (redundant acceleration copies only) */
        cursor = &v->arena_cursor;  zone_free = &v->arena_free;
        fail_run = &v->arena_fail_run;
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

    /* WP30 Phase 4: data allocations must respect metadata reservation */
    if (type == INVFS_ALLOC_DATA && v->sb.meta_reserved_pct > 0) {
        uint64_t meta_reserve =
            (v->sb.total_blocks * v->sb.meta_reserved_pct) / 100;
        if (v->free_blocks - n < meta_reserve)
            return 0;  /* ENOSPC */
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
                for (k = start; k <= i; k++) {
                    bit_set(v->bitmap, k);
                    /* WP30: track metadata allocations separately */
                    if (type == INVFS_ALLOC_META && v->meta_type_bitmap)
                        bit_set(v->meta_type_bitmap, k);
                }
                bm_dirty(v, start);
                bm_dirty(v, i);
                /* WP20b: fresh shadow content invalidates its stripes'
                 * parity (the block's old content was zero-as-absent) */
                if (zone_start == v->sb.shadow_zone_start)
                    seal_dirty_mark(v, start, n);
                *cursor = (i + 1 < zone_end) ? i + 1 : zone_start;
                v->free_blocks -= n;
                *zone_free -= n;
                /* WP30: track metadata free blocks separately */
                if (type == INVFS_ALLOC_META)
                    v->meta_free_blocks -= n;
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

/* WP-DZ: allocate `nblocks` for RAW-class content out of the shared free
 * pool. The zone fields of the superblock are ADVISORY POLICY, not hard
 * regions: the raw extent is the preferred home of raw-class content (and
 * its fair share), and once that share cannot satisfy the request the same
 * allocation simply continues into shadow-space blocks. Either way the
 * content class is RAW -- *zone_out is always INVFS_ZONE_RAW. Placement no
 * longer decides what a block IS, so there is no spill path and no
 * mixed-zone file: the sweep's RAW walk keys on the zone TAG, so every
 * raw-class segment stays sweepable wherever it landed (pre-WP-DZ the
 * spill tagged overflow segments BINARY, which made the sweep skip the
 * file forever). Raw-class blocks in the shadow extent are ordinary
 * occupants there: the seal's pba-range stripes cover them like any other
 * occupied shadow block. The reverse crossing does not exist --
 * shadow-class requests keep their canonical shadow-side placement (the
 * seal's stripes are defined over the shadow extent, and on two-device
 * volumes dev1 is the canonical side); only the RAW class is elastic.
 * The preference costs O(1) when RAW is full: alloc_blocks short-circuits
 * on the per-region free count / fail-run hint before touching the
 * bitmap. */
uint64_t alloc_raw_or_shadow(invfs_volume *v, uint64_t nblocks, int *zone_out)
{
    uint64_t pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks,
                                nblocks, 0, INVFS_ALLOC_DATA);
    if (pba == 0)
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           nblocks, 0, INVFS_ALLOC_DATA);
    if (zone_out) *zone_out = INVFS_ZONE_RAW;
    return pba;
}

/* ==================== WP30: Dynamic Metadata Extents ==================== */

static uint16_t meta_crc16(const invfs_meta_wal *e)
{
    return invfs_crc32c(e, offsetof(invfs_meta_wal, crc)) & 0xFFFFu;
}

int meta_journal_alloc(invfs_volume *v, uint16_t ext_idx, uint64_t pba,
                       uint8_t size_class)
{
    invfs_meta_wal e;
    memset(&e, 0, sizeof e);
    e.type = INVFS_JRN_META_ALLOC;
    e.ext_idx = ext_idx;
    e.pba = pba;
    e.size_class = size_class;
    e.crc = meta_crc16(&e);
    return jrn_push_meta_op(v, &e);
}

int meta_journal_extend(invfs_volume *v, uint16_t ext_idx,
                        uint8_t size_class, uint8_t aux)
{
    invfs_meta_wal e;
    memset(&e, 0, sizeof e);
    e.type = INVFS_JRN_META_EXTEND;
    e.ext_idx = ext_idx;
    e.size_class = size_class;
    e.aux = aux;
    e.crc = meta_crc16(&e);
    return jrn_push_meta_op(v, &e);
}

int meta_journal_shrink(invfs_volume *v, uint16_t ext_idx, uint8_t size_class)
{
    invfs_meta_wal e;
    memset(&e, 0, sizeof e);
    e.type = INVFS_JRN_META_SHRINK;
    e.ext_idx = ext_idx;
    e.size_class = size_class;
    e.crc = meta_crc16(&e);
    return jrn_push_meta_op(v, &e);
}

int meta_journal_free(invfs_volume *v, uint16_t ext_idx)
{
    invfs_meta_wal e;
    memset(&e, 0, sizeof e);
    e.type = INVFS_JRN_META_FREE;
    e.ext_idx = ext_idx;
    e.crc = meta_crc16(&e);
    return jrn_push_meta_op(v, &e);
}

int meta_journal_merge(invfs_volume *v, uint16_t dst_idx, uint16_t src_idx)
{
    invfs_meta_wal e;
    memset(&e, 0, sizeof e);
    e.type = INVFS_JRN_META_MERGE;
    e.ext_idx = dst_idx;
    e.aux = (uint8_t)src_idx;
    e.crc = meta_crc16(&e);
    return jrn_push_meta_op(v, &e);
}

/* WP30: allocate a metadata extent from the free pool.
 * v0.3.0+: mapper table is pre-allocated at mkfs; just find a free slot.
 * size_class: 0=64KB, 1=128KB, 2=256KB... up to 15=2GB
 * Returns mapper entry index (0 on error). */
uint64_t alloc_meta_extent(invfs_volume *v, uint8_t size_class)
{
    uint64_t extent_size = 65536ULL << size_class;
    uint64_t nblocks = extent_size / INVFS_BLOCK_SIZE;
    uint64_t pba;

    if (!v->meta_mapper)
        return 0;  /* Mapper not loaded - should not happen after vol_open */

    /* Check meta reservation: ensure free_blocks won't drop below
     * (meta_reserved_pct of total) after this allocation */
    if (v->sb.meta_reserved_pct > 0) {
        uint64_t meta_reserve = (v->sb.total_blocks * v->sb.meta_reserved_pct) / 100;
        if (v->free_blocks - nblocks < meta_reserve)
            return 0;  /* Would violate meta reservation */
    }

    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       nblocks, 0, INVFS_ALLOC_META);
    if (pba == 0)
        return 0;

    /* Find a free mapper entry */
    if (v->meta_mapper_n >= INVFS_META_EXT_ENTRIES)
        return 0;

    uint64_t idx = 0;
    for (idx = 0; idx < INVFS_META_EXT_ENTRIES; idx++) {
        if (invfs_meta_ext_pba(v->meta_mapper[idx]) == 0)
            break;
    }
    if (idx >= INVFS_META_EXT_ENTRIES)
        return 0;

    v->meta_mapper[idx] = invfs_meta_ext_encode(pba, size_class);
    if (idx >= v->meta_mapper_n)
        v->meta_mapper_n = idx + 1;

    meta_journal_alloc(v, (uint16_t)idx, pba, size_class);
    return idx + 1;  /* 1-based index for callers */
}

/* WP30: try to extend the active extent in-place if physically adjacent
 * run is free. Returns 1 if extended, 0 if not possible, -1 on error. */
int extend_meta_extent(invfs_volume *v, uint64_t extent_idx, uint8_t new_size_class)
{
    uint64_t old_entry, old_pba, old_size;
    uint8_t old_class;
    uint64_t old_blocks, new_blocks;
    uint64_t adj_pba, adj_blocks;

    if (extent_idx >= INVFS_META_EXT_ENTRIES || extent_idx >= v->meta_mapper_n)
        return 0;

    old_entry = v->meta_mapper[extent_idx];
    old_pba = invfs_meta_ext_pba(old_entry);
    old_class = invfs_meta_ext_class(old_entry);
    old_size = invfs_meta_ext_size(old_entry);
    old_blocks = old_size / INVFS_BLOCK_SIZE;

    if (new_size_class <= old_class)
        return 0;  /* Can only extend, not shrink */

    new_blocks = (65536ULL << new_size_class) / INVFS_BLOCK_SIZE;
    if (new_blocks <= old_blocks)
        return 0;

    adj_blocks = new_blocks - old_blocks;
    adj_pba = old_pba + old_blocks;

    if (bit_get(v->bitmap, adj_pba) == 0) {
        uint64_t k;
        int can_extend = 1;
        for (k = 1; k < adj_blocks; k++) {
            if (bit_get(v->bitmap, adj_pba + k) != 0) {
                can_extend = 0;
                break;
            }
        }
        if (can_extend) {
            for (k = 0; k < adj_blocks; k++) {
                bit_set(v->bitmap, adj_pba + k);
                if (v->meta_type_bitmap)
                    bit_set(v->meta_type_bitmap, adj_pba + k);
            }
            bm_dirty(v, old_pba);
            bm_dirty(v, adj_pba + adj_blocks - 1);
            v->free_blocks -= adj_blocks;
            v->meta_free_blocks -= adj_blocks;

            v->meta_mapper[extent_idx] = invfs_meta_ext_encode(old_pba, new_size_class);
            meta_journal_extend(v, (uint16_t)extent_idx, new_size_class, adj_blocks);
            return 1;
        }
    }
    return 0;  /* Adjacent run not free */
}


/* write raw-class data (RAW-preferred over the shared pool, WP-DZ);
 * returns first pba (0 on error) */
uint64_t vol_write_raw(invfs_volume *v, const uint8_t *data, size_t len)
{
    uint64_t nblocks = (len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    uint64_t pba = alloc_raw_or_shadow(v, nblocks, NULL);
    if (pba == 0) return 0;
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, data, (size_t)nblocks * INVFS_BLOCK_SIZE) != 0)
        return 0;
    /* WP25: raw-zone writes dual-write to the dev1 mirror (raw_mirror) */
    if (v->ndev == 2 && v->raw_mirror && !v->degraded &&
        pba >= v->sb.raw_zone_start &&
        pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
        if (wp25_rawm_write(v, pba, data, nblocks) != 0) {
            vol_io_error_latch(v, "raw mirror write (dev1)");
            return 0;
        }
    }
    return pba;
}


/* (inode,lba) key mixer: shared by the WP-L2Q session index and
 * vol_heat.c's per-inode table (identical hash -> identical bucket
 * choice). */
static uint64_t mapset_hash(uint64_t inode, uint64_t lba)
{
    uint64_t h = inode * 0x9E3779B97F4A7C15ull ^ lba;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h;
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

    /* In-memory only until the flush. vol_flush is the single writer of
     * the journal: it appends the queued ops (jops) at the active slot's
     * log end, never rewriting durable entries (WP22d).
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
     * The capacity bound is the slot payload (slotted) or the whole area
     * (legacy, pre-migration): the compacted image must always fit one
     * slot. */
    {
        uint64_t room = v->j_slotted
            ? (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1) * INVFS_BLOCK_SIZE
            : (uint64_t)JOURNAL_BLOCKS * INVFS_BLOCK_SIZE;
        if ((uint64_t)(v->l2p_count + 2) * sizeof(e) > room) {
            fprintf(stderr, "L2P journal full\n");
            return -1;
        }
    }

    /* in-memory */
    if (v->l2p_count == v->l2p_cap) {
        v->l2p_cap = v->l2p_cap ? v->l2p_cap * 2 : 256;
        v->l2p = (invfs_l2p_entry *)realloc(v->l2p, v->l2p_cap * sizeof(invfs_l2p_entry));
        if (!v->l2p) return -1;
    }
    v->l2p[v->l2p_count++] = e;
    if (jrn_push_op(v, &e) != 0) { v->l2p_count--; return -1; }
    /* WP-L2Q: the index tracks the append (a re-MAP of the same key
     * overwrites -- newest wins) */
    l2p_idx_put(v, inode, lba, (uint64_t)(v->l2p_count - 1));
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
    /* WP-L2Q: the session index answers O(1). A miss is authoritative
     * (the index mirrors the table); a hit is cross-checked against the
     * table and any inconsistency falls through to the scan below. */
    if (v->l2p_idx) {
        size_t k = (size_t)mapset_hash(inode, lba) & v->l2p_idx_mask;
        while (v->l2p_idx[k].inode) {
            if (v->l2p_idx[k].inode == inode && v->l2p_idx[k].lba == lba) {
                uint64_t slot = v->l2p_idx[k].slot;
                if (slot < v->l2p_count) {
                    const invfs_l2p_entry *e = &v->l2p[slot];
                    if (e->type == INVFS_JRN_MAP && e->inode == inode &&
                        e->lba == lba) {
                        *pba_out = e->pba;
                        *len_out = e->length;
                        return 0;
                    }
                }
                break;   /* diverged: scan (cannot happen) */
            }
            k = (k + 1) & v->l2p_idx_mask;
        }
        if (!v->l2p_idx[k].inode)
            return -1;
    }
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
void l2p_remove_mem(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    size_t i, w = 0;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba)
            continue;  /* drop */
        if (w != i) {
            v->l2p[w] = v->l2p[i];
            /* the moved entry may be its key's newest: fix the index */
            l2p_idx_reslot(v, e->inode, e->lba, (uint64_t)i, (uint64_t)w);
        }
        w++;
    }
    v->l2p_count = w;
    l2p_idx_del(v, inode, lba);   /* every occurrence of the key died */
}


/* In-memory compaction + the UNMAP op for the journal. The on-disk
 * journal is append-only (WP22d): the unmap is queued, the old MAP entry
 * is never rewritten. Replay folds MAP-then-UNMAP to the same view. */
void l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    uint64_t pba, len;
    if (vol_lookup_entry(v, inode, lba, &pba, &len) == 0) {
        invfs_l2p_entry e;
        memset(&e, 0, sizeof e);
        e.type = INVFS_JRN_UNMAP;
        e.inode = inode;
        e.lba = lba;
        jrn_push_op(v, &e);   /* OOM: the op is lost; the stale MAP is an
                               * orphan the next fsck reclaims -- degraded,
                               * never corrupt */
    }
    l2p_remove_mem(v, inode, lba);
}


/* public wrapper (used by dedupe pass) */
void vol_l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    l2p_remove(v, inode, lba);
}


/* ---- WP22d: consistent-cut machinery ------------------------------------
 * WP27: file records carry their segments' pbas, so "the record is the
 * map": a live record can never name an unmapped segment any more. The
 * cut's per-version "broken" test degrades to an entry-pba sanity check
 * (rec_pba_miss); drop-torn DATA is content-level (segment CRC at read
 * time; fsck's INVFS_FSCK_CONTENT pass) exactly as it always was.
 */

/* Count a record's AST entries whose pba is invalid (0, or past the volume
 * end); the first few lost ranges are collected for the loud log
 * (miss_off/miss_len, up to *miss_n... in: capacity is 4, out: stored
 * count). An entry with length 0 never references data. */
uint64_t rec_pba_miss(const uint8_t *rec, uint32_t rec_len,
                      uint64_t total_blocks, uint64_t *miss_off,
                      uint64_t *miss_len, unsigned *miss_n)
{
    invfs_ast_hdr ah;
    size_t off;
    uint32_t i;
    uint64_t miss = 0;

    *miss_n = 0;
    off = sizeof(invfs_inode_rec);
    if (off + INVFS_AST_HDR_V1_LEN > rec_len ||
        invfs_ast_hdr_parse(rec + off, rec_len - off, &ah) != 0)
        return 1;   /* unparseable header: treat as broken */
    off += ah.hdr_len;
    for (i = 0; i < ah.num_blocks; i++) {
        invfs_ast_block_entry e;
        if (off + sizeof(e) > rec_len) { miss++; break; }
        memcpy(&e, rec + off, sizeof(e));
        off += sizeof(e);
        if (e.length == 0) continue;
        if (e.pba == 0 || e.pba >= total_blocks) {
            if (*miss_n < 4) {
                miss_off[*miss_n] = e.file_offset;
                miss_len[*miss_n] = e.length;
                (*miss_n)++;
            }
            miss++;
        }
    }
    return miss;
}


/* ---- WP-L2Q: session-persistent L2P index ------------------------------
 * Open addressing over (inode,lba) -> table slot (the mapset hash, but
 * slot numbers instead of pointers: v->l2p reallocs on growth). The index
 * is a pure cache of the newest-wins scan's answer: every table mutation
 * keeps it in lock-step, so vol_lookup_entry answers O(1) and the scan
 * remains only as the fallback for a disabled index. */

/* 0 = on (default). A disabled index frees immediately and every hook
 * becomes a no-op, so a half-maintained index can never be consulted. */
static int l2p_idx_enabled(void)
{
    const char *e = getenv("INVFS_L2P_IDX");
    return !e || strcmp(e, "0") != 0;
}

static void l2p_idx_disable(invfs_volume *v)
{
    free(v->l2p_idx);
    v->l2p_idx = NULL;
    v->l2p_idx_mask = v->l2p_idx_n = 0;
}

void l2p_idx_reset(invfs_volume *v)
{
    if (!v->l2p_idx) return;
    memset(v->l2p_idx, 0, (v->l2p_idx_mask + 1) * sizeof *v->l2p_idx);
    v->l2p_idx_n = 0;
}

/* grow to 2x and rehash; on allocation failure the index disables itself
 * (the caller's table mutation already happened -- the scan fallback
 * keeps answering correctly) */
static void l2p_idx_grow(invfs_volume *v)
{
    size_t ncap = (v->l2p_idx_mask + 1) * 2, i;
    l2p_idx_ent *nt = (l2p_idx_ent *)calloc(ncap, sizeof *nt);
    if (!nt) { l2p_idx_disable(v); return; }
    for (i = 0; i <= v->l2p_idx_mask; i++) {
        if (v->l2p_idx[i].inode) {
            size_t k = (size_t)mapset_hash(v->l2p_idx[i].inode,
                                           v->l2p_idx[i].lba) & (ncap - 1);
            while (nt[k].inode) k = (k + 1) & (ncap - 1);
            nt[k] = v->l2p_idx[i];
        }
    }
    free(v->l2p_idx);
    v->l2p_idx = nt;
    v->l2p_idx_mask = ncap - 1;
}

void l2p_idx_put(invfs_volume *v, uint64_t inode, uint64_t lba,
                 uint64_t slot)
{
    size_t mask, k;
    if (!v->l2p_idx) {
        size_t cap = 1024;
        if (!l2p_idx_enabled()) return;
        v->l2p_idx = (l2p_idx_ent *)calloc(cap, sizeof *v->l2p_idx);
        if (!v->l2p_idx) return;
        v->l2p_idx_mask = cap - 1;
        v->l2p_idx_n = 0;
    }
    if ((v->l2p_idx_n + 1) * 10 >= (v->l2p_idx_mask + 1) * 7) {
        l2p_idx_grow(v);
        if (!v->l2p_idx) return;   /* disabled mid-flight */
    }
    mask = v->l2p_idx_mask;
    k = (size_t)mapset_hash(inode, lba) & mask;
    while (v->l2p_idx[k].inode) {
        if (v->l2p_idx[k].inode == inode && v->l2p_idx[k].lba == lba) {
            v->l2p_idx[k].slot = slot;   /* a newer MAP supersedes */
            return;
        }
        k = (k + 1) & mask;
    }
    v->l2p_idx[k].inode = inode;
    v->l2p_idx[k].lba = lba;
    v->l2p_idx[k].slot = slot;
    v->l2p_idx_n++;
}

void l2p_idx_del(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    size_t mask, k, j;
    if (!v->l2p_idx) return;
    mask = v->l2p_idx_mask;
    k = (size_t)mapset_hash(inode, lba) & mask;
    while (v->l2p_idx[k].inode) {
        if (v->l2p_idx[k].inode == inode && v->l2p_idx[k].lba == lba)
            break;
        k = (k + 1) & mask;
    }
    if (!v->l2p_idx[k].inode) return;   /* not present */
    /* Reinsert the rest of the cluster one past the hole (deletion by
     * rehash -- no tombstones, lookups never pass a stale entry). */
    v->l2p_idx[k].inode = 0;
    v->l2p_idx_n--;
    j = (k + 1) & mask;
    while (v->l2p_idx[j].inode) {
        l2p_idx_ent e = v->l2p_idx[j];
        v->l2p_idx[j].inode = 0;
        v->l2p_idx_n--;
        k = (size_t)mapset_hash(e.inode, e.lba) & mask;
        while (v->l2p_idx[k].inode) k = (k + 1) & mask;
        v->l2p_idx[k] = e;
        v->l2p_idx_n++;
        j = (j + 1) & mask;
    }
}

void l2p_idx_reslot(invfs_volume *v, uint64_t inode, uint64_t lba,
                    uint64_t old_slot, uint64_t new_slot)
{
    size_t mask, k;
    if (!v->l2p_idx) return;
    mask = v->l2p_idx_mask;
    k = (size_t)mapset_hash(inode, lba) & mask;
    while (v->l2p_idx[k].inode) {
        if (v->l2p_idx[k].inode == inode && v->l2p_idx[k].lba == lba) {
            if (v->l2p_idx[k].slot == old_slot)
                v->l2p_idx[k].slot = new_slot;
            return;
        }
        k = (k + 1) & mask;
    }
}

void l2p_idx_rebuild(invfs_volume *v)
{
    size_t i;
    l2p_idx_reset(v);
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP)
            l2p_idx_put(v, e->inode, e->lba, (uint64_t)i);
        if (!v->l2p_idx) return;   /* disabled mid-rebuild */
    }
}

const invfs_l2p_entry *l2p_idx_get(invfs_volume *v, uint64_t inode,
                                   uint64_t lba)
{
    size_t mask, k;
    if (!v->l2p_idx) return NULL;
    mask = v->l2p_idx_mask;
    k = (size_t)mapset_hash(inode, lba) & mask;
    while (v->l2p_idx[k].inode) {
        if (v->l2p_idx[k].inode == inode && v->l2p_idx[k].lba == lba) {
            uint64_t slot = v->l2p_idx[k].slot;
            const invfs_l2p_entry *e;
            if (slot >= v->l2p_count) return NULL;   /* cannot happen */
            e = &v->l2p[slot];
            /* defensive cross-check: the index must name a live MAP for
             * exactly this key; anything else is a bug, answered via the
             * scan fallback by the caller treating this as "unknown" */
            if (e->type != INVFS_JRN_MAP || e->inode != inode ||
                e->lba != lba)
                return NULL;
            return e;
        }
        k = (k + 1) & mask;
    }
    return NULL;
}


static scan_name *scs_find(const scan_set *ss, const char *name, size_t nlen)
{
    size_t b = (size_t)(idx_hash(name, nlen) & ss->mask);
    scan_name *e;
    for (e = ss->buck[b]; e; e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0)
            return e;
    return NULL;
}

static void scs_grow(scan_set *ss)
{
    size_t ncap = (ss->mask + 1) * 2, i;
    scan_name **nb = (scan_name **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= ss->mask; i++) {
        scan_name *e = ss->buck[i];
        while (e) {
            scan_name *nx = e->next;
            size_t b = (size_t)(idx_hash(e->name, e->nlen) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(ss->buck);
    ss->buck = nb;
    ss->mask = ncap - 1;
}

int scanset_inod(scan_set *ss, const char *name, size_t nlen,
                 uint64_t id, uint64_t pos, uint64_t size, uint64_t ctime,
                 uint64_t miss)
{
    scan_name *e;
    scan_ver *sv;
    size_t b;
    if (nlen == 0 || nlen > 255) return 0;
    if (!ss->buck) {
        ss->buck = (scan_name **)calloc(1024, sizeof *ss->buck);
        if (!ss->buck) return -1;
        ss->mask = 1023;
    }
    e = scs_find(ss, name, nlen);
    if (!e) {
        e = (scan_name *)malloc(sizeof *e + nlen);
        if (!e) return -1;
        memcpy(e->name, name, nlen);
        e->name[nlen] = 0;
        e->nlen = (uint32_t)nlen;
        e->vers = NULL;
        e->nvers = e->capvers = 0;
        b = (size_t)(idx_hash(name, nlen) & ss->mask);
        e->next = ss->buck[b];
        ss->buck[b] = e;
        ss->count++;
        if (ss->count > ss->mask + 1) scs_grow(ss);
    }
    if (e->nvers == e->capvers) {
        uint32_t ncap = e->capvers ? e->capvers * 2 : 4;
        scan_ver *nv = (scan_ver *)realloc(e->vers, ncap * sizeof *nv);
        if (!nv) return -1;
        e->vers = nv;
        e->capvers = ncap;
    }
    sv = &e->vers[e->nvers++];
    sv->pos = pos;
    sv->id = id;
    sv->size = size;
    sv->ctime = ctime;
    sv->miss = miss;
    sv->broken = miss != 0;
    return 0;
}

void scanset_delt(scan_set *ss, const char *name, size_t nlen,
                  uint64_t id, uint64_t killpos)
{
    scan_name *e;
    if (!ss->buck || nlen == 0 || nlen > 255) return;
    e = scs_find(ss, name, nlen);
    if (!e || !e->nvers) return;
    if (killpos == 0) {
        /* legacy kill-by-id. A match on the NEWEST version is a name
         * delete (idx_del semantics): clear every version -- older ones
         * are not resurrected (a delete that was acknowledged stays
         * acknowledged). A match on an OLDER version is a retire of that
         * superseded version: drop exactly it -- with the same
         * torn-retire guard as the v2 kill. The guard keeps the target
         * only when the target is HEALTHY and its successor is broken
         * (the retire transaction tore): a broken target is unreadable
         * either way, so letting it die is what makes a quarantine chain
         * converge instead of resurrecting a useless record forever. */
        uint32_t i;
        if (e->vers[e->nvers - 1].id == id) { e->nvers = 0; return; }
        for (i = 0; i < e->nvers; i++) {
            if (e->vers[i].id == id) {
                if (i + 1 < e->nvers && e->vers[i + 1].broken &&
                    !e->vers[i].broken)
                    return;
                memmove(e->vers + i, e->vers + i + 1,
                        (e->nvers - i - 1) * sizeof *e->vers);
                e->nvers--;
                return;
            }
        }
        return;
    }
    /* v2 position kill: drop exactly that version -- UNLESS the target is
     * healthy and its successor is broken: then the kill belongs to a
     * retire transaction whose replacement record lost its mappings (a
     * torn sweep/write), and dropping the healthy fallback would present
     * the name as deleted while its replacement is unreadable. A BROKEN
     * target is no fallback -- it is unreadable either way -- so the kill
     * lands: that is what lets a quarantine chain (fsck -f) converge
     * instead of resurrecting a useless record on every scan. */
    {
        uint32_t i;
        for (i = 0; i < e->nvers; i++) {
            if (e->vers[i].pos == killpos) {
                if (i + 1 < e->nvers && e->vers[i + 1].broken &&
                    !e->vers[i].broken)
                    return;
                memmove(e->vers + i, e->vers + i + 1,
                        (e->nvers - i - 1) * sizeof *e->vers);
                e->nvers--;
                return;
            }
        }
    }
}

const scan_ver *scanset_live(const scan_name *e)
{
    uint32_t i;
    for (i = e->nvers; i-- > 0; )
        if (!e->vers[i].broken)
            return &e->vers[i];
    return NULL;
}

void scanset_free(scan_set *ss)
{
    size_t i;
    if (!ss->buck) return;
    for (i = 0; i <= ss->mask; i++) {
        scan_name *e = ss->buck[i];
        while (e) {
            scan_name *nx = e->next;
            free(e->vers);
            free(e);
            e = nx;
        }
    }
    free(ss->buck);
    ss->buck = NULL;
}


/* read one block worth of raw bytes at pba */
int vol_read_block(invfs_volume *v, uint64_t pba, void *buf)
{
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, INVFS_BLOCK_SIZE) != 0) {
        /* WP25: a raw-zone block on an absent/failed dev0 is served by
         * its dev1 mirror block (mirror = 1:1 block copy) */
        uint64_t mpba = 0, mlen = 0;
        if (v->ndev == 2 &&
            pba >= v->sb.raw_zone_start &&
            pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks &&
            wp25_rawm_lookup(v, pba, &mpba, &mlen) == 0 && mlen >= 1) {
            if (io_seek(&v->io, mpba * INVFS_BLOCK_SIZE) == 0 &&
                io_read(&v->io, buf, INVFS_BLOCK_SIZE) == 0)
                return 0;
        }
        return -1;
    }
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
        io_write(&v->io, buf, span) != 0) {
        /* WP25 rule 3: with raw_mirror the segment's second copy lands on
         * dev1; a dev0 RAW write failure is logged and the mirror becomes
         * the surviving copy (reads fail over to it) -- NOT a full latch.
         * A dev1 (mirror) write failure latches. */
        if (v->ndev == 2 && v->raw_mirror && !v->degraded &&
            pba >= v->sb.raw_zone_start &&
            pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
            if (!v->rawio_logged) {
                v->rawio_logged = 1;
                fprintf(stderr, "vol: dev0 RAW write failed at pba %llu; "
                        "continuing on the dev1 mirror (raw_mirror=1)\n",
                        (unsigned long long)pba);
            }
            if (wp25_rawm_write(v, pba, buf, phys_blocks) != 0) {
                vol_io_error_latch(v, "raw mirror write (dev1)");
                return -1;
            }
            return 0;
        }
        return -1;
    }
    /* WP25: the RAW mirror -- every raw-zone segment is dual-written to
     * dev1 (SSD loss loses nothing). The mirror is an ordinary canonical
     * shadow allocation owned by "\x01rawm". */
    if (v->ndev == 2 && v->raw_mirror && !v->degraded &&
        pba >= v->sb.raw_zone_start &&
        pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
        if (wp25_rawm_write(v, pba, buf, phys_blocks) != 0) {
            vol_io_error_latch(v, "raw mirror write (dev1)");
            return -1;
        }
    }
    /* WP20b: overwriting occupied shadow blocks dirties their stripes */
    seal_dirty_mark(v, pba, phys_blocks);
    return 0;
}


/* ---- WP27: segment extents --------------------------------------------
 * The 32-byte AST entry carries the pba but no physical block count (the
 * wire format has no room); a framed segment's extent derives from its own
 * 8-byte header: [4B csize LE][4B crc32c] at pba, plen = ceil((csize+8)/
 * 4096). csize == 0 never names a written segment (an empty file has no
 * entries; blob creators refuse empty blobs), so it reads as "unknown". */
int seg_extent(invfs_volume *v, uint64_t pba, uint32_t *csize_out,
               uint64_t *plen_out)
{
    uint8_t hdr[8];
    uint32_t csize;
    uint64_t plen;
    if (!pba || pba >= v->sb.total_blocks) return -1;
    if (io_seek(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, hdr, 8) != 0)
        return -1;
    memcpy(&csize, hdr, 4);
    if (!csize) return -1;
    plen = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    if (plen > v->sb.total_blocks - pba) return -1;
    if (csize_out) *csize_out = csize;
    if (plen_out) *plen_out = plen;
    return 0;
}


/* The destructive-free gate: derive the extent AND prove the whole derived
 * run is still marked allocated in the in-memory bitmap (a segment's
 * blocks are one contiguous exclusively-owned run). A torn header almost
 * certainly fails one of the two checks; the failure mode is a leak
 * (fsck reclaims), never an over-free. */
int seg_extent_checked(invfs_volume *v, uint64_t pba, uint64_t *plen_out)
{
    uint64_t plen = 0, b;
    if (seg_extent(v, pba, NULL, &plen) != 0)
        return -1;
    for (b = pba; b < pba + plen; b++)
        if (!bit_get(v->bitmap, b))
            return -1;
    *plen_out = plen;
    return 0;
}


/* ---- WP27: pba reference map (PB7 without the L2P) --------------------
 * Counts, per segment pba, how many LIVE records' AST entries name it
 * (dedupe shares, WP4b session aliases, hardlink twin records). Only
 * non-owner records' non-TEXT entries count: owner records ("\x01...")
 * free through the owner WAL, and TEXT member entries name owner-owned
 * batch blocks the member's retire must never free (the WP10 §7 gate). */
static uint64_t pba_ref_hash(uint64_t pba)
{
    pba ^= pba >> 30; pba *= 0xbf58476d1ce4e5b9ULL;
    pba ^= pba >> 27; pba *= 0x94d049bb133111ebULL;
    return pba ^ (pba >> 31);
}

static void pba_ref_free(invfs_volume *v)
{
    free(v->pba_ref);
    v->pba_ref = NULL;
    v->pba_ref_mask = 0;
    v->pba_ref_on = 0;
}

/* drop the map (a path that rewrote records without the apply hooks --
 * the in-place sweep fallback, the fsck rebuild); the next retire/dedupe
 * rebuilds it lazily from the live set */
void pba_ref_reset(invfs_volume *v)
{
    pba_ref_free(v);
}

/* grow to 2x and rehash; failure drops the whole map (the callers' free
 * decisions then run WITHOUT sharing knowledge -- the pre-PB7 leak
 * direction, never corruption: a refcount can only be lost, and a count
 * that reads 0 for a shared pba would over-free, so the map DEGRADES TO
 * "never free" instead: pba_ref_count returns 2 on a missing map). */
static int pba_ref_grow(invfs_volume *v)
{
    size_t ncap = (v->pba_ref_mask + 1) * 2, i;
    pba_ref_ent *nt = (pba_ref_ent *)calloc(ncap, sizeof *nt);
    if (!nt) { pba_ref_free(v); return -1; }
    for (i = 0; i <= v->pba_ref_mask; i++) {
        if (v->pba_ref[i].pba) {
            size_t k = (size_t)pba_ref_hash(v->pba_ref[i].pba) & (ncap - 1);
            while (nt[k].pba) k = (k + 1) & (ncap - 1);
            nt[k] = v->pba_ref[i];
        }
    }
    free(v->pba_ref);
    v->pba_ref = nt;
    v->pba_ref_mask = ncap - 1;
    return 0;
}

/* delta = +1 (record appended) / -1 (record killed). Owner records and
 * TEXT entries are skipped (see the section comment). Decrement floors at
 * 0: a record absent from the build (dead before the map existed) must not
 * drive counts negative. */
void pba_ref_apply(invfs_volume *v, const uint8_t *rec, uint32_t rec_len,
                   int delta)
{
    invfs_ast_hdr ah;
    const invfs_inode_rec *rh;
    size_t base = sizeof(invfs_inode_rec), off;
    uint32_t i;
    size_t ncount = 0;

    if (!v->pba_ref_on || !v->pba_ref) return;
    if (rec_len < base + INVFS_AST_HDR_V1_LEN) return;
    rh = (const invfs_inode_rec *)rec;
    if (rh->magic != INODE_REC_MAGIC) return;
    if (rh->name_len && rh->name[0] == 0x01) return;   /* owner: WAL-owned */
    if (invfs_ast_hdr_parse(rec + base, rec_len - base, &ah) != 0) return;
    off = base + ah.hdr_len;
    if ((size_t)ah.num_blocks * sizeof(invfs_ast_block_entry) >
        rec_len - off)
        return;
    for (i = 0; i < ah.num_blocks; i++) {
        const invfs_ast_block_entry *e = (const invfs_ast_block_entry *)
            (rec + off + (size_t)i * sizeof(*e));
        size_t k;
        if (e->zone == INVFS_ZONE_TEXT || !e->pba) continue;
        if (e->pba >= v->sb.total_blocks) continue;   /* invalid: never
                                                         * counted */
        if (v->pba_ref_n * 10 >= (v->pba_ref_mask + 1) * 7 &&
            pba_ref_grow(v) != 0)
            return;
        k = (size_t)pba_ref_hash(e->pba) & v->pba_ref_mask;
        while (v->pba_ref[k].pba && v->pba_ref[k].pba != e->pba)
            k = (k + 1) & v->pba_ref_mask;
        if (v->pba_ref[k].pba) {
            if (delta > 0) v->pba_ref[k].n++;
            else if (v->pba_ref[k].n) v->pba_ref[k].n--;
        } else if (delta > 0) {
            v->pba_ref[k].pba = e->pba;
            v->pba_ref[k].n = 1;
            v->pba_ref_n++;
            ncount++;
        }
    }
    (void)ncount;
}

uint32_t pba_ref_count(invfs_volume *v, uint64_t pba)
{
    size_t k;
    if (!v->pba_ref_on || !v->pba_ref) return 2;   /* unknown: never free */
    k = (size_t)pba_ref_hash(pba) & v->pba_ref_mask;
    while (v->pba_ref[k].pba) {
        if (v->pba_ref[k].pba == pba) return v->pba_ref[k].n;
        k = (k + 1) & v->pba_ref_mask;
    }
    return 0;
}

/* Build the map from the live name-index set (one read per live record).
 * Idempotent. Runs lazily on the first retire/dedupe of a session; the
 * open path does not pay for it. */
int pba_ref_ensure(invfs_volume *v)
{
    size_t b;
    if (v->pba_ref_on) return 0;
    v->pba_ref_mask = 1023;
    v->pba_ref = (pba_ref_ent *)calloc(v->pba_ref_mask + 1,
                                     sizeof *v->pba_ref);
    if (!v->pba_ref) { v->pba_ref_mask = 0; return -1; }
    v->pba_ref_n = 0;
    v->pba_ref_on = 1;
    for (b = 0; b <= v->nmask; b++) {
        const name_index_entry *ne;
        for (ne = v->nbuck[b]; ne; ne = ne->next) {
            uint8_t *rec = NULL;
            uint32_t rl = 0;
            if (!ne->nlen || ne->name[ne->nlen - 1] == '/') continue;
            if (!ne->nlen || ne->name[0] == 0x01) continue;   /* owners */
            if (meta_read_record_by_id(v, ne->inode_id, &rec, &rl,
                                       NULL, 0, NULL) != 0)
                continue;
            pba_ref_apply(v, rec, rl, +1);
            free(rec);
        }
    }
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
    /* WP25: a degraded mount (dev0 absent) is read-only by construction */
    if (v->degraded) return 0;
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
    /* WP21: while a sweep checkpoint (CKP0) is live, NOTHING is freed. The
     * blocks stay allocated in the bitmap (that is what bars their reuse
     * for the rest of the checkpoint's life -- the rollback fidelity
     * guarantee) and are recorded in retmap, the realize-time registry
     * list. Content does not change, so seal stripes stay valid and the
     * free counters stay honest (the blocks are NOT free). Idempotent per
     * block, so the PB7 shared-pba cases mark twice without consequence.
     *
     * Retention keys on the ON-DISK state (ck_present), not on this
     * session having armed the checkpoint (v->retain): the arming sweep
     * exits with the checkpoint still live, and a later process' frees
     * (a FUSE write's retire path, a delete, an unseal's parity release)
     * would otherwise free PRE-checkpoint blocks for real and let a
     * post-checkpoint allocation reuse them -- invf-rollback then
     * resurrects the pre-sweep record over somebody else's bytes (F4, the
     * leg-5 soak's THIRD STATE). A NULL retmap (any process that did not
     * arm) degrades registration to "stays allocated, unregistered": the
     * rollback rebuild still keeps the resurrected references live, and
     * the post-resolution fsck reclaims what nothing references.
     * retain_release exempts the checkpoint machinery's own deliberate
     * frees (realize / arm unwind / no-op disarm). */
    if ((v->retain || v->ck_present) && !v->retain_release) {
        if (v->retmap)
            for (i = pba; i < end; i++)
                bit_set(v->retmap, i);
        return;
    }
    /* WP25: a REAL free (retention above holds its blocks) drops the
     * redundant second copy: the dev1 mirror of a raw-zone run, or the
     * dev0 acceleration copy of a canonical dev1-shadow run. */
    if (v->ndev == 2)
        wp25_on_free(v, pba, end - pba);
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
    } else if (v->ndev == 2 && pba >= v->dev0_blocks &&
               pba < v->sb.shadow_zone_start) {
        /* WP25 reserved dev1 span (metadata mirror): never allocated,
         * never freed -- defensive classification only */
    } else if (v->ndev == 2 && pba >= v->arena_start &&
               pba < v->dev0_blocks) {
        v->arena_free += nblocks;
        v->arena_fail_run = 0;
        if (pba < v->arena_cursor) v->arena_cursor = pba;
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

int vol_zone_free(invfs_volume *v, uint64_t *raw_free, uint64_t *raw_total,
                  uint64_t *shadow_free, uint64_t *shadow_total)
{
    if (!v) return -1;
    if (raw_free)    *raw_free = v->raw_free;
    if (raw_total)   *raw_total = v->sb.raw_zone_blocks;
    if (shadow_free) *shadow_free = v->shadow_free;
    if (shadow_total) *shadow_total = v->sb.shadow_zone_blocks;
    return 0;
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
    /* WP25: the dev0 tier arena (0 blocks on single-device volumes) */
    v->arena_cursor = v->arena_start;
    v->arena_free = v->arena_blocks
                  ? zone_count_free(v, v->arena_start, v->arena_blocks) : 0;
    v->arena_fail_run = 0;
    v->raw_fail_run = v->shadow_fail_run = 0;
    v->bm_lo = 1; v->bm_hi = 0;   /* on-disk bitmap matches memory */
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[alloc_state_reset] raw_free=%llu/%llu shadow_free=%llu/%llu\n",
                (unsigned long long)v->raw_free, (unsigned long long)v->sb.raw_zone_blocks,
                (unsigned long long)v->shadow_free, (unsigned long long)v->sb.shadow_zone_blocks);
    if (getenv("INVFS_DEBUG_FILE")) {
        FILE *df = fopen(getenv("INVFS_DEBUG_FILE"), "a");
        if (df) { fprintf(df, "[alloc_state_reset] raw_free=%llu/%llu shadow_free=%llu/%llu\n",
                    (unsigned long long)v->raw_free, (unsigned long long)v->sb.raw_zone_blocks,
                    (unsigned long long)v->shadow_free, (unsigned long long)v->sb.shadow_zone_blocks);
                  fclose(df); }
    }
}
