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

#include "invarifs.h"
#include "volume.h"

static invfs_volume *g_vol;
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
/* open-handle counter: background sweep only when idle */
static volatile int g_open_handles = 0;
static void rebuild_table_locked(void);   /* fwd (defined below) */

/* ---- file table snapshot ---- */
typedef struct {
    char name[256];
    uint64_t inode_id;
    uint64_t size;
    uint64_t ctime;
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

        if (magic == 0x544C4544u) {  /* tombstone: kill only this version */
            for (i = 0; i < g_nentries; i++) {
                if (g_entries[i].inode_id == inode_id) {
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

/* ---- FUSE operations ---- */
static int invf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    memset(st, 0, sizeof(*st));
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
    {
        fs_entry *e = find_entry(path + 1);
        if (!e) {
            int is_dir;
            pthread_mutex_lock(&g_io_lock);
            is_dir = vol_is_dir(g_vol, path + 1);
            pthread_mutex_unlock(&g_io_lock);
            if (is_dir) {
                st->st_mode = S_IFDIR | 0555;
                st->st_nlink = 2;
                return 0;
            }
            return -ENOENT;
        }
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size = (off_t)e->size;
        st->st_mtime = e->ctime;
        return 0;
    }
}

static int invf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    invfs_dirent ents[4096];
    const char *dir = path[0] == '/' && path[1] ? path + 1 : "";
    pthread_mutex_lock(&g_io_lock);
    if (dir[0] && !vol_is_dir(g_vol, dir)) {
        pthread_mutex_unlock(&g_io_lock);
        return -ENOENT;
    }
    int n = vol_list_dir(g_vol, dir, ents, 4096);
    pthread_mutex_unlock(&g_io_lock);
    int i;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (i = 0; i < n; i++) {
        struct stat st;
        memset(&st, 0, sizeof st);
        st.st_mode = ents[i].is_dir ? S_IFDIR | 0555 : S_IFREG | 0444;
        filler(buf, ents[i].name, &st, 0, 0);
    }
    return 0;
}

static int invf_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    int rc;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
    pthread_mutex_lock(&g_io_lock);
    vol_ensure_path(g_vol, path + 1);
    uint64_t d = vol_mkdir(g_vol, path + 1);
    if (d) { vol_flush(g_vol); rebuild_table_locked(); }
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
} wctx;

static int invf_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    fs_entry *e = find_entry(path + 1);
    int got;
    (void)fi;
    if (!e)
        return -ENOENT;
    if ((uint64_t)offset >= e->size)
        return 0;
    pthread_mutex_lock(&g_io_lock);
    got = vol_read_range(g_vol, e->inode_id, (uint64_t)offset, size, buf);
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
    if (!find_entry(path + 1))
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
        pthread_mutex_lock(&g_io_lock);
        vol_replace_file(g_vol, path + 1, NULL, 0);
        vol_flush(g_vol);
        rebuild_table_locked();
        pthread_mutex_unlock(&g_io_lock);
    }
    return 0;
}

static int invf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    wctx *c;
    (void)mode;
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
    vol_flush(g_vol);
    rebuild_table_locked();
    pthread_mutex_unlock(&g_io_lock);
    c = (wctx *)calloc(1, sizeof(wctx));
    if (!c) return -ENOMEM;
    strncpy(c->name, path + 1, 255);
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
        if (g_open_handles != 0) continue;
        pthread_mutex_lock(&g_io_lock);
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

static int invf_flush(const char *path, struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    (void)path;
    fprintf(stderr, "[flush] %s fh=%llu\n", path, (unsigned long long)fi->fh);
    if (!c) return 0;
    if (c->buf && c->len > 0) {
        pthread_mutex_lock(&g_io_lock);
        vol_ensure_path(g_vol, c->name);   /* auto-create parent dirs */
        /* append the new record before tombstoning the old one: the delete-first
           order lost the existing file when the create could not fit */
        uint64_t nid = vol_replace_file(g_vol, c->name, c->buf, c->len);
        if (nid == 0)
            fprintf(stderr, "invf: vol_replace_file failed (%s)\n", c->name);
        else {
            fprintf(stderr, "invf: flush %s len=%zu ok\n", c->name, c->len);
            vol_mark_pending(g_vol, nid);   /* on-demand sweep */
        }
        vol_flush(g_vol);
        rebuild_table_locked();
        pthread_mutex_unlock(&g_io_lock);
    } else {
        fprintf(stderr, "invf: flush %s empty buf\n", c->name);
    }
    return 0;
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
    .release = invf_release,
    .unlink = invf_unlink,
};

int main(int argc, char *argv[])
{
    /* usage: invf-fuse [-f] <image> <mountpoint> */
    const char *img = NULL, *mnt = NULL;
    int fg = 0, err, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "-d") == 0)
            fg = 1;
        else if (!img) img = argv[i];
        else if (!mnt) mnt = argv[i];
    }
    if (!img || !mnt) {
        fprintf(stderr, "usage: invf-fuse [-f] <image> <mountpoint>\n");
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

    /* build fuse args: [progname] [-f] [mountpoint] */
    {
        char *fuse_argv[8];
        int fuse_argc = 0;
        int rc;
        int k;
        fuse_argv[fuse_argc++] = "invf-fuse";
        if (fg) fuse_argv[fuse_argc++] = "-f";
        fuse_argv[fuse_argc++] = "-d";  /* workaround: fuse3 3.18 mis-parses argv without -d */
        fuse_argv[fuse_argc++] = (char *)mnt;
        fuse_argv[fuse_argc] = NULL;
        for (k = 0; k < fuse_argc; k++)
            fprintf(stderr, "[fuse_arg %d] %s\n", k, fuse_argv[k]);
        rc = fuse_main(fuse_argc, fuse_argv, &invf_ops, NULL);
        fprintf(stderr, "[fuse_main rc=%d]\n", rc);
        return rc;
    }
}
