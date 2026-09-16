/* vol_dirs.c — virtual directories (prefix-based), rename/move,
 * hard links, replace/unlink. Split from volume.c. */

#include "volume_internal.h"


/* ---- virtual directories (prefix-based; mkdir creates an empty anchor
   file named "dir/"; nested files "dir/x" make dir visible) ---- */

/* does a directory exist? anchor "name/" or any file under "name/" */
int vol_is_dir(invfs_volume *v, const char *name)
{
    char pre[300];
    int plen;
    if (name[0] == 0) return 1;   /* root always exists */
    plen = snprintf(pre, sizeof pre, "%s/", name);
    if (plen <= 0 || (size_t)plen >= sizeof pre) return 0;
    /* nonzero live count under "name/" -- the anchor record counts too, so
       an empty directory stays visible, and a tombstoned one does not */
    return idx_dir_live(v, pre, (size_t)plen);
}


/* create empty directory anchor "name/" */
uint64_t vol_mkdir(invfs_volume *v, const char *name)
{
    char anchor[300];
    if (v->sb.vol_flags & VOLF_READONLY) return 0;   /* EROFS */
    if (!name || name[0] == 0 || strlen(name) > 240) return 0;
    if (vol_find(v, name) != 0) return 0;   /* plain file with same name */
    snprintf(anchor, sizeof anchor, "%s/", name);
    return vol_create_file(v, anchor, NULL, 0);
}


/* remove directory: must be empty (only the anchor, no files inside) */
int vol_rmdir(invfs_volume *v, const char *name)
{
    char anchor[300];
    int alen;
    /* H5: NOT gated on VOLF_READONLY -- a delete only frees space, and the
     * free-space latch must not weld its own exit shut. needs_recovery
     * (the true "never mutate" state) is still enforced via vol_mark_dirty
     * in the delete below. */
    if (v->needs_recovery) return -1;
    if (!name || name[0] == 0) return -1;
    alen = snprintf(anchor, sizeof anchor, "%s/", name);
    if (alen <= 0 || (size_t)alen >= sizeof anchor) return -1;
    /* Live records under "name/", minus the anchor itself, are the
       directory's contents. Counting through the index also fixes the old
       scan, which accepted records already killed by a later tombstone and
       so kept an emptied directory ENOTEMPTY forever. */
    if (idx_dir_count(v, anchor, (size_t)alen) >
        (idx_get(v, anchor, (size_t)alen) ? 1u : 0u)) {
        /* Ghost tolerance: unlink-while-open (.fuse_hidden dance) can
         * leave children whose index entry outlived its record. If every
         * listed child resolves to no live name index entry, the dir is
         * de-facto empty -- scrub and remove instead of ENOTEMPTY. */
        int listed = 0, alive = 0;
        char child[600];
        invfs_dirent ents[64];
        int n = vol_list_dir(v, name, ents, 64);
        int k;
        for (k = 0; k < n; k++) {
            if (ents[k].name[0] == 0) continue;
            snprintf(child, sizeof child, "%s/%s", name, ents[k].name);
            listed++;
            if (vol_find(v, child) != 0 || vol_is_dir(v, child)) alive++;
        }
        if (alive == 0 && listed > 0 && n <= 64) {
            for (k = 0; k < n; k++) {
                if (ents[k].name[0] == 0) continue;
                snprintf(child, sizeof child, "%s/%s", name, ents[k].name);
                idx_del(v, child, (size_t)strlen(child), 0);
                idx_bump_dirs(v, child, strlen(child), -1);
            }
            v->dirty = 1;
        } else {
            return -2;   /* genuinely not empty */
        }
    }
    return vol_delete_file(v, anchor);
}


/* auto-create parent anchors for a path: "a/b/c.txt" -> "a/", "a/b/" */
int vol_ensure_path(invfs_volume *v, const char *name)
{
    char tmp[256];
    size_t n = strlen(name);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, name, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (tmp[0] && !vol_is_dir(v, tmp))
                vol_mkdir(v, tmp);
            tmp[i] = '/';
        }
    }
    return 0;
}


static int dirent_cmp(const void *a, const void *b)
{
    return strcmp(((const invfs_dirent *)a)->name,
                  ((const invfs_dirent *)b)->name);
}


