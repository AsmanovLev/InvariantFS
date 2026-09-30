/* invf-import.c — bulk tree importer for InvariantFS (engine-level, no FUSE).
 *
 * Walks a source directory and creates files/dirs/symlinks/specials in the
 * volume with full format-v2 metadata (type/mode/uid/gid/mtime) applied in
 * one rewrite per entry. This is the doc/13-style rootfs packer: a Gentoo
 * stage3 lands at ~2 record-appends per file instead of ~6 metadata
 * rewrites through the kernel+FUSE round trip.
 *
 * WP138: EVERY path this tool does not import is named on stderr with the
 * reason, and the count is restated on stdout. Before WP138 a skip was a
 * bare increment of one unsigned long, so an import run as an ordinary
 * user dropped /etc/shadow and every other root-only path, exited 0, and
 * left a volume on which invf-fsck reports OK -- nothing in the volume
 * references what is missing. A file the source has and the volume does
 * not is silent DATA LOSS, a different failure shape from a wrong byte:
 * there is nothing for a checker to find.
 *
 * `--fail-on-skip` makes that loss fatal (exit 3) for callers that want
 * the strict policy; without it the exit status is unchanged, and the
 * report is the whole contract.
 *
 * Usage: invf-import [--fail-on-skip] <volume.img|dev> <src-dir> [exclude-dir-name]...
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
static unsigned long n_skipped_unreadable;
static int exclude_count;
static const char **excludes;

/* WP138 — the skip taxonomy.
 *
 * "Skipped" used to be one number meaning six unrelated things, so a
 * reader could not tell a format limit from a lost file. These are the
 * six, and the split that matters is the first one: SKIP_UNREADABLE is
 * the source had this path and we were not allowed to read it, which is
 * silent data loss; everything else is a reason the import could not
 * have carried the path even with privileges. An operator needs that
 * distinction because only the first is fixed by re-running as root. */
typedef enum {
    SKIP_UNREADABLE,   /* open/read/readlink/opendir refused: DATA LOSS */
    SKIP_TOOBIG,       /* > MAX_FILE_SIZE, volume path too long, too deep */
    SKIP_UNSUPPORTED,  /* an st_mode type the volume cannot name */
    SKIP_NOMEM,        /* malloc/realloc failed in this process */
    SKIP_VOLUME,       /* the volume refused the write/mkdir/metadata */
    SKIP_GONE          /* lstat failed: the source vanished under us */
} skip_reason;

static const char *reason_word(skip_reason w)
{
    switch (w) {
    case SKIP_UNREADABLE:
        return "unreadable source (DATA LOSS -- this path is NOT in the volume; "
               "re-run as root to read it)";
    case SKIP_TOOBIG:
        return "cannot be represented (the volume path is too long or too deep)";
    case SKIP_UNSUPPORTED:
        return "cannot be represented (file type the volume cannot name)";
    case SKIP_NOMEM:
        return "invf-import ran out of memory";
    case SKIP_VOLUME:
        return "the volume refused the write";
    case SKIP_GONE:
        return "the source vanished during the import";
    }
    return "unknown reason";
}

/* Every skip goes through here, which is what makes the list exhaustive:
 * the summary's count and the number of lines on stderr are the same
 * number by construction, so they cannot drift the way a print-here,
 * count-there pair does. Two sites printed before WP138 ("meta failed",
 * "mkdir failed") while eighteen printed nothing; they printed *instead*
 * of recording, so their paths could not be told from the silent ones.
 *
 * `spath` is the SOURCE path, not the volume name: that is the path the
 * reader has to fix permissions on. `detail` carries errno. */
static void skip(const char *spath, skip_reason why, const char *detail)
{
    fprintf(stderr, "invf-import: skipped %s: %s", spath, reason_word(why));
    if (detail && *detail) fprintf(stderr, " (%s)", detail);
    fputc('\n', stderr);
    if (why == SKIP_UNREADABLE) n_skipped_unreadable++;
    n_skipped++;
}

static int excluded(const char *base)
{
    int i;
    for (i = 0; i < exclude_count; i++)
        if (strcmp(excludes[i], base) == 0) return 1;
    return 0;
}

