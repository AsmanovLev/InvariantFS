/*
 * fuse_fs.c — InvariantFS FUSE filesystem (read-only)
 *
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
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/statvfs.h>

#include "invarifs.h"
#include "volume.h"

static invfs_volume *g_vol;
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
/* open-handle counter: background sweep only when idle */
static volatile int g_open_handles = 0;
/* set at unmount: stops the background sweep thread before vol_close */
static volatile int g_shutdown = 0;
static void rebuild_table_locked(void);   /* fwd (defined below) */

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

static void build_file_table(void)
{
    const invfs_superblock *sb = vol_sb(g_vol);
    uint64_t bm = (sb->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    uint64_t p = (sb->metadata_zone_start + bm + INVFS_JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
    uint64_t end = (sb->metadata_zone_start + sb->metadata_zone_blocks) * INVFS_BLOCK_SIZE;
    int i;

    g_entries = NULL; g_nentries = 0; g_cap = 0;
    while (p + 8 <= end) {
        uint32_t magic, rec_len;
        uint64_t inode_id, file_size, ctime;
        uint32_t name_len;
        char name[257];

        if (vol_read_raw(g_vol, p, &magic, 4) != 0) break;
        if (magic != 0x444F4E49u && magic != 0x544C4544u) break;  /* INOD | DELT */
        if (vol_read_raw(g_vol, p + 4, &rec_len, 4) != 0) break;
        if (vol_read_raw(g_vol, p + 8, &inode_id, 8) != 0) break;
        if (vol_read_raw(g_vol, p + 16, &file_size, 8) != 0) break;
        if (vol_read_raw(g_vol, p + 24, &ctime, 8) != 0) break;
        if (vol_read_raw(g_vol, p + 32, &name_len, 4) != 0) break;
        if (name_len > 256) break;
        if (vol_read_raw(g_vol, p + 36, name, name_len) != 0) break;
        name[name_len] = 0;

        if (magic == 0x544C4544u) {  /* tombstone */
            /* v2: file_size carries the killed record's position; legacy
             * tombstones carry 0 there and kill by inode id */
            for (i = 0; i < g_nentries; i++) {
                if ((file_size != 0 && g_entries[i].pos == file_size) ||
                    (file_size == 0 && g_entries[i].inode_id == inode_id)) {
                    memmove(&g_entries[i], &g_entries[i + 1],
                            (size_t)(g_nentries - i - 1) * sizeof(fs_entry));
                    g_nentries--;
                    break;
                }
            }
            p += (uint64_t)rec_len + 4;
            continue;
        }

        /* last write wins: replace existing entry with same name */
        for (i = 0; i < g_nentries; i++) {
            if (strcmp(g_entries[i].name, name) == 0) {
                g_entries[i].inode_id = inode_id;
                g_entries[i].size = file_size;
                g_entries[i].ctime = ctime;
                g_entries[i].pos = p;
                goto next_rec;
            }
        }
        if (g_nentries == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 16;
            g_entries = (fs_entry *)realloc(g_entries, g_cap * sizeof(fs_entry));
            if (!g_entries) return;
        }
        memset(&g_entries[g_nentries], 0, sizeof(fs_entry));
        strncpy(g_entries[g_nentries].name, name, 255);
        g_entries[g_nentries].inode_id = inode_id;
        g_entries[g_nentries].size = file_size;
        g_entries[g_nentries].ctime = ctime;
        g_entries[g_nentries].pos = p;
        g_nentries++;
    next_rec:
        p += (uint64_t)rec_len + 4;
    }
}

static fs_entry *find_entry(const char *name)
{
    int i;
    for (i = 0; i < g_nentries; i++)
        if (strcmp(g_entries[i].name, name) == 0)
            return &g_entries[i];
    return NULL;
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
        if (!got) meta_defaults(name, 0, m);
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
    if (nid) { vol_flush(g_vol); rebuild_table_locked(); }
    pthread_mutex_unlock(&g_io_lock);
    return nid ? 0 : -ENOSPC;
}

/* ---- FUSE operations ---- */
static int invf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
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
            meta_defaults(name, size, &m);
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
    if (!vol_write_enabled(g_vol))
        return -EROFS;
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
        m.mode = mode & 07777;
        if (!m.mode) m.mode = 0755;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 2;
        m.mtime = m.atime = (int64_t)time(NULL);
        vol_apply_meta(g_vol, anchor, &m);
        vol_flush(g_vol);
        rebuild_table_locked();
    }
    pthread_mutex_unlock(&g_io_lock);
    rc = d ? 0 : -EEXIST;
    return rc;
}