/* list one directory level: first path component after "dir/" */
int vol_list_dir(invfs_volume *v, const char *dir, invfs_dirent *ents, int max)
{
    char pre[300];
    size_t pren;
    size_t n = 0, b;
    /* Dedup set over the entries collected so far. The old code compared each
       candidate against every entry already found, which is O(n^2) on top of
       the scan; a directory of 1600 files cost ~1.3M strcmp per listing.
       `at` is the slot in `ents`: a subdirectory is usually discovered from a
       file inside it, and its own anchor record -- the one holding its ctime
       -- may only turn up later in the walk. */
    struct dedup { struct dedup *next; size_t at; int is_dir; char name[1]; };
    struct dedup **seen = NULL;
    size_t seen_mask = 0;

    if (dir[0] == 0) { pre[0] = 0; pren = 0; }
    else {
        int pl = snprintf(pre, sizeof pre, "%s/", dir);
        if (pl <= 0 || (size_t)pl >= sizeof pre) return 0;
        pren = (size_t)pl;
    }

    /* Walk the in-memory name index instead of re-reading the inode area.
       Windows enumerates a directory after every file creation, so this path
       ran once per created file and read every record on the volume each
       time: it was the entire cost of writing a file (10.4 ms of a 10.7 ms
       write at 1600 files), and it grew linearly with the file count.
       The index holds exactly the live names -- last-record-wins and the
       tombstone rules are applied as it is maintained -- so the separate
       tombstone post-pass that used to follow is gone too. */
    if (!v->nbuck) return 0;
    {
        size_t sc = 64;
        while (sc < v->ncount) sc *= 2;
        seen = (struct dedup **)calloc(sc, sizeof *seen);
        if (!seen) return -1;
        seen_mask = sc - 1;
    }

    for (b = 0; b <= v->nmask && n < (size_t)max; b++) {
        const name_index_entry *e;
        for (e = v->nbuck[b]; e && n < (size_t)max; e = e->next) {
            const char *rest, *slash;
            size_t nl = e->nlen, rl, flen, hb;
            char first[256];
            int is_dir;
            struct dedup *d;

            if (nl <= pren || memcmp(e->name, pre, pren) != 0) continue;
            rest = e->name + pren;
            rl = nl - pren;
            slash = (const char *)memchr(rest, '/', rl);
            if (slash) { flen = (size_t)(slash - rest); is_dir = 1; }
            else { flen = rl; is_dir = 0; }
            if (flen == 0 || flen >= sizeof first) continue;   /* "a//b" */
            memcpy(first, rest, flen);
            first[flen] = 0;
            /* skip internal '!' siblings (recipe/cover/part/jxl) */
            if (memchr(first, '!', flen)) continue;
            /* hide control-prefixed internal names (the "\x01tzb" owner) */
            if ((uint8_t)first[0] == 0x01) continue;

            hb = (size_t)(idx_hash(first, flen) & seen_mask);
            for (d = seen[hb]; d; d = d->next)
                if (d->is_dir == is_dir && strcmp(d->name, first) == 0) break;
            if (d) {
                /* The anchor record "d/" is the directory's own record; its
                   ctime is the one to report. Any other record that maps to
                   the same entry is a file underneath it. */
                if (is_dir && rl == flen + 1) ents[d->at].ctime = e->ctime;
                continue;
            }
            d = (struct dedup *)malloc(sizeof *d + flen);
            if (!d) continue;
            memcpy(d->name, first, flen + 1);
            d->is_dir = is_dir;
            d->at = n;
            d->next = seen[hb];
            seen[hb] = d;

            memcpy(ents[n].name, first, flen + 1);
            ents[n].is_dir = is_dir;
            /* Carried from the index so the caller needs no per-entry stat.
               A directory's size is meaningless; report 0 unless its own
               anchor record comes along. */
            ents[n].size = is_dir ? 0 : e->size;
            ents[n].ctime = e->ctime;
            n++;
        }
    }

    for (b = 0; b <= seen_mask; b++) {
        struct dedup *d = seen[b];
        while (d) { struct dedup *nx = d->next; free(d); d = nx; }
    }
    free(seen);
    /* Hash order is an implementation detail and would shift between mounts.
       The inode-area scan used to yield creation order; sort by name so the
       listing is at least stable. */
    if (n > 1) qsort(ents, n, sizeof *ents, dirent_cmp);
    return (int)n;
}


