/*
 * fuse_fs.c — InvariantFS FUSE filesystem (rw; POSIX v2 metadata,
 * POSIX ACLs + daemon-side permission enforcement (WP-A),
 * control xattr namespace, manual/opt-in sweep)
 *   invf-fuse [-f] <image> <mountpoint>
 *
 * Built on the same volume.c core as the Windows/WinFsp port.
 * Linux: gcc -O2 -o invf-fuse fuse_fs.c volume.c crc32c.c lz4.c -lfuse3 -lzstd
 */
#define _GNU_SOURCE
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/statvfs.h>
#include <ctype.h>

#include "invarifs.h"
#include "volume.h"
#include "tmpstore.h"

static invfs_volume *g_vol;
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
/* open-handle counter: background sweep only when idle */
static volatile int g_open_handles = 0;
/* set at unmount: stops the background sweep thread before vol_close */
static volatile int g_shutdown = 0;
static volatile sig_atomic_t g_sweep_now = 0;
static char g_img_path[512] = "?";
static volatile int g_sweep_busy = 0;
static int g_tt = 0;            /* WP24-lite: time-travel (at_checkpoint) mount */
static double g_attr_t = 1.0;   /* -o attr_t= override; 0 = bench-honest */
/* WP26: RAW-zone fill watermark (percent, 0 = lazy default). Set by
 * -o raw_watermark=<pct> or INVFS_RAW_WATERMARK; when the RAW fill
 * exceeds it the background sweep thread kicks an early sweep. */
static int g_raw_watermark = 0;
static tmp_area_mode g_tmp_area = TMP_AREA_AUTO;
static size_t g_tmp_max_bytes = 128 * 1024 * 1024;
static void invf_sweep_worker(int arm_ckp);   /* defined below sweep thread */
static void table_rebuild_locked(void);   /* fwd (defined below) */

/* Mutations only mark the name table stale; the next consumer pays for one
 * rebuild instead of every close paying one. tar/cp imports fire thousands
 * of closes -- rebuilding per close scanned the whole inode area each time
 * and made imports O(N^2). */
static int g_table_stale = 0;

static void table_mark_stale(void) { g_table_stale = 1; }
static void table_upsert_locked(const char *name, uint64_t ino,
                                uint64_t size, uint64_t ctime);
static void table_remove_name(const char *name);

static int is_temp_path(const char *path)
{
    return strstr(path, "/tmp/") != NULL ||
           strstr(path, "/var/tmp/") != NULL;
}

static void table_sync_one(const char *name)
{
    uint64_t id = 0, sz = 0, ct = 0;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return; }
    id = vol_find(g_vol, name);
    if (!id) {
        table_remove_name(name);
        pthread_mutex_unlock(&g_io_lock);
        return;
    }
    vol_stat_full(g_vol, name, &id, &sz, &ct);
    table_upsert_locked(name, id, sz, ct);
    pthread_mutex_unlock(&g_io_lock);
}

static void table_refresh_if_stale_locked(void)
{
    if (g_table_stale && g_vol) {
        table_rebuild_locked();
        g_table_stale = 0;
    }
}

/* ---- file table snapshot ---- */
typedef struct {
    char name[256];
    uint64_t inode_id;
    uint64_t size;
    uint64_t ctime;
    uint64_t pos;      /* byte offset of this version's INOD record;
                          v2 tombstones kill by position (DELT.file_size) */
} fs_entry;

static fs_entry *g_entries;
static int g_nentries, g_cap;

/* sort helpers */
static int cmp_entry_pos(const void *pa, const void *pb)
{
    const fs_entry *a = (const fs_entry *)pa, *b = (const fs_entry *)pb;
    if (a->pos < b->pos) return -1;
    if (a->pos > b->pos) return 1;
    return 0;
}

static int cmp_entry_name_pos(const void *pa, const void *pb)
{
    const fs_entry *a = (const fs_entry *)pa, *b = (const fs_entry *)pb;
    int c = strcmp(a->name, b->name);
    if (c) return c;
    if (a->pos < b->pos) return -1;
    if (a->pos > b->pos) return 1;
    return 0;
}

static int cmp_entry_key(const void *key, const void *element)
{
    return strcmp((const char *)key, ((const fs_entry *)element)->name);
}

static int cmp_u64(const void *pa, const void *pb)
{
    const uint64_t *a = (const uint64_t *)pa, *b = (const uint64_t *)pb;
    if (*a < *b) return -1;
    if (*a > *b) return 1;
    return 0;
}

/* WP49: pass-1 collector fed by the bounded, index-ordered
 * vol_records_walk (a position-driven vol_inode_next loop can cycle when
 * the mapper table is not pba monotonic). */
typedef struct {
    fs_entry *recs;
    uint64_t nrecs, caprecs;
    uint64_t *tpos, *tid;
    uint64_t ntomb, captomb;
    int oom;
} bft_ctx;

static int bft_cb(void *ctx_, uint64_t rec_pos,
                  const invfs_inode_rec *h, const uint8_t *rec)
{
    bft_ctx *c = (bft_ctx *)ctx_;
    (void)rec;
    if (h->magic == TOMBSTONE_MAGIC) {   /* DELT */
        if (c->ntomb == c->captomb) {
            uint64_t nc = c->captomb ? c->captomb * 2 : 64;
            uint64_t *tp = (uint64_t *)realloc(c->tpos, nc * sizeof(uint64_t));
            uint64_t *ti = (uint64_t *)realloc(c->tid, nc * sizeof(uint64_t));
            if (!tp || !ti) { c->oom = 1; if (tp) c->tpos = tp; if (ti) c->tid = ti; return 1; }
            c->tpos = tp; c->tid = ti; c->captomb = nc;
        }
        /* v2 kills by position (file_size); legacy by inode id (0) */
        c->tpos[c->ntomb] = h->file_size;
        c->tid[c->ntomb] = h->inode_id;
        c->ntomb++;
        return 0;
    }
    if (c->nrecs == c->caprecs) {
        uint64_t nc = c->caprecs ? c->caprecs * 2 : 1024;
        fs_entry *nr = (fs_entry *)realloc(c->recs, nc * sizeof(fs_entry));
        if (!nr) { c->oom = 1; return 1; }
        c->recs = nr; c->caprecs = nc;
    }
    memset(&c->recs[c->nrecs], 0, sizeof(fs_entry));
    {
        size_t nl = h->name_len < 255 ? h->name_len : 255;
        memcpy(c->recs[c->nrecs].name, h->name, nl);
        c->recs[c->nrecs].name[nl] = 0;
    }
    c->recs[c->nrecs].inode_id = h->inode_id;
    c->recs[c->nrecs].size = h->file_size;
    c->recs[c->nrecs].ctime = h->ctime;
    c->recs[c->nrecs].pos = rec_pos;
    c->nrecs++;
    return 0;
}

static void build_file_table(void)
{
    const invfs_superblock *sb = vol_sb(g_vol);
    (void)sb;
    bft_ctx c;
    fs_entry *recs = NULL;
    uint64_t nrecs = 0, caprecs = 0;
    /* tombstone kill list: (position-or-0, inode-id) pairs */
    uint64_t *tpos = NULL, *tid = NULL;
    uint64_t ntomb = 0, captomb = 0;

    g_entries = NULL; g_nentries = 0; g_cap = 0;

    /* pass 1: collect raw records. vol_records_walk is mapper-aware
     * (dynamic extents via the mapper) and bounded for legacy volumes. */
    memset(&c, 0, sizeof c);
    if (vol_records_walk(g_vol, bft_cb, &c) != 0 && !c.oom) {
        /* walk error: fall through with whatever was collected */
    }
    recs = c.recs; nrecs = c.nrecs; caprecs = c.caprecs;
    tpos = c.tpos; tid = c.tid; ntomb = c.ntomb; captomb = c.captomb;

    /* pass 2: apply tombstones (records sorted by pos for bsearch) */
    qsort(recs, (size_t)nrecs, sizeof(fs_entry), cmp_entry_pos);

    /* Build hash set for legacy tombstones (inode-id kill) to avoid O(T×N) scan */
    uint64_t *legacy_kill_ids = NULL;
    uint64_t nlegacy = 0, caplegacy = 0;
    for (uint64_t t = 0; t < ntomb; t++) {
        if (tpos[t] == 0) {
            if (nlegacy == caplegacy) {
                caplegacy = caplegacy ? caplegacy * 2 : 256;
                legacy_kill_ids = (uint64_t *)realloc(legacy_kill_ids, caplegacy * sizeof(uint64_t));
                if (!legacy_kill_ids) goto done;
            }
            legacy_kill_ids[nlegacy++] = tid[t];
        }
    }

    /* Sort legacy kill ids for faster lookup */
    qsort(legacy_kill_ids, (size_t)nlegacy, sizeof(uint64_t), cmp_u64);

    for (uint64_t t = 0; t < ntomb; t++) {
        if (tpos[t] != 0) {
            /* v2 position kill: exact record offset */
            uint64_t lo = 0, hi = nrecs;
            while (lo < hi) {
                uint64_t mid = (lo + hi) / 2;
                if (recs[mid].pos < tpos[t]) lo = mid + 1;
                else hi = mid;
            }
            if (lo < nrecs && recs[lo].pos == tpos[t]) recs[lo].inode_id = UINT64_MAX; /* dead */
        }
        /* Legacy kill now handled in pass 3 via hash set */
    }

    /* pass 3: drop dead, then last-write-wins per name.
     * Also apply legacy kills using the sorted id array (binary search). */
    {
        uint64_t w = 0, r;
        for (r = 0; r < nrecs; r++) {
            int dead = 0;
            if (recs[r].inode_id == UINT64_MAX) {
                dead = 1;
            } else if (nlegacy > 0) {
                /* Binary search in legacy_kill_ids */
                uint64_t lo = 0, hi = nlegacy;
                while (lo < hi) {
                    uint64_t mid = (lo + hi) / 2;
                    if (legacy_kill_ids[mid] < recs[r].inode_id) lo = mid + 1;
                    else hi = mid;
                }
                if (lo < nlegacy && legacy_kill_ids[lo] == recs[r].inode_id) dead = 1;
            }
            if (!dead) recs[w++] = recs[r];
        }
        nrecs = w;
    }
    free(legacy_kill_ids); legacy_kill_ids = NULL;

    /* pass 3: drop dead, then last-write-wins per name */
    {
        uint64_t w = 0, r;
        for (r = 0; r < nrecs; r++)
            if (recs[r].inode_id != UINT64_MAX) recs[w++] = recs[r];
        nrecs = w;
    }
    qsort(recs, (size_t)nrecs, sizeof(fs_entry), cmp_entry_name_pos);
    {
        uint64_t w = 0, r;
        for (r = 0; r < nrecs; r++) {
            if (w > 0 && strcmp(recs[w - 1].name, recs[r].name) == 0)
                recs[w - 1] = recs[r];      /* newer pos wins */
            else
                recs[w++] = recs[r];
        }
        nrecs = w;
    }

    g_entries = recs;
    g_cap = (int)caprecs;
    g_nentries = (nrecs > 0x7fffffff) ? 0x7fffffff : (int)nrecs;
    free(tpos); free(tid);
    return;
done:
    free(recs); free(tpos); free(tid);
}

static fs_entry *find_entry(const char *name)
{
    if (g_nentries == 0) return NULL;
    return (fs_entry *)bsearch(name, g_entries, (size_t)g_nentries,
                               sizeof(fs_entry), cmp_entry_key);
}
/* ---- incremental single-name sync (avoids full rebuild per mutation) ----
 * Common ops (create/flush/unlink/truncate/mkdir/rmdir) touch exactly one
 * name; syncing that name costs one engine lookup instead of rescanning a
 * 200k+ record inode area. Complex ops (rename/link/sweep) keep mark_stale.
 * All helpers require g_io_lock held. */