static int invf_rmdir(const char *path)
{
    int rc;
    pthread_mutex_lock(&g_io_lock);
    rc = vol_rmdir(g_vol, path + 1);
    if (rc == 0) { vol_flush(g_vol); rebuild_table_locked(); }
    pthread_mutex_unlock(&g_io_lock);
    return rc == 0 ? 0 : (rc == -2 ? -ENOTEMPTY : -ENOENT);
}

/* ---- write context (buffered write-back) ---- */
typedef struct {
    char name[256];
    uint8_t *buf;
    size_t len, cap;
    int have_meta;             /* stamp this meta after the flush replace */
    invfs_meta_pub meta;
} wctx;

static int invf_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    uint64_t ino = 0, size64 = 0, ctime;
    int got;
    (void)fi;
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

static void rebuild_table_locked(void)
{
    /* free old table, rescan */
    free(g_entries);
    g_entries = NULL; g_nentries = 0; g_cap = 0;
    build_file_table();
}

static int invf_open(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, "/") == 0)
        return -EISDIR;
    if (!snapshot_entry(path + 1, NULL, NULL, NULL))
        return -ENOENT;
    __sync_fetch_and_add(&g_open_handles, 1);
    if ((fi->flags & O_ACCMODE) == O_RDONLY)
        return 0;  /* plain read open */
    /* read-write open: allocate write context, load existing content */
    {
        wctx *c = (wctx *)calloc(1, sizeof(wctx));
        if (!c) return -ENOMEM;
        strncpy(c->name, path + 1, 255);
        /* load via vol_find (not the snapshot g_entries): an append right
           after a write+close may hit a stale table */
        if (!(fi->flags & O_TRUNC)) {
            uint8_t *data = NULL;
            size_t len = 0;
            pthread_mutex_lock(&g_io_lock);
            uint64_t ino = vol_find(g_vol, path + 1);
            if (ino && vol_read_file(g_vol, ino, &data, &len) == 0 && len > 0) {
                c->buf = data;
                c->len = len;
                c->cap = len;
            } else {
                free(data);
            }
            pthread_mutex_unlock(&g_io_lock);
        }
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
        vol_flush(g_vol);
        rebuild_table_locked();
        pthread_mutex_unlock(&g_io_lock);
    }
    return 0;
}

static int invf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    wctx *c;
    struct fuse_context *ctx = fuse_get_context();
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    fprintf(stderr, "[create] %s\n", path);
    if (strcmp(path, "/") == 0)
        return -EISDIR;
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
        m.mode = mode & 07777;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 1;
        m.mtime = m.atime = (int64_t)time(NULL);
        vol_apply_meta(g_vol, path + 1, &m);
        vol_flush(g_vol);
        rebuild_table_locked();
    }
    pthread_mutex_unlock(&g_io_lock);
    c = (wctx *)calloc(1, sizeof(wctx));
    if (!c) return -ENOMEM;
    strncpy(c->name, path + 1, 255);
    c->have_meta = 1;
    {
        struct fuse_context *cx = fuse_get_context();
        c->meta.type = INVFS_ITYP_REG;
        c->meta.mode = mode & 07777;
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
    if (!vol_write_enabled(g_vol))
        return -ENOSPC;
    size_t need = (size_t)offset + size;
    (void)path;
    fprintf(stderr, "[write] %s off=%lld size=%zu fh=%llu\n", path, (long long)offset, size,
            (unsigned long long)fi->fh);
    if (!c) return -EBADF;
    if (need > c->cap) {
        size_t ncap = c->cap ? c->cap : 4096;
        while (ncap < need) ncap *= 2;
        c->buf = (uint8_t *)realloc(c->buf, ncap);
        if (!c->buf) return -ENOMEM;
        if (need > c->len)
            memset(c->buf + c->len, 0, need - c->len);  /* zero fill hole */
        c->cap = ncap;
    } else if (need > c->len) {
        memset(c->buf + c->len, 0, need - c->len);
    }
    memcpy(c->buf + offset, buf, size);
    if (need > c->len) c->len = need;
    return (int)size;
}