/* WP59a: pin config/codecpack files so they stay builtin-readable. */
static int is_anchored_path(const char *vname)
{
    return strncmp(vname, ".invariantfs/config/", 20) == 0 ||
           strncmp(vname, ".invariantfs/codecpacks/", 24) == 0;
}

static void set_anchor_if_needed(const char *vname)
{
    uint64_t ino;
    uint8_t val = 1;
    if (!is_anchored_path(vname)) return;
    ino = vol_find(vol, vname);
    if (ino)
        vol_set_xattr(vol, ino, INVFS_XATTR_ANCHOR, &val, sizeof val);
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
        /* WP138: routed through skip() like every other site, so this path
         * lands in the same report and the same count as the ones that
         * used to print nothing. A metadata failure is a VOLUME failure,
         * so this one is named by its volume path, not the source path. */
        skip(vname, SKIP_VOLUME, strerror(errno));
    }
}

static void import_file(const char *spath, const char *vname)
{
    int fd;
    uint8_t *buf = NULL, *p;
    size_t len = 0, cap = 0;
    ssize_t r;
    struct stat st;
    invfs_meta_pub m;

    if (lstat(spath, &st) != 0) { skip(spath, SKIP_GONE, strerror(errno)); return; }
    if ((uint64_t)st.st_size > MAX_FILE_SIZE) {
        skip(spath, SKIP_TOOBIG, "exceeds MAX_FILE_SIZE");
        return;
    }

    /* WP138: this open() is the site that silently ate /etc/shadow and
     * every other root-only file in a rootfs imported as an ordinary
     * user. lstat above SUCCEEDS on those paths -- stat needs no read
     * permission -- which is why the tool walked right up to them,
     * believed it had found them, and dropped them. */
    fd = open(spath, O_RDONLY);
    if (fd < 0) { skip(spath, SKIP_UNREADABLE, strerror(errno)); return; }
    if (st.st_size > 0) {
        cap = (size_t)st.st_size;
        buf = (uint8_t *)malloc(cap);
        if (!buf) { close(fd); skip(spath, SKIP_NOMEM, "malloc"); return; }
        p = buf;
        while ((r = read(fd, p, cap - len)) > 0) {
            p += r; len += (size_t)r;
            if (len == cap) {
                size_t old = cap;
                uint8_t *nb;
                cap += cap / 2 + 65536;
                nb = (uint8_t *)realloc(buf, cap);
                if (!nb) {
                    free(buf); close(fd);
                    skip(spath, SKIP_NOMEM, "realloc");
                    return;
                }
                buf = nb; p = buf + old;
            }
        }
        if (r < 0) {
            /* a partial read is still a loss: the volume would have held a
             * truncated file that looks complete */
            free(buf); close(fd);
            skip(spath, SKIP_UNREADABLE, strerror(errno));
            return;
        }
    }
    close(fd);

    /* build metadata for inline embedding */
    memset(&m, 0, sizeof(m));
    m.type  = INVFS_ITYP_REG;
    m.mode  = st.st_mode & 07777;
    m.uid   = getenv("INVFS_IMPORT_KEEP_OWNER") ? st.st_uid : 0;
    m.gid   = getenv("INVFS_IMPORT_KEEP_OWNER") ? st.st_gid : 0;
    m.mtime = (int64_t)st.st_mtim.tv_sec;
    m.atime = (int64_t)st.st_atim.tv_sec;
    m.nlink = 1;

    vol_ensure_path(vol, vname);
    /* The volume-s answer decides whether this file arrived, and the answer
     * used to be thrown away. vol_replace_file_with_meta returns the inode id
     * and 0 on REFUSAL, so the old condition -- `== 0 && !buf` -- was the
     * "refused, and there was nothing to store" case, and the fallback create
     * below existed for a genuinely empty source file. What was missing is
     * what happened when the volume refused for any OTHER reason: the return
     * was dropped and n_files++ ran unconditionally, so a file that never
     * arrived was counted as imported, n_skipped stayed 0 and the exit code
     * was 0. That is the defect WP138 fixed on the read side, one site over --
     * a lost file is not an import, and "did everything arrive" has to be
     * answerable. */
    {
        uint64_t rid = vol_replace_file_with_meta(vol, vname, buf, len, &m);
        if (rid == 0 && !buf && vol_find(vol, vname) == 0)
            rid = vol_create_file_with_meta(vol, vname, NULL, 0, &m);
        if (rid == 0) {
            skip(vname, SKIP_VOLUME, "the volume refused the write");
            free(buf);
            return;
        }
    }
    set_anchor_if_needed(vname);
    free(buf);
    n_files++;
}