static void table_upsert_locked(const char *name, uint64_t ino, uint64_t size,
                                uint64_t ctime)
{
    int lo = 0, hi = g_nentries;
    while (lo < hi) {                    /* lower_bound by name */
        int mid = (lo + hi) / 2;
        if (strcmp(g_entries[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_nentries && strcmp(g_entries[lo].name, name) == 0) {
        g_entries[lo].inode_id = ino;
        g_entries[lo].size = size;
        g_entries[lo].ctime = ctime;
        return;
    }
    if (g_nentries == g_cap) {
        int ncap = g_cap ? g_cap * 2 : 1024;
        fs_entry *ne = (fs_entry *)realloc(g_entries, ncap * sizeof(fs_entry));
        if (!ne) return;               /* keep old table on OOM */
        g_entries = ne;
        g_cap = ncap;
    }
    memmove(&g_entries[lo + 1], &g_entries[lo],
            (size_t)(g_nentries - lo) * sizeof(fs_entry));
    memset(&g_entries[lo], 0, sizeof(fs_entry));
    strncpy(g_entries[lo].name, name, sizeof(g_entries[lo].name) - 1);
    g_entries[lo].inode_id = ino;
    g_entries[lo].size = size;
    g_entries[lo].ctime = ctime;
    g_nentries++;
}

static void table_remove_name(const char *name)
{
    int lo = 0, hi = g_nentries;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(g_entries[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g_nentries || strcmp(g_entries[lo].name, name) != 0) return;
    memmove(&g_entries[lo], &g_entries[lo + 1],
            (size_t)(g_nentries - lo - 1) * sizeof(fs_entry));
    g_nentries--;
}

/* re-read one name from the engine into the table. Call WITHOUT the lock. */
/* same as table_sync_one but caller already holds g_io_lock */
static void table_sync_one_locked(const char *name)
{
    uint64_t id, sz = 0, ct = 0;
    if (!g_vol) return;
    id = vol_find(g_vol, name);
    if (!id) { table_remove_name(name); return; }
    if (vol_stat_full(g_vol, name, &id, &sz, &ct) == 0)
        table_upsert_locked(name, id, sz, ct);
}


/* Race-safe lookup (audit H1): g_entries is freed and rebuilt by every
 * flush/create/unlink/sweep, so callers must never hold the pointer across
 * a rebuild. Copy the fields out under g_io_lock instead. */
static int snapshot_entry(const char *name, uint64_t *ino_out, uint64_t *size_out,
                          uint64_t *ctime_out)
{
    int found = 0;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) {
        pthread_mutex_unlock(&g_io_lock);
        return 0;
    }
    {
        fs_entry *e = find_entry(name);
        if (!e) table_refresh_if_stale_locked();
        if (!e) e = find_entry(name);
        if (e) {
            if (ino_out)   *ino_out   = e->inode_id;
            if (size_out)  *size_out  = e->size;
            if (ctime_out) *ctime_out = e->ctime;
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_io_lock);
    return found;
}

/* ---- format v2 metadata overlay ----
 * Records written before v2 have no INO2 ext; those get POSIX-ish defaults
 * (uid/gid 0, 0644 files / 0755 dirs). Symlinks are records whose meta type
 * is INVFS_ITYP_LNK: the payload is the target string, st_size is its len. */

static int is_anchor_name(const char *name)   /* "dir/" anchor record */
{
    size_t n = strlen(name);
    return n > 0 && name[n - 1] == '/';
}

static void meta_defaults(const char *name, uint64_t size, invfs_meta_pub *m)
{
    memset(m, 0, sizeof(*m));
    if (is_anchor_name(name)) {
        m->type = INVFS_ITYP_DIR;
        m->mode = 0755;
        m->nlink = 2;
    } else {
        m->type = INVFS_ITYP_REG;
        m->mode = 0644;
        m->nlink = 1;
    }
    m->mtime = (int64_t)time(NULL);
    m->atime = m->mtime;
    (void)size;
}

/* resolve path -> engine record name ("dir" -> "dir/" anchor) + metadata.
 * Returns 1 and fills ename/m on success, 0 if nothing exists there. */
static int meta_for_path(const char *path, char *ename, size_t ecapsz,
                         invfs_meta_pub *m)
{
    const char *name = path[0] == '/' ? path + 1 : path;
    uint64_t ino = 0, size = 0, ctime = 0;

    if (snapshot_entry(name, &ino, &size, &ctime)) {
        int got;
        pthread_mutex_lock(&g_io_lock);
        got = g_vol ? (vol_get_meta(g_vol, ino, m) == 0) : 0;
        pthread_mutex_unlock(&g_io_lock);
        if (!got) meta_defaults(name, size, m);
        snprintf(ename, ecapsz, "%s", name);
        return 1;
    }
    /* not a file entry: a directory's anchor record is "name/" */
    {
        char anchor[300];
        int is_dir = 0, got = 0;
        pthread_mutex_lock(&g_io_lock);
        is_dir = g_vol ? vol_is_dir(g_vol, name) : 0;
        pthread_mutex_unlock(&g_io_lock);
        if (!is_dir) return 0;
        snprintf(anchor, sizeof anchor, "%s/", name);
        if (snapshot_entry(anchor, &ino, &size, &ctime)) {
            pthread_mutex_lock(&g_io_lock);
            got = g_vol ? (vol_get_meta(g_vol, ino, m) == 0) : 0;
            pthread_mutex_unlock(&g_io_lock);
        }
        if (!got) {
            /* anchor record may predate its INO2 ext (or ext parse failed):
             * report sane DIR defaults; first setattr rewrites the ext */
            meta_defaults(name, 0, m);
            m->type = INVFS_ITYP_DIR;
            m->nlink = 2;
        }
        snprintf(ename, ecapsz, "%s", anchor);
        return 1;
    }
}

static void fill_stat_from_meta(struct stat *st, const invfs_meta_pub *m,
                                uint64_t size, uint64_t ctime)
{
    memset(st, 0, sizeof(*st));
    switch (m->type) {
    case INVFS_ITYP_DIR:  st->st_mode = S_IFDIR | (m->mode & 07777); break;
    case INVFS_ITYP_LNK:  st->st_mode = S_IFLNK | 0777;
                          st->st_size = (off_t)strlen(m->target); break;
    case INVFS_ITYP_FIFO: st->st_mode = S_IFIFO | (m->mode & 07777); break;
    case INVFS_ITYP_SOCK: st->st_mode = S_IFSOCK | (m->mode & 07777); break;
    case INVFS_ITYP_CHR:  st->st_mode = S_IFCHR | (m->mode & 07777); break;
    case INVFS_ITYP_BLK:  st->st_mode = S_IFBLK | (m->mode & 07777); break;
    default:              st->st_mode = S_IFREG | (m->mode & 07777); break;
    }
    st->st_uid = m->uid;
    st->st_gid = m->gid;
    st->st_nlink = m->nlink ? m->nlink : 1;
    if (m->type != INVFS_ITYP_LNK)
        st->st_size = (off_t)size;
    st->st_mtime = (time_t)(m->mtime ? m->mtime : (int64_t)ctime);
    st->st_atime = (time_t)(m->atime ? m->atime : (int64_t)ctime);
    st->st_ctime = (time_t)ctime;
}

/* merge a partial patch over current/default metadata and persist it */
#define MM_TYPE  0x01
#define MM_MODE  0x02
#define MM_OWNER 0x04
#define MM_TIMES 0x08
static int meta_apply_patch(const char *path, unsigned mask,
                            const invfs_meta_pub *patch)
{
    char ename[300];
    invfs_meta_pub cur;
    uint64_t nid;
    if (!meta_for_path(path, ename, sizeof ename, &cur))
        return -ENOENT;
    if (mask & MM_MODE)  cur.mode = patch->mode & 07777;
    if (mask & MM_OWNER) { cur.uid = patch->uid; cur.gid = patch->gid; }
    if (mask & MM_TIMES) { cur.mtime = patch->mtime; cur.atime = patch->atime; }
    if (mask & MM_TYPE)  cur.type = patch->type;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol || !vol_write_enabled(g_vol)) {
        pthread_mutex_unlock(&g_io_lock);
        return g_vol ? -EROFS : -EIO;
    }
    nid = vol_apply_meta(g_vol, ename, &cur);
    /* no vol_flush / table rebuild here: tar/cp fire setattr per file and
     * both would make imports O(N^2). Records land durably at close/flush;
     * the name index stays valid because apply_meta keeps inode ids. */
    pthread_mutex_unlock(&g_io_lock);
    return nid ? 0 : -ENOSPC;
}

/* ==================== POSIX ACLs (WP-A) ====================
 * Storage: the standard Linux posix_acl xattr blob (u32 version-2 header,
 * then {u16 tag, u16 perm, u32 id} entries, little-endian) is kept
 * VERBATIM as the value of the system.posix_acl_access /
 * system.posix_acl_default xattrs inside the inode's INO2 TLV area
 * (vol_get/set_xattr) -- the kernel and acl(5) tools parse that layout,
 * so nothing is re-encoded on the way in or out.
 *
 * Enforcement split (daemon vs kernel): this mount does NOT use
 * default_permissions, and without it this kernel never delegates
 * permission() decisions to the daemon either (verified: as a foreign
 * uid, open/read/readdir/unlink of 0600/0700 objects all succeeded
 * unchecked), so the daemon is the sole object-level permission
 * authority. Every op entry point that matters evaluates here:
 *   open            R/W per flags       readdir          R on the dir
 *   access(2)       the asked mask      truncate(path)   W
 *   create/mkdir/mknod/symlink/link     parent dir W|X (+inheritance)
 *   unlink/rmdir/rename                 parent W|X (+t ownership rule)
 *   all path ops                        X on intermediate components
 * The kernel still owns: umask masking of create/mkdir modes
 * (vfs_prepare_mode -- SB_POSIXACL is NOT negotiated, see below), and
 * chmod/chown ownership rules (notify_change refuses non-owners before
 * the daemon is ever called; chmod additionally folds the new mode into
 * a stored access ACL here, POSIX.1e mask rule).
 *
 * uid 0 and the daemon's own euid bypass all checks (the pre-WP-A
 * CAP_DAC_OVERRIDE analog: a single-admin image has no security boundary
 * against its own administrator, and refusing the image owner breaks
 * ordinary tooling). fuse_context carries uid/gid only; supplementary
 * groups come from /proc/<caller-pid>/status (best effort: unreadable
 * process -> primary gid only).
 *
 * FUSE_CAP_POSIX_ACL is deliberately NOT requested: the generic FUSE
 * xattr handler already transports system.posix_acl_* blobs verbatim
 * (verified), and negotiating it would move umask handling into the
 * daemon for zero semantic gain. Consequence (documented deviation):
 * when a parent default ACL exists, the kernel has already applied the
 * caller's umask to the create mode before we see it, so inheritance
 * masks the umask-filtered mode instead of the raw one.
 */

#define INVFS_ACL_VERSION 2u   /* posix_acl_xattr_header.a_version */
#define ACL_USER_OBJ  0x01
#define ACL_USER      0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP     0x08
#define ACL_MASK      0x10
#define ACL_OTHER     0x20
#define ACL_UNDEF_ID  0xffffffffu

#define XATTR_ACL_ACCESS  "system.posix_acl_access"
#define XATTR_ACL_DEFAULT "system.posix_acl_default"

static uint16_t acl_ent_tag(const uint8_t *e)  { uint16_t v; memcpy(&v, e, 2); return v; }
static uint16_t acl_ent_perm(const uint8_t *e) { uint16_t v; memcpy(&v, e + 2, 2); return v; }
static uint32_t acl_ent_id(const uint8_t *e)   { uint32_t v; memcpy(&v, e + 4, 4); return v; }
static void acl_ent_set_perm(uint8_t *e, uint16_t p) { memcpy(e + 2, &p, 2); }

/* structural check of a blob about to be stored: canonical tag order,
 * exactly one of each base entry, perms in 0..7 */
static int acl_blob_valid(const uint8_t *b, size_t n)
{
    size_t nent, i;
    uint32_t ver;
    int seen_uobj = 0, seen_gobj = 0, seen_other = 0;
    int stage = 0;   /* 0 USER_OBJ | 1 USER | 2 GROUP_OBJ | 3 GROUP | 4 MASK | 5 OTHER */
    if (n < 4 || (n - 4) % 8) return 0;
    memcpy(&ver, b, 4);
    if (ver != INVFS_ACL_VERSION) return 0;
    nent = (n - 4) / 8;
    if (nent < 3 || nent > 128) return 0;
    for (i = 0; i < nent; i++) {
        const uint8_t *e = b + 4 + i * 8;
        uint16_t tag = acl_ent_tag(e);
        unsigned perm = acl_ent_perm(e);
        uint32_t id = acl_ent_id(e);
        if (perm & ~7u) return 0;
        switch (tag) {
        case ACL_USER_OBJ:
            if (i != 0 || id != ACL_UNDEF_ID) return 0;
            seen_uobj = 1; stage = 1; break;
        case ACL_USER:
            if (stage > 1 || id == ACL_UNDEF_ID) return 0;
            stage = 1; break;
        case ACL_GROUP_OBJ:
            if (stage > 1 || id != ACL_UNDEF_ID) return 0;
            seen_gobj = 1; stage = 2; break;
        case ACL_GROUP:
            if (stage < 2 || stage > 3 || id == ACL_UNDEF_ID) return 0;
            stage = 3; break;
        case ACL_MASK:
            if (stage < 2 || stage > 3 || id != ACL_UNDEF_ID) return 0;
            stage = 4; break;
        case ACL_OTHER:
            if (i != nent - 1 || id != ACL_UNDEF_ID) return 0;
            seen_other = 1; stage = 5; break;
        default:
            return 0;
        }
    }
    return seen_uobj && seen_gobj && seen_other;
}

typedef struct acreds {
    uid_t uid;
    gid_t gid;
    gid_t grps[64];
    int ngr;
    int bypass;   /* root or the daemon's euid: CAP_DAC_OVERRIDE analog */
} acreds;

static void acreds_get(struct acreds *c)
{
    struct fuse_context *ctx = fuse_get_context();
    c->uid = ctx ? ctx->uid : 0;
    c->gid = ctx ? ctx->gid : 0;
    c->ngr = 0;
    c->bypass = (c->uid == 0 || c->uid == (uid_t)geteuid());
    if (c->bypass || !ctx || ctx->pid <= 0) return;
    /* supplementary groups of the caller; fuse_context does not carry them */
    {
        char procpath[64], line[1024];
        FILE *f;
        snprintf(procpath, sizeof procpath, "/proc/%d/status", (int)ctx->pid);
        f = fopen(procpath, "r");
        if (!f) return;
        while (fgets(line, sizeof line, f)) {
            char *s;
            if (strncmp(line, "Groups:", 7) != 0) continue;
            s = line + 7;
            while (*s && *s != '\n' && c->ngr < (int)(sizeof c->grps / sizeof c->grps[0])) {
                char *end;
                unsigned long v;
                while (*s == ' ' || *s == '\t') s++;
                if (!isdigit((unsigned char)*s)) break;
                v = strtoul(s, &end, 10);
                if (end == s) break;
                c->grps[c->ngr++] = (gid_t)v;
                s = end;
            }
            break;
        }
        fclose(f);
    }
}

static int acreds_in_group(const struct acreds *c, gid_t g)
{
    int i;
    if (c->gid == g) return 1;
    for (i = 0; i < c->ngr; i++)
        if (c->grps[i] == g) return 1;
    return 0;
}

/* mode+ACL evaluation, mirrors kernel posix_acl_permission_masq:
 * named-user hits and every group-class hit are ANDed with ACL_MASK (if
 * present); a caller that matched the group class but was granted nothing
 * never falls through to OTHER. want = R_OK|W_OK|X_OK bits. 0 allowed,
 * -1 denied. No ACL -> the plain mode triad (group = primary OR any
 * supplementary group match). Malformed blobs fail closed. */
static int acl_eval(const invfs_meta_pub *m, const uint8_t *acl, size_t alen,
                    const struct acreds *c, unsigned want)
{
    size_t nent, i;
    const uint8_t *base;
    unsigned mask_perm = 7;   /* no MASK entry: group class unbounded */
    int found_group = 0;

    if (!acl || alen < 4 + 3 * 8 || (alen - 4) % 8) {
        unsigned shift = c->uid == m->uid ? 6 :
                         acreds_in_group(c, m->gid) ? 3 : 0;
        return (((unsigned)m->mode >> shift) & want) == want ? 0 : -1;
    }
    base = acl + 4;
    nent = (alen - 4) / 8;
    for (i = 0; i < nent; i++)
        if (acl_ent_tag(base + i * 8) == ACL_MASK) {
            mask_perm = acl_ent_perm(base + i * 8);
            break;
        }
    for (i = 0; i < nent; i++) {
        const uint8_t *e = base + i * 8;
        unsigned perm = acl_ent_perm(e);
        uint32_t id = acl_ent_id(e);
        switch (acl_ent_tag(e)) {
        case ACL_USER_OBJ:
            if (c->uid == m->uid)
                return (perm & want) == want ? 0 : -1;
            break;
        case ACL_USER:
            if (id == c->uid)
                return (perm & mask_perm & want) == want ? 0 : -1;
            break;
        case ACL_GROUP_OBJ:
            if (acreds_in_group(c, m->gid)) {
                found_group = 1;
                if ((perm & mask_perm & want) == want) return 0;
            }
            break;
        case ACL_GROUP:
            if (acreds_in_group(c, id)) {
                found_group = 1;
                if ((perm & mask_perm & want) == want) return 0;
            }
            break;
        case ACL_MASK:
            break;
        case ACL_OTHER:
            if (found_group) return -1;
            return (perm & want) == want ? 0 : -1;
        default:
            return -1;
        }
    }
    return -1;   /* no OTHER entry: malformed */
}

/* the root directory has no record: fixed meta (matches invf_getattr) */
static void root_meta(invfs_meta_pub *m)
{
    memset(m, 0, sizeof *m);
    m->type = INVFS_ITYP_DIR;
    m->mode = 0755;
    m->uid = 0;
    m->gid = 0;
    m->nlink = 2;
}

/* mode+ACL check for one existing path; want = R_OK|W_OK|X_OK bits
 * (0 = existence only). 0 ok, -EACCES denied, -ENOENT missing. */
static int perm_check_cred(const struct acreds *c, const char *path,
                           unsigned want)
{
    invfs_meta_pub m;
    char ename[300];
    int is_root = strcmp(path, "/") == 0;

    if (is_root) {
        root_meta(&m);
    } else if (!meta_for_path(path, ename, sizeof ename, &m)) {
        return -ENOENT;
    }
    if (c->bypass || !want)
        return 0;
    /* fetch the access ACL (the root has no record, hence none) */
    {
        uint8_t acl[INVFS_META_XATTR_MAX];
        size_t alen = 0;
        const uint8_t *aclp = NULL;
        if (!is_root) {
            uint64_t ino;
            size_t vlen = sizeof acl;
            pthread_mutex_lock(&g_io_lock);
            ino = g_vol ? vol_find(g_vol, ename) : 0;
            if (ino && vol_get_xattr(g_vol, ino, XATTR_ACL_ACCESS,
                                     acl, &vlen) == 0) {
                alen = vlen;
                aclp = acl;
            }
            pthread_mutex_unlock(&g_io_lock);
        }
        return acl_eval(&m, aclp, alen, c, want) == 0 ? 0 : -EACCES;
    }
}

static int perm_check(const char *path, unsigned want)
{
    struct acreds c;
    acreds_get(&c);
    return perm_check_cred(&c, path, want);
}

/* X_OK on every intermediate directory component of a FUSE path (the final
 * component is each op's own business). Bypass callers return before any
 * record I/O, so the mounting user's hot path is unchanged. */
static int perm_check_traversal_cred(const struct acreds *c, const char *path)
{
    const char *p = path[0] == '/' ? path + 1 : path;
    const char *s;
    if (c->bypass) return 0;
    for (s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
        char comp[300];
        char ename[300];
        invfs_meta_pub m;
        size_t n = (size_t)(s - p);
        int rc;
        if (n == 0 || n >= sizeof comp) continue;
        memcpy(comp, p, n);
        comp[n] = 0;
        if (!meta_for_path(comp, ename, sizeof ename, &m))
            return -ENOENT;
        if (m.type != INVFS_ITYP_DIR)
            return -ENOTDIR;
        rc = perm_check_cred(c, comp, X_OK);
        if (rc) return rc;
    }
    return 0;
}

static int perm_check_traversal(const char *path)
{
    struct acreds c;
    acreds_get(&c);
    return perm_check_traversal_cred(&c, path);
}

/* `want` on the parent directory of a FUSE path ("/x" -> "/") */
static int perm_check_parent_cred(const struct acreds *c, const char *path,
                                  unsigned want)
{
    char parent[300];
    const char *p = path[0] == '/' ? path + 1 : path;
    const char *s = strrchr(p, '/');
    if (c->bypass) return 0;
    if (!s) return perm_check_cred(c, "/", want);
    {
        size_t n = (size_t)(s - p);
        if (n == 0 || n >= sizeof parent) return -ENAMETOOLONG;
        memcpy(parent, p, n);
        parent[n] = 0;
    }
    return perm_check_cred(c, parent, want);
}

static int perm_check_parent(const char *path, unsigned want)
{
    struct acreds c;
    acreds_get(&c);
    return perm_check_parent_cred(&c, path, want);
}

/* +t directory rule for unlink/rmdir/rename: in a sticky dir only the
 * entry's owner, the dir's owner or a bypass caller may remove/rename an
 * entry (Linux: EPERM). */
static int perm_check_sticky(const struct acreds *c, const char *path)
{
    invfs_meta_pub em, pm;
    char ename[300];
    const char *p = path[0] == '/' ? path + 1 : path;
    const char *s = strrchr(p, '/');

    if (c->bypass) return 0;
    if (!meta_for_path(path, ename, sizeof ename, &em))
        return -ENOENT;
    if (!s) {
        root_meta(&pm);
    } else {
        char parent[300];
        size_t n = (size_t)(s - p);
        if (n == 0 || n >= sizeof parent) return -ENAMETOOLONG;
        memcpy(parent, p, n);
        parent[n] = 0;
        if (!meta_for_path(parent, ename, sizeof ename, &pm))
            return -ENOENT;
    }
    if (!(pm.mode & 01000)) return 0;
    if (c->uid == pm.uid || c->uid == em.uid) return 0;
    return -EPERM;
}

/* fetch the default ACL of path's parent directory. 1 = present (valid
 * blob in buf, *len bytes), 0 = none. The root has no default ACL.
 * Call without g_io_lock held. */
static int parent_default_acl(const char *path, uint8_t *buf, size_t *len)
{
    char anchor[300];
    const char *p = path[0] == '/' ? path + 1 : path;
    const char *s = strrchr(p, '/');
    uint64_t ino;
    size_t vlen;
    int rc;

    if (!s) return 0;                    /* parent is "/" */
    {
        size_t n = (size_t)(s - p);
        if (n + 2 > sizeof anchor) return 0;
        memcpy(anchor, p, n);
        anchor[n] = '/';                 /* the dir anchor record name */
        anchor[n + 1] = 0;
    }
    vlen = INVFS_META_XATTR_MAX;         /* bounded by the TLV budget */
    pthread_mutex_lock(&g_io_lock);
    ino = g_vol ? vol_find(g_vol, anchor) : 0;
    rc = ino ? vol_get_xattr(g_vol, ino, XATTR_ACL_DEFAULT, buf, &vlen) : -1;
    pthread_mutex_unlock(&g_io_lock);
    if (rc != 0 || !acl_blob_valid(buf, vlen))
        return 0;                        /* absent (or corrupt: treat as none) */
    *len = vlen;
    return 1;
}

/* posix_acl_create_masq: fold the create mode into an inherited ACL (in
 * place). Base entries' perms are ANDed with the matching mode triad; the
 * mode's group bits come back as the effective group class (MASK when
 * present, else GROUP_OBJ). Returns 1 when the result still carries named
 * entries (the blob must be stored), 0 when it degraded to exactly the
 * mode (no xattr needed). */
static int acl_create_masq(uint8_t *acl, size_t alen, unsigned *mode_p)
{
    size_t nent = (alen - 4) / 8, i;
    uint8_t *base = acl + 4;
    uint8_t *group_obj = NULL, *mask_obj = NULL;
    unsigned mode = *mode_p;
    int not_equiv = 0;

    for (i = 0; i < nent; i++) {
        uint8_t *e = base + i * 8;
        unsigned perm = acl_ent_perm(e);
        switch (acl_ent_tag(e)) {
        case ACL_USER_OBJ:
            perm &= (mode >> 6) & 7;
            acl_ent_set_perm(e, (uint16_t)perm);
            mode = (mode & ~0700u) | (perm << 6);
            break;
        case ACL_USER:
        case ACL_GROUP:
            not_equiv = 1;
            break;
        case ACL_GROUP_OBJ:
            group_obj = e;
            break;
        case ACL_OTHER:
            perm &= mode & 7;
            acl_ent_set_perm(e, (uint16_t)perm);
            mode = (mode & ~07u) | perm;
            break;
        case ACL_MASK:
            mask_obj = e;
            not_equiv = 1;
            break;
        default:
            return -1;
        }
    }
    {
        uint8_t *gce = mask_obj ? mask_obj : group_obj;
        unsigned perm;
        if (!gce) return -1;
        perm = acl_ent_perm(gce) & ((mode >> 3) & 7);
        acl_ent_set_perm(gce, (uint16_t)perm);
        mode = (mode & ~070u) | (perm << 3);
    }
    *mode_p = mode & 0777u;
    return not_equiv;
}

/* posix_acl_chmod_masq: fold a chmod into the stored access ACL. Base
 * entries are SET from the mode (not ANDed); the group-class slot is MASK
 * when one is present, else GROUP_OBJ. Returns 1 when named entries
 * remain (store the blob back), 0 when the ACL is now exactly the mode
 * (the kernel drops the xattr in that case -- so do we). */
static int acl_chmod_masq(uint8_t *acl, size_t alen, unsigned mode)
{
    size_t nent = (alen - 4) / 8, i;
    uint8_t *base = acl + 4;
    uint8_t *group_obj = NULL, *mask_obj = NULL;
    int not_equiv = 0;

    for (i = 0; i < nent; i++) {
        uint8_t *e = base + i * 8;
        switch (acl_ent_tag(e)) {
        case ACL_USER_OBJ:
            acl_ent_set_perm(e, (uint16_t)((mode >> 6) & 7));
            break;
        case ACL_USER:
        case ACL_GROUP:
            not_equiv = 1;
            break;
        case ACL_GROUP_OBJ:
            group_obj = e;
            break;
        case ACL_MASK:
            mask_obj = e;
            not_equiv = 1;
            break;
        case ACL_OTHER:
            acl_ent_set_perm(e, (uint16_t)(mode & 7));
            break;
        default:
            return -1;
        }
    }
    {
        uint8_t *gce = mask_obj ? mask_obj : group_obj;
        if (!gce) return -1;
        acl_ent_set_perm(gce, (uint16_t)((mode >> 3) & 7));
    }
    return not_equiv;
}

/* posix_acl_update_mode: after an access ACL is set, st_mode's 9 perm bits
 * mirror it (group bits = MASK when present, else GROUP_OBJ). Special
 * bits (setuid/setgid/sticky) are kept. The kernel does this inside
 * ->set_acl on real filesystems; the generic-xattr transport never calls
 * it for FUSE, so the daemon syncs here. */
static unsigned acl_sync_mode(const uint8_t *acl, size_t alen,
                              unsigned old_mode)
{
    size_t nent = (alen - 4) / 8, i;
    const uint8_t *base = acl + 4;
    unsigned mode = old_mode & ~0777u;
    int gobj_perm = -1, mask_perm = -1;

    for (i = 0; i < nent; i++) {
        const uint8_t *e = base + i * 8;
        switch (acl_ent_tag(e)) {
        case ACL_USER_OBJ:
            mode |= (unsigned)(acl_ent_perm(e) & 7) << 6;
            break;
        case ACL_GROUP_OBJ:
            gobj_perm = acl_ent_perm(e) & 7;
            break;
        case ACL_MASK:
            mask_perm = acl_ent_perm(e) & 7;
            break;
        case ACL_OTHER:
            mode |= (unsigned)(acl_ent_perm(e) & 7);
            break;
        default:
            break;
        }
    }
    mode |= (unsigned)(mask_perm >= 0 ? mask_perm :
                       gobj_perm >= 0 ? gobj_perm : 0) << 3;
    return mode;
}

/* ---- FUSE operations ---- */
static int invf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    {
        /* foreign uids: path walk needs X on every component (the kernel
         * never asks us -- without default_permissions nothing does) */
        int rc = perm_check_traversal(path);
        if (rc) return rc;
    }
    if (strcmp(path, "/") == 0) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_uid = 0;
        st->st_gid = 0;
        return 0;
    }
    {
        char ename[300];
        invfs_meta_pub m;
        uint64_t size = 0, ctime = 0;
        /* symlink targets must NOT be followed here: getattr on the link
         * itself is how the kernel discovers S_IFLNK */
        const char *name = path[0] == '/' ? path + 1 : path;
        if (!snapshot_entry(name, NULL, &size, &ctime)) {
            int is_dir = 0;
            pthread_mutex_lock(&g_io_lock);
            is_dir = g_vol ? vol_is_dir(g_vol, name) : 0;
            pthread_mutex_unlock(&g_io_lock);
            if (!is_dir) return -ENOENT;
        }
        if (!meta_for_path(path, ename, sizeof ename, &m)) {
            int isd;
            pthread_mutex_lock(&g_io_lock);
            isd = g_vol ? vol_is_dir(g_vol, name) : 0;
            pthread_mutex_unlock(&g_io_lock);
            meta_defaults(name, size, &m);
            if (isd) { m.type = INVFS_ITYP_DIR; m.nlink = 2; }
        }
        fill_stat_from_meta(st, &m, size, ctime);
        return 0;
    }
}

static int invf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    const char *dir = path[0] == '/' && path[1] ? path + 1 : "";
    {
        /* listing needs R on the dir itself, X on the way there */
        int rc = perm_check_traversal(path);
        if (rc) return rc;
        rc = perm_check(path, R_OK);
        if (rc) return rc;
    }
    /* grow-on-demand: the old fixed ents[4096] (~1.1 MB stack, silent
     * truncation) dropped entries in large dirs like /usr/share (audit H7) */
    int cap = 1024, n = 0;
    invfs_dirent *ents = NULL;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    if (dir[0] && !vol_is_dir(g_vol, dir)) {
        pthread_mutex_unlock(&g_io_lock);
        return -ENOENT;
    }
    for (;;) {
        free(ents);
        ents = (invfs_dirent *)malloc((size_t)cap * sizeof(invfs_dirent));
        if (!ents) { pthread_mutex_unlock(&g_io_lock); return -ENOMEM; }
        n = vol_list_dir(g_vol, dir, ents, cap);
        if (n < cap || n < 0) break;
        cap *= 2;   /* possibly truncated: retry with a bigger buffer */
    }
    pthread_mutex_unlock(&g_io_lock);
    int i;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (i = 0; i < n; i++) {
        /* internal names (WP10 batch owner "\x01tzb") stay hidden */
        if ((unsigned char)ents[i].name[0] == 0x01) continue;
        /* no cached stat: let the kernel re-stat each entry through
         * getattr, which knows about v2 types (dirs, symlinks, devices) */
        filler(buf, ents[i].name, NULL, 0, 0);
    }
    free(ents);
    return 0;
}