/* background on-demand sweep (FUSE): drain pending while no open handles */
static void *fuse_sweep_thread(void *arg)
{
    (void)arg;
    const char *iv = getenv("INVFS_SWEEP_INTERVAL");
    int interval = iv ? atoi(iv) : 30;
    if (interval < 1) interval = 1;
    for (;;) {
        sleep(interval);
        if (g_shutdown || g_open_handles != 0) continue;
        pthread_mutex_lock(&g_io_lock);
        if (g_shutdown || !g_vol) {   /* unmount won the race */
            pthread_mutex_unlock(&g_io_lock);
            break;
        }
        if (vol_pending_count(g_vol) > 0) {
            int n = vol_sweep_pending(g_vol);
            if (n > 0) {
                vol_flush(g_vol);
                rebuild_table_locked();
                fprintf(stderr, "[sweep] on-demand: processed %d pending\n", n);
            }
        }
        pthread_mutex_unlock(&g_io_lock);
    }
    return NULL;
}

/* Commit a write-back context into the volume. Shared by .flush and .fsync
 * so that fsync() before a crash actually persists buffered data. */
static int commit_wctx(wctx *c)
{
    if (!c) return 0;
    if (!c->buf || c->len == 0) return 0;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    vol_ensure_path(g_vol, c->name);   /* auto-create parent dirs */
    /* append the new record before tombstoning the old one: the delete-first
       order lost the existing file when the create could not fit */
    uint64_t nid = vol_replace_file(g_vol, c->name, c->buf, c->len);
    if (nid == 0) {
        /* late ENOSPC must be visible to the writer, never dropped
         * silently (audit H4; Dokan already reports STATUS_DISK_FULL) */
        fprintf(stderr, "invf: vol_replace_file failed (%s)\n", c->name);
        vol_flush(g_vol);
        rebuild_table_locked();
        pthread_mutex_unlock(&g_io_lock);
        return -ENOSPC;
    }
    fprintf(stderr, "invf: flush %s len=%zu ok\n", c->name, c->len);
    /* re-stamp metadata: the replace built a fresh record without it */
    if (c->have_meta)
        vol_apply_meta(g_vol, c->name, &c->meta);
    vol_mark_pending(g_vol, nid);   /* on-demand sweep */
    vol_flush(g_vol);
    rebuild_table_locked();
    pthread_mutex_unlock(&g_io_lock);
    return 0;
}

static int invf_flush(const char *path, struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    (void)path;
    fprintf(stderr, "[flush] %s fh=%llu\n", path, (unsigned long long)fi->fh);
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
        rc = g_vol ? (vol_flush(g_vol) == 0 ? 0 : -EIO) : -EIO;
        pthread_mutex_unlock(&g_io_lock);
    }
    return rc;
}

