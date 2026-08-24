/* invf-import.c — bulk tree importer for InvariantFS (engine-level, no FUSE).
 *
 * Walks a source directory and creates files/dirs/symlinks/specials in the
 * volume with full format-v2 metadata (type/mode/uid/gid/mtime) applied in
 * one rewrite per entry. This is the doc/13-style rootfs packer: a Gentoo
 * stage3 lands at ~2 record-appends per file instead of ~6 metadata
 * rewrites through the kernel+FUSE round trip.
 *
 * Usage: invf-import <volume.img|dev> <src-dir> [exclude-dir-name]...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#define VNAMESZ 512

#include "volume.h"
#include "invarifs.h"

static invfs_volume *vol;
static unsigned long n_files, n_dirs, n_links, n_special, n_skipped;
static int exclude_count;
static const char **excludes;

static int excluded(const char *base)
{
    int i;
    for (i = 0; i < exclude_count; i++)
        if (strcmp(excludes[i], base) == 0) return 1;
    return 0;
}

static void apply_meta_or_die(const char *vname, const struct stat *st,
                              const char *target)
{
    invfs_meta_pub m;
    uint64_t nid;
    memset(&m, 0, sizeof(m));
    switch (st->st_mode & S_IFMT) {
    case S_IFDIR:  m.type = INVFS_ITYP_DIR;  break;
    case S_IFLNK:  m.type = INVFS_ITYP_LNK;  break;
    case S_IFIFO:  m.type = INVFS_ITYP_FIFO; break;
    case S_IFSOCK: m.type = INVFS_ITYP_SOCK; break;
    case S_IFCHR:  m.type = INVFS_ITYP_CHR;  break;
    case S_IFBLK:  m.type = INVFS_ITYP_BLK;  break;
    default:       m.type = INVFS_ITYP_REG;  break;
    }
    m.mode  = st->st_mode & 07777;
    /* distro trees are authored to be extracted as root; keeping the
     * extractor's uid/gid would leave every file owned by an
     * irrelevant host user inside the image */
    if (getenv("INVFS_IMPORT_KEEP_OWNER")) {
        m.uid   = st->st_uid;
        m.gid   = st->st_gid;
    } else {
        m.uid   = 0;
        m.gid   = 0;
    }
    m.mtime = (int64_t)st->st_mtim.tv_sec;
    m.atime = (int64_t)st->st_atim.tv_sec;
    m.nlink = 0;                       /* engine default per type */
    if (target) snprintf(m.target, sizeof(m.target), "%s", target);
    nid = vol_apply_meta(vol, vname, &m);
    if (!nid) {
        fprintf(stderr, "meta failed: %s (%s)\n", vname, strerror(errno));
        n_skipped++;
    }
}

static void import_file(const char *spath, const char *vname)
{
    int fd;
    uint8_t *buf = NULL, *p;
    size_t len = 0, cap = 0;
    ssize_t r;
    struct stat st;

    if (lstat(spath, &st) != 0) { n_skipped++; return; }
    if ((uint64_t)st.st_size > MAX_FILE_SIZE) { n_skipped++; return; }

    fd = open(spath, O_RDONLY);
    if (fd < 0) { n_skipped++; return; }
    if (st.st_size > 0) {
        cap = (size_t)st.st_size;
        buf = (uint8_t *)malloc(cap);
        if (!buf) { close(fd); n_skipped++; return; }
        p = buf;
        while ((r = read(fd, p, cap - len)) > 0) {
            p += r; len += (size_t)r;
            if (len == cap) {
                size_t old = cap;
                uint8_t *nb;
                cap += cap / 2 + 65536;
                nb = (uint8_t *)realloc(buf, cap);
                if (!nb) { free(buf); close(fd); n_skipped++; return; }
                buf = nb; p = buf + old;
            }
        }
        if (r < 0) { free(buf); close(fd); n_skipped++; return; }
    }
    close(fd);

    if (vol_replace_file(vol, vname, buf, len) == 0 && !buf) {
        /* replace_file with NULL/0 only overwrites an existing name; ensure
         * creation for fresh empty entries too */
        if (vol_find(vol, vname) == 0)
            vol_create_file(vol, vname, NULL, 0);
    }
    free(buf);
    apply_meta_or_die(vname, &st, NULL);
    n_files++;
}

static void import_dir(const char *spath, const char *vname, int depth);