static int invf_mkdir(const char *path, mode_t mode)
{
    int rc;
    struct fuse_context *ctx = fuse_get_context();
    /* inheritance: the parent's default ACL (if any) becomes the child's
     * access ACL masked by the create mode, and the child's own default */
    uint8_t aacl[INVFS_META_XATTR_MAX], dacl[INVFS_META_XATTR_MAX];
    size_t aalen = 0, dlen = 0;
    mode_t cmode = mode & 07777;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    rc = perm_check_traversal(path);
    if (rc) return rc;
    rc = perm_check_parent(path, W_OK | X_OK);
    if (rc) return rc;
    if (parent_default_acl(path, dacl, &dlen)) {
        unsigned mm = cmode;
        memcpy(aacl, dacl, dlen);
        if (acl_create_masq(aacl, dlen, &mm) > 0)
            aalen = dlen;              /* named entries remain: store */
        cmode = (mode_t)(mm & 0777);
    }
    pthread_mutex_lock(&g_io_lock);
    vol_ensure_path(g_vol, path + 1);
    uint64_t d = vol_mkdir(g_vol, path + 1);
    if (d) {
        /* stamp real owner/mode on the "dir/" anchor record */
        invfs_meta_pub m;
        char anchor[300];
        snprintf(anchor, sizeof anchor, "%s/", path + 1);
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_DIR;
        m.mode = cmode;
        if (!m.mode) m.mode = 0755;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 2;
        m.mtime = m.atime = (int64_t)time(NULL);
        if (!vol_apply_meta(g_vol, anchor, &m))
            fprintf(stderr, "invf: mkdir stamp FAILED %s (area full?)\n", anchor);
        if (dlen) {
            uint64_t ino2 = vol_find(g_vol, anchor);
            if (ino2) {
                if (aalen &&
                    vol_set_xattr(g_vol, ino2, XATTR_ACL_ACCESS, aacl, aalen) != 0)
                    fprintf(stderr, "invf: mkdir ACL inherit FAILED %s\n", anchor);
                if (vol_set_xattr(g_vol, ino2, XATTR_ACL_DEFAULT, dacl, dlen) != 0)
                    fprintf(stderr, "invf: mkdir defACL inherit FAILED %s\n", anchor);
            }
        }
        table_sync_one_locked(anchor);
    }
    pthread_mutex_unlock(&g_io_lock);
    rc = d ? 0 : -EEXIST;
    return rc;
}