static void import_dir(const char *spath, const char *vname, int depth);

static void import_entry(const char *spath, const char *vname, int depth)
{
    struct stat st;
    char anchor[VNAMESZ];

    if (lstat(spath, &st) != 0) { skip(spath, SKIP_GONE, strerror(errno)); return; }
    /* vname is the plain volume path WITHOUT trailing slash; dir records
     * are addressed as "path/" anchors inside the volume. */
    if (strlen(vname) >= VNAMESZ - 2) {
        skip(spath, SKIP_TOOBIG, "volume path exceeds VNAMESZ");
        return;
    }

    switch (st.st_mode & S_IFMT) {
    case S_IFDIR: {
        /* vol_mkdir returns 0 on failure; the old condition was inverted
         * (tested success as failure), silently skipping the error and
         * proceeding to apply_meta_or_die on a non-existent anchor. */
        uint64_t mid = vol_mkdir(vol, vname);
        if (mid == 0 && !vol_is_dir(vol, vname)) {
            skip(vname, SKIP_VOLUME, "vol_mkdir");
            return;
        }
        snprintf(anchor, sizeof(anchor), "%s/", vname);
        apply_meta_or_die(anchor, &st, NULL);
        n_dirs++;
        /* WP138: the depth limit is a SKIP -- the whole subtree below this
         * point is absent from the volume -- so it counts and it names the
         * directory. It used to print "too deep" and increment nothing,
         * which put it outside the summary count as well as the report. */
        if (depth < 32) import_dir(spath, vname, depth + 1);
        else skip(spath, SKIP_TOOBIG, "directory nesting deeper than 32; "
                                     "everything below it was not imported");
        break;
    }
    case S_IFREG:
        import_file(spath, vname);
        break;
    case S_IFLNK: {
        char tgt[1024];
        invfs_meta_pub m;
        ssize_t tl = readlink(spath, tgt, sizeof(tgt) - 1);
        if (tl < 0) { skip(spath, SKIP_UNREADABLE, strerror(errno)); return; }
        tgt[tl] = 0;
        memset(&m, 0, sizeof(m));
        m.type  = INVFS_ITYP_LNK;
        m.mode  = 0777;
        if (getenv("INVFS_IMPORT_KEEP_OWNER")) {
            m.uid   = st.st_uid;
            m.gid   = st.st_gid;
        }
        m.mtime = (int64_t)st.st_mtim.tv_sec;
        m.atime = (int64_t)st.st_atim.tv_sec;
        m.nlink = 1;
        snprintf(m.target, sizeof(m.target), "%s", tgt);
        if (vol_create_file_with_meta(vol, vname, NULL, 0, &m) == 0) {
            skip(spath, SKIP_VOLUME, "vol_create_file_with_meta");
            return;
        }
        n_links++;
        break;
    }
    case S_IFIFO: case S_IFSOCK:
        if (vol_create_special(vol, vname,
                               (st.st_mode & S_IFMT) == S_IFIFO ?
                                   INVFS_ITYP_FIFO : INVFS_ITYP_SOCK,
                               st.st_mode & 07777, 0) == 0) {
            skip(spath, SKIP_VOLUME, "vol_create_special");
            return;
        }
        apply_meta_or_die(vname, &st, NULL);
        n_special++;
        break;
    case S_IFCHR: case S_IFBLK:
        if (vol_create_special(vol, vname,
                               (st.st_mode & S_IFMT) == S_IFCHR ?
                                   INVFS_ITYP_CHR : INVFS_ITYP_BLK,
                               st.st_mode & 07777, (uint64_t)st.st_rdev) == 0) {
            skip(spath, SKIP_VOLUME, "vol_create_special");
            return;
        }
        apply_meta_or_die(vname, &st, NULL);
        n_special++;
        break;
    default:
        skip(spath, SKIP_UNSUPPORTED, "unknown st_mode file type");
        break;
    }
}