/* Replace `name` with `data`, or create it if absent.
 *
 * The write commit path used to delete by name and then create, which loses
 * the user's existing file outright when the create fails: the old record is
 * already tombstoned and the new bytes are gone with it. ENOSPC on an
 * overwrite is a normal condition, not a corner case -- the whole point of
 * this filesystem is that data survives.
 *
 * Append the new record first. The inode area is append-only and readers are
 * version-aware, so the newer record wins from the moment it lands; only then
 * is the old inode tombstoned. idx_del ignores a mismatched id, so the
 * tombstone cannot take the replacement's name down with it (the sweep has
 * relied on exactly this ordering all along).
 *
 * Returns the new inode id, or 0 with the volume untouched. */
uint64_t vol_replace_file(invfs_volume *v, const char *name,
                          const uint8_t *data, size_t len)
{
    uint64_t old_id, nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;   /* EROFS */
    old_id = vol_find(v, name);
    /* WP19: write-heat is the one counter a rewrite must NOT reset --
     * "rewritten often" is history the rewrite itself destroys. Capture
     * the old entries' max now (they retire below) and carry old+1 onto
     * the replacement's fresh entries (born wheat 1 in vol_create_file).
     * Read-heat resets by design: past reads say nothing about new bytes. */
    {
        uint8_t wold = old_id ? heat_file_maxw(v, old_id) : 0;
        nid = vol_create_file(v, name, data, len);
        if (nid == 0) return 0;                      /* old file still there */
        if (old_id != 0)
            heat_file_setw(v, nid, wold == 0xFF ? 0xFF : (uint8_t)(wold + 1));
    }
    if (old_id != 0) {
        /* the replacement is plain RAW, so the old file's recipe/parts/covers
           describe bytes that no longer exist under this name */
        int owns_siblings = 1;   /* unknown: scan conservatively */
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        if (meta_read_record_by_id(v, old_id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        vol_delete_inode(v, old_id, name);
        /* sibling deletion walks the WHOLE inode area (pread per record):
         * plain files -- everything written through FUSE -- can only have
         * '!' siblings when their AST lists children or a container algo,
         * so skip the walk */
        if (owns_siblings)
            vol_delete_siblings(v, name);
    }
    return nid;
}


uint64_t vol_replace_file_with_meta(invfs_volume *v, const char *name,
                                    const uint8_t *data, size_t len,
                                    const invfs_meta_pub *meta)
{
    uint64_t old_id, nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    old_id = vol_find(v, name);
    {
        uint8_t wold = old_id ? heat_file_maxw(v, old_id) : 0;
        nid = vol_create_file_with_meta(v, name, data, len, meta);
        if (nid == 0) return 0;
        if (old_id != 0)
            heat_file_setw(v, nid, wold == 0xFF ? 0xFF : (uint8_t)(wold + 1));
    }
    if (old_id != 0) {
        int owns_siblings = 1;
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        if (meta_read_record_by_id(v, old_id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        vol_delete_inode(v, old_id, name);
        if (owns_siblings)
            vol_delete_siblings(v, name);
    }
    return nid;
}


int vol_delete_file(invfs_volume *v, const char *name)
{
    uint64_t inode_id;
    /* H5: allowed under the VOLF_READONLY space latch -- a delete only
     * frees blocks and appends a tombstone, moving the volume AWAY from
     * the wall. needs_recovery still refuses via vol_mark_dirty below. */
    if (v->needs_recovery) return -1;
    inode_id = vol_find(v, name);
    if (inode_id == 0)
        return -1;
    return vol_delete_inode(v, inode_id, name);
}


/* Delete a file and the sibling records that belong to it. This is what an
   unlink means for a transcoded file; vol_delete_file alone strands them. */
/* remove ONE name of a multi-name (hardlinked) inode: tombstone only
 * this name's record; blocks stay alive for the surviving names.
 * nlink on surviving records is not rewritten (drift only delays block
 * retirement; fsck recount fixes it). */
int vol_unlink_name(invfs_volume *v, const char *name)
{
    uint64_t id, pos = 0;
    uint32_t crc;
    size_t nl;
    invfs_inode_rec rec;
    const name_index_entry *e;

    /* H5: allowed under the VOLF_READONLY space latch (tombstone only,
     * blocks stay alive for the surviving names -- see vol_delete_file) */
    if (v->needs_recovery) return -1;
    nl = strlen(name);
    e = idx_get(v, name, nl);
    if (!e) return -1;
    id = e->inode_id;
    /* The kill target is the record THIS NAME points at. Resolving the
     * position through the inode id (meta_read_record_by_id /
     * idx_get_id) is wrong here: the rename fast path's vol_hardlink has
     * just re-pointed the id index at the NEW record, which shares the
     * id -- the position-kill tombstone then named the replacement and
     * killed nothing at replay, so the old name stayed live and a later
     * retire of the shared id dropped the surviving name's mappings
     * (WP22c/F2). The name index tracks each name's own record. */
    pos = e->pos;
    /* sanity: the index entry must name a live INOD for this id (the
     * vol_forget_name ghost pattern) */
    if (!pos ||
        pos < v->inode_area_start * INVFS_BLOCK_SIZE ||
        pos + sizeof(rec) > v->inode_area_pos)
        return -1;
    if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rec, sizeof(rec)) != 0)
        return -1;
    if (rec.magic != INODE_REC_MAGIC || rec.inode_id != id)
        return -1;
    if (vol_mark_dirty(v) != 0)
        return -1;
    memset(&rec, 0, sizeof(rec));
    rec.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    rec.rec_len = (uint32_t)sizeof(rec);
    rec.inode_id = id;
    rec.file_size = pos;                    /* v2 position-kill */
    rec_set_name(&rec, name);
    crc = invfs_crc32c((const uint8_t *)&rec, sizeof(rec));
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, &rec, sizeof(rec)) != 0 ||
        io_write(&v->io, &crc, 4) != 0)
        return -1;
    v->inode_area_pos += sizeof(rec) + 4;
    idx_del_at(v, name, nl, pos);
    idx_bump_dirs(v, name, nl, -1);
    return 0;
}