static int invf_rmdir(const char *path)
{
    int rc;
    struct acreds c;
    if (g_tt) return -EROFS;   /* WP24-lite: time-travel views never mutate */
    acreds_get(&c);
    rc = perm_check_traversal_cred(&c, path);
    if (rc) return rc;
    rc = perm_check_parent_cred(&c, path, W_OK | X_OK);
    if (rc) return rc;
    rc = perm_check_sticky(&c, path);
    if (rc) return rc;
    pthread_mutex_lock(&g_io_lock);
    rc = vol_rmdir(g_vol, path + 1);
    if (rc == 0) {
        char anc[300];
        snprintf(anc, sizeof anc, "%s/", path + 1);
        table_remove_name(anc);
    }
    pthread_mutex_unlock(&g_io_lock);
    return rc == 0 ? 0 : (rc == -2 ? -ENOTEMPTY : -ENOENT);
}

/* ---- write context (WP4b streaming ranged writes) ----
 * A handle stages at most one partially-written 64K segment (the tail
 * window) and streams everything else straight into an engine write
 * session (vol_write_begin/vol_write_range): per-handle memory is O(1)
 * plus O(segments) AST metadata in the engine, never O(filesize).
 * The engine session forks the file under a new inode id and commits it
 * (new record, then old retire) at flush/fsync/release. */
typedef struct wctx {
    char name[256];
    invfs_wsession *ws;      /* lazily begun on first write/truncate */
    int have_meta;           /* stamp this meta after the commit */
    invfs_meta_pub meta;
    /* sub-segment tail: bytes in [tail_lo,tail_hi) of segment tail_seg
     * are staged in tail[] and not yet written to the session */
    uint8_t *tail;
    uint32_t tail_seg, tail_lo, tail_hi;
    struct wctx *next_dirty;   /* g_dirty list link (live sessions) */
} wctx;

typedef struct tctx {
    char name[256];
    int have_meta;
    invfs_meta_pub meta;
} tctx;

/* Active write sessions, newest first: lets invf_read serve .write data
 * before commit (a clean page the kernel evicted must never come back
 * stale). Requires g_io_lock for all list ops. */
static wctx *g_dirty;

static void dirty_add_locked(wctx *c)
{
    wctx *p;
    for (p = g_dirty; p; p = p->next_dirty)
        if (p == c) return;    /* already listed */
    c->next_dirty = g_dirty;
    g_dirty = c;
}

static void dirty_del_locked(wctx *c)
{
    wctx **pp = &g_dirty;
    while (*pp) {
        if (*pp == c) { *pp = c->next_dirty; c->next_dirty = NULL; return; }
        pp = &(*pp)->next_dirty;
    }
}

static wctx *dirty_find_locked(const char *name)
{
    wctx *p;
    for (p = g_dirty; p; p = p->next_dirty)
        if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

/* push the staged tail window into the session. Requires g_io_lock.
 * On error the window is kept staged (a retry rewrites the same bytes --
 * idempotent -- and the failure surfaces to the next writer/flusher). */
static int wctx_flush_tail_locked(wctx *c)
{
    int rc;
    if (!c->tail || c->tail_hi <= c->tail_lo) return 0;
    rc = vol_write_range(c->ws,
                         (uint64_t)c->tail_seg * INVFS_SEGMENT_SIZE +
                         c->tail_lo,
                         c->tail + c->tail_lo, c->tail_hi - c->tail_lo);
    if (rc == 0) c->tail_lo = c->tail_hi = 0;
    return rc;
}

/* begin the engine write session on first use. Requires g_io_lock. */
static int wctx_ensure_ws_locked(wctx *c)
{
    if (c->ws) return 0;
    if (!g_vol || !vol_write_begin(g_vol, c->name, 0, &c->ws) || !c->ws)
        return -1;
    dirty_add_locked(c);
    if (!c->have_meta) {
        /* carry current metadata so the commit can refresh mtime without
         * losing mode/owner (the engine also carries the raw ext; this
         * stamp is what updates mtime) */
        uint64_t ino = vol_find(g_vol, c->name);
        if (ino && vol_get_meta(g_vol, ino, &c->meta) == 0)
            c->have_meta = 1;
    }
    return 0;
}

static int invf_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    uint64_t ino = 0, size64 = 0, ctime;
    int got;
    wctx *w;
    (void)fi;
    (void)is_temp_path;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    /* WP22c: on an io-latched volume (a flush/sync failed THIS session)
     * the in-RAM state may describe bytes that never reached the device;
     * serving them would be the silent-corruption flavor of the same
     * bug. Reads fail loudly until the remount that re-anchors truth.
     * (A volume that merely OPENED dirty stays readable -- inspection.) */
    if (vol_io_latched(g_vol)) {
        pthread_mutex_unlock(&g_io_lock);
        return -EIO;
    }
    /* a live write session for this path owns the freshest bytes:
     * .write data must stay readable before commit even if the kernel
     * evicted a clean (already-writtenback) page. Flush staged tails of
     * every session on this name first so the session view is complete. */
    for (w = g_dirty; w; w = w->next_dirty)
        if (strcmp(w->name, path + 1) == 0)
            wctx_flush_tail_locked(w);
    w = dirty_find_locked(path + 1);
    if (w) {
        got = vol_write_read(w->ws, (uint64_t)offset, (uint8_t *)buf, size);
        pthread_mutex_unlock(&g_io_lock);
        return got < 0 ? -EIO : got;
    }
    pthread_mutex_unlock(&g_io_lock);
    if (!snapshot_entry(path + 1, &ino, &size64, &ctime))
        return -ENOENT;
    if ((uint64_t)offset >= size64)
        return 0;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    got = vol_read_range(g_vol, ino, (uint64_t)offset, size, buf);
    pthread_mutex_unlock(&g_io_lock);
    if (got < 0)
        return -EIO;
    return got;
}

static void table_rebuild_locked(void)
{
    /* free old table, rescan */
    free(g_entries);
    g_entries = NULL; g_nentries = 0; g_cap = 0;
    build_file_table();
}

static int invf_open(const char *path, struct fuse_file_info *fi)
{
    struct acreds c;
    if (strcmp(path, "/") == 0)
        return -EISDIR;
    if (!snapshot_entry(path + 1, NULL, NULL, NULL))
        return -ENOENT;
    acreds_get(&c);
    /* O_PATH is a handle-only open (stat material): no data access, hence
     * no permission gate (POSIX: O_PATH needs none) */
    if (!c.bypass && !(fi->flags & O_PATH)) {
        /* mode+ACL gate on the open mode (read() / write() are not
         * re-checked per call -- POSIX checks at open) */
        unsigned want = ((fi->flags & O_ACCMODE) == O_WRONLY) ? (unsigned)W_OK :
                        ((fi->flags & O_ACCMODE) == O_RDWR) ?
                        (unsigned)(R_OK | W_OK) : (unsigned)R_OK;
        int rc = perm_check_traversal_cred(&c, path);
        if (rc) return rc;
        rc = perm_check_cred(&c, path, want);
        if (rc) return rc;
    }
    __sync_fetch_and_add(&g_open_handles, 1);
    if ((fi->flags & O_ACCMODE) == O_RDONLY) {
        /* WP17: keep page cache across open/close. Every content change
         * flows through this kernel mount (single-mount model; sweeps keep
         * logical bytes identical), so the kernel always knows when to
         * invalidate -- same argument as the pre-existing RDWR branch. */
        fi->keep_cache = 1;
        return 0;  /* plain read open */
    }
    /* WP22d: refuse a write open on a non-writable volume (latched /
     * read-only) HERE, at open: with WRITEBACK_CACHE, a write() buffers
     * payload in the kernel page cache before this daemon is asked, and a
     * later-refused write leaves exactly those un-acked bytes in the cache
     * -- the next read would serve the phantom. Failing the open is the
     * POSIX EROFS shape and keeps the phantom out of the cache entirely. */
    if (!vol_write_enabled(g_vol)) {
        __sync_fetch_and_sub(&g_open_handles, 1);
        return -EROFS;
    }
    /* read-write open: allocate a write context. Content is NOT loaded:
     * the engine session (begun lazily at the first write) forks the
     * file's segment layout incrementally, so memory stays bounded no
     * matter how large the file is (WP4b). */
    {
        wctx *c = (wctx *)calloc(1, sizeof(wctx));
        if (!c) return -ENOMEM;
        strncpy(c->name, path + 1, 255);
        fi->fh = (uint64_t)(uintptr_t)c;
        fi->keep_cache = 1;
    }
    if (fi->flags & O_TRUNC) {
        /* truncate to empty; the old file's transcode siblings describe bytes
           that are gone, so vol_replace_file drops them with it */
        invfs_meta_pub keep;
        int have_keep = 0;
        pthread_mutex_lock(&g_io_lock);
        {
            uint64_t ino = vol_find(g_vol, path + 1);
            if (ino && vol_get_meta(g_vol, ino, &keep) == 0)
                have_keep = 1;
        }
        vol_replace_file(g_vol, path + 1, NULL, 0);
        if (have_keep)
            vol_apply_meta(g_vol, path + 1, &keep);
        table_sync_one_locked(path + 1);
        pthread_mutex_unlock(&g_io_lock);
    }
    return 0;
}

static int invf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int temp = is_temp_path(path);
    fprintf(stderr, "invf_create: path=%s temp=%d mode=%o fi->flags=%o\n", path, temp, mode, fi->flags);
    wctx *c;
    struct fuse_context *ctx = fuse_get_context();
    uint8_t aacl[INVFS_META_XATTR_MAX];
    size_t aalen = 0;
    mode_t cmode = mode & 07777;
    (void)temp;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    fprintf(stderr, "[create] %s\n", path);
    if (strcmp(path, "/") == 0)
        return -EISDIR;
    {
        int rc;
        struct acreds cr;
        acreds_get(&cr);
        if (!cr.bypass) {
            rc = perm_check_traversal_cred(&cr, path);
            if (rc) return rc;
            rc = perm_check_parent_cred(&cr, path, W_OK | X_OK);
            if (rc) return rc;
            /* O_CREAT over an existing file is an open: needs W on it.
             * (The daemon replaces the record either way -- pre-existing
             * semantics -- but never for a caller the file would refuse.) */
            if (snapshot_entry(path + 1, NULL, NULL, NULL)) {
                rc = perm_check_cred(&cr, path, W_OK);
                if (rc) return rc;
            }
        }
    }
    if ((fi->flags & O_EXCL) && snapshot_entry(path + 1, NULL, NULL, NULL))
        return -EEXIST;
    /* default-ACL inheritance: child access ACL = parent's default,
     * masked by the create mode (posix_acl_create_masq) */
    {
        size_t dlen = 0;
        if (parent_default_acl(path, aacl, &dlen)) {
            unsigned mm = cmode;
            if (acl_create_masq(aacl, dlen, &mm) > 0)
                aalen = dlen;
            cmode = (mode_t)(mm & 0777);
        }
    }
    /* create empty file immediately so getattr-after-create works */
    pthread_mutex_lock(&g_io_lock);
    if (vol_replace_file(g_vol, path + 1, NULL, 0) == 0) {
        pthread_mutex_unlock(&g_io_lock);
        return -ENOSPC;
    }
    {
        /* stamp owner/mode right away; re-stamped at flush (the replace
         * in commit_wctx builds a fresh record) */
        invfs_meta_pub m;
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_REG;
        m.mode = cmode;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 1;
        m.mtime = m.atime = (int64_t)time(NULL);
        if (!vol_apply_meta(g_vol, path + 1, &m))
            fprintf(stderr, "invf: mknod stamp FAILED %s\n", path);
        if (aalen) {
            uint64_t ino2 = vol_find(g_vol, path + 1);
            if (ino2 &&
                vol_set_xattr(g_vol, ino2, XATTR_ACL_ACCESS, aacl, aalen) != 0)
                fprintf(stderr, "invf: create ACL inherit FAILED %s\n", path);
        }
        table_sync_one_locked(path + 1);
    }
    pthread_mutex_unlock(&g_io_lock);
    c = (wctx *)calloc(1, sizeof(wctx));
    if (!c) return -ENOMEM;
    strncpy(c->name, path + 1, 255);
    c->have_meta = 1;
    {
        struct fuse_context *cx = fuse_get_context();
        c->meta.type = INVFS_ITYP_REG;
        c->meta.mode = cmode;   /* inheritance-masked mode */
        c->meta.uid = cx ? cx->uid : 0;
        c->meta.gid = cx ? cx->gid : 0;
        c->meta.nlink = 1;
        c->meta.mtime = c->meta.atime = (int64_t)time(NULL);
    }
    fi->fh = (uint64_t)(uintptr_t)c;
    fi->keep_cache = 1;
    return 0;
}