static void import_entry(const char *spath, const char *vname, int depth)
{
    struct stat st;
    char anchor[VNAMESZ];

    if (lstat(spath, &st) != 0) { n_skipped++; return; }
    /* vname is the plain volume path WITHOUT trailing slash; dir records
     * are addressed as "path/" anchors inside the volume. */
    if (strlen(vname) >= VNAMESZ - 2) { n_skipped++; return; }

    switch (st.st_mode & S_IFMT) {
    case S_IFDIR:
        /* vol_mkdir appends the trailing slash itself; passing an anchor
         * form would create records named "dir//" */
        if (vol_mkdir(vol, vname) != 0 && vol_is_dir(vol, vname) == 0) {
            fprintf(stderr, "mkdir failed: %s\n", vname);
            n_skipped++;
            return;
        }
        snprintf(anchor, sizeof(anchor), "%s/", vname);
        apply_meta_or_die(anchor, &st, NULL);
        n_dirs++;
        if (depth < 32) import_dir(spath, vname, depth + 1);
        else fprintf(stderr, "too deep: %s\n", spath);
        break;
    case S_IFREG:
        import_file(spath, vname);
        break;
    case S_IFLNK: {
        char tgt[1024];
        ssize_t tl = readlink(spath, tgt, sizeof(tgt) - 1);
        if (tl < 0) { n_skipped++; return; }
        tgt[tl] = 0;
        if (vol_create_symlink(vol, vname, tgt) == 0) { n_skipped++; return; }
        /* keep the real target through the metadata restamp */
        apply_meta_or_die(vname, &st, tgt);
        n_links++;
        break;
    }
    case S_IFIFO: case S_IFSOCK:
        if (vol_create_special(vol, vname,
                               (st.st_mode & S_IFMT) == S_IFIFO ?
                                   INVFS_ITYP_FIFO : INVFS_ITYP_SOCK,
                               st.st_mode & 07777, 0) == 0) { n_skipped++; return; }
        apply_meta_or_die(vname, &st, NULL);
        n_special++;
        break;
    case S_IFCHR: case S_IFBLK:
        if (vol_create_special(vol, vname,
                               (st.st_mode & S_IFMT) == S_IFCHR ?
                                   INVFS_ITYP_CHR : INVFS_ITYP_BLK,
                               st.st_mode & 07777, (uint64_t)st.st_rdev) == 0) {
            n_skipped++; return;
        }
        apply_meta_or_die(vname, &st, NULL);
        n_special++;
        break;
    default:
        n_skipped++;
    }
}

static void import_dir(const char *spath, const char *vname, int depth)
{
    DIR *d;
    struct dirent *de;
    d = opendir(spath);
    if (!d) { n_skipped++; return; }
    while ((de = readdir(d)) != NULL) {
        char sp2[1024], sv[512];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (excluded(de->d_name)) continue;
        snprintf(sp2, sizeof(sp2), "%s/%s", spath, de->d_name);
        if (vname[0]) snprintf(sv, sizeof(sv), "%s/%s", vname, de->d_name);
        else          snprintf(sv, sizeof(sv), "%s", de->d_name);
        import_entry(sp2, sv, depth);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    int err = 0;
    struct timespec t0, t1;
    struct stat root_st;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <volume> <src-dir> [exclude]...\n", argv[0]);
        return 2;
    }
    excludes = (const char **)&argv[3];
    exclude_count = argc - 3;

    vol = vol_open(argv[1], &err);
    if (!vol) {
        fprintf(stderr, "vol_open %s failed (err %d)\n", argv[1], err);
        return 1;
    }
    if (!(vol_sb(vol)->total_blocks)) { fprintf(stderr, "bad sb\n"); return 1; }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (lstat(argv[2], &root_st) == 0 && S_ISDIR(root_st.st_mode)) {
        invfs_meta_pub rm;
        memset(&rm, 0, sizeof(rm));
        rm.type = INVFS_ITYP_DIR;
        rm.mode = root_st.st_mode & 07777;
        rm.uid  = root_st.st_uid;
        rm.gid  = root_st.st_gid;
        rm.mtime = (int64_t)root_st.st_mtim.tv_sec;
        rm.atime = (int64_t)root_st.st_atim.tv_sec;
        vol_apply_meta(vol, "", &rm);   /* root anchor meta (best effort) */
    }
    import_dir(argv[2], "", 0);
    vol_flush(vol);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    printf("imported: %lu dirs, %lu files, %lu symlinks, %lu specials, "
           "%lu skipped in %.1fs\n",
           n_dirs, n_files, n_links, n_special, n_skipped,
           (double)(t1.tv_sec - t0.tv_sec) +
               (t1.tv_nsec - t0.tv_nsec) / 1e9);
    vol_close(vol);
    return 0;
}