static int invf_release(const char *path, struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    (void)path;
    if (c) {
        free(c->buf);
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
    if (flags)
        return -EINVAL;   /* RENAME_NOREPLACE / RENAME_EXCHANGE unsupported */
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    rc = vol_rename(g_vol, from + 1, to + 1);
    if (rc == 0) { vol_flush(g_vol); rebuild_table_locked(); }
    pthread_mutex_unlock(&g_io_lock);
    switch (rc) {
    case 0:  return 0;
    case -2: return -EEXIST;
    case -3: return -ENOSPC;
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

/* permission model is still v1 (0444 files / 0555 dirs, uid 0): report
 * honestly instead of pretending (WP2 adds real metadata storage) */
static int invf_access(const char *path, int mask)
{
    if (mask & W_OK)
        return vol_write_enabled(g_vol) ? -EACCES : -EROFS;
    if (snapshot_entry(path + 1, NULL, NULL, NULL))
        return 0;
    /* directories are prefix anchors: check via engine */
    {
        int is_dir = 0;
        pthread_mutex_lock(&g_io_lock);
        is_dir = g_vol ? vol_is_dir(g_vol, path + 1) : 0;
        pthread_mutex_unlock(&g_io_lock);
        return is_dir ? 0 : -ENOENT;
    }
}

static int resize_volume_file(const char *name, off_t len)
{
    int rc = 0;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    /* preserve v2 metadata across the record rewrite below */
    {
        uint64_t oid = vol_find(g_vol, name + 1);
        invfs_meta_pub keep;
        int have = oid ? (vol_get_meta(g_vol, oid, &keep) == 0) : 0;
        if (len == 0) {
            if (vol_replace_file(g_vol, name + 1, NULL, 0) == 0)
                rc = -ENOSPC;
        } else {
            uint8_t *data = NULL;
            size_t oldlen = 0;
            if (oid && vol_read_file(g_vol, oid, &data, &oldlen) != 0) {
                data = NULL; oldlen = 0;
            }
            {
                size_t nlen = (size_t)len;
                uint8_t *nb = (uint8_t *)malloc(nlen ? nlen : 1);
                if (!nb) { free(data); pthread_mutex_unlock(&g_io_lock); return -ENOMEM; }
                if (data) {
                    memcpy(nb, data, oldlen < nlen ? oldlen : nlen);
                    if (nlen > oldlen) memset(nb + oldlen, 0, nlen - oldlen);
                } else {
                    memset(nb, 0, nlen);
                }
                free(data);
                if (vol_replace_file(g_vol, name + 1, nb, nlen) == 0)
                    rc = -ENOSPC;
                else
                    vol_mark_pending(g_vol, vol_find(g_vol, name + 1));
                free(nb);
            }
        }
        if (have && rc == 0)
            vol_apply_meta(g_vol, name + 1, &keep);
    }
    vol_flush(g_vol);
    rebuild_table_locked();
    pthread_mutex_unlock(&g_io_lock);
    return rc;
}

/* libfuse3: one truncate entry point; fi != NULL for ftruncate-style calls
 * where the data may still live in the write-back buffer (cheap path) */
static int invf_truncate(const char *path, off_t len, struct fuse_file_info *fi)
{
    wctx *c = fi ? (wctx *)(uintptr_t)fi->fh : NULL;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    if (len < 0)
        return -EINVAL;
    if (!c)
        return resize_volume_file(path, len);
    if ((uint64_t)len > c->cap) {
        size_t ncap = c->cap ? c->cap : 4096;
        while (ncap < (size_t)len) ncap *= 2;
        c->buf = (uint8_t *)realloc(c->buf, ncap);
        if (!c->buf) return -ENOMEM;
        c->cap = ncap;
    }
    if ((size_t)len > c->len)
        memset(c->buf + c->len, 0, (size_t)len - c->len);
    c->len = (size_t)len;
    return 0;
}

/* format v2: persist real timestamps by merging into the INO2 ext */
static int invf_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi)
{
    (void)fi;
    invfs_meta_pub patch;
    memset(&patch, 0, sizeof patch);
    patch.mtime = tv[1].tv_sec;   /* [0]=atime, [1]=mtime */
    patch.atime = tv[0].tv_sec;
    return meta_apply_patch(path, MM_TIMES, &patch);
}

static int invf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    invfs_meta_pub patch;
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
    if (!meta_for_path(path, ename, sizeof ename, &cur))
        return -ENOENT;
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
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    if (strlen(target) >= INVFS_META_TARGET_MAX)
        return -ENAMETOOLONG;
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
        vol_apply_meta(g_vol, linkpath + 1, &m);
        vol_flush(g_vol);
        rebuild_table_locked();
    }
    pthread_mutex_unlock(&g_io_lock);
    return nid ? 0 : -ENOSPC;
}

static int invf_readlink(const char *path, char *buf, size_t size)
{
    char ename[300];
    invfs_meta_pub m;
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    if (m.type != INVFS_ITYP_LNK)
        return -EINVAL;
    if (strlen(m.target) >= size)
        return -ENAMETOOLONG;
    strcpy(buf, m.target);
    return 0;
}