static int invf_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    size_t done = 0;
    (void)is_temp_path;
    if (!vol_write_enabled(g_vol))
        return -ENOSPC;
    /* WP22a: the 4 GB wall is gone -- the commit writes a v2 recipe header
     * (u64 file_size) once v1's u32 overflows. MAX_FILE_SIZE (1 TB) remains
     * as the format sanity bound. */
    if ((uint64_t)offset + size > MAX_FILE_SIZE)
        return -EFBIG;
    (void)path;
    if (!c) return -EBADF;
    /* mt loop: the per-handle state is mutable shared state once the kernel
     * can dispatch two writes of one inode to different worker threads;
     * keep it under the same big lock as everything else */
    pthread_mutex_lock(&g_io_lock);
    if (wctx_ensure_ws_locked(c) != 0) {
        pthread_mutex_unlock(&g_io_lock);
        return -ENOSPC;
    }
    while (done < size) {
        uint64_t off = (uint64_t)offset + done;
        uint32_t seg = (uint32_t)(off / INVFS_SEGMENT_SIZE);
        uint32_t soff = (uint32_t)(off % INVFS_SEGMENT_SIZE);
        size_t piece = INVFS_SEGMENT_SIZE - soff;
        int rc = 0;
        if (piece > size - done) piece = size - done;

        if (soff == 0 && piece == INVFS_SEGMENT_SIZE) {
            /* full segment: straight to the engine, no staging */
            rc = wctx_flush_tail_locked(c);
            if (rc == 0)
                rc = vol_write_range(c->ws, off,
                                     (const uint8_t *)buf + done, piece);
        } else {
            /* sub-segment window: stage in the 64K tail buffer so that
             * consecutive small writes cost one RMW per segment, not one
             * per write call */
            if (!c->tail) {
                c->tail = (uint8_t *)malloc(INVFS_SEGMENT_SIZE);
                if (!c->tail) { pthread_mutex_unlock(&g_io_lock); return -ENOMEM; }
                c->tail_lo = c->tail_hi = 0;
            }
            if (c->tail_hi > c->tail_lo && c->tail_seg != seg)
                rc = wctx_flush_tail_locked(c);
            if (rc == 0 && c->tail_hi > c->tail_lo &&
                (soff > c->tail_hi || soff + piece < c->tail_lo))
                rc = wctx_flush_tail_locked(c);   /* disjoint window */
            if (rc != 0) break;
            if (c->tail_hi <= c->tail_lo) {
                c->tail_seg = seg;
                c->tail_lo = soff;
                c->tail_hi = soff;
            }
            memcpy(c->tail + soff, buf + done, piece);
            if (soff < c->tail_lo) c->tail_lo = soff;
            if (soff + piece > c->tail_hi)
                c->tail_hi = (uint32_t)(soff + piece);
        }
        if (rc != 0) {
            pthread_mutex_unlock(&g_io_lock);
            return rc == -2 ? -ENOSPC : -EIO;
        }
        done += piece;
    }
    pthread_mutex_unlock(&g_io_lock);
    return (int)size;
}

/* background sweep (FUSE): OPT-IN via INVFS_SWEEP_INTERVAL=<seconds>.
 * Automatic transcoding of a root filesystem in the background was the
 * wrong default (constant HDD wakeups, surprise CPU, churn during
 * emerge). Default OFF: sweeping happens explicitly (invf-sweep CLI)
 * or when this env var is set. */
static void on_sweep_signal(int sig)
{
    (void)sig;
    g_sweep_now = 1;
}

/* Manual full pass: collect every data file and sweep it with per-file
 * locking (system stays responsive). Progress to stderr (= console in
 * the guest init context). Trigger: kill -USR1 $(pidof invf-fuse) or
 * /usr/local/bin/invf-sweep. INVFS_SWEEP_INTERVAL=<sec> additionally
 * enables the periodic mode.
 *
 * arm_ckp (WP26, watermark-triggered passes only): bracket the walk with
 * the WP21 checkpoint machinery, same shape as the offline invf-sweep --
 * vol_ckp_begin BEFORE the walk (a previous pass's checkpoint is
 * auto-realized after the new arm, so the rollback window is always the
 * LAST sweep) and vol_ckp_end after it (the retention registry holds the
 * retired blocks for invf-rollback). A pass that retires nothing arms
 * nothing (vol_ckp_end disarms an identity). The SIGUSR1 path passes 0
 * and keeps its historic uncheckpointed behavior. */
static void invf_sweep_worker(int arm_ckp)
{
    uint64_t *ids = NULL;
    size_t max = 300000, n, i;
    long saved = 0, swept = 0, skipped = 0, failed = 0;
    int armed = 0;

    ids = malloc(max * sizeof(*ids));
    if (!ids) return;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); free(ids); return; }
    if (arm_ckp) {
        armed = vol_ckp_begin(g_vol, 0);   /* 1 armed, 0 declined, -1 error */
        if (armed < 0) {
            fprintf(stderr, "[watermark] checkpoint arm failed; sweeping "
                            "without one\n");
            armed = 0;
        }
        vol_heat_sweep_begin(g_vol);   /* one decay pass per sweep run */
    }
    n = vol_collect_sweepables(g_vol, ids, max);
    fprintf(stderr, "[sweep] %s pass started: %zu files\n",
            arm_ckp ? "watermark" : "manual", n);
    for (i = 0; i < n; i++) {
        int rc;
        if (g_shutdown || !g_vol) break;
        if (arm_ckp) {
            /* the daemon's own driver (same as the pending drain):
             * policy/pack-aware -- the plain vol_sweep_file floor defers
             * everything on small images (WP16b margin) and would never
             * relieve the watermark's pressure */
            char nm[256];
            if (vol_sweep_name_of(g_vol, ids[i], nm, sizeof nm)) {
                rc = vol_sweep_one(g_vol, ids[i], nm);
                /* one rc: 0 = nothing to do, >0 = swept/deferred, <0 = err */
                if (rc > 0)       { swept++;   }
                else if (rc == 0) { skipped++; }
                else              { failed++; }
            } else {
                skipped++;   /* deleted between collect and walk */
            }
        } else {
            rc = vol_sweep_file(g_vol, ids[i]);
            if (rc == 0)      { swept++;   }
            else if (rc == 1) { skipped++; }
            else              { failed++; }
        }
        if ((i+1) % 250 == 0)
            fprintf(stderr, "[sweep] %zu/%zu done=%ld skip=%ld fail=%ld\n",
                    i+1, n, swept, skipped, failed);
        pthread_mutex_unlock(&g_io_lock);
        usleep(500);                       /* let the system breathe */
        pthread_mutex_lock(&g_io_lock);
    }
    if (g_vol) {
        if (arm_ckp)
            vol_heat_promote(g_vol);   /* extract read-hot batch members */
        vol_tz_flush(g_vol);   /* seal anything the walk deferred */
        if (armed) {
            uint64_t rr = 0, rb = 0;
            if (vol_ckp_end(g_vol, &rr, &rb) != 0)
                fprintf(stderr, "[watermark] checkpoint registry write "
                                "failed (the checkpoint itself is intact)\n");
            else if (rb)
                fprintf(stderr, "[watermark] checkpoint: %llu retained "
                                "blocks held for rollback (%llu ranges)\n",
                        (unsigned long long)rb, (unsigned long long)rr);
        }
        vol_flush(g_vol);
    }
    pthread_mutex_unlock(&g_io_lock);
    fprintf(stderr, "[sweep] DONE files=%zu swept=%ld skipped=%ld failed=%ld\n",
            n, swept, skipped, failed);
    free(ids);
}

static void *fuse_sweep_thread(void *arg)
{
    (void)arg;
    signal(SIGUSR1, on_sweep_signal);
    const char *iv = getenv("INVFS_SWEEP_INTERVAL");
    int interval = iv ? atoi(iv) : 0;
    int tick = 0;
    /* WP26: RAW fill (blocks) at the end of the last watermark-kicked
     * pass; the next kick waits for the fill to rise above it. While a
     * pass's checkpoint is live its retention holds the retired blocks,
     * so the fill cannot drop until a later pass's realize-after-arm --
     * without this floor the daemon would chain passes (each
     * auto-realizing and finally disarming the previous checkpoint) and
     * destroy the rollback window it just created. 0 = no kick yet (or
     * the fill dropped below the mark: re-armed). */
    uint64_t wm_floor = 0;
    /* A checkpoint left live by a previous mount (watermark pass or CLI
     * sweep) still holds its retired blocks, so the fill reads high from
     * the start. Kicking on that stale reading would run a no-op walk
     * whose arm auto-realizes the old checkpoint and whose end disarms
     * the new one -- the rollback window would evaporate on a plain
     * remount. Seed the floor with the current fill instead: the next
     * pass needs genuinely NEW pressure. */
    if (g_raw_watermark > 0) {
        pthread_mutex_lock(&g_io_lock);
        if (g_vol && vol_ckp_armed(g_vol)) {
            uint64_t rf = 0, rt = 0;
            vol_zone_free(g_vol, &rf, &rt, NULL, NULL);
            if (rt) wm_floor = rt - rf;
        }
        pthread_mutex_unlock(&g_io_lock);
    }
    for (;;) {
        sleep(1);
        if (g_shutdown) break;
        if (g_sweep_now) {
            g_sweep_now = 0;
            if (!g_sweep_busy) {
                g_sweep_busy = 1;
                invf_sweep_worker(0);
                g_sweep_busy = 0;
                continue;
            }
        }
        if (interval > 0 && ++tick >= interval) {
            tick = 0;
            if (g_shutdown || g_open_handles != 0) continue;
        }
        pthread_mutex_lock(&g_io_lock);
        if (g_shutdown || !g_vol) {   /* unmount won the race */
            pthread_mutex_unlock(&g_io_lock);
            break;
        }
        if (vol_pending_count(g_vol) > 0) {
            int n = vol_sweep_pending(g_vol);
            if (n > 0) {
                vol_tz_flush(g_vol);   /* seal the partial text batch */
                vol_flush(g_vol);
                /* no table invalidation: sweeping rewrites block layout
                 * only -- name/inode_id/file_size are invariant, and the
                 * generic ZSTD path clones the record verbatim */
                fprintf(stderr, "[sweep] on-demand: processed %d pending\n", n);
            }
        }
        pthread_mutex_unlock(&g_io_lock);

        /* WP26: watermark-triggered early sweep -- the first rung of the
         * pressure ladder (the WP23 write path adapts effort; this rung
         * reclaims). Checked after each drain pass: RAW fill above
         * raw_watermark% kicks a checkpoint-armed full sweep pass. The
         * kick rearms only on RISING fill (a new high above the last
         * pass's exit fill); it never chains passes by itself, so the
         * last pass's checkpoint survives as the rollback window. Default
         * (no raw_watermark): lazy, this block is inert. */
        if (g_raw_watermark > 0 && !g_shutdown && !g_sweep_busy) {
            uint64_t rf = 0, rt = 0, fill = 0, after = 0;
            pthread_mutex_lock(&g_io_lock);
            if (g_vol) vol_zone_free(g_vol, &rf, &rt, NULL, NULL);
            pthread_mutex_unlock(&g_io_lock);
            if (rt) fill = rt - rf;
            if (rt && fill * 100 <= (uint64_t)g_raw_watermark * rt) {
                wm_floor = 0;               /* below the mark: re-arm */
            } else if (rt && fill > wm_floor) {
                fprintf(stderr, "[watermark] RAW fill %llu/%llu over %d%%: "
                                "kicking a sweep pass\n",
                        (unsigned long long)fill, (unsigned long long)rt,
                        g_raw_watermark);
                g_sweep_busy = 1;
                invf_sweep_worker(1);
                g_sweep_busy = 0;
                pthread_mutex_lock(&g_io_lock);
                if (g_vol) {
                    rf = rt = 0;
                    vol_zone_free(g_vol, &rf, &rt, NULL, NULL);
                    if (rt) after = rt - rf;
                }
                pthread_mutex_unlock(&g_io_lock);
                wm_floor = after;
            }
        }
    }
    return NULL;
}

/* Commit a write context's session into the volume. Shared by .flush,
 * .fsync and .release so that fsync() before a crash actually persists
 * written data, and so mmap-writeback traffic (which may arrive between
 * the last close() and the final release) is committed too. */
static int commit_wctx(wctx *c)
{
    int rc = 0;
    if (!c) return 0;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    if (!c->ws) { pthread_mutex_unlock(&g_io_lock); return 0; }
    vol_ensure_path(g_vol, c->name);   /* auto-create parent dirs */
    rc = wctx_flush_tail_locked(c);
    if (rc == 0) rc = vol_write_commit(c->ws);
    if (rc != 0) {
        /* late ENOSPC must be visible to the writer, never dropped
         * silently (audit H4; Dokan already reports STATUS_DISK_FULL).
         * The abort below rolls the volume back to the pre-session
         * state, so a failed commit never leaves a torn file. */
        fprintf(stderr, "invf: commit %s failed (%s)\n", c->name,
                rc == -2 ? "ENOSPC" : "io");
        vol_write_abort(c->ws);
        c->ws = NULL;
        dirty_del_locked(c);
        vol_flush(g_vol);
        table_sync_one_locked(c->name);
        pthread_mutex_unlock(&g_io_lock);
        return rc == -2 ? -ENOSPC : -EIO;
    }
    vol_write_abort(c->ws);   /* committed: frees the session memory only */
    c->ws = NULL;
    dirty_del_locked(c);
    /* re-stamp metadata with a fresh mtime (the commit carries the old
     * ext, so mode/owner/xattrs survive; this updates the write time) */
    if (c->have_meta) {
        c->meta.mtime = (int64_t)time(NULL);
        if (!vol_apply_meta(g_vol, c->name, &c->meta))
            fprintf(stderr, "invf: close stamp FAILED %s (area full?)\n", c->name);
    }
    vol_mark_pending(g_vol, vol_find(g_vol, c->name));   /* on-demand sweep */
    vol_flush(g_vol);
    table_sync_one_locked(c->name);
    pthread_mutex_unlock(&g_io_lock);
    return 0;
}

static int invf_flush(const char *path, struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    (void)path;
    (void)is_temp_path;
    return commit_wctx(c);
}