int vol_unlink(invfs_volume *v, const char *name)
{
    int rc;
    /* H5: allowed under the VOLF_READONLY space latch (frees only) */
    if (v->needs_recovery) return -1;
    if (strchr(name, '!')) return vol_delete_file(v, name);   /* a sibling */
    /* capture the record first: sibling deletion is a full-area walk and
     * plain files (no children, no container algo) never have '!' siblings */
    {
        int owns_siblings = 1;
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        uint64_t id = vol_find(v, name);
        if (id && meta_read_record_by_id(v, id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        rc = vol_delete_file(v, name);
        if (rc == 0 && owns_siblings) vol_delete_siblings(v, name);
    }
    return rc;
}


/* ---- rename / move ---- */

/* Move ONE inode record to a new name.
 *
 * The name lives inside the inode record and the inode area is append-only,
 * so a rename is: append a copy of the record with the AST payload verbatim
 * (the data blocks are never read, rewritten or recompressed), re-key that
 * inode's L2P mappings, then tombstone the old name.
 *
 * The copy deliberately gets a NEW inode id. vol_fsck_scan kills live records
 * by inode_id alone (pass 2), so if both names shared an id, the old name's
 * tombstone would make `invf-fsck --fix` free the renamed file's blocks. */
static int rename_one(invfs_volume *v, const char *from, const char *to)
{
    uint64_t old_id, new_id, rec_pos = 0;
    invfs_inode_rec rh, *nh;
    uint8_t *rec;
    uint32_t crc;
    size_t tolen = strlen(to), fromlen = strlen(from);
    const name_index_entry *ie;

    (void)fromlen;
    if (tolen >= sizeof(rh.name)) return -1;
    ie = idx_get(v, from, fromlen);
    if (!ie) return -1;
    old_id = ie->inode_id;
    rec_pos = ie->pos;          /* the live record — the index tracks it */
    if (rec_pos == 0) return -1;

    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, &rh, sizeof rh) != 0) return -1;
    if (rh.rec_len < sizeof(invfs_inode_rec)) return -1;

    /* room for the copy AND the tombstone that follows it — running out
       between the two would leave the file reachable under both names.
       WP27 churn backstop: compact the dead prefix first; the compaction
       moves every record, so the live position is re-resolved after it. */
    if (v->inode_area_pos + rh.rec_len + 4 +
        sizeof(invfs_inode_rec) + 4 > v->inode_area_end) {
        if (inode_area_make_room(v, rh.rec_len + 4 +
                sizeof(invfs_inode_rec) + 4) != 0)
            return -3;   /* inode area full */
        ie = idx_get(v, from, fromlen);
        if (!ie) return -1;
        rec_pos = ie->pos;
        if (rec_pos == 0) return -1;
        if (io_seek(&v->io, rec_pos) != 0 ||
            io_read(&v->io, &rh, sizeof rh) != 0) return -1;
        if (rh.rec_len < sizeof(invfs_inode_rec)) return -1;
    }

    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0) { free(rec); return -1; }

    new_id = v->next_inode_id++;
    nh = (invfs_inode_rec *)rec;
    nh->inode_id = new_id;
    nh->name_len = (uint32_t)tolen;
    memset(nh->name, 0, sizeof nh->name);
    memcpy(nh->name, to, tolen);
    crc = invfs_crc32c(rec, rh.rec_len);

    /* WP27: the copied record carries the pbas verbatim -- the rename needs
     * no map re-key at all. The blocks' durability is the same as on the
     * day the original record landed, and vol_pre_record's flush ordering
     * (bitmap durable before the record that names its pbas) is kept. The
     * old entries stay until vol_delete_inode below, so a crash mid-rename
     * leaves the old name fully readable, and the delete frees nothing the
     * copy still references (the pba_ref sharer count). */
    if (vol_pre_record(v) != 0 ||
        io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rh.rec_len) != 0 ||
        io_write(&v->io, &crc, 4) != 0) { free(rec); return -1; }
    v->inode_area_pos += rh.rec_len + 4;
    idx_put(v, to, tolen, new_id, v->inode_area_pos - rh.rec_len - 4,
            nh->file_size, nh->ctime);
    idx_put_id(v, new_id, v->inode_area_pos - rh.rec_len - 4);
    pba_ref_apply(v, rec, rh.rec_len, +1);
    free(rec);

    return vol_delete_inode(v, old_id, from);
}