static int invf_mknod(const char *path, mode_t mode, dev_t rdev)
{
    uint8_t typ;
    uint64_t nid;
    struct fuse_context *ctx = fuse_get_context();
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    switch (mode & S_IFMT) {
    case S_IFIFO:  typ = INVFS_ITYP_FIFO; break;
    case S_IFSOCK: typ = INVFS_ITYP_SOCK; break;
    case S_IFCHR:  typ = INVFS_ITYP_CHR;  break;
    case S_IFBLK:  typ = INVFS_ITYP_BLK;  break;
    default: return -EPERM;   /* regular files go through .create */
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    vol_ensure_path(g_vol, path + 1);
    nid = vol_create_special(g_vol, path + 1, typ,
                             (uint16_t)(mode & 07777), (uint64_t)rdev);
    if (nid) { vol_flush(g_vol); rebuild_table_locked(); }
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
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) { pthread_mutex_unlock(&g_io_lock); return -ENOENT; }
    rc = vol_set_xattr(g_vol, ino, name, value, size);
    if (rc == 0) { vol_flush(g_vol); rebuild_table_locked(); }
    pthread_mutex_unlock(&g_io_lock);
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
    if (!meta_for_path(path, ename, sizeof ename, &m))
        return -ENOENT;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) { pthread_mutex_unlock(&g_io_lock); return -ENOENT; }
    rc = vol_remove_xattr(g_vol, ino, name);
    if (rc == 0) { vol_flush(g_vol); rebuild_table_locked(); }
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
}

static int invf_unlink(const char *path)
{
    int rc;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    pthread_mutex_lock(&g_io_lock);
    rc = vol_unlink(g_vol, path + 1);
    if (rc == 0) {
        vol_flush(g_vol);
        rebuild_table_locked();
    }
    pthread_mutex_unlock(&g_io_lock);
    return rc == 0 ? 0 : -ENOENT;
}

static const struct fuse_operations invf_ops = {
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
    .readlink = invf_readlink,
    .mknod = invf_mknod,
    .getxattr = invf_getxattr,
    .setxattr = invf_setxattr,
    .listxattr = invf_listxattr,
    .removexattr = invf_removexattr,
    .destroy = invf_destroy,
};

int main(int argc, char *argv[])
{
    /* usage: invf-fuse [-f] [-o opt[,opt...]] <image> <mountpoint> */
    const char *img = NULL, *mnt = NULL, *opts = NULL;
    int fg = 0, err, i;

    for (i = 1; i < argc; i++) {
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

    g_vol = vol_open(img, &err);
    if (!g_vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    build_file_table();
    fprintf(stderr, "InvariantFS mounted: %d files\n", g_nentries);

    /* background on-demand sweep thread (drains pending list when idle) */
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, fuse_sweep_thread, NULL) == 0)
            pthread_detach(tid);
    }

    /* build fuse args: [progname] [-f] [-o opts] [mountpoint].
     * No forced -d: debug spam slowed IO and -o options (allow_other,
     * default_permissions, ro, ...) were unparseable before (audit H11). */
    {
        char *fuse_argv[8];
        int fuse_argc = 0;
        int rc;
        int k;
        fuse_argv[fuse_argc++] = "invf-fuse";
        if (fg) fuse_argv[fuse_argc++] = "-f";
        if (opts) {
            fuse_argv[fuse_argc++] = "-o";
            fuse_argv[fuse_argc++] = (char *)opts;
        }
        fuse_argv[fuse_argc++] = (char *)mnt;
        fuse_argv[fuse_argc] = NULL;
        for (k = 0; k < fuse_argc; k++)
            fprintf(stderr, "[fuse_arg %d] %s\n", k, fuse_argv[k]);
        rc = fuse_main(fuse_argc, fuse_argv, &invf_ops, NULL);
        fprintf(stderr, "[fuse_main rc=%d]\n", rc);
        /* fallback: if the session never reached .destroy (early mount
         * failure), still close so a read-only-opened volume is not left
         * looking crashed */
        if (g_vol) {
            g_shutdown = 1;
            vol_close(g_vol);
            g_vol = NULL;
        }
        return rc;
    }
}