static int invf_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    int rc;
    (void)path; (void)datasync;   /* whole-volume flush covers both */
    rc = commit_wctx(c);
    if (rc == 0) {
        pthread_mutex_lock(&g_io_lock);
        /* fsync/fdatasync = durability contract: journal + bitmap + data
         * past a real storage barrier, not just into the OS page cache */
        rc = g_vol ? (vol_sync(g_vol) == 0 ? 0 : -EIO) : -EIO;
        if (rc != 0) {
            /* WP22c: the commit landed in-RAM but the barrier failed, so
             * the new content is past the durable anchor and will not
             * survive the remount. Drop the name from the table: a later
             * open() must fail (ENOENT) instead of being served the
             * un-acked bytes out of the kernel page cache (a failed write
             * is complete-or-absent, never a third state). The volume is
             * latched now; the next mount rebuilds the table from the
             * device. */
            table_remove_name(path + 1);
        }
        pthread_mutex_unlock(&g_io_lock);
    }
    return rc;
}

static int invf_release(const char *path, struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    (void)path;
    (void)is_temp_path;
    if (c) {
        commit_wctx(c);
        free(c->tail);
        free(c);
    }
    __sync_fetch_and_sub(&g_open_handles, 1);
    return 0;
}

/* rename is fully implemented in the engine (sibling-safe, dir-prefix
 * aware); audit PB8: it was simply never wired into this ops table */
static int invf_rename(const char *from, const char *to, unsigned int flags)
{
    int rc;
    struct acreds c;
    if (flags)
        return -EINVAL;   /* RENAME_NOREPLACE / RENAME_EXCHANGE unsupported */
    if (g_tt) return -EROFS;   /* WP24-lite: time-travel views never mutate */
    acreds_get(&c);
    if (!c.bypass) {
        rc = perm_check_traversal_cred(&c, from);
        if (rc) return rc;
        rc = perm_check_traversal_cred(&c, to);
        if (rc) return rc;
        rc = perm_check_parent_cred(&c, from, W_OK | X_OK);
        if (rc) return rc;
        rc = perm_check_parent_cred(&c, to, W_OK | X_OK);
        if (rc) return rc;
        rc = perm_check_sticky(&c, from);
        if (rc) return rc;
        /* an overwritten victim is a delete: same sticky rule */
        if (meta_for_path(to, (char[300]){0}, 300, &(invfs_meta_pub){0})) {
            rc = perm_check_sticky(&c, to);
            if (rc) return rc;
        }
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    {
        int was_dir = vol_is_dir(g_vol, from + 1);
        rc = vol_rename(g_vol, from + 1, to + 1);
        if (rc == 0) {
            /* WP22c: a rename that reports OK must never silently vanish.
             * The record pair rides the same buffered page cache as every
             * other write, and without a barrier an error window can
             * acknowledge the rename and then kill it in writeback -- the
             * source name resurrects at the next mount (a file nobody
             * deleted reappearing is worse than a failed call). The
             * barrier pins the pair; its failure latches the volume and
             * fails the rename loudly instead of promising a lie. */
            if (vol_sync(g_vol) != 0) {
                pthread_mutex_unlock(&g_io_lock);
                return -EIO;
            }
            /* plain-file renames are the hot path (portage atomic
             * moves): sync the two names incrementally. Directory
             * renames rewrite every child name prefix -> full rebuild */
            if (!was_dir) {
                table_remove_name(from + 1);
                table_sync_one_locked(to + 1);
            } else {
                table_mark_stale();
            }
        }
    }
    pthread_mutex_unlock(&g_io_lock);
    switch (rc) {
    case 0:  return 0;
    case -2: return -EEXIST;
    case -3: return -ENOSPC;
    case -4: return -EBUSY;    /* a sweep checkpoint is live (WP21/WP22c) */
    default: return rc == -1 ? -ENOENT : -EIO;
    }
}

static int invf_statfs(const char *path, struct statvfs *st)
{
    const invfs_superblock *sb;
    uint64_t free_blocks;
    (void)path;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    sb = vol_sb(g_vol);
    free_blocks = vol_free_blocks_cached(g_vol);
    pthread_mutex_unlock(&g_io_lock);
    memset(st, 0, sizeof(*st));
    st->f_bsize = INVFS_BLOCK_SIZE;
    st->f_frsize = INVFS_BLOCK_SIZE;
    st->f_blocks = sb->total_blocks;
    st->f_bfree = free_blocks;
    st->f_bavail = free_blocks;
    st->f_files = 0;      /* name-indexed fs: inode count unbounded */
    st->f_ffree = 0;
    st->f_namemax = INVFS_MAX_NAME;
    return 0;
}

/* access(2): full mode+ACL evaluation (see the WP-A section above).
 * NOTE: on this kernel the FUSE ->permission hook is never consulted
 * without default_permissions, so this runs for access(2) calls only --
 * open/create/unlink/... enforce through their own entry-point checks. */
static int invf_access(const char *path, int mask)
{
    struct acreds c;
    int rc;
    if (mask & ~(R_OK | W_OK | X_OK | F_OK))
        return -EINVAL;
    acreds_get(&c);
    rc = perm_check_traversal_cred(&c, path);
    if (rc) return rc;
    return perm_check_cred(&c, path, (unsigned)(mask & (R_OK | W_OK | X_OK)));
}

static int resize_volume_file(const char *name, off_t len)
{
    int rc = 0;
    invfs_wsession *ws = NULL;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    /* session-based resize: no whole-file buffer. The engine carries the
     * v2 metadata ext across the commit; a swept file is materialized to
     * RAW first (a truncate IS a write). */
    if (!vol_write_begin(g_vol, name + 1, 0, &ws) || !ws) {
        pthread_mutex_unlock(&g_io_lock);
        return -ENOSPC;
    }
    rc = vol_write_truncate(ws, (uint64_t)len);
    if (rc == 0) rc = vol_write_commit(ws);
    vol_write_abort(ws);
    if (rc == 0) {
        vol_mark_pending(g_vol, vol_find(g_vol, name + 1));
        vol_flush(g_vol);
    }
    table_sync_one_locked(name + 1);
    pthread_mutex_unlock(&g_io_lock);
    return rc == 0 ? 0 : (rc == -2 ? -ENOSPC : -EIO);
}

/* libfuse3: one truncate entry point; fi != NULL for ftruncate-style calls
 * (routed into the handle's write session). A truncate must be VISIBLE
 * immediately: libfuse re-stats the file for the SETATTR reply, and the
 * kernel caches that size -- so the session is committed synchronously
 * here, not deferred to flush. */
static int invf_truncate(const char *path, off_t len, struct fuse_file_info *fi)
{
    wctx *c = fi ? (wctx *)(uintptr_t)fi->fh : NULL;
    int rc;
    (void)is_temp_path;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    if (len < 0)
        return -EINVAL;
    if (!c) {
        /* truncate(2) by path: the ftruncate-on-handle route was gated at
         * open; this one needs its own W check */
        rc = perm_check_traversal(path);
        if (rc) return rc;
        rc = perm_check(path, W_OK);
        if (rc) return rc;
        return resize_volume_file(path, len);
    }
    /* mt loop: per-handle state mutation, same locking as invf_write */
    pthread_mutex_lock(&g_io_lock);
    rc = wctx_ensure_ws_locked(c);
    if (rc == 0) rc = wctx_flush_tail_locked(c);   /* writes precede the cut */
    if (rc == 0) rc = vol_write_truncate(c->ws, (uint64_t)len);
    if (rc == 0) rc = vol_write_commit(c->ws);
    vol_write_abort(c->ws);
    c->ws = NULL;
    dirty_del_locked(c);
    if (rc == 0) {
        if (c->have_meta) {
            c->meta.mtime = (int64_t)time(NULL);
            vol_apply_meta(g_vol, c->name, &c->meta);
        }
        vol_mark_pending(g_vol, vol_find(g_vol, c->name));
        vol_flush(g_vol);
        table_sync_one_locked(c->name);
    }
    pthread_mutex_unlock(&g_io_lock);
    return rc == 0 ? 0 : (rc == -2 ? -ENOSPC : -EIO);
}

/* format v2: persist real timestamps by merging into the INO2 ext */
static int invf_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi)
{
    (void)fi;
    invfs_meta_pub patch;
    int rc = perm_check_traversal(path);
    if (rc) return rc;
    /* the mount root has no record to patch (rsync utimens(".") first);
     * succeed silently instead of a bogus ENOSPC (see invf_chown). */
    if (strcmp(path, "/") == 0) return 0;
    memset(&patch, 0, sizeof patch);
    patch.mtime = tv[1].tv_sec;   /* [0]=atime, [1]=mtime */
    patch.atime = tv[0].tv_sec;
    return meta_apply_patch(path, MM_TIMES, &patch);
}

static int invf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    invfs_meta_pub patch;
    char ename[300];
    invfs_meta_pub cur;
    struct acreds c;
    int rc = perm_check_traversal(path);
    if (rc) return rc;
    if (!meta_for_path(path, ename, sizeof ename, &cur))
        return -ENOENT;
    /* ownership rule (notify_change enforces it at VFS level already;
     * kept here as the daemon is the sole authority on everything else) */
    acreds_get(&c);
    if (!c.bypass && c.uid != cur.uid)
        return -EPERM;
    /* POSIX.1e: fold the new mode into a stored access ACL -- USER_OBJ
     * and OTHER are set from the mode, the group class (MASK if present,
     * else GROUP_OBJ) gets the mode's group bits; a now-trivial ACL is
     * dropped. No ACL -> nothing to do. */
    pthread_mutex_lock(&g_io_lock);
    if (g_vol) {
        uint64_t ino = vol_find(g_vol, ename);
        if (ino) {
            uint8_t acl[INVFS_META_XATTR_MAX];
            size_t alen = sizeof acl;
            if (vol_get_xattr(g_vol, ino, XATTR_ACL_ACCESS, acl, &alen) == 0) {
                if (acl_chmod_masq(acl, alen, (unsigned)(mode & 0777)) > 0)
                    vol_set_xattr(g_vol, ino, XATTR_ACL_ACCESS, acl, alen);
                else
                    vol_remove_xattr(g_vol, ino, XATTR_ACL_ACCESS);
                vol_flush(g_vol);
            }
        }
    }
    /* the mount root has no record to patch; POSIX: chmod on "." = ENOSYS
     * is loud but rsync fires it first -- succeed silently instead. */
    if (strcmp(path, "/") == 0) return 0;
    pthread_mutex_unlock(&g_io_lock);
    memset(&patch, 0, sizeof patch);
    patch.mode = mode & 07777;
    return meta_apply_patch(path, MM_MODE, &patch);
}

static int invf_chown(const char *path, uid_t uid, gid_t gid,
                      struct fuse_file_info *fi)
{
    (void)fi;
    char ename[300];
    invfs_meta_pub cur;
    struct acreds c;
    /* rsync/tar chgrp the MOUNT ROOT ("/") as their first op; the root has
     * no record, so meta_apply_patch would report ENOSPC. POSIX chown on
     * "." succeeds silently (nothing to persist) -- return 0 here. */
    if (strcmp(path, "/") == 0) return 0;
    int rc = perm_check_traversal(path);
    if (rc) return rc;
    if (!meta_for_path(path, ename, sizeof ename, &cur))
        return -ENOENT;
    /* POSIX chown rules, daemon-side: on this kernel the VFS does NOT
     * police chown for a default_permissions-less FUSE mount (verified:
     * a foreign uid could chown away someone else's file). Owner change
     * is privileged (CAP_CHOWN); a group change is allowed for the owner
     * into a group they belong to. */
    acreds_get(&c);
    if (!c.bypass) {
        if (uid != (uid_t)-1 && uid != cur.uid)
            return -EPERM;
        if (gid != (gid_t)-1 && gid != cur.gid &&
            (c.uid != cur.uid || !acreds_in_group(&c, gid)))
            return -EPERM;
    }
    if (uid != (uid_t)-1) cur.uid = uid;
    if (gid != (gid_t)-1) cur.gid = gid;
    {
        invfs_meta_pub patch = cur;
        return meta_apply_patch(path, MM_OWNER | MM_MODE, &patch);
    }
}

static int invf_symlink(const char *target, const char *linkpath)
{
    uint64_t nid;
    int rc;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    if (strlen(target) >= INVFS_META_TARGET_MAX)
        return -ENAMETOOLONG;
    rc = perm_check_traversal(linkpath);
    if (rc) return rc;
    rc = perm_check_parent(linkpath, W_OK | X_OK);
    if (rc) return rc;
    /* symlinks carry no ACL (POSIX: their perms are never consulted) */
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    vol_ensure_path(g_vol, linkpath + 1);
    nid = vol_create_symlink(g_vol, linkpath + 1, target);
    if (nid) {
        /* owner = mounting user; mode 0777 per POSIX symlink convention */
        invfs_meta_pub m;
        struct fuse_context *ctx = fuse_get_context();
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_LNK;
        m.mode = 0777;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 1;
        m.mtime = m.atime = (int64_t)time(NULL);
        snprintf(m.target, sizeof m.target, "%s", target);
        if (!vol_apply_meta(g_vol, linkpath + 1, &m))
            fprintf(stderr, "invf: symlink stamp FAILED %s\n", linkpath);
        table_sync_one_locked(linkpath + 1);
    }
    pthread_mutex_unlock(&g_io_lock);
    return nid ? 0 : -ENOSPC;
}

static int invf_readlink(const char *path, char *buf, size_t size)
{
    char ename[300];
    invfs_meta_pub m;
    int rc = perm_check_traversal(path);
    if (rc) return rc;
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    if (m.type != INVFS_ITYP_LNK)
        return -EINVAL;
    if (strlen(m.target) >= size)
        return -ENAMETOOLONG;
    strcpy(buf, m.target);
    return 0;
}