/* Rename a file or directory.
 *
 * Transcoded files keep their payload in sibling records ("name!recipe",
 * "name!partN", "name!coverN", "name!jxl") that the read path resolves BY
 * NAME, so moving a file has to move its siblings too — otherwise the
 * bit-exact rebuild silently loses its recipe. Directories are virtual
 * (anchor "dir/" + prefix), so moving one rewrites every record under the
 * prefix, anchor included.
 *
 * Returns 0, -1 = ENOENT/EINVAL/IO, -2 = destination exists, -3 = ENOSPC.
 * The inode area is append-only, so a multi-record move that fails partway
 * leaves the already-moved records at their new names; re-running the rename
 * completes it. */
int vol_rename(invfs_volume *v, const char *from, const char *to)
{
    char (*names)[256] = NULL;
    char pre[300];
    size_t n = 0, cap = 0, pren = 0, flen, i;
    uint64_t pos, end;
    int dir, rc = 0;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;   /* EROFS */
    /* WP21/WP22c: refuse to rename while a sweep checkpoint is live. The
     * rollback decapitates the inode area at the checkpoint's append
     * pointer, discarding the whole rename pair -- the copy dies AND the
     * tombstone dies, so the source name resurrects: a file nobody
     * deleted reappearing out of nowhere is the one outcome the crash
     * contract cannot allow (the chaos soak reads it as a ghost). Resolve
     * the checkpoint first (invf-rollback / invf-sweep --realize). */
    if (v->ck_present) return -4;
    if (!v || !from || !to || !from[0] || !to[0]) return -1;
    if (strcmp(from, to) == 0) return 0;
    flen = strlen(from);
    if (strlen(to) >= 240) return -1;
    /* "a" -> "a/b" would move a directory inside itself */
    if (strncmp(to, from, flen) == 0 && to[flen] == '/') return -1;

    dir = vol_is_dir(v, from);
    if (!dir && vol_find(v, from) == 0) return -1;            /* ENOENT */
    /* POSIX rename replaces an existing plain-file destination
     * silently; directory destinations stay unsupported (EEXIST). */
    if (vol_is_dir(v, to)) return -2;
    {
        uint64_t toid = vol_find(v, to);
        if (toid != 0) {
            if (dir) return -1;                    /* dir over file: ENOTDIR-ish */
            /* the victim takes its '!' siblings down with it (the
             * vol_unlink cascade): a container victim whose members were
             * only renamed in would otherwise strand them as parentless
             * live records (the chaos soak reads one as a ghost). */
            int owns_siblings = 1;
            uint8_t *obuf = NULL;
            uint32_t orl = 0;
            if (meta_read_record_by_id(v, toid, &obuf, &orl, NULL, 0,
                                       NULL) == 0) {
                owns_siblings = record_owns_siblings(obuf, orl);
                free(obuf);
            }
            if (vol_delete_file(v, to) != 0) return -1;
            if (owns_siblings) vol_delete_siblings(v, to);
        }
    }

    vol_ensure_path(v, to);   /* parents of the destination */

    /* FAST PATH: plain files without container siblings are renamed as
     * hardlink(new)+unlink_name(old) -- both are O(1) index-hint ops.
     * The legacy full-area sibling scan below only runs for directories
     * and container anchors ('!' siblings / num_children>0); on big
     * volumes it cost minutes per rename (dracut does hundreds). */
    if (!dir && !strchr(from, '!') && !strchr(to, '!')) {
        uint8_t *buf = NULL;
        uint32_t rl = 0;
        uint64_t fid = vol_find(v, from);
        int simple = 0;
        if (fid != 0 &&
            meta_read_record_by_id(v, fid, &buf, &rl, NULL, 0, NULL) == 0)
            /* Extraction containers (TARR/EXER/...) carry num_children==0
             * yet keep their payload in "name!..." siblings the read path
             * resolves BY NAME: a hardlink rename would move the anchor
             * and strand the recipe under the old name (the renamed file
             * never reads again). record_owns_siblings is the conservative
             * test; anything unknown takes the slow path. */
            simple = !record_owns_siblings(buf, rl);
        free(buf);
        if (simple) {
            if (vol_hardlink(v, from, to) != 0) return -1;
            if (vol_unlink_name(v, from) != 0) return -1;
            return 0;
        }
    }

    if (dir) { snprintf(pre, sizeof pre, "%s/", from); pren = strlen(pre); }

    /* Collect every record the move touches BEFORE appending anything:
       each rename_one extends the inode area, and a live scan would then
       walk into the records it had just written. */
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        char nm[257];
        size_t nl;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &h, sizeof h) != 0) {
            rc = -1; break;
        }
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(invfs_inode_rec)) break;
        pos += h.rec_len + 4;
        if (h.magic != INODE_REC_MAGIC) continue;
        nl = h.name_len < 256 ? h.name_len : 256;
        memcpy(nm, h.name, nl);
        nm[nl] = 0;
        if (dir) {
            if (nl < pren || strncmp(nm, pre, pren) != 0) continue;
        } else {
            /* the file itself, plus its "name!..." siblings */
            if (nl < flen || strncmp(nm, from, flen) != 0) continue;
            if (nm[flen] != 0 && nm[flen] != '!') continue;
        }
        if (vol_find(v, nm) == 0) continue;   /* superseded/tombstoned */
        for (i = 0; i < n; i++)
            if (strcmp(names[i], nm) == 0) break;
        if (i < n) continue;                  /* older record for same name */
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            void *nn = realloc(names, ncap * sizeof(*names));
            if (!nn) { free(names); return -1; }
            names = (char (*)[256])nn;
            cap = ncap;
        }
        memcpy(names[n++], nm, nl + 1);
    }

    if (rc == 0 && n == 0) rc = -1;   /* nothing matched */

    for (i = 0; i < n && rc == 0; i++) {
        char nn[300];
        if (dir) snprintf(nn, sizeof nn, "%s/%s", to, names[i] + pren);
        else     snprintf(nn, sizeof nn, "%s%s", to, names[i] + flen);
        if (strlen(nn) >= 256) { rc = -1; break; }
        rc = rename_one(v, names[i], nn);
    }
    free(names);
    return rc;
}