static void import_dir(const char *spath, const char *vname, int depth)
{
    DIR *d;
    struct dirent *de;
    d = opendir(spath);
    if (!d) {
        /* WP138: this is the site that hid /root. One increment, and the
         * ENTIRE subtree is gone -- on a trixie rootfs that is
         * .bashrc, .profile and .bash_logout, none of which is ever
         * looked at. That is why the skip count could not be used as a
         * file count, and why the report says "directory". */
        char detail[128];
        snprintf(detail, sizeof detail, "%s; every entry under it was "
                 "skipped too, and NONE of them are in the volume",
                 strerror(errno));
        skip(spath, SKIP_UNREADABLE, detail);
        return;
    }
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

static void usage(const char *me)
{
    fprintf(stderr,
            "usage: %s [--fail-on-skip] <volume> <src-dir> [exclude]...\n"
            "\n"
            "  --fail-on-skip   exit 3 if any path was not imported (default:\n"
            "                   report every skipped path on stderr and exit 0)\n"
            "\n"
            "Every path the source has and the volume does not is named on\n"
            "stderr with the reason. An unreadable source is DATA LOSS, not a\n"
            "format limit: re-run as root (or under sudo) to read root-only\n"
            "files such as /etc/shadow.\n", me);
}

int main(int argc, char **argv)
{
    int err = 0;
    int fail_on_skip = 0, ai = 1;
    struct timespec t0, t1;
    struct stat root_st;

    /* WP138: flags are parsed BEFORE the positional scan, because the old
     * `excludes = &argv[3]` assumed argv[1] and argv[2] were the volume and
     * the tree -- a flag in that position was silently read as an exclude
     * pattern. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "--fail-on-skip") == 0) {
            fail_on_skip = 1;
            ai = i + 1;
        }
    }

    if (argc - ai < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *img = argv[ai], *src = argv[ai + 1];
    excludes = (const char **)&argv[ai + 2];
    exclude_count = argc - ai - 2;

    vol = vol_open(img, &err);
    if (!vol) {
        fprintf(stderr, "vol_open %s failed (err %d)\n", img, err);
        return 1;
    }
    if (!(vol_sb(vol)->total_blocks)) { fprintf(stderr, "bad sb\n"); return 1; }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* INVFS_IMPORT_PREFIX=var/db/repos/gentoo imports UNDER an existing
     * volume directory instead of the root (target must already exist) */
    const char *prefix = getenv("INVFS_IMPORT_PREFIX");
    if (prefix && !prefix[0]) prefix = NULL;
    if (lstat(src, &root_st) == 0 && S_ISDIR(root_st.st_mode)) {
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
    import_dir(src, prefix ? prefix : "", 0);
    vol_flush(vol);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    printf("imported: %lu dirs, %lu files, %lu symlinks, %lu specials, "
           "%lu skipped in %.1fs\n",
           n_dirs, n_files, n_links, n_special, n_skipped,
           (double)(t1.tv_sec - t0.tv_sec) +
               (t1.tv_nsec - t0.tv_nsec) / 1e9);

    /* WP138: restate the count on STDOUT, where it cannot be lost with the
     * per-path lines on stderr, and cannot be read as a clean run. The
     * summary's own format is unchanged, so nothing that parses it breaks.
     *
     * The unreadable count is called out separately because it is the one
     * class that is DATA LOSS rather than a limit: those paths are in the
     * source tree and are not in the volume, and re-running with privilege
     * is what fixes it. `n_skipped` is a count of failed syscalls, not of
     * files -- one unreadable directory costs one increment and loses its
     * whole subtree -- so the two numbers are deliberately not equated. */
    if (n_skipped)
        printf("WARNING: %lu path(s) were NOT imported; each one is named "
               "with its reason on stderr.\n"
               "WARNING: %lu of them were UNREADABLE SOURCE -- data loss. "
               "This volume does not contain those paths and invf-fsck "
               "cannot report them as missing.\n",
               n_skipped, n_skipped_unreadable);
    vol_close(vol);
    return (fail_on_skip && n_skipped) ? 3 : 0;
}