static int invf_link(const char *from, const char *dest)
{
    int rc;
    struct acreds c;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    acreds_get(&c);
    if (!c.bypass) {
        rc = perm_check_traversal_cred(&c, from);
        if (rc) return rc;
        rc = perm_check_traversal_cred(&c, dest);
        if (rc) return rc;
        rc = perm_check_parent_cred(&c, dest, W_OK | X_OK);
        if (rc) return rc;
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    rc = vol_hardlink(g_vol, from + 1, dest + 1);
    /* portage creates lockfiles via link() in hot loops; a full table
     * rebuild here (mark_stale) made every later negative lookup cost
     * O(area). The new name is one upsert. */
    if (rc == 0) {
        /* both names share blocks now: record nlink=2 so unlinking ONE
         * name takes the name-only path instead of retiring blocks */
        const char *names[2] = { from + 1, dest + 1 };
        for (int q = 0; q < 2; q++) {
            invfs_meta_pub mm;
            uint64_t nid2 = vol_find(g_vol, names[q]);
            if (nid2 && vol_get_meta(g_vol, nid2, &mm) == 0) {
                mm.nlink = 2;
                vol_apply_meta(g_vol, names[q], &mm);
            }
        }
        table_sync_one_locked(dest + 1);
        table_sync_one_locked(from + 1);
    }
    pthread_mutex_unlock(&g_io_lock);
    switch (rc) {
    case 0:  return 0;
    case -2: return -EEXIST;
    case -3: return -EPERM;
    default: return -ENOENT;
    }
}

static int invf_mknod(const char *path, mode_t mode, dev_t rdev)
{    uint8_t typ;
    uint64_t nid;
    struct fuse_context *ctx = fuse_get_context();
    uint8_t aacl[INVFS_META_XATTR_MAX];
    size_t aalen = 0, dlen = 0;
    mode_t cmode = mode & 07777;
    int rc;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    switch (mode & S_IFMT) {
    case S_IFIFO:  typ = INVFS_ITYP_FIFO; break;
    case S_IFSOCK: typ = INVFS_ITYP_SOCK; break;
    case S_IFCHR:  typ = INVFS_ITYP_CHR;  break;
    case S_IFBLK:  typ = INVFS_ITYP_BLK;  break;
    default: return -EPERM;   /* regular files go through .create */
    }
    rc = perm_check_traversal(path);
    if (rc) return rc;
    rc = perm_check_parent(path, W_OK | X_OK);
    if (rc) return rc;
    /* same default-ACL inheritance as .create (no default ACL onward:
     * only dirs carry one) */
    if (parent_default_acl(path, aacl, &dlen)) {
        unsigned mm = cmode;
        if (acl_create_masq(aacl, dlen, &mm) > 0)
            aalen = dlen;
        cmode = (mode_t)(mm & 0777);
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    vol_ensure_path(g_vol, path + 1);
    nid = vol_create_special(g_vol, path + 1, typ,
                             (uint16_t)cmode, (uint64_t)rdev);
    if (nid) {
        if (aalen &&
            vol_set_xattr(g_vol, nid, XATTR_ACL_ACCESS, aacl, aalen) != 0)
            fprintf(stderr, "invf: mknod ACL inherit FAILED %s\n", path);
        table_sync_one_locked(path + 1);
    }
    pthread_mutex_unlock(&g_io_lock);
    if (nid && ctx && (ctx->uid || ctx->gid)) {
        /* non-root mount: stamp the creating user */
        invfs_meta_pub patch;
        memset(&patch, 0, sizeof patch);
        patch.uid = ctx->uid;
        patch.gid = ctx->gid;
        meta_apply_patch(path, MM_OWNER, &patch);
    }
    return nid ? 0 : -ENOSPC;
}

/* ---- xattrs (stored in the INO2 ext TLVs) ---- */
static int invf_getxattr(const char *path, const char *name, char *value,
                         size_t size)
{
    char ename[300];
    invfs_meta_pub m;
    uint64_t ino;
    int rc;
    /* virtual control namespace on the mount root. Reads are cheap
     * (RAM counters / bitmap pass); the sweep trigger is ROOT ONLY
     * (a background sweep is an administrative action). */
    if (strcmp(path, "/") == 0 && strcmp(name, "user.invfs") == 0) {
        char buf[512];
        int n;
        pthread_mutex_lock(&g_io_lock);
        n = snprintf(buf, sizeof buf,
                     "volume=%s\n"
                     "entries=%d\n"
                     "free_blocks=%llu\n"
                     "pending_sweep=%llu\n"
                     "sweep_busy=%d\n",
                     g_img_path, g_nentries,
                     (unsigned long long)(g_vol ? vol_count_free(g_vol) : 0),
                     (unsigned long long)(g_vol ? vol_pending_count(g_vol) : 0),
                     g_sweep_busy);
        pthread_mutex_unlock(&g_io_lock);
        if (!value || size == 0) return n;      /* size query */
        if ((size_t)n + 1 > size) return -ERANGE;
        memcpy(value, buf, (size_t)n + 1);
        return n;
    }
    /* hot population counters: maintained incrementally by the index,
     * seeded automatically by the open scan. O(1) read here; the only
     * non-trivial part is a 256KiB bitmap pass for per-zone usage.
     * WP-DZ: the raw/shadow_used_bytes numbers are per-REGION (the
     * advisory extents -- exactly the view the WP23/WP26 pressure ladder
     * reads); per-CONTENT-CLASS physical usage is the offline walk in
     * vol_compute_stats (invf-stats). */
    if (strcmp(path, "/") == 0 && strcmp(name, "user.invfs.stats") == 0) {
        char buf[1024];
        int n;
        const invfs_superblock *sb;
        uint64_t raw_used = 0, shadow_used = 0;
        pthread_mutex_lock(&g_io_lock);
        if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
        sb = vol_sb(g_vol);
        raw_used = vol_zone_used_bytes(g_vol, sb->raw_zone_start,
                                       sb->raw_zone_start + sb->raw_zone_blocks);
        shadow_used = vol_zone_used_bytes(g_vol, sb->shadow_zone_start,
                                          sb->shadow_zone_start + sb->shadow_zone_blocks);
        uint64_t hfiles, hdirs, htombs, hlogic;
        vol_hot_counters(g_vol, &hfiles, &hdirs, &htombs, &hlogic);
        n = snprintf(buf, sizeof buf,
                "files=%llu\ndirs=%llu\ntombstones=%llu\n"
                "logical_bytes=%llu\n"
                "raw_used_bytes=%llu\nshadow_used_bytes=%llu\n"
                "free_blocks=%llu\n",
                (unsigned long long)hfiles,
                (unsigned long long)hdirs,
                (unsigned long long)htombs,
                (unsigned long long)hlogic,
                (unsigned long long)raw_used,
                (unsigned long long)shadow_used,
                (unsigned long long)vol_count_free(g_vol));
        pthread_mutex_unlock(&g_io_lock);
        if (n < 0 || (size_t)n >= sizeof buf) n = sizeof buf - 1;
        if (!value || size == 0) return n;
        if ((size_t)n + 1 > size) return -ERANGE;
        memcpy(value, buf, (size_t)n + 1);
        return n;
    }
    {
        int trc = perm_check_traversal(path);
        if (trc) return trc;
    }
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) {
        pthread_mutex_unlock(&g_io_lock);
        return meta_for_path(path, ename, sizeof ename, &m)
               ? -ENODATA : -ENOENT;
    }
    {
        size_t vlen = size;
        rc = vol_get_xattr(g_vol, ino, name, value, &vlen);
        pthread_mutex_unlock(&g_io_lock);
        if (rc == 0) return (int)vlen;
        return rc == -1 ? -ENODATA : -ERANGE;
    }
}

static int invf_setxattr(const char *path, const char *name,
                         const char *value, size_t size, int flags)
{
    char ename[300];
    invfs_meta_pub m;
    uint64_t ino;
    int rc;
    /* virtual control namespace on the mount root:
     *   setxattr user.invfs.sweep = "1"  -> trigger a manual sweep pass
     * (same as kill -USR1; kept so scripts need no signal access) */
    if (strcmp(path, "/") == 0 && strcmp(name, "user.invfs.sweep") == 0) {
        if (!vol_write_enabled(g_vol)) return -EROFS;
        g_sweep_now = 1;
        fprintf(stderr, "[sweep] triggered via xattr\n");
        return 0;
    }
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    {
        int trc = perm_check_traversal(path);
        if (trc) return trc;
    }
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    /* ACL xattrs are security state: only the owner (or the bypass
     * admin) may set them, the blob must be a well-formed version-2
     * posix_acl blob, and a default ACL only makes sense on a dir.
     * (On a real fs the kernel checks inode_owner_or_capable in
     * posix_acl_xattr_set; the generic-xattr FUSE transport skips that,
     * so the daemon owns the rule.) */
    if (strcmp(name, XATTR_ACL_ACCESS) == 0 ||
        strcmp(name, XATTR_ACL_DEFAULT) == 0) {
        struct acreds c;
        acreds_get(&c);
        if (!c.bypass && c.uid != m.uid)
            return -EPERM;
        if (!acl_blob_valid((const uint8_t *)value, size))
            return -EINVAL;
        if (strcmp(name, XATTR_ACL_DEFAULT) == 0 && m.type != INVFS_ITYP_DIR)
            return -EACCES;
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) { pthread_mutex_unlock(&g_io_lock); return -ENOENT; }
    rc = vol_set_xattr(g_vol, ino, name, value, size);
    if (rc == 0) { vol_flush(g_vol); table_sync_one_locked(ename); }
    pthread_mutex_unlock(&g_io_lock);
    if (rc == 0 && strcmp(name, XATTR_ACL_ACCESS) == 0) {
        /* posix_acl_update_mode: mirror the ACL into st_mode's 9 bits
         * (the kernel does this inside ->set_acl on real filesystems;
         * the generic-xattr transport never calls it for FUSE) */
        invfs_meta_pub patch;
        memset(&patch, 0, sizeof patch);
        patch.mode = (uint16_t)acl_sync_mode((const uint8_t *)value, size,
                                             m.mode & 07777);
        meta_apply_patch(path, MM_MODE, &patch);
    }
    if (rc == -2) return -ERANGE;
    if (rc == -3) return -EEXIST;      /* XATTR_CREATE on existing */
    if (rc == -4) return -ENODATA;     /* XATTR_REPLACE on missing */
    return rc == 0 ? 0 : -EIO;
}

static int invf_listxattr(const char *path, char *list, size_t size)
{
    char ename[300];
    invfs_meta_pub m;
    uint64_t ino;
    int rc;
    {
        int trc = perm_check_traversal(path);
        if (trc) return trc;
    }
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) { pthread_mutex_unlock(&g_io_lock); return 0; }
    /* engine semantics: positive = bytes written (= total needed);
     * -2 = buffer too small; 0/-1 = no xattrs */
    rc = vol_list_xattr(g_vol, ino, list, size);
    pthread_mutex_unlock(&g_io_lock);
    if (rc > 0) return rc;
    if (rc == -2) return -ERANGE;
    return 0;
}

static int invf_removexattr(const char *path, const char *name)
{
    char ename[300];
    invfs_meta_pub m;
    uint64_t ino;
    int rc;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    {
        int trc = perm_check_traversal(path);
        if (trc) return trc;
    }
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    /* removing an ACL is the same privilege as setting one */
    if (strcmp(name, XATTR_ACL_ACCESS) == 0 ||
        strcmp(name, XATTR_ACL_DEFAULT) == 0) {
        struct acreds c;
        acreds_get(&c);
        if (!c.bypass && c.uid != m.uid)
            return -EPERM;
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) { pthread_mutex_unlock(&g_io_lock); return -ENOENT; }
    rc = vol_remove_xattr(g_vol, ino, name);
    if (rc == 0) { vol_flush(g_vol); table_sync_one_locked(ename); }
    pthread_mutex_unlock(&g_io_lock);
    return rc == 0 ? 0 : (rc == -1 ? -ENODATA : -EIO);
}
static void invf_destroy(void *private_data)
{
    (void)private_data;
    g_shutdown = 1;
    pthread_mutex_lock(&g_io_lock);
    if (g_vol) {
        vol_close(g_vol);   /* flush + journal compact + sb CLEAN */
        g_vol = NULL;
        fprintf(stderr, "invf: volume closed cleanly\n");
    }
    pthread_mutex_unlock(&g_io_lock);
    tmpstore_destroy();
}

static int invf_unlink(const char *path)
{
    int rc;
    struct acreds c;
    (void)is_temp_path;
    /* WP24-lite: a delete is deliberately allowed under the ordinary
     * read-only space latch (H5, it is the way out), but a time-travel
     * view is not the present -- the tombstone would land on the live
     * volume's post-checkpoint records. Refuse like every other write. */
    if (g_tt) return -EROFS;
    acreds_get(&c);
    rc = perm_check_traversal_cred(&c, path);
    if (rc) return rc;
    rc = perm_check_parent_cred(&c, path, W_OK | X_OK);
    if (rc) return rc;
    rc = perm_check_sticky(&c, path);
    if (rc) return rc;
    /* H5: deliberately NOT gated on vol_write_enabled: a delete never
     * allocates data blocks, and it is the way OUT of the ENOSPC
     * READONLY latch (the engine frees the blocks, vol_free_blocks
     * releases the latch above the hysteresis band). The engine still
     * refuses on a recovery-pending volume. */
    {
        char ename[300];
        invfs_meta_pub hm;
        int have_meta = meta_for_path(path, ename, sizeof ename, &hm);
        if (have_meta && hm.nlink > 1) {
            pthread_mutex_lock(&g_io_lock);
            if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
            rc = vol_unlink_name(g_vol, path + 1);
            if (rc == 0) table_remove_name(path + 1);
            pthread_mutex_unlock(&g_io_lock);
            return rc == 0 ? 0 : -ENOENT;
        }
    }
    pthread_mutex_lock(&g_io_lock);
    rc = vol_unlink(g_vol, path + 1);
    if (rc == 0) {
        table_remove_name(path + 1);   /* no flush: tombstone+bitmap are
        durable on the next flush/close; per-unlink fsync-class writes
        made rm -rf of a source tree take minutes */
    } else {
        /* index ghost? record already tombstoned by a killed process:
         * drop the entry instead of reporting ENOENT forever */
        if (vol_forget_name(g_vol, path + 1) == 0) {
            table_remove_name(path + 1);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&g_io_lock);
    if (rc == 0) return 0;
    return vol_write_enabled(g_vol) ? -ENOENT : -EROFS;
}

/* Negotiate transport features (WP17). Splice moves read payload
 * /dev/fuse -> page cache without a userspace copy; async_read keeps
 * kernel readahead concurrent (libfuse default, stated explicitly).
 * Mask with capable: only what this kernel offers.
 * API quirk: at FUSE_USE_VERSION=31 libfuse rejects the -o max_write=/
 * max_readahead=/splice_* conn options (fuse_conn_info_opts wiring is
 * API >= 32), so request sizes are set on conn directly here. The kernel
 * clamps max_write/max_read to FUSE_MAX_PAGES_PER_REQ (1 MiB on 4K pages). */
static void *invf_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)cfg;
    conn->want |= conn->capable & (FUSE_CAP_SPLICE_READ |
                                   FUSE_CAP_SPLICE_WRITE |
                                   FUSE_CAP_SPLICE_MOVE |
                                   FUSE_CAP_ASYNC_READ |
                                   FUSE_CAP_WRITEBACK_CACHE);
    /* WRITEBACK_CACHE is what makes shared-writable mmap safe: dirty
     * mmap pages are written back through the ordinary .write path
     * (which streams into an engine session) instead of being refused
     * or silently dropped. Coherency is sound because every content
     * change flows through this single mount. If the kernel does not
     * offer it, mmap(PROT_WRITE, MAP_SHARED) fails ENODEV in the kernel
     * -- loudly, nothing faked. */
    /* max_read: -o max_read= sets the SESSION value (libfuse sizes its
     * buffers from it) but leaves conn->max_read at 0 here; init must
     * restate the same value or libfuse aborts with "requested different
     * maximum read size". max_write/max_readahead have no such check.
     * The kernel clamps all three to FUSE_MAX_PAGES_PER_REQ (1 MiB). */
    conn->max_read = 1048576;
    conn->max_write = 1048576;
    conn->max_readahead = 1048576;
    if (g_tmp_area != TMP_AREA_VOLUME)
        tmpstore_init(g_tmp_max_bytes, g_tmp_area);
    fprintf(stderr, "invf: conn max_read=%u max_write=%u max_readahead=%u splice=%c%c%c wbc=%c tmp_area=%d\n",
            conn->max_read, conn->max_write, conn->max_readahead,
            (conn->want & FUSE_CAP_SPLICE_READ)  ? 'r' : '-',
            (conn->want & FUSE_CAP_SPLICE_MOVE)  ? 'm' : '-',
            (conn->want & FUSE_CAP_SPLICE_WRITE) ? 'w' : '-',
            (conn->want & FUSE_CAP_WRITEBACK_CACHE) ? '+' : '-');
    return NULL;
}