/* Hard link: a second NAME record sharing the same inode_id (and thus the
 * same AST/L2P blocks). LIMITATION: no block refcounts yet -- unlinking
 * EITHER name retires the shared blocks and dangles the survivor
 * (audit WP6). Portage lock files and similar transient links are fine. */
int vol_hardlink(invfs_volume *v, const char *from, const char *to)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0, crc;
    invfs_inode_rec *nh;
    uint64_t id, pos;
    size_t nl;

    if (!v || !from || !to || !from[0] || !to[0]) return -1;
    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    if (strlen(to) >= 256) return -1;

    id = vol_find(v, from);
    if (!id) return -1;                              /* ENOENT */
    {
        uint64_t toid = vol_find(v, to);
        if (toid) {
            /* stale-index self-heal: killed emerges leave table entries
             * whose latest record is already a tombstone. The id-index
             * hint always points at the newest version we wrote, so
             * liveness = the header AT THE HINT is an INOD for this id.
             * (meta_read_record_by_id alone would false-positive: it
             * happily reads OLDER same-id versions still present in the
             * append-only area.) */
            int alive = 0;
            uint64_t hp = idx_get_id(v, toid);
            if (hp >= v->inode_area_start * INVFS_BLOCK_SIZE &&
                hp + sizeof(invfs_inode_rec) <= v->inode_area_pos &&
                io_seek(&v->io, hp) == 0) {
                invfs_inode_rec hh;
                if (io_read(&v->io, &hh, sizeof(hh)) == 0 &&
                    hh.magic == INODE_REC_MAGIC &&
                    hh.inode_id == toid)
                    alive = 1;
            }
            if (alive) {
                return -2;                           /* EEXIST */
            }
        }
    }
    if (vol_is_dir(v, from)) return -3;              /* dirs can't hardlink */

    if (meta_read_record_by_id(v, id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (rl < sizeof(invfs_inode_rec)) { free(buf); return -1; }

    /* name[] is a fixed 256-byte field: record size is unchanged */
    nh = (invfs_inode_rec *)buf;
    nl = strlen(to);
    memset(nh->name, 0, sizeof(nh->name));
    memcpy(nh->name, to, nl);
    nh->name_len = (uint32_t)nl;

    if (vol_mark_dirty(v) != 0) { free(buf); return -1; }
    crc = invfs_crc32c(buf, rl);
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, buf, rl) != 0 ||
        io_write(&v->io, &crc, 4) != 0) {
        free(buf);
        return -1;
    }
    pos = v->inode_area_pos;
    v->inode_area_pos += (uint64_t)rl + 4;
    idx_put(v, nh->name, nl, id, pos, nh->file_size, nh->ctime);
    idx_put_id(v, id, pos);
    /* the second name is a live child of its parent directories; without
     * this bump, unlinking it later drives parent counts negative and
     * rmdir starts refusing empty dirs ("Directory not empty") */
    idx_bump_dirs(v, to, nl, +1);
    free(buf);
    return 0;
}



/* Drop a name whose backing record is already gone (index ghost left by
 * a killed process). Returns 0 if the entry was forgotten, -1 if the
 * name looks live (caller should use the normal unlink path). */
int vol_forget_name(invfs_volume *v, const char *name)
{
    uint64_t id, pos = 0;
    invfs_inode_rec hh;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    id = vol_find(v, name);
    if (!id) return 0;                       /* already forgotten */
    pos = idx_get_id(v, id);
    if (!pos ||
        pos < v->inode_area_start * INVFS_BLOCK_SIZE ||
        pos + sizeof(hh) > v->inode_area_pos)
        return -1;
    if (io_seek(&v->io, pos) != 0 ||
        io_read(&v->io, &hh, sizeof(hh)) != 0)
        return -1;
    if (hh.magic == INODE_REC_MAGIC && hh.inode_id == id)
        return -1;                           /* record is alive */
    /* dead: drop the index entry and undo the directory child count */
    idx_del_at(v, name, strlen(name), pos);
    idx_bump_dirs(v, name, strlen(name), -1);
    vol_mark_dirty(v);
    return 0;
}