static const struct fuse_operations invf_ops = {
    .init = invf_init,
    .getattr = invf_getattr,
    .readdir = invf_readdir,
    .mkdir = invf_mkdir,
    .rmdir = invf_rmdir,
    .open = invf_open,
    .read = invf_read,
    .create = invf_create,
    .write = invf_write,
    .flush = invf_flush,
    .fsync = invf_fsync,
    .truncate = invf_truncate,
    .utimens = invf_utimens,
    .statfs = invf_statfs,
    .access = invf_access,
    .rename = invf_rename,
    .release = invf_release,
    .unlink = invf_unlink,
    .chmod = invf_chmod,
    .chown = invf_chown,
    .symlink = invf_symlink,
    .link = invf_link,
    .readlink = invf_readlink,
    .mknod = invf_mknod,
    .getxattr = invf_getxattr,
    .setxattr = invf_setxattr,
    .listxattr = invf_listxattr,
    .removexattr = invf_removexattr,
    .destroy = invf_destroy,
};

/* "<n>[K|M|G]" mount-option sizes, same grammar as the INVFS_ARC_BYTES
 * parser in volume.c. 1 + *out on success, 0 on garbage. */
static int parse_size_opt(const char *s, uint64_t *out)
{
    char *endp = NULL;
    unsigned long long want = strtoull(s, &endp, 10);
    unsigned long long mult = 1;
    int ok = (endp != s);
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
    if (ok) *out = (uint64_t)(want * mult);
    return ok;
}

int main(int argc, char *argv[])
{
    /* usage: invf-fuse [-f] [-o opt[,opt...]] <image> <mountpoint> */
    const char *img = NULL, *mnt = NULL, *opts = NULL;
    int fg = 0, err, i;

    /* WP50: --probe-uuid <dev> — print the volume's 16-byte uuid as hex if
     * <dev> carries an InvariantFS superblock, else exit nonzero. Reads
     * only the superblock magic (0x00) and uuid (0x08); no mount, no scan.
     * The initramfs uses it to identify volumes by uuid instead of unstable
     * kernel names (sda/sdb) and a stale mknod. */
    if (argc >= 3 && strcmp(argv[1], "--probe-uuid") == 0) {
        unsigned char sb[8 + 16];
        ssize_t n;
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) return 1;
        n = pread(fd, sb, sizeof sb, 0);
        close(fd);
        if (n != (ssize_t)sizeof sb || memcmp(sb, "InvariFS", 8) != 0)
            return 1;
        for (i = 0; i < 16; i++) printf("%02x", sb[8 + i]);
        printf("\n");
        return 0;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-fuse [-f] [-o opt,opt] <image> <mountpoint>\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "-d") == 0)
            fg = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            /* join multiple -o into one comma list for fuse_main */
            if (!opts)
                opts = argv[++i];
            else {
                static char obuf[1024];
                i++;
                snprintf(obuf, sizeof obuf, "%s,%s", opts, argv[i]);
                opts = obuf;
            }
        }
        else if (!img) img = argv[i];
        else if (!mnt) mnt = argv[i];
    }
    if (!img || !mnt) {
        fprintf(stderr, "usage: invf-fuse [-f] [-o opt,opt] <image> <mountpoint>\n");
        return 2;
    }

    /* WP10: pull our keys out of the -o list before libfuse sees it --
     * unknown options make fuse_new reject the mount. */
    uint64_t arc_limit = 0, dec_mem_limit = 0;
    int have_arc = 0, have_dec = 0;
    /* WP24-lite: -o at_checkpoint[=<seq>] mounts the read-only view at the
     * live sweep checkpoint (no arg = the live one; K=1 contract). */
    uint64_t ckpt_seq = 0;
    int at_ckpt = 0;
    if (opts) {
        static char fbuf[1024];
        char tmp[1024];
        char *save = NULL, *tok;
        size_t fl = 0;
        snprintf(tmp, sizeof tmp, "%s", opts);
        for (tok = strtok_r(tmp, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            if (strncmp(tok, "arc_limit=", 10) == 0) {
                if (parse_size_opt(tok + 10, &arc_limit)) have_arc = 1;
                else fprintf(stderr, "invf: bad -o arc_limit=%s; ignored\n",
                             tok + 10);
            } else if (strncmp(tok, "dec_mem_limit=", 14) == 0) {
                if (parse_size_opt(tok + 14, &dec_mem_limit)) have_dec = 1;
                else fprintf(stderr, "invf: bad -o dec_mem_limit=%s; ignored\n",
                             tok + 14);
            } else if (strcmp(tok, "at_checkpoint") == 0) {
                at_ckpt = 1;
            } else if (strncmp(tok, "at_checkpoint=", 14) == 0) {
                char *ep = NULL;
                unsigned long long sq = strtoull(tok + 14, &ep, 10);
                if (ep != tok + 14 && *ep == '\0' && sq > 0) {
                    at_ckpt = 1;
                    ckpt_seq = (uint64_t)sq;
                } else {
                    fprintf(stderr, "invf: bad -o %s; ignored\n", tok);
                }
            } else if (strncmp(tok, "attr_t=", 7) == 0) {
                /* attr/entry cache TTL in seconds (WP17); 0 restores the
                 * old bench-honest mode where every stat hits the daemon */
                char *ep = NULL;
                double v = strtod(tok + 7, &ep);
                if (ep != tok + 7 && *ep == '\0' && v >= 0.0 && v <= 86400.0)
                    g_attr_t = v;
                else
                    fprintf(stderr, "invf: bad -o attr_t=%s; ignored\n", tok + 7);
            } else if (strncmp(tok, "raw_watermark=", 14) == 0) {
                /* WP26: RAW-zone fill percent that makes the background
                 * daemon kick an early (checkpoint-armed) sweep; 0 = the
                 * lazy default */
                char *ep = NULL;
                long wv = strtol(tok + 14, &ep, 10);
                if (ep != tok + 14 && *ep == '\0' && wv >= 0 && wv <= 99)
                    g_raw_watermark = (int)wv;
                else
                    fprintf(stderr, "invf: bad -o raw_watermark=%s; ignored\n",
                            tok + 14);
            } else if (strncmp(tok, "tmp_area=", 9) == 0) {
                const char *mode = tok + 9;
                if (strcmp(mode, "ram") == 0)
                    g_tmp_area = TMP_AREA_RAM;
                else if (strcmp(mode, "volume") == 0)
                    g_tmp_area = TMP_AREA_VOLUME;
                else if (strcmp(mode, "auto") == 0)
                    g_tmp_area = TMP_AREA_AUTO;
                else
                    fprintf(stderr, "invf: bad -o tmp_area=%s; ignored\n", mode);
            } else if (strncmp(tok, "tmp_max_bytes=", 14) == 0) {
                uint64_t mb = 0;
                if (parse_size_opt(tok + 14, &mb))
                    g_tmp_max_bytes = mb;
                else
                    fprintf(stderr, "invf: bad -o tmp_max_bytes=%s; ignored\n",
                            tok + 14);
            } else {
                size_t tl = strlen(tok);
                if (fl + tl + 2 < sizeof fbuf) {
                    if (fl) fbuf[fl++] = ',';
                    memcpy(fbuf + fl, tok, tl + 1);
                    fl += tl;
                }
            }
        }
        opts = fl ? fbuf : NULL;
    }

    if (at_ckpt) {
        g_vol = vol_open_at(img, ckpt_seq, &err);
        if (!g_vol && err == -11)
            fprintf(stderr, "invf: %s: cannot mount at_checkpoint: no live "
                    "sweep checkpoint (or its retention registry is gone -- "
                    "already realized?); run invf-sweep first\n", img);
    } else {
        g_vol = vol_open(img, &err);
    }
    if (!g_vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }
    g_tt = vol_time_travel(g_vol);
    if (g_tt)
        fprintf(stderr, "invf: time-travel mount (read-only): all writes "
                "will fail with EROFS; the live volume is untouched\n");
    if (have_arc) vol_set_arc_budget(g_vol, arc_limit);
    if (have_dec) vol_set_dec_mem_limit(g_vol, dec_mem_limit);
    /* WP26 env fallback (CLI sessions that cannot pass -o): the mount
     * option wins when both are given */
    if (!g_raw_watermark) {
        const char *wm = getenv("INVFS_RAW_WATERMARK");
        if (wm && *wm) {
            char *ep = NULL;
            long wv = strtol(wm, &ep, 10);
            if (ep != wm && *ep == '\0' && wv > 0 && wv <= 99)
                g_raw_watermark = (int)wv;
            else
                fprintf(stderr, "invf: bad INVFS_RAW_WATERMARK=%s; ignored\n",
                        wm);
        }
    }
    if (g_raw_watermark)
        fprintf(stderr, "invf: RAW watermark sweep at >%d%% fill\n",
                g_raw_watermark);
    snprintf(g_img_path, sizeof g_img_path, "%s", img);
    setvbuf(stderr, NULL, _IONBF, 0);
    build_file_table();
    fprintf(stderr, "InvariantFS mounted: %d files%s\n", g_nentries,
            g_tt ? " (checkpoint view)" : "");

    /* background on-demand sweep thread (drains pending list when idle);
     * pointless on a time-travel view (nothing is ever pending and every
     * mutation is refused) -- do not even start it */
    if (!g_tt)
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, fuse_sweep_thread, NULL) == 0)
            pthread_detach(tid);
    }

    /* build fuse args: [progname] [-f] [-o opts]; mountpoint handled
     * explicitly below so we control the full lifecycle.
     * Explicit fuse_set_signal_handlers(): without it SIGTERM (openrc
     * shutdown, fusermount rivals) leaves the daemon stuck and the
     * volume permanently DIRTY. */
    {
        char *fuse_argv[8];
        int fuse_argc = 0;
        struct fuse_args fa = FUSE_ARGS_INIT(0, NULL);
        struct fuse *f;
        struct fuse_session *se;
        int rc;
        int k;
        fuse_argv[fuse_argc++] = "invf-fuse";
        /* WP17 transport tuning. attr/entry TTL: production default 1.0 s
         * (same-process daemon writes invalidate through the mount
         * naturally); -o attr_t=0 restores the old bench-honest mode where
         * a distinct writer process and stat-ing process never see a stale
         * cached size. max_read is an -o here (libfuse sizes its session
         * buffers from it); max_write/max_readahead/splice caps are set in
         * .init -- at FUSE_USE_VERSION=31 libfuse rejects those as -o
         * options. User -o opts come after this string and win. */
        {
            static char tuning[128];
            snprintf(tuning, sizeof tuning,
                     "attr_timeout=%.3f,ac_attr_timeout=%.3f,entry_timeout=%.3f,"
                     "max_read=1048576",
                     g_attr_t, g_attr_t, g_attr_t);
            fuse_argv[fuse_argc++] = "-o";
            fuse_argv[fuse_argc++] = tuning;
        }
        /* identify ourselves in /proc/mounts: source field becomes
         * "invfs" instead of anonymous /dev/fuse, so `mount | grep invfs`
         * and findmnt -t fuse.invfs work. User -o fsname= overrides. */
        {
            static char fsname[300];
            const char *base = strrchr(img, '/');
            snprintf(fsname, sizeof fsname, "fsname=invfs[%s]",
                     base ? base + 1 : img);
            fuse_argv[fuse_argc++] = "-o";
            fuse_argv[fuse_argc++] = fsname;
        }
        /* -f/-d are consumed by us (fuse_daemonize below); fuse_new
         * rejects them as unknown options */
        if (opts) {
            fuse_argv[fuse_argc++] = "-o";
            fuse_argv[fuse_argc++] = (char *)opts;
        }
        fuse_argv[fuse_argc] = NULL;

        fa.argc = fuse_argc;
        fa.argv = fuse_argv;
        fa.allocated = 0;

        rc = 1;
        f = fuse_new(&fa, &invf_ops, sizeof(invf_ops), NULL);
        if (!f || fuse_mount(f, (char *)mnt) != 0) {
            fprintf(stderr, "[fuse_mount failed]\n");
            if (f) fuse_destroy(f);
            if (g_vol) { g_shutdown = 1; vol_close(g_vol); g_vol = NULL; }
            return 1;
        }
        if (!fg) fuse_daemonize(0);
        se = fuse_get_session(f);
        fuse_set_signal_handlers(se);

        /* WP17: multithreaded loop. volume.c has NO internal locks, so all
         * engine/table state stays serialized by the single g_io_lock (every
         * op already takes it; the per-handle write-buffer paths were closed
         * in this wave). mt then overlaps kernel<->daemon IPC (request
         * dispatch, splice copies) with the locked engine work.
         * API quirk: at FUSE_USE_VERSION=31 fuse_loop_mt(f, arg) maps to
         * fuse_loop_mt_31(f, clone_fd) -- struct fuse_loop_config only
         * exists at API >= 32 -- so pass clone_fd=0 and take libfuse
         * defaults (demand-grown pool, max_idle_threads=10 >= min(cpus,8)). */
        rc = fuse_loop_mt(f, 0);

        /* graceful path: signal or unmount -> close volume CLEAN */
        fuse_remove_signal_handlers(se);
        fuse_unmount(f);
        fuse_destroy(f);
        fprintf(stderr, "[fuse_main rc=%d]\n", rc);
        /* fallback: if .destroy never ran (early failure), still close */
        if (g_vol) {
            g_shutdown = 1;
            vol_close(g_vol);
            g_vol = NULL;
        }
        return rc;
    }
}
