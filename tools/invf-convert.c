/* invf-convert.c — ext4 -> InvariantFS, phase 0 (offline, verified).
 *
 * WHAT THIS IS. Two commands, and the second one is the deliverable:
 *
 *   invf-convert convert <src-image|src-dir> <dst-image> [options]
 *       Ask the ext4fs containerpack what the source's bytes ARE (its
 *       `classify` GCFG table: meta_structure / garbage / member), loop-mount
 *       the source read-only, import the TREE into a fresh invf-mkfs volume
 *       (real files: mode, uid, gid, nanosecond mtime, symlinks, hardlinks as
 *       one inode with nlink > 1, xattrs, specials), then run the engine's own
 *       gates on the result (invf-fsck, invf-verify --deep) and cross-check the
 *       import against the pack's own member enumeration. Any path the volume
 *       cannot represent ABORTS the run by name (exit 3) and removes the
 *       destination: a converter that reports "done" over a dropped file is a
 *       data-loss tool, which is the one failure shape invf-fsck cannot find
 *       (nothing in the volume references what is missing).
 *
 *   invf-convert verify <src-image|src-dir> <dst-image|dst-dir> [options]
 *       Mount both (loop, read-only ext4; fuse for the InvFS volume) and
 *       compare every path: presence, type, size, mode, uid, gid, mtime
 *       (seconds AND nanoseconds), file bytes by BLAKE3, symlink targets,
 *       hardlink identity, and every xattr the source carries. Prints counts
 *       per class and every difference with its path; exits non-zero if
 *       anything differs. A converter that cannot say this is not done.
 *
 * WHY THE PACK IS INVOLVED AT ALL. The engine cannot parse ext4, and this tool
 * must not either: the pack owns that knowledge (tools/codecpacks/
 * ext4fs.codecpack), and it is exec'd exactly the way the engine execs a pack
 * (its manifest argv, its own binary, no pack internals linked in here). The
 * classify table answers two questions this tool cannot answer for itself:
 * whether the image is one the pack can read (a refusal is the only honest
 * "unsupported"), and how many bytes are STRUCTURE vs GARBAGE vs MEMBER — the
 * last of which is the space rule the phase-1 in-place design needs
 * (docs/../impl_docs/CONVERT-DESIGN.md derives it from this number).
 *
 * PHASE 0 ONLY. The destination is an ordinary new image file. Putting the
 * volume on the whole device and keeping the OLD filesystem as a sparse file
 * inside it is phase 1; an InvFS volume is device-absolute (block 0 is the
 * superblock, total_blocks-1 is the ANC0 anchor) so it cannot live in another
 * filesystem's free blocks. This tool is the pipeline the phase-1 harness will
 * be built on, with both ends as plain files.
 *
 * Usage: see usage(). Run as root: the source is loop-mounted read-only and
 * a rootfs has files an ordinary user cannot read (WP138 measured that
 * failure shape in invf-import: /etc/shadow, /root, every root-only path).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <linux/loop.h>

#include "volume.h"
#include "invarifs.h"
#include "blake3.h"

/* exit codes. 3 is "refused": the source is one this pipeline will not
 * convert (pack declined, or a path the volume cannot represent), which is a
 * different answer from "the run broke" (2) and from "the trees differ" (1). */
#define XV_OK       0
#define XV_DIFF     1
#define XV_ERR      2
#define XV_REFUSED  3

#define P_CAP       4096
/* The engine's own namespace limit, not a guess: vol_dirs.c splits every
 * volume name through `char parent[600]`, so a volume path whose parent part
 * does not fit is refused by the engine at create time. Checking it here makes
 * the refusal a NAMED entry in the report instead of a silent ENOENT-shaped
 * "the volume refused the write". 850 = 599 (parent) + 1 (slash) + 250 (leaf
 * with room), rounded down. */
#define VNAME_CAP   850
#define CHUNK       (1024u * 1024u)

static const char *g_argv0;
static char g_bindir[P_CAP];    /* bin/ holding invf-mkfs, invf-fuse, ... */
static char g_repo[P_CAP];

/* ------------------------------------------------------------------ util */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "invf-convert: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* A failure list: every unrepresentable / unreadable source path, named with
 * its reason. The run aborts on the list's existence, so the count and the
 * lines cannot drift (the WP138 lesson in invf-import: a bare counter next to a
 * print-here pair reports "N skipped" beside fewer than N lines). */
typedef struct {
    char **v;
    size_t n, cap;
} strvec;

static void sv_push(strvec *s, const char *fmt, ...)
{
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        char **nv = (char **)realloc(s->v, nc * sizeof *nv);
        if (!nv) { fprintf(stderr, "invf-convert: out of memory\n"); exit(XV_ERR); }
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n++] = strdup(buf);
}

static void sv_free(strvec *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* percentiles over a little array of doubles (nearest-rank, no interpolation:
 * "the p95 file took 3.4 ms" is a measurement, not a model). */
static int dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pctl(double *v, size_t n, double q)
{
    size_t k;
    if (!n) return 0.0;
    qsort(v, n, sizeof *v, dbl_cmp);
    k = (size_t)(q * (double)(n - 1) + 0.5);
    if (k >= n) k = n - 1;
    return v[k];
}

/* join up to three path pieces into out. Returns 0, or -1 when they do not
 * fit (out then holds a truncated prefix, and every caller checks). Written by
 * hand rather than as snprintf("%s%s%s") because gcc cannot see through the
 * source buffers and warns about truncation that cannot happen — a warning
 * nobody can act on is the same as no warning. */
static int jcat(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    size_t la, lb, lc, need;
    a = a ? a : ""; b = b ? b : ""; c = c ? c : "";
    la = strlen(a); lb = strlen(b); lc = strlen(c);
    need = la + lb + lc + 1;
    if (!cap) return -1;
    if (need > cap) {
        memcpy(out, a, la < cap - 1 ? la : cap - 1);
        out[la < cap - 1 ? la : cap - 1] = 0;
        return -1;
    }
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc);
    out[la + lb + lc] = 0;
    return 0;
}

static const char *human(double bytes, char *buf, size_t cap)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    double b = bytes;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    if (i == 0) snprintf(buf, cap, "%.0f %s", b, u[i]);
    else        snprintf(buf, cap, "%.2f %s", b, u[i]);
    return buf;
}

/* ------------------------------------------------------------------ exec */

/* exec <argv> with stdout+stderr captured into buf (NUL-terminated, the tail
 * kept when the child is chattier than cap). Returns 0 if the child could be
 * started, *status gets its exit code (-1 for a signal). */
static int exec_capture(char *const argv[], char *buf, size_t cap, int *status)
{
    int pfd[2];
    pid_t pid;
    size_t used = 0;
    ssize_t r;

    if (buf && cap) buf[0] = 0;
    if (pipe(pfd) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    while ((r = read(pfd[0], (buf ? buf + used : NULL), (buf && cap > used + 1) ? cap - used - 1 : 0)) > 0) {
        if (buf && cap > used + 1) used += (size_t)r;
        /* A full buffer keeps draining: a child blocked on a full pipe is a
         * hang, not a result. */
        if (!buf || used + 1 >= cap) { char sink[4096]; while (read(pfd[0], sink, sizeof sink) > 0) { /* drain */ } break; }
    }
    if (buf && cap) buf[used] = 0;
    close(pfd[0]);
    { int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { /* retry */ }
      *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
    return 0;
}

static void resolve_bindir(void)
{
    char exe[P_CAP];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;

    exe[n > 0 ? (size_t)n : 0] = 0;
    if (n > 0 && (slash = strrchr(exe, '/')) != NULL) {
        *slash = 0;
        snprintf(g_bindir, sizeof g_bindir, "%s", exe);
        /* repo = parent of bin/ (this is how the tool finds tools/codecpacks) */
        {
            char tmp[P_CAP];
            snprintf(tmp, sizeof tmp, "%s", exe);
            if ((slash = strrchr(tmp, '/')) != NULL) {
                *slash = 0;
                snprintf(g_repo, sizeof g_repo, "%s", tmp);
            }
        }
    } else {
        snprintf(g_bindir, sizeof g_bindir, "bin");
        snprintf(g_repo, sizeof g_repo, ".");
    }
    /* A build with OUT=<dir> still leaves bin/ next to the repo root; when the
     * resolved parent has no tools/codecpacks, fall back to the CWD, which is
     * how every other tool in this tree resolves tools/ (they are documented
     * "run from the repo root"). */
    {
        char probe[P_CAP];
        jcat(probe, sizeof probe, g_repo, "/tools/codecpacks", "");
        if (access(probe, X_OK) != 0)
            snprintf(g_repo, sizeof g_repo, ".");
    }
}

/* Remove a scratch tree without a shell. `system("rm -rf '%s'")` is both a
 * truncation warning and a quoting hazard: the mount root is a path the caller
 * chose, and one holding a quote would end up as shell syntax. */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    if (!d) { (void)unlink(path); return; }
    while ((de = readdir(d)) != NULL) {
        char child[P_CAP];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (jcat(child, sizeof child, path, "/", de->d_name) != 0) continue;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { rm_rf(child); (void)rmdir(child); }
        else (void)unlink(child);
    }
    closedir(d);
    (void)rmdir(path);
}

static int xbin(const char *name, char *out, size_t cap)
{
    jcat(out, cap, g_bindir, "/", name);
    return access(out, X_OK) == 0;
}

/* ------------------------------------------------- the pack as an oracle */

typedef struct {
    uint64_t image_size;
    uint64_t block_size;
    uint32_t n_ent;
    uint64_t bytes[3];         /* cls 0 meta_structure / 1 garbage / 2 member */
    uint64_t zero_garbage;
    uint64_t member_inodes;    /* from `enumerate` */
    uint64_t member_bytes;     /* sum of member i_size */
    /* One entry per `enumerate` line: the ext4 inode number, its i_size, and
     * the NAME the pack's own directory walk found for it ("ino<N>" when no
     * dirent names it). Kept per inode because that is what makes the
     * cross-check exact: when the source is loop-mounted, st_ino IS the ext4
     * inode number, so "did the tree import carry this member" is a lookup, not
     * a byte-count coincidence. */
    struct {
        uint64_t ino;
        uint64_t usize;
        char     name[64];
    } *mem;
    size_t n_mem, cap_mem;
} packinfo;

static uint64_t le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);          /* x86-64 / aarch64 LE build: this tree's only targets */
    return v;
}

static uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Read the GCFG table `classify` wrote. The header is
 *   "GCFG" u32 version u32 n_entries u32 block_size, then 24 B entries
 *   { u64 off; u64 len; u8 cls; u8 flags; u16 rsvd }
 * off ascending, tiling [0, image_size). We VERIFY the tiling rather than
 * trust it: an oracle whose output need not be consistent is not an oracle,
 * and this table is what tells a phase-1 run how much space the copy needs. */
static int gcfg_load(const char *path, packinfo *pi, strvec *fail)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[16], *tab = NULL;
    uint64_t expect_off = 0;
    uint32_t i;
    int rc = -1;

    if (!f) { sv_push(fail, "classify: cannot read %s: %s", path, strerror(errno)); return -1; }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        memcmp(hdr, "GCFG", 4) != 0 || le32(hdr + 4) != 1) {
        sv_push(fail, "classify: %s is not a GCFG v1 table", path);
        goto out;
    }
    pi->n_ent = le32(hdr + 8);
    pi->block_size = le32(hdr + 12);
    if (!pi->n_ent || pi->n_ent > (uint32_t)16 * 1024 * 1024) {
        sv_push(fail, "classify: implausible entry count %u", pi->n_ent);
        goto out;
    }
    tab = (uint8_t *)malloc((size_t)pi->n_ent * 24u);
    if (!tab) { sv_push(fail, "out of memory reading the byte map"); goto out; }
    if (fread(tab, 24, pi->n_ent, f) != pi->n_ent) {
        sv_push(fail, "classify: truncated GCFG table");
        goto out;
    }
    for (i = 0; i < pi->n_ent; i++) {
        const uint8_t *e = tab + (size_t)i * 24u;
        uint64_t off = le64(e), len = le64(e + 8);
        uint8_t cls = e[16], flags = e[17];
        if (off != expect_off) {
            sv_push(fail, "classify: byte map does not tile: entry %u starts at "
                          "%llu, expected %llu", i,
                    (unsigned long long)off, (unsigned long long)expect_off);
            goto out;
        }
        if (cls > 2) {
            sv_push(fail, "classify: entry %u has unknown class %u", i, cls);
            goto out;
        }
        pi->bytes[cls] += len;
        if (cls == 1 && (flags & 0x01)) pi->zero_garbage += len;
        expect_off = off + len;
    }
    pi->image_size = expect_off;
    rc = 0;
out:
    free(tab);
    fclose(f);
    return rc;
}

/* `enumerate` prints "ino\tname\ti_size" per member. Used only for the
 * cross-check: mem'ber inodes are every IN-USE regular inode of the source
 * (reachable or not), so a tree import that carries FEWER bytes than the pack
 * enumerated means the walk did not see something the pack can see — the exact
 * data-loss shape this tool exists to refuse. */
static int enumerate_load(const char *path, packinfo *pi, strvec *fail)
{
    FILE *f = fopen(path, "r");
    char line[512];
    if (!f) { sv_push(fail, "enumerate: cannot read %s: %s", path, strerror(errno)); return -1; }
    while (fgets(line, sizeof line, f)) {
        unsigned long long ino, sz;
        char nm[64];
        if (sscanf(line, "%llu\t%63[^\t]\t%llu", &ino, nm, &sz) != 3) continue;
        if (pi->n_mem == pi->cap_mem) {
            size_t nc = pi->cap_mem ? pi->cap_mem * 2 : 256;
            void *np = realloc(pi->mem, nc * sizeof *pi->mem);
            if (!np) { fclose(f); sv_push(fail, "out of memory reading the member list"); return -1; }
            pi->mem = np;
            pi->cap_mem = nc;
        }
        pi->mem[pi->n_mem].ino = ino;
        pi->mem[pi->n_mem].usize = sz;
        snprintf(pi->mem[pi->n_mem].name, sizeof pi->mem[pi->n_mem].name, "%s", nm);
        pi->n_mem++;
        pi->member_inodes++;
        pi->member_bytes += sz;
    }
    fclose(f);
    return 0;
}

/* The pack dir. Precedence: --pack-dir, $INVFS_CONVERT_PACK, <repo>/
 * tools/codecpacks/ext4fs.codecpack. The binary is required to EXIST: building
 * it silently would be the tool deciding which pack code is under test. */
static int resolve_pack(const char *opt, char *dir, size_t cap, strvec *fail)
{
    const char *cand[3];
    int i;
    char bin[P_CAP];

    cand[0] = opt;
    cand[1] = getenv("INVFS_CONVERT_PACK");
    {
        static char repocand[P_CAP];
        jcat(repocand, sizeof repocand, g_repo, "/tools/codecpacks/ext4fs.codecpack", "");
        cand[2] = repocand;
    }
    for (i = 0; i < 3; i++) {
        if (!cand[i] || !cand[i][0]) continue;
        jcat(bin, sizeof bin, cand[i], "/bin/ext4fs", "");
        if (access(bin, X_OK) == 0) { snprintf(dir, cap, "%s", cand[i]); return 0; }
    }
    sv_push(fail, "the ext4fs pack helper is not built: expected "
                  "<packdir>/bin/ext4fs (tried %s%s%s%s). Build it with:\n"
                  "  cc -std=c11 -O2 -Wall -Wextra -Werror -o %s/bin/ext4fs %s/ext4fs.c",
            cand[0] && cand[0][0] ? cand[0] : "-", cand[1] ? ", $" : "",
            cand[1] && cand[1][0] ? "INVFS_CONVERT_PACK" : (cand[0] && cand[0][0] ? "" : ""),
            cand[1] && cand[1][0] ? "" : (cand[0] && cand[0][0] ? "" : ", $INVFS_CONVERT_PACK or <repo>/tools/..."),
            cand[2], cand[2]);
    return -1;
}

/* Run one pack verb. `out` is the file the verb writes (classify/enumerate). */
static int pack_verb(const char *packdir, const char *verb,
                     const char *img, const char *outfile, char *log, size_t logcap)
{
    char bin[P_CAP];
    char *argv[5];
    int status = -1;

    jcat(bin, sizeof bin, packdir, "/bin/ext4fs", "");
    /* the manifest names argv[0] as "bin/ext4fs" (a pack-relative path). The
     * engine resolves it inside the staged pack dir; we exec the absolute
     * path, which is the same binary the manifest points at. */
    argv[0] = bin;
    argv[1] = (char *)verb;
    argv[2] = (char *)img;
    argv[3] = (char *)outfile;
    argv[4] = NULL;
    /* The pack's decline reasons are DEBUG-gated (EXT4FS_DEBUG=1, see its
     * note()); a converter whose refusal says only "exit 3" has not failed
     * loudly, it has failed quietly with a number. */
    setenv("EXT4FS_DEBUG", "1", 1);
    log[0] = 0;
    if (exec_capture(argv, log, logcap, &status) != 0) {
        snprintf(log, logcap, "cannot exec %s: %s", bin, strerror(errno));
        return -1;
    }
    return status;
}

/* ------------------------------------------------------------ src mount */

typedef struct {
    char root[P_CAP];       /* the directory the tree is read through */
    int  ours;              /* 1 = we mounted it and must undo that */
    int  is_fuse;
    char loopdev[64];
    char img[P_CAP];
    pid_t fuse_pid;
    char fuselog[P_CAP];
} mountpt;

static int mnt_listed(const char *mnt)
{
    FILE *f = setmntent("/proc/self/mounts", "r");
    struct mntent *me;
    int found = 0;
    if (!f) return 0;
    while ((me = getmntent(f)) != NULL) {
        if (strcmp(me->mnt_dir, mnt) == 0) { found = 1; break; }
    }
    endmntent(f);
    return found;
}

/* Attach `img` to a read-only loop device. Devices are WALKED rather than
 * asked of LOOP_CTL_GET_FREE alone: tools/lib-loopmount.sh documents a wedged
 * loop4 on this host that --find keeps handing back, and a wedge discovered
 * through a mount is a confusing failure. */
static int loop_attach(const char *img, char *dev, size_t devcap, strvec *fail)
{
    int ctl = open("/dev/loop-control", O_RDWR);
    int n = -1, lfd = -1, ifd = -1;
    struct loop_info64 li;

    ifd = open(img, O_RDONLY);
    if (ifd < 0) { sv_push(fail, "cannot open %s: %s", img, strerror(errno)); return -1; }
    if (ctl >= 0) {
        n = ioctl(ctl, LOOP_CTL_GET_FREE);
        close(ctl);
    }
    if (n >= 0) {
        snprintf(dev, devcap, "/dev/loop%d", n);
        lfd = open(dev, O_RDWR);
        if (lfd >= 0 && ioctl(lfd, LOOP_SET_FD, ifd) == 0) {
            close(lfd); lfd = -1;
        } else {
            if (lfd >= 0) close(lfd);
            lfd = -1;
            n = -1;
        }
    }
    if (n < 0) {
        int i;
        for (i = 0; i < 64; i++) {
            snprintf(dev, devcap, "/dev/loop%d", i);
            lfd = open(dev, O_RDWR);
            if (lfd < 0) continue;
            if (ioctl(lfd, LOOP_SET_FD, ifd) == 0) { close(lfd); lfd = -1; break; }
            close(lfd);
            lfd = -1;
        }
        if (i == 64) {
            sv_push(fail, "no free loop device for %s (some may be wedged ro=1)", img);
            close(ifd);
            return -1;
        }
    }
    close(ifd);
    memset(&li, 0, sizeof li);
    li.lo_flags = LO_FLAGS_READ_ONLY;
    lfd = open(dev, O_RDWR);
    if (lfd >= 0) {
        (void)ioctl(lfd, LOOP_SET_STATUS64, &li);
        close(lfd);
    }
    return 0;
}

static void loop_detach(const char *dev)
{
    int lfd;
    if (!dev[0]) return;
    lfd = open(dev, O_RDWR);
    if (lfd >= 0) {
        (void)ioctl(lfd, LOOP_CLR_FD, 0);
        close(lfd);
    }
}

/* Mount the SOURCE. A directory is taken as an already-mounted tree (the
 * harness may have mounted it itself); a regular file is loop-mounted
 * read-only; a block device is mounted directly. */
static int src_mount(const char *path, const char *mount_root, mountpt *mp, strvec *fail)
{
    struct stat st;
    char mnt[P_CAP];

    memset(mp, 0, sizeof *mp);
    if (stat(path, &st) != 0) { sv_push(fail, "cannot stat %s: %s", path, strerror(errno)); return -1; }
    if (S_ISDIR(st.st_mode)) {
        if (realpath(path, mp->root) == NULL) snprintf(mp->root, sizeof mp->root, "%s", path);
        return 0;
    }
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
        sv_push(fail, "%s is neither an image, a block device nor a directory", path);
        return -1;
    }
    jcat(mnt, sizeof mnt, mount_root, "/mnt", "");
    if (mkdir(mnt, 0700) != 0 && errno != EEXIST) {
        sv_push(fail, "cannot create %s: %s", mnt, strerror(errno));
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        if (loop_attach(path, mp->loopdev, sizeof mp->loopdev, fail) != 0) return -1;
    }
    snprintf(mp->img, sizeof mp->img, "%s", path);
    {
        const char *dev = S_ISBLK(st.st_mode) ? path : mp->loopdev;
        if (mount(dev, mnt, "ext4", MS_RDONLY, NULL) != 0) {
            int e = errno;
            if (mp->loopdev[0]) loop_detach(mp->loopdev);
            mp->loopdev[0] = 0;
            sv_push(fail, "mount -o ro %s %s failed: %s%s", dev, mnt, strerror(e),
                    e == ENODEV ? " (is it an ext4 filesystem? the loop mount is the "
                                  "only way this tool reads a source)" : "");
            return -1;
        }
    }
    snprintf(mp->root, sizeof mp->root, "%s", mnt);
    mp->ours = 1;
    return 0;
}

static void src_unmount(mountpt *mp)
{
    if (mp->ours) {
        (void)umount2(mp->root, MNT_DETACH);
        mp->ours = 0;
    }
    if (mp->loopdev[0]) { loop_detach(mp->loopdev); mp->loopdev[0] = 0; }
}

/* Mount the DESTINATION volume through the daemon, which is what the user
 * sees: comparing through the volume API would skip the whole presentation
 * layer (getattr/xattr/readdir), and the presentation layer is where a
 * converter's output is actually judged. */
static int fuse_mount(const char *img, const char *mount_root, mountpt *mp, strvec *fail)
{
    char fusebin[P_CAP], mnt[P_CAP];
    pid_t pid;
    int i;

    memset(mp, 0, sizeof *mp);
    if (!xbin("invf-fuse", fusebin, sizeof fusebin)) {
        sv_push(fail, "cannot find invf-fuse next to this binary (%s)", g_bindir);
        return -1;
    }
    jcat(mnt, sizeof mnt, mount_root, "/fuse", "");
    if (mkdir(mnt, 0700) != 0 && errno != EEXIST) {
        sv_push(fail, "cannot create %s: %s", mnt, strerror(errno));
        return -1;
    }
    jcat(mp->fuselog, sizeof mp->fuselog, mnt, "/fuse.log", "");
    pid = fork();
    if (pid < 0) { sv_push(fail, "fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        int fd = open(mp->fuselog, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        char *argv[7];
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        argv[0] = fusebin;
        argv[1] = (char *)img;
        argv[2] = mnt;
        argv[3] = (char *)"-o";
        /* noatime: the comparison does not check atime (see the verify
         * report), and reading every file must not churn it either. */
        argv[4] = (char *)"attr_t=0,noatime";
        argv[5] = (char *)"-f";
        argv[6] = NULL;
        execv(fusebin, argv);
        _exit(127);
    }
    for (i = 0; i < 100; i++) {
        if (mnt_listed(mnt)) break;
        if (waitpid(pid, NULL, WNOHANG) == pid) {
            sv_push(fail, "invf-fuse exited before mounting %s (log: %s)", mnt, mp->fuselog);
            return -1;
        }
        usleep(100 * 1000);
    }
    if (i == 100) {
        sv_push(fail, "invf-fuse did not mount %s within 10 s (log: %s)", mnt, mp->fuselog);
        kill(pid, SIGTERM);
        return -1;
    }
    snprintf(mp->root, sizeof mp->root, "%s", mnt);
    snprintf(mp->img, sizeof mp->img, "%s", img);
    mp->ours = 1;
    mp->is_fuse = 1;
    mp->fuse_pid = pid;
    return 0;
}

static void fuse_unmount(mountpt *mp)
{
    int i;
    if (!mp->ours) return;
    (void)umount2(mp->root, MNT_DETACH);
    for (i = 0; i < 200; i++) {
        if (waitpid(mp->fuse_pid, NULL, WNOHANG) == mp->fuse_pid) break;
        usleep(20 * 1000);
    }
    if (i == 200) { kill(mp->fuse_pid, SIGTERM); (void)waitpid(mp->fuse_pid, NULL, 0); }
    mp->ours = 0;
}

/* ----------------------------------------------------------- conversion */

typedef struct {
    unsigned long long dirs, files, links, specials, hardlink_names, hardlink_inodes;
    unsigned long long content_bytes, xattr_names, xattr_bytes;
    unsigned long long max_file;
    /* the pack oracle's counterpart */
    unsigned long long reg_inodes;
} convstats;

static invfs_volume *g_vol;
static convstats g_st;
static strvec *g_fail;

/* Every UNIQUE regular inode the walk carried: (ext4 inode number, i_size).
 * One entry per inode, not per name, so a hardlink pair is one member — which
 * is also how the pack counts it. */
typedef struct { uint64_t ino, size; } tino;
static tino *g_tino;
static size_t g_n_tino, g_cap_tino;

static int tino_cmp(const void *a, const void *b)
{
    const tino *x = (const tino *)a, *y = (const tino *)b;
    return x->ino < y->ino ? -1 : x->ino > y->ino;
}

static void tino_add(uint64_t ino, uint64_t size)
{
    if (g_n_tino == g_cap_tino) {
        size_t nc = g_cap_tino ? g_cap_tino * 2 : 512;
        tino *nv = (tino *)realloc(g_tino, nc * sizeof *nv);
        if (!nv) { fprintf(stderr, "invf-convert: out of memory\n"); exit(XV_ERR); }
        g_tino = nv;
        g_cap_tino = nc;
    }
    g_tino[g_n_tino].ino = ino;
    g_tino[g_n_tino].size = size;
    g_n_tino++;
}

static const tino *tino_find(uint64_t ino)
{
    size_t lo = 0, hi = g_n_tino;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_tino[mid].ino == ino) return &g_tino[mid];
        if (g_tino[mid].ino < ino) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* hardlink map: (dev, ino) -> the first volume path that carried this inode */
typedef struct hl {
    uint64_t dev, ino;
    char *path;
    struct hl *next;
} hl;
#define HL_BUCKETS 65536
static hl *g_hl[HL_BUCKETS];

static unsigned hl_hash(uint64_t dev, uint64_t ino)
{
    uint64_t h = ino * 0x9E3779B97F4A7C15ull ^ (dev + 0x165667B19E3779F9ull);
    h ^= h >> 29;
    return (unsigned)(h % HL_BUCKETS);
}

static const char *hl_lookup(uint64_t dev, uint64_t ino)
{
    hl *e;
    for (e = g_hl[hl_hash(dev, ino)]; e; e = e->next)
        if (e->dev == dev && e->ino == ino) return e->path;
    return NULL;
}

static void hl_insert(uint64_t dev, uint64_t ino, const char *path)
{
    unsigned b = hl_hash(dev, ino);
    hl *e = (hl *)calloc(1, sizeof *e);
    if (!e) { fprintf(stderr, "invf-convert: out of memory\n"); exit(XV_ERR); }
    e->dev = dev; e->ino = ino; e->path = strdup(path);
    e->next = g_hl[b];
    g_hl[b] = e;
}

static void hl_free(void)
{
    int i;
    for (i = 0; i < HL_BUCKETS; i++) {
        hl *e = g_hl[i];
        while (e) { hl *n = e->next; free(e->path); free(e); e = n; }
        g_hl[i] = NULL;
    }
}

/* Build the engine's metadata view of one source inode. Timestamps are PACKED
 * (sec << 30 | nsec, ino row_version 3): the engine stores nanoseconds in the
 * one i64 it has, and writing raw seconds here is what makes a converted
 * mtime read back as the wrong decade — the exact defect measured in
 * invf-import before this tool existed. */
static void meta_of(invfs_meta_pub *m, const struct stat *st)
{
    memset(m, 0, sizeof *m);
    switch (st->st_mode & S_IFMT) {
    case S_IFDIR:  m->type = INVFS_ITYP_DIR; break;
    case S_IFLNK:  m->type = INVFS_ITYP_LNK; break;
    case S_IFIFO:  m->type = INVFS_ITYP_FIFO; break;
    case S_IFSOCK: m->type = INVFS_ITYP_SOCK; break;
    case S_IFCHR:  m->type = INVFS_ITYP_CHR; break;
    case S_IFBLK:  m->type = INVFS_ITYP_BLK; break;
    default:       m->type = INVFS_ITYP_REG; break;
    }
    m->mode  = (uint16_t)(st->st_mode & 07777);
    m->uid   = st->st_uid;
    m->gid   = st->st_gid;
    m->mtime = invfs_time_pack((int64_t)st->st_mtim.tv_sec, (int64_t)st->st_mtim.tv_nsec);
    m->atime = invfs_time_pack((int64_t)st->st_atim.tv_sec, (int64_t)st->st_atim.tv_nsec);
    m->rdev  = (uint64_t)st->st_rdev;
    /* nlink stays 0: the engine's "per type default" (1 for a file), and
     * vol_hardlink is what raises it. Writing the SOURCE's nlink here would
     * make fsck's fan-in audit compare a count we invented against the names
     * we actually created. */
    m->nlink = 0;
}

/* Can the volume represent this volume-path at all? Every "no" is a named
 * entry in the report, never a silent skip. */
static int vname_representable(const char *vn, const char *sp)
{
    const char *base = strrchr(vn, '/');
    size_t nl;

    base = base ? base + 1 : vn;
    nl = strlen(base);
    if (nl > (size_t)INVFS_MAX_NAME) {
        sv_push(g_fail, "%s: name is %zu bytes, the volume's INVFS_MAX_NAME is %d",
                sp, nl, INVFS_MAX_NAME);
        return 0;
    }
    if (strchr(base, '!') != NULL) {
        /* '!' is the container sibling namespace ("name!part0", "name!jxl").
         * The engine refuses it in every user-name entry point (WP135), so a
         * source name carrying it cannot be created — a loss that has to be
         * named, because nothing in the volume will reference it. */
        sv_push(g_fail, "%s: the name contains '!', which the volume reserves for "
                        "container siblings (WP135)", sp);
        return 0;
    }
    if (strlen(vn) > (size_t)VNAME_CAP) {
        sv_push(g_fail, "%s: volume path is %zu bytes, the engine's name splitter "
                        "holds %d", sp, strlen(vn), VNAME_CAP);
        return 0;
    }
    return 1;
}

static int xattr_engine_owned(const char *n)
{
    /* The engine's own attribute namespace: invfs.class is stamped by the
     * sweep, invfs.anchor marks a volume-owned pack. They have no source
     * counterpart and are not metadata the converter lost. `user.invfs.*` is
     * the daemon's control surface (user.invfs.sweep, user.invfs.stats). */
    return strncmp(n, "invfs.", 6) == 0 || strncmp(n, "user.invfs", 10) == 0;
}

static void imp_xattrs(const char *sp, const char *vn)
{
    ssize_t n, got;
    char *names = NULL;
    const char *p;
    uint64_t id;

    n = llistxattr(sp, NULL, 0);
    if (n < 0) {
        sv_push(g_fail, "%s: listxattr: %s (the source's attributes were not read, "
                        "so they would be lost)", sp, strerror(errno));
        return;
    }
    if (n == 0) return;
    names = (char *)malloc((size_t)n);
    if (!names) { sv_push(g_fail, "%s: out of memory reading xattrs", sp); return; }
    got = llistxattr(sp, names, (size_t)n);
    if (got < 0) {
        free(names);
        sv_push(g_fail, "%s: listxattr: %s", sp, strerror(errno));
        return;
    }
    id = vol_find(g_vol, vn);
    if (!id) {
        free(names);
        sv_push(g_fail, "%s: created but not found in the volume, so its xattrs "
                        "cannot be attached", vn);
        return;
    }
    {
        uint64_t budget = 0;
        for (p = names; p < names + got; p += strlen(p) + 1) {
            ssize_t vl;
            uint8_t *val;
            if (!*p) continue;
            vl = lgetxattr(sp, p, NULL, 0);
            if (vl < 0) {
                sv_push(g_fail, "%s: xattr %s not readable: %s (data loss: the "
                                "source has it, the volume would not)", sp, p,
                        strerror(errno));
                continue;
            }
            budget += (uint64_t)strlen(p) + 1 + (uint64_t)vl;
            val = (uint8_t *)malloc(vl ? (size_t)vl : 1);
            if (!val) { sv_push(g_fail, "%s: out of memory for xattr %s", sp, p); continue; }
            if (vl > 0 && lgetxattr(sp, p, val, (size_t)vl) != vl) {
                sv_push(g_fail, "%s: xattr %s read short", sp, p);
                free(val);
                continue;
            }
            if (vol_set_xattr(g_vol, id, p, val, (size_t)vl) != 0) {
                sv_push(g_fail, "%s: the volume refused xattr %s (%zd bytes)", sp, p, vl);
                free(val);
                continue;
            }
            g_st.xattr_names++;
            g_st.xattr_bytes += (uint64_t)vl;
            free(val);
        }
        /* 64 KiB per inode is the engine's store limit (XATTR_MAX_TOTAL,
         * src/core/vol_btree.c). Reported as a format limit with the measured
         * size, not discovered halfway through a copy. */
        if (budget > 64u * 1024u)
            sv_push(g_fail, "%s: xattrs total %llu bytes, the volume's per-inode "
                            "budget is 65536", sp, (unsigned long long)budget);
    }
    free(names);
}

/* One regular file: create the row, stream the content in CHUNK windows, then
 * apply the metadata as the authoritative last word (vol_write_bulk's own
 * ordering: the commit carries the created row, set_meta decides mode/owner/
 * times). Whole-file buffering is what invf-import does; a rootfs has 100 MiB
 * files, and the session path costs O(segments) metadata, no file data. */
static void imp_file(const char *sp, const char *vn, const struct stat *st)
{
    invfs_meta_pub m;
    uint8_t *buf = NULL;
    invfs_wsession *ws = NULL;
    int fd = -1;
    uint64_t off = 0;

    meta_of(&m, st);
    if ((uint64_t)st->st_size > MAX_FILE_SIZE) {
        sv_push(g_fail, "%s: %llu bytes exceeds the format limit MAX_FILE_SIZE",
                sp, (unsigned long long)st->st_size);
        return;
    }
    if (vol_create_node(g_vol, vn, &m) == 0) {
        sv_push(g_fail, "%s: the volume refused to create the file", sp);
        return;
    }
    if (st->st_size > 0) {
        fd = open(sp, O_RDONLY);
        if (fd < 0) {
            sv_push(g_fail, "%s: open: %s (data loss: the volume would hold an "
                            "empty copy)", sp, strerror(errno));
            return;
        }
        if (vol_write_begin(g_vol, vn, 1, &ws) == 0 || !ws) {
            close(fd);
            sv_push(g_fail, "%s: the volume refused a write session", sp);
            return;
        }
        buf = (uint8_t *)malloc(CHUNK);
        if (!buf) { vol_write_abort(ws); close(fd); sv_push(g_fail, "%s: out of memory", sp); return; }
        for (;;) {
            size_t want = (uint64_t)(st->st_size - (off_t)off) < CHUNK
                        ? (size_t)(st->st_size - (off_t)off) : CHUNK;
            size_t got = 0;
            while (got < want) {
                ssize_t r = read(fd, buf + got, want - got);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    sv_push(g_fail, "%s: read at %llu: %s", sp,
                            (unsigned long long)(off + got), strerror(errno));
                    goto abort;
                }
                if (r == 0) break;
                got += (size_t)r;
            }
            if (got == 0) break;         /* the source shrank under us */
            if (vol_write_range(ws, off, buf, got) != 0) {
                sv_push(g_fail, "%s: the volume refused %zu bytes at offset %llu "
                                "(ENOSPC?)", sp, got, (unsigned long long)off);
                goto abort;
            }
            off += got;
            if (got < want) break;
        }
        if (vol_write_commit(ws) != 0) {
            /* commit failed: the session is dead (abort is still the owner) */
            sv_push(g_fail, "%s: the volume refused to commit the content", sp);
            vol_write_abort(ws);
            goto out;
        }
        if (off != (uint64_t)st->st_size)
            sv_push(g_fail, "%s: imported %llu of %llu bytes (the source changed "
                            "during the copy)", sp, (unsigned long long)off,
                    (unsigned long long)st->st_size);
        close(fd); fd = -1;
        free(buf); buf = NULL;
    }
    if (vol_apply_meta(g_vol, vn, &m) == 0) {
        sv_push(g_fail, "%s: the volume refused the metadata (mode/owner/mtime)", sp);
        return;
    }
    g_st.files++;
    g_st.content_bytes += (unsigned long long)st->st_size;
    if ((unsigned long long)st->st_size > g_st.max_file) g_st.max_file = (unsigned long long)st->st_size;
    imp_xattrs(sp, vn);
    return;

abort:
    vol_write_abort(ws);
out:
    if (fd >= 0) close(fd);
    free(buf);
}

static void imp_symlink(const char *sp, const char *vn, const struct stat *st)
{
    char tgt[INVFS_META_TARGET_MAX * 4];
    invfs_meta_pub m;
    ssize_t tl;

    tl = readlink(sp, tgt, sizeof tgt - 1);
    if (tl < 0) {
        sv_push(g_fail, "%s: readlink: %s", sp, strerror(errno));
        return;
    }
    tgt[tl] = 0;
    if ((size_t)tl >= (size_t)INVFS_META_TARGET_MAX) {
        /* Measured limit, not a guess: a symlink target rides in the recipe
         * blob and INVFS_META_TARGET_MAX is 1024 (invarifs.h). ext4 allows
         * more, so this is a real (rare) format limit and it is named. */
        sv_push(g_fail, "%s: symlink target is %zd bytes, the volume's "
                        "INVFS_META_TARGET_MAX is %u", sp, tl, INVFS_META_TARGET_MAX);
        return;
    }
    meta_of(&m, st);
    memcpy(m.target, tgt, (size_t)tl + 1);   /* tl < INVFS_META_TARGET_MAX, checked above */
    m.mode = 0777;                       /* the engine presents LNK as 0777 */
    if (vol_create_node(g_vol, vn, &m) == 0) {
        sv_push(g_fail, "%s: the volume refused to create the symlink", sp);
        return;
    }
    if (vol_apply_meta(g_vol, vn, &m) == 0) {
        sv_push(g_fail, "%s: the volume refused the symlink metadata", sp);
        return;
    }
    g_st.links++;
    imp_xattrs(sp, vn);
}

static void imp_special(const char *sp, const char *vn, const struct stat *st)
{
    invfs_meta_pub m;
    uint8_t type;

    switch (st->st_mode & S_IFMT) {
    case S_IFIFO:  type = INVFS_ITYP_FIFO; break;
    case S_IFSOCK: type = INVFS_ITYP_SOCK; break;
    case S_IFCHR:  type = INVFS_ITYP_CHR;  break;
    default:       type = INVFS_ITYP_BLK;  break;
    }
    if (vol_create_special(g_vol, vn, type, (uint16_t)(st->st_mode & 07777),
                           (uint64_t)st->st_rdev) == 0) {
        sv_push(g_fail, "%s: the volume refused to create the special file", sp);
        return;
    }
    meta_of(&m, st);
    if (vol_apply_meta(g_vol, vn, &m) == 0) {
        sv_push(g_fail, "%s: the volume refused the metadata", sp);
        return;
    }
    g_st.specials++;
    imp_xattrs(sp, vn);
}

static void imp_dir(const char *sp, const char *vn, int depth);

static void imp_entry(const char *sp, const char *vn, int depth)
{
    struct stat st;

    if (lstat(sp, &st) != 0) {
        sv_push(g_fail, "%s: lstat: %s (the source changed under the walk)", sp, strerror(errno));
        return;
    }
    if (!vname_representable(vn, sp)) return;
    switch (st.st_mode & S_IFMT) {
    case S_IFDIR:
        if (vol_mkdir(g_vol, vn) == 0 && !vol_is_dir(g_vol, vn)) {
            char detail[160];
            snprintf(detail, sizeof detail, "%s", strerror(errno));
            sv_push(g_fail, "%s: the volume refused to create the directory%s%s",
                    sp, detail[0] ? ": " : "", detail);
            return;
        }
        g_st.dirs++;
        if (depth < 64) imp_dir(sp, vn, depth + 1);
        else sv_push(g_fail, "%s: nested deeper than 64 levels; everything below it "
                             "was not imported", sp);
        {
            /* POST-order: children were created under this directory after it
             * existed, so its own mode/owner/mtime is written last and nothing
             * the import did to its subtree can overwrite it. */
            invfs_meta_pub m;
            meta_of(&m, &st);
            if (vol_apply_meta(g_vol, vn, &m) == 0)
                sv_push(g_fail, "%s: the volume refused the directory metadata", sp);
            else
                imp_xattrs(sp, vn);
        }
        break;
    case S_IFREG: {
        const char *first = NULL;
        if (st.st_nlink > 1) {
            first = hl_lookup((uint64_t)st.st_dev, (uint64_t)st.st_ino);
            if (first) {
                /* Hardlinks are one inode with nlink > 1 — the shape the engine
                 * models (vol_hardlink: a second dirent on the same row, count
                 * bumped BEFORE the name is visible). Two independent copies
                 * would pass a content hash and still be a different
                 * filesystem: `ln` semantics would be gone. */
                if (vol_hardlink(g_vol, first, vn) != 0) {
                    sv_push(g_fail, "%s: the volume refused the hardlink to %s", sp, first);
                    return;
                }
                g_st.hardlink_names++;
                return;
            }
            hl_insert((uint64_t)st.st_dev, (uint64_t)st.st_ino, vn);
            g_st.hardlink_inodes++;
        }
        g_st.reg_inodes++;
        tino_add((uint64_t)st.st_ino, (uint64_t)st.st_size);
        imp_file(sp, vn, &st);
        break;
    }
    case S_IFLNK:
        imp_symlink(sp, vn, &st);
        break;
    case S_IFIFO: case S_IFSOCK: case S_IFCHR: case S_IFBLK:
        imp_special(sp, vn, &st);
        break;
    default:
        sv_push(g_fail, "%s: file type 0%o cannot be named by the volume", sp,
                (unsigned)(st.st_mode & S_IFMT));
        break;
    }
}

static void imp_dir(const char *spath, const char *vname, int depth)
{
    DIR *d = opendir(spath);
    struct dirent *de;

    if (!d) {
        /* One increment, one lost SUBTREE: named, with the subtree spelled out
         * (the WP138 lesson — this is the site that hid /root). */
        sv_push(g_fail, "%s: opendir: %s — every entry below it is missing from "
                        "the volume", spath, strerror(errno));
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char sp[P_CAP], vn[P_CAP];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (snprintf(sp, sizeof sp, "%s%s%s", spath, spath[strlen(spath) - 1] == '/' ? "" : "/",
                     de->d_name) >= (int)sizeof sp) {
            sv_push(g_fail, "%s/%s: source path exceeds %d bytes", spath, de->d_name, P_CAP);
            continue;
        }
        if (snprintf(vn, sizeof vn, "%s%s%s", vname, vname[0] ? "/" : "", de->d_name)
            >= (int)sizeof vn) {
            sv_push(g_fail, "%s: volume path exceeds %d bytes", sp, P_CAP);
            continue;
        }
        imp_entry(sp, vn, depth);
    }
    closedir(d);
}

/* Destination sizing. The floor is the source image's own size rounded up: a
 * conversion that only fits when the source was half empty is a land mine for
 * the operator, and btrfs-convert's shape (same size out) is the least
 * surprising one. The engine needs, per the measurements in AGENTS.md §2.7,
 * ~2.03 META blocks (8.3 KiB) per file plus the file's own bytes, so the
 * estimate is content + 16 KiB/inode + 64 MiB of working room. */
static double dest_size_gb(uint64_t src_image_bytes, convstats *st)
{
    double need = (double)st->content_bytes
                + 16.0 * 1024.0 * (double)(st->reg_inodes + st->dirs + st->links + st->specials)
                + 64.0 * 1024.0 * 1024.0;
    double src = (double)src_image_bytes;
    double gb = (need > src ? need : src) / (1024.0 * 1024.0 * 1024.0);
    /* mkfs accepts a fractional GiB and refuses < 64 MiB. */
    if (gb < 0.0625) gb = 0.0625;
    return gb;
}

/* ------------------------------------------------------------- reporting */

static void print_fails(const char *what, strvec *v)
{
    size_t i;
    if (!v || !v->n) return;
    fprintf(stderr, "\n%s: %zu problem(s):\n", what, v->n);
    for (i = 0; i < v->n; i++) fprintf(stderr, "  %s\n", v->v[i]);
}

/* ------------------------------------------------------------------ gates */

/* The engine's own gates on the result, run as the engine ships them (the
 * binaries next to this one), so a converter cannot disagree with the checker
 * it will be judged by. */
static int run_gates(const char *dst, strvec *notes)
{
    char bin[P_CAP], log[8192];
    char *argv[5];
    int status = -1, bad = 0;

    if (!xbin("invf-fsck", bin, sizeof bin)) {
        sv_push(notes, "gate: invf-fsck not found next to this binary");
        return -1;
    }
    argv[0] = bin; argv[1] = (char *)dst; argv[2] = NULL;
    if (exec_capture(argv, log, sizeof log, &status) != 0) {
        sv_push(notes, "gate: cannot run invf-fsck");
        return -1;
    }
    {
        /* The verdict, not the first line: vol_open's own log line ("format v1
         * ... base root engine up") is noise, and a gate summary that shows
         * noise instead of OK/DAMAGED is a summary nobody reads. */
        char *verdict = NULL, *p2 = log;
        while (p2 && *p2) {
            char *nl = strchr(p2, '\n');
            size_t len = nl ? (size_t)(nl - p2) : strlen(p2);
            if (len == 2 && strncmp(p2, "OK", 2) == 0) verdict = p2;
            else if (len >= 7 && strncmp(p2, "DAMAGED", 7) == 0) verdict = p2;
            else if (len >= 8 && strncmp(p2, "REPAIRED", 8) == 0) verdict = p2;
            p2 = nl ? nl + 1 : NULL;
        }
        if (verdict) { char *nl = strchr(verdict, '\n'); if (nl) *nl = 0; }
        sv_push(notes, "gate: invf-fsck exit %d: %s", status, verdict ? verdict : log);
    }
    if (status != 0) bad = 1;

    if (!xbin("invf-verify", bin, sizeof bin)) {
        sv_push(notes, "gate: invf-verify not found next to this binary");
        return -1;
    }
    {
        char *av[4];
        av[0] = bin; av[1] = (char *)dst; av[2] = (char *)"--deep"; av[3] = NULL;
        if (exec_capture(av, log, sizeof log, &status) != 0) {
            sv_push(notes, "gate: cannot run invf-verify --deep");
            return -1;
        }
    }
    {
        /* invf-verify --deep prints one summary line on success; keep the LAST
         * non-empty line, which is the verdict, and the corrupt count when it
         * names one. */
        char *last = NULL, *p = log;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (*p != '\n') last = p;
            if (!nl) break;
            p = nl + 1;
        }
        if (last) {
            char *nl = strchr(last, '\n');
            if (nl) *nl = 0;
        }
        sv_push(notes, "gate: invf-verify --deep exit %d: %s", status, last ? last : "");
    }
    if (status != 0) bad = 1;
    return bad ? -1 : 0;
}

/* ---------------------------------------------------------------- verify */

typedef struct {
    char *path;
    mode_t type;              /* S_IFMT of the source inode, compared verbatim */
    uint16_t mode;
    uint32_t uid, gid;
    uint64_t size;
    int64_t mtime_sec;
    int32_t mtime_nsec;
    uint64_t dev, ino;
    uint32_t nlink;
    uint64_t rdev;
    uint64_t eng_id;          /* the volume inode id this path resolved to (0 = unknown) */
    int has_size;             /* REG/LNK only: a directory's size is an allocator artefact */
} vent;

typedef struct {
    vent *v;
    size_t n, cap;
} ventvec;

static void vv_push(ventvec *vv, const char *path, const struct stat *st)
{
    vent *e;
    if (vv->n == vv->cap) {
        size_t nc = vv->cap ? vv->cap * 2 : 512;
        vent *nv = (vent *)realloc(vv->v, nc * sizeof *nv);
        if (!nv) { fprintf(stderr, "invf-convert: out of memory\n"); exit(XV_ERR); }
        vv->v = nv;
        vv->cap = nc;
    }
    e = &vv->v[vv->n++];
    memset(e, 0, sizeof *e);
    e->path = strdup(path);
    e->type = st->st_mode & S_IFMT;
    e->mode = (uint16_t)(st->st_mode & 07777);
    e->uid = (uint32_t)st->st_uid;
    e->gid = (uint32_t)st->st_gid;
    e->size = (uint64_t)st->st_size;
    e->mtime_sec = (int64_t)st->st_mtim.tv_sec;
    e->mtime_nsec = (int32_t)st->st_mtim.tv_nsec;
    e->dev = (uint64_t)st->st_dev;
    e->ino = (uint64_t)st->st_ino;
    e->nlink = (uint32_t)st->st_nlink;
    e->rdev = (uint64_t)st->st_rdev;
    e->has_size = (S_ISREG(st->st_mode) || S_ISLNK(st->st_mode));
}

static void vv_free(ventvec *vv)
{
    size_t i;
    for (i = 0; i < vv->n; i++) free(vv->v[i].path);
    free(vv->v);
    vv->v = NULL;
    vv->n = vv->cap = 0;
}

static int vent_cmp(const void *a, const void *b)
{
    return strcmp(((const vent *)a)->path, ((const vent *)b)->path);
}

/* Every path under root, symlinks NOT followed (a symlink is an entry, and
 * following one would compare content the source does not have at that path). */
static int collect_tree(const char *root, const char *rel, ventvec *vv, strvec *fail)
{
    char full[P_CAP];
    DIR *d;
    struct dirent *de;

    if (snprintf(full, sizeof full, "%s%s%s", root, rel[0] ? "/" : "", rel) >= (int)sizeof full) {
        sv_push(fail, "%s: path exceeds %d bytes", rel, P_CAP);
        return -1;
    }
    d = opendir(full);
    if (!d) {
        sv_push(fail, "%s: opendir: %s", full, strerror(errno));
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char crel[P_CAP], cfull[P_CAP];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (snprintf(crel, sizeof crel, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name)
            >= (int)sizeof crel ||
            snprintf(cfull, sizeof cfull, "%s/%s", root, crel) >= (int)sizeof cfull) {
            sv_push(fail, "%s: path exceeds %d bytes", de->d_name, P_CAP);
            continue;
        }
        if (lstat(cfull, &st) != 0) {
            sv_push(fail, "%s: lstat: %s", cfull, strerror(errno));
            continue;
        }
        vv_push(vv, crel, &st);
        if (S_ISDIR(st.st_mode)) collect_tree(root, crel, vv, fail);
    }
    closedir(d);
    return 0;
}

/* BLAKE3 of a file's LOGICAL content. Holes read as zeros, which is exactly
 * what the comparison is about: a sparse file and a dense copy of it are the
 * same file, and this is the read that proves the volume serves those zeros. */
static int hash_file(const char *path, uint8_t out[BLAKE3_OUT_LEN], uint64_t *bytes, strvec *fail)
{
    int fd = open(path, O_RDONLY);
    uint8_t *buf;
    blake3_hasher h;
    ssize_t r;

    if (fd < 0) { sv_push(fail, "%s: open: %s", path, strerror(errno)); return -1; }
    buf = (uint8_t *)malloc(CHUNK);
    if (!buf) { close(fd); sv_push(fail, "%s: out of memory", path); return -1; }
    blake3_hasher_init(&h);
    *bytes = 0;
    while ((r = read(fd, buf, CHUNK)) > 0) {
        blake3_hasher_update(&h, buf, (size_t)r);
        *bytes += (uint64_t)r;
    }
    if (r < 0) {
        sv_push(fail, "%s: read: %s", path, strerror(errno));
        free(buf); close(fd);
        return -1;
    }
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    free(buf);
    close(fd);
    return 0;
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    static const char *hx = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 15];
    }
    out[n * 2] = 0;
}

/* xattrs present on <path>: names in a NUL-separated list (malloc'd). */
static char *xattr_names(const char *path, ssize_t *len, strvec *fail)
{
    ssize_t n = llistxattr(path, NULL, 0);
    char *b;
    if (n < 0) {
        sv_push(fail, "%s: listxattr: %s", path, strerror(errno));
        *len = 0;
        return NULL;
    }
    *len = n;
    if (!n) return NULL;
    b = (char *)malloc((size_t)n);
    if (!b) return NULL;
    if (llistxattr(path, b, (size_t)n) < 0) { free(b); *len = 0; return NULL; }
    return b;
}

static int xattr_get(const char *path, const char *name, uint8_t **val, size_t *vlen)
{
    ssize_t n = lgetxattr(path, name, NULL, 0);
    if (n < 0) return -1;
    *val = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (!*val) return -1;
    if (n > 0 && lgetxattr(path, name, *val, (size_t)n) != n) { free(*val); *val = NULL; return -1; }
    *vlen = (size_t)n;
    return 0;
}

typedef struct {
    unsigned long long n_diffs;
    unsigned long long n_printed;
    unsigned long long max_print;
} diffstate;

static void diff(diffstate *ds, const char *kind, const char *path, const char *fmt, ...)
{
    va_list ap;
    ds->n_diffs++;
    if (ds->n_printed >= ds->max_print) return;
    ds->n_printed++;
    printf("DIFF %-14s %s", kind, path);
    if (fmt && *fmt) {
        fputc(' ', stdout);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    fputc('\n', stdout);
}

/* hardlink structure: for each inode with more than one name on a side, the
 * names that share it. Compared as a PARTITION, not as numbers: `ln a b` and
 * `cp a b` both give two files with identical bytes and nlink 1 vs 2 is the
 * only difference — and a converter that inlines every hardlink has silently
 * multiplied the storage of every dedup'ed rootfs. */
typedef struct {
    uint64_t ino;
    size_t first, count;   /* indices into ventvec */
} hlgroup;

static int hlg_cmp(const void *a, const void *b)
{
    const hlgroup *x = (const hlgroup *)a, *y = (const hlgroup *)b;
    if (x->ino != y->ino) return x->ino < y->ino ? -1 : 1;
    return x->first < y->first ? -1 : x->first > y->first;
}

/* ventvec is sorted by path, so grouping by inode means one sort per side. */
static size_t build_groups(ventvec *vv, hlgroup **out)
{
    size_t i, n = 0, cap = 0;
    hlgroup *g = NULL;

    for (i = 0; i < vv->n; ) {
        size_t j = i + 1;
        while (j < vv->n && vv->v[j].ino == vv->v[i].ino) j++;
        if (j - i > 1 && vv->v[i].type == S_IFREG) {
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 64;
                hlgroup *ng = (hlgroup *)realloc(g, nc * sizeof *ng);
                if (!ng) break;
                g = ng;
                cap = nc;
            }
            g[n].ino = vv->v[i].ino;
            g[n].first = i;
            g[n].count = j - i;
            n++;
        }
        i = j;
    }
    /* Instances of the same inode are not adjacent in a path-sorted array. */
    if (n) {
        size_t k, m = 0;
        qsort(g, n, sizeof *g, hlg_cmp);
        for (k = 0, m = 0; k < n; k++) {
            if (m && g[m - 1].ino == g[k].ino) g[m - 1].count += g[k].count;
            else g[m++] = g[k];
        }
        n = m;
    }
    *out = g;
    return n;
}

/* The inode a PATH belongs to on a side; -1 when the path is not there. */
static long ino_of(ventvec *vv, const char *path)
{
    size_t lo = 0, hi = vv->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(vv->v[mid].path, path);
        if (c == 0) return (long)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

static vent *find_ent(ventvec *vv, const char *path)
{
    long i = ino_of(vv, path);
    return i < 0 ? NULL : &vv->v[i];
}

/* The engine's inode id for a source path, read from the DESTINATION IMAGE
 * (not through the mount). One open for the whole comparison. */
typedef struct {
    int have;
} destmap;

static uint64_t dest_ino_of(const vent *e, destmap *dm)
{
    if (!dm || !dm->have || e->type != S_IFREG) return 0;
    return e->eng_id;
}

static int cmd_verify(int argc, char **argv);
static int cmd_convert(int argc, char **argv);

/* ------------------------------------------------------------------- CLI */

static void usage(FILE *f, const char *me)
{
    fprintf(f,
"usage: %s convert <src-image|src-dir> <dst-image> [options]\n"
"       %s verify  <src-image|src-dir> <dst-image|dst-dir> [options]\n"
"       %s source-map <src-image>\n"
"\n"
"convert  reads the source's byte map with the ext4fs codecpack (its `classify`\n"
"         GCFG table), loop-mounts the source read-only, imports the tree into a\n"
"         fresh invf-mkfs volume (mode/uid/gid/nanosecond mtime, symlinks,\n"
"         hardlinks as one inode, xattrs, specials, real file content), then runs\n"
"         invf-fsck and invf-verify --deep. Any path the volume cannot represent\n"
"         ABORTS the run and removes the destination.\n"
"verify   mounts both ends (loop read-only / fuse) and compares every path:\n"
"         presence, type, size, mode, uid, gid, mtime (sec+nsec), BLAKE3 of the\n"
"         logical content, symlink targets, hardlink identity and xattrs. Exits\n"
"         %d when anything differs, %d when the run itself could not happen.\n"
"source-map  print the pack's classification of a source image and stop.\n"
"\n"
"options (convert):\n"
"  --pack-dir=DIR     ext4fs codecpack dir (default $INVFS_CONVERT_PACK or\n"
"                     <repo>/tools/codecpacks/ext4fs.codecpack)\n"
"  --size=GB          destination size (default: max(source image size,\n"
"                     content + 16 KiB/inode) in GiB)\n"
"  --meta-frac=N      INVFS_META_FRAC for invf-mkfs (default: leave mkfs's own\n"
"                     default; rootfs imports want 16 per AGENTS.md 2.7)\n"
"  --keep-partial     keep the destination image when the run fails\n"
"  --no-gates         skip invf-fsck / invf-verify --deep\n"
"  --mount-root=DIR   where the loop / fuse mounts are created (default\n"
"                     /var/tmp/invf-convert.XXXXXX)\n"
"\n"
"options (verify):\n"
"  --max-diffs=N      print at most N differences (default 200; the count is\n"
"                     always exact)\n"
"  --mount-root=DIR   same as above\n"
"\n"
"Both commands need root: the source is loop-mounted and a rootfs contains\n"
"files an ordinary user cannot read.\n",
            me, me, me, XV_DIFF, XV_ERR);
}

int main(int argc, char **argv)
{
    g_argv0 = argv[0];
    resolve_bindir();
    if (argc < 2) { usage(stderr, argv[0]); return XV_ERR; }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return XV_OK;
    }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME,
                INVFS_LICENSE);
        return XV_OK;
    }
    if (strcmp(argv[1], "convert") == 0) return cmd_convert(argc - 1, argv + 1);
    if (strcmp(argv[1], "verify") == 0) return cmd_verify(argc - 1, argv + 1);
    if (strcmp(argv[1], "source-map") == 0) {
        /* classify only: the phase-1 space rule's input, without a conversion. */
        strvec fail = { 0 };
        char packdir[P_CAP], tmpl[P_CAP], cls[P_CAP], log[4096];
        packinfo pi;
        int status, rc = XV_ERR, i;
        const char *img = NULL;
        memset(&pi, 0, sizeof pi);
        for (i = 2; i < argc; i++) {
            if (argv[i][0] != '-' && strncmp(argv[i], "--", 2) != 0) img = argv[i];
        }
        if (!img) { usage(stderr, argv[0]); return XV_ERR; }
        {
            const char *pd = NULL;
            for (i = 2; i < argc; i++)
                if (strncmp(argv[i], "--pack-dir=", 11) == 0) pd = argv[i] + 11;
            if (resolve_pack(pd, packdir, sizeof packdir, &fail) != 0) {
                print_fails("source-map", &fail);
                sv_free(&fail);
                return XV_REFUSED;
            }
        }
        snprintf(tmpl, sizeof tmpl, "/tmp/invf-convert-map.XXXXXX");
        if (!mkdtemp(tmpl)) { die("mkdtemp: %s", strerror(errno)); return XV_ERR; }
        jcat(cls, sizeof cls, tmpl, "/classify", "");
        status = pack_verb(packdir, "classify", img, cls, log, sizeof log);
        if (status != 0) {
            sv_push(&fail, "the ext4fs pack REFUSED %s (exit %d): %s", img, status,
                    log[0] ? log : "no reason given");
            print_fails("source-map", &fail);
            rm_rf(tmpl);
            sv_free(&fail);
            return XV_REFUSED;
        }
        if (gcfg_load(cls, &pi, &fail) == 0) {
            char b1[32], b2[32], b3[32], b4[32];
            printf("image      : %s\n", img);
            printf("byte map   : GCFG v1, %u ranges, %llu-byte blocks\n",
                   pi.n_ent, (unsigned long long)pi.block_size);
            printf("  meta_structure : %14llu B  %s\n",
                   (unsigned long long)pi.bytes[0], human((double)pi.bytes[0], b1, sizeof b1));
            printf("  garbage        : %14llu B  %s (zero-verified %s)\n",
                   (unsigned long long)pi.bytes[1], human((double)pi.bytes[1], b2, sizeof b2),
                   human((double)pi.zero_garbage, b3, sizeof b3));
            printf("  member         : %14llu B  %s\n",
                   (unsigned long long)pi.bytes[2], human((double)pi.bytes[2], b4, sizeof b4));
            printf("  carry (meta+member) = %llu B of %llu B (%s)\n",
                   (unsigned long long)(pi.bytes[0] + pi.bytes[2]),
                   (unsigned long long)pi.image_size,
                   human((double)(pi.bytes[0] + pi.bytes[2]), b1, sizeof b1));
            rc = XV_OK;
        } else {
            print_fails("source-map", &fail);
            rc = XV_REFUSED;
        }
        rm_rf(tmpl);
        sv_free(&fail);
        return rc;
    }
    die("unknown command '%s'", argv[1]);
    usage(stderr, argv[0]);
    return XV_ERR;
}

/* ------------------------------------------------------------- convert */

static int cmd_convert(int argc, char **argv)
{
    const char *src = NULL, *dst = NULL, *packdir_opt = NULL, *size_opt = NULL;
    const char *mount_root_opt = NULL;
    int meta_frac = 0, keep_partial = 0, gates = 1, i, rc = XV_ERR;
    char packdir[P_CAP], mount_root[P_CAP], tmpl[P_CAP];
    char clsfile[P_CAP], enumfile[P_CAP], log[8192];
    packinfo pi;
    mountpt srcmp;
    strvec fail = { 0 }, notes = { 0 };
    double t_start = now_s(), t_classify, t_import, t_gates;
    int status = 0;
    int dst_created = 0;

    memset(&pi, 0, sizeof pi);
    memset(&srcmp, 0, sizeof srcmp);
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--pack-dir=", 11) == 0) packdir_opt = argv[i] + 11;
        else if (strncmp(argv[i], "--size=", 7) == 0) size_opt = argv[i] + 7;
        else if (strncmp(argv[i], "--meta-frac=", 12) == 0) meta_frac = atoi(argv[i] + 12);
        else if (strncmp(argv[i], "--mount-root=", 13) == 0) mount_root_opt = argv[i] + 13;
        else if (strcmp(argv[i], "--keep-partial") == 0) keep_partial = 1;
        else if (strcmp(argv[i], "--no-gates") == 0) gates = 0;
        else if (argv[i][0] == '-') { die("unknown option %s", argv[i]); return XV_ERR; }
        else if (!src) src = argv[i];
        else if (!dst) dst = argv[i];
        else { die("too many arguments"); return XV_ERR; }
    }
    if (!src || !dst) { usage(stderr, g_argv0); return XV_ERR; }
    if (geteuid() != 0)
        fprintf(stderr, "invf-convert: WARNING: not root — the source is loop-mounted "
                        "read-only and root-only files will be UNREADABLE, which this "
                        "tool treats as data loss and refuses.\n");

    /* 1. the pack, and the source's byte map, BEFORE the volume exists: an
     * unsupported source must not leave a destination behind. */
    t_classify = now_s();
    if (resolve_pack(packdir_opt, packdir, sizeof packdir, &fail) != 0) { rc = XV_REFUSED; goto out_fail; }
    {
        struct stat sst;
        if (stat(src, &sst) != 0) {
            sv_push(&fail, "cannot stat %s: %s", src, strerror(errno));
            rc = XV_ERR;
            goto out_fail;
        }
        if (S_ISDIR(sst.st_mode)) {
            sv_push(&fail, "%s is a directory: the byte map comes from an image "
                           "(classify reads the ext4 structures). Point convert at the "
                           "image, or leave verify to compare trees.", src);
            rc = XV_ERR;
            goto out_fail;
        }
    }
    snprintf(mount_root, sizeof mount_root, "%s",
             mount_root_opt ? mount_root_opt : "/var/tmp/invf-convert.XXXXXX");
    snprintf(tmpl, sizeof tmpl, "%s", mount_root);
    if (!mkdtemp(tmpl)) {
        /* an explicit --mount-root may already exist; mkdtemp only works on a
           template */
        if (mount_root_opt) {
            if (mkdir(tmpl, 0700) != 0 && errno != EEXIST) {
                die("cannot create %s: %s", tmpl, strerror(errno));
                return XV_ERR;
            }
        } else {
            die("mkdtemp(%s): %s", tmpl, strerror(errno));
            return XV_ERR;
        }
    }
    jcat(clsfile, sizeof clsfile, tmpl, "/classify.gcfg", "");
    jcat(enumfile, sizeof enumfile, tmpl, "/enumerate.txt", "");
    status = pack_verb(packdir, "classify", src, clsfile, log, sizeof log);
    if (status != 0) {
        sv_push(&fail, "the ext4fs pack REFUSED this source (classify exit %d)%s%s",
                status, log[0] ? ": " : "", log);
        rc = XV_REFUSED;
        goto out_fail;
    }
    if (gcfg_load(clsfile, &pi, &fail) != 0) { rc = XV_REFUSED; goto out_fail; }
    status = pack_verb(packdir, "enumerate", src, enumfile, log, sizeof log);
    if (status != 0) {
        sv_push(&fail, "the ext4fs pack REFUSED this source (enumerate exit %d)%s%s",
                status, log[0] ? ": " : "", log);
        rc = XV_REFUSED;
        goto out_fail;
    }
    (void)enumerate_load(enumfile, &pi, &fail);
    t_classify = now_s() - t_classify;

    /* 2. the tree: mount the source read-only and import it. */
    if (src_mount(src, tmpl, &srcmp, &fail) != 0) { rc = XV_ERR; goto out_fail; }
    t_import = now_s();
    {
        int err = 0;
        struct stat rst;
        char mkfs[P_CAP];
        char sizebuf[64];

        /* Destination size needs the tree's byte count, which needs the walk;
         * two passes over the source (measure, then import) would double the
         * read cost of a rootfs. Instead: size on the SOURCE IMAGE (known
         * before the walk) with the content estimate folded in from the pack's
         * member total, which is the same number to within the hardlink/block
         * sharing that the estimate's per-inode margin covers. */
        snprintf(sizebuf, sizeof sizebuf, "%.6f",
                 size_opt ? atof(size_opt) : dest_size_gb(pi.image_size, &g_st));
        {
            /* the estimate uses the pack's member bytes, not g_st (not walked
             * yet): patch them in for the sizing call only. */
            convstats est;
            memset(&est, 0, sizeof est);
            est.content_bytes = pi.member_bytes;
            est.reg_inodes = pi.member_inodes;
            if (!size_opt) {
                double gb_est = dest_size_gb(pi.image_size, &est);
                snprintf(sizebuf, sizeof sizebuf, "%.6f", gb_est);
            }
        }
        if (!xbin("invf-mkfs", mkfs, sizeof mkfs)) {
            sv_push(&fail, "cannot find invf-mkfs next to this binary (%s)", g_bindir);
            rc = XV_ERR;
            goto out_fail;
        }
        {
            char *av[4];
            char mlog[8192];
            av[0] = mkfs; av[1] = (char *)dst; av[2] = sizebuf; av[3] = NULL;
            if (meta_frac > 0) {
                char env[32];
                snprintf(env, sizeof env, "%d", meta_frac);
                setenv("INVFS_META_FRAC", env, 1);
            }
            if (exec_capture(av, mlog, sizeof mlog, &status) != 0 || status != 0) {
                sv_push(&fail, "invf-mkfs %s %s failed (exit %d): %s", dst, sizebuf,
                        status, mlog);
                rc = XV_ERR;
                goto out_fail;
            }
            dst_created = 1;
        }
        g_vol = vol_open(dst, &err);
        if (!g_vol) {
            sv_push(&fail, "vol_open(%s) failed (err %d)", dst, err);
            rc = XV_ERR;
            goto out_fail;
        }
        g_fail = &fail;
        /* root's own metadata: applied AFTER the walk (its mtime must not be
         * bumped by anything the walk does to its children). */
        if (lstat(srcmp.root, &rst) == 0) {
            /* remember it for the post-walk stamp */
        }
        imp_dir(srcmp.root, "", 0);
        if (lstat(srcmp.root, &rst) == 0) {
            invfs_meta_pub rm;
            meta_of(&rm, &rst);
            (void)vol_apply_meta(g_vol, "", &rm);
        }
        vol_flush(g_vol);
        vol_close(g_vol);
        g_vol = NULL;
        hl_free();
    }
    t_import = now_s() - t_import;
    src_unmount(&srcmp);

    /* 3. the gates, on the finished volume, unmounted. */
    t_gates = now_s();
    if (gates) (void)run_gates(dst, &notes);
    t_gates = now_s() - t_gates;

    /* 4. the report. */
    {
        char b1[32], b2[32], b3[32], b4[32];
        uint64_t carry = pi.bytes[0] + pi.bytes[2];
        printf("== invf-convert: ext4 -> InvariantFS (phase 0) ==\n");
        printf("source      : %s\n", src);
        printf("pack        : %s (classify + enumerate, exec'd from the pack dir)\n", packdir);
        printf("byte map    : %llu B image, %llu-byte blocks, %u ranges, tiles the image\n",
               (unsigned long long)pi.image_size, (unsigned long long)pi.block_size, pi.n_ent);
        printf("  meta_structure %14llu B  %s\n", (unsigned long long)pi.bytes[0],
               human((double)pi.bytes[0], b1, sizeof b1));
        printf("  garbage        %14llu B  %s (zero-verified %s)\n",
               (unsigned long long)pi.bytes[1], human((double)pi.bytes[1], b2, sizeof b2),
               human((double)pi.zero_garbage, b3, sizeof b3));
        printf("  member         %14llu B  %s\n", (unsigned long long)pi.bytes[2],
               human((double)pi.bytes[2], b4, sizeof b4));
        printf("  carry = meta+member = %llu B (%s) -- the space a phase-1 copy needs\n",
               (unsigned long long)carry, human((double)carry, b1, sizeof b1));
        printf("tree import : %llu dirs, %llu files (%llu hardlink names in %llu inodes), "
               "%llu symlinks, %llu specials\n",
               g_st.dirs, g_st.files, g_st.hardlink_names, g_st.hardlink_inodes,
               g_st.links, g_st.specials);
        printf("              %llu B of content, largest file %s\n",
               g_st.content_bytes, human((double)g_st.max_file, b1, sizeof b1));
        printf("              %llu xattr name(s) / %llu B\n", g_st.xattr_names, g_st.xattr_bytes);
        printf("pack oracle : %llu member inode(s) / %llu B enumerated; tree carried %llu "
               "unique inode(s) / %llu B",
               (unsigned long long)pi.member_inodes, (unsigned long long)pi.member_bytes,
               g_st.reg_inodes, g_st.content_bytes);
        printf("\n");
        printf("destination : %s\n", dst);
        printf("timing      : classify %.2f s, import %.2f s, gates %.2f s, total %.2f s\n",
               t_classify, t_import, t_gates, now_s() - t_start);
        for (i = 0; i < (int)notes.n; i++) printf("%s\n", notes.v[i]);
    }
    /* The oracle, inode by inode. A member is CARRIED when the tree holds a
     * regular inode with that ext4 inode number and that i_size. Anything left
     * over is one of two very different things and the report says which:
     *
     *   - the pack's name column is a PATH  -> a real file the tree walk did
     *     not import. That is data loss, and it aborts the run;
     *   - the name column is "ino<N>"       -> no dirent names this inode. ext4
     *     keeps its own inodes this way (the journal, the resize inode, the
     *     quota files, orphan_file), and debris from an unlink can look the
     *     same. Nothing a user can reach is lost, so it is REPORTED with its
     *     inode number and size, not silently dropped and not used to fail a
     *     conversion of a healthy image.
     *
     * A byte total alone cannot tell those apart, which is why the check is
     * per inode: on this corpus the journal's 128 KiB made a correct import
     * look 131072 B short (measured 2026-11-27). */
    {
        size_t k, unmatched = 0;
        uint64_t unmatched_bytes = 0;
        qsort(g_tino, g_n_tino, sizeof *g_tino, tino_cmp);
        for (k = 0; k < pi.n_mem; k++) {
            const tino *t = tino_find(pi.mem[k].ino);
            int named = !(pi.mem[k].name[0] == 'i' && strncmp(pi.mem[k].name, "ino", 3) == 0 &&
                          pi.mem[k].name[3] >= '0' && pi.mem[k].name[3] <= '9');
            if (t && t->size == pi.mem[k].usize) continue;
            if (t) {
                sv_push(&fail, "inode %llu (%s): the pack reports %llu bytes, the tree "
                               "carried %llu", (unsigned long long)pi.mem[k].ino,
                        pi.mem[k].name, (unsigned long long)pi.mem[k].usize,
                        (unsigned long long)t->size);
                continue;
            }
            unmatched++;
            unmatched_bytes += pi.mem[k].usize;
            if (named)
                sv_push(&fail, "%s (ext4 inode %llu, %llu bytes): the pack's directory "
                               "walk REACHES this file and the tree import does not — "
                               "every name it has is missing from the volume",
                        pi.mem[k].name, (unsigned long long)pi.mem[k].ino,
                        (unsigned long long)pi.mem[k].usize);
        }
        printf("nk match   : %zu of %llu member inode(s) carried byte-for-byte\n",
               pi.n_mem - unmatched, (unsigned long long)pi.n_mem);
        if (unmatched)
            printf("unmatched  : %zu inode(s), %llu B, with NO dirent in the source "
                   "(ext4's own reserved inodes and unlink debris; nothing a path can "
                   "reach):\n", unmatched, (unsigned long long)unmatched_bytes);
        for (k = 0; k < pi.n_mem && unmatched; k++) {
            const tino *t = tino_find(pi.mem[k].ino);
            if (t && t->size == pi.mem[k].usize) continue;
            if (t) continue;                  /* already named as a size mismatch */
            printf("             ino %-6llu %10llu B  %s\n",
                   (unsigned long long)pi.mem[k].ino,
                   (unsigned long long)pi.mem[k].usize, pi.mem[k].name);
        }
    }

    rc = fail.n ? XV_REFUSED : XV_OK;
    if (rc == XV_OK) {
        int gate_bad = 0;
        for (i = 0; i < (int)notes.n; i++)
            if (strstr(notes.v[i], "exit 0") == NULL) gate_bad = 1;
        if (gate_bad) rc = XV_ERR;
    }
    if (rc == XV_OK)
        printf("RESULT: converted. This is only half the promise: run\n"
               "        `%s verify %s %s` for the file-by-file comparison.\n",
               g_argv0, src, dst);
    else
        printf("RESULT: FAILED — see the %zu named problem(s) below.\n", fail.n);

out_fail:
    if (g_vol) { vol_close(g_vol); g_vol = NULL; }
    if (srcmp.root[0]) src_unmount(&srcmp);
    print_fails(rc == XV_REFUSED ? "conversion REFUSED" : "conversion failed", &fail);
    if (rc != XV_OK && dst_created && !keep_partial) {
        if (unlink(dst) == 0)
            printf("removed the destination %s (nothing half-converted is left to "
                   "mistake for a result)\n", dst);
        else
            fprintf(stderr, "invf-convert: could not remove %s: %s\n", dst, strerror(errno));
    } else if (rc != XV_OK && dst_created) {
        printf("--keep-partial: %s is left in place, unverified\n", dst);
    }
    rm_rf(tmpl);
    sv_free(&fail);
    sv_free(&notes);
    return rc;
}

/* -------------------------------------------------------------- verify */

static int cmd_verify(int argc, char **argv)
{
    const char *src = NULL, *dst = NULL, *mount_root_opt = NULL;
    unsigned long long max_diffs = 200;
    char mount_root[P_CAP], tmpl[P_CAP];
    mountpt smp, dmp;
    ventvec sv = { 0 }, dv = { 0 };
    destmap dmap = { 0 };
    strvec fail = { 0 };
    diffstate ds = { 0 };
    double t0, t_coll, t_cmp;
    size_t i;
    int rc = XV_OK, a;
    unsigned long long n_reg = 0, n_bytes = 0, n_xattr = 0, n_xattr_skipped = 0;
    unsigned long long n_engine_xattr = 0;
    double *per_file = NULL;
    size_t n_per = 0, cap_per = 0;

    memset(&smp, 0, sizeof smp);
    memset(&dmp, 0, sizeof dmp);
    for (a = 1; a < argc; a++) {
        if (strncmp(argv[a], "--max-diffs=", 12) == 0) max_diffs = strtoull(argv[a] + 12, NULL, 10);
        else if (strncmp(argv[a], "--mount-root=", 13) == 0) mount_root_opt = argv[a] + 13;
        else if (argv[a][0] == '-') { die("unknown option %s", argv[a]); return XV_ERR; }
        else if (!src) src = argv[a];
        else if (!dst) dst = argv[a];
        else { die("too many arguments"); return XV_ERR; }
    }
    if (!src || !dst) { usage(stderr, g_argv0); return XV_ERR; }
    ds.max_print = max_diffs;

    snprintf(mount_root, sizeof mount_root, "%s",
             mount_root_opt ? mount_root_opt : "/var/tmp/invf-convert-v.XXXXXX");
    snprintf(tmpl, sizeof tmpl, "%s", mount_root);
    if (!mkdtemp(tmpl)) {
        if (mount_root_opt) {
            if (mkdir(tmpl, 0700) != 0 && errno != EEXIST) {
                die("cannot create %s: %s", tmpl, strerror(errno));
                return XV_ERR;
            }
        } else {
            die("mkdtemp(%s): %s", tmpl, strerror(errno));
            return XV_ERR;
        }
    }

    t0 = now_s();
    if (src_mount(src, tmpl, &smp, &fail) != 0) { rc = XV_ERR; goto out; }

    /* A "/" entry exists on both sides and is compared like the rest, except
     * for the metadata the daemon does not present (see the report note). */
    {
        struct stat st;
        if (lstat(smp.root, &st) != 0) { sv_push(&fail, "%s: lstat: %s", smp.root, strerror(errno)); rc = XV_ERR; goto out; }
    }
    collect_tree(smp.root, "", &sv, &fail);

    /* The engine's inode map has to be read BEFORE the daemon is up: the
     * daemon holds an exclusive flock on the image, so an offline vol_open of a
     * MOUNTED volume fails with "image is in use by another process"
     * (measured — the hardlink partition silently degraded to the LIMIT line
     * on the first run of this suite). One open, one pass, closed before the
     * mount. */
    {
        struct stat dstst;
        if (stat(dst, &dstst) == 0 && S_ISREG(dstst.st_mode)) {
            int verr = 0;
            invfs_volume *dv_ = vol_open(dst, &verr);
            if (dv_) {
                size_t k;
                dmap.have = 1;
                /* Resolve every source regular-file path to its engine inode id
                 * and CLOSE: the daemon mounts with an exclusive flock on the
                 * image, so holding this open through fuse_mount makes the mount
                 * fail (measured). */
                for (k = 0; k < sv.n; k++)
                    if (sv.v[k].type == S_IFREG)
                        sv.v[k].eng_id = vol_find(dv_, sv.v[k].path);
                vol_close(dv_);
            } else {
                sv_push(&fail, "cannot open the destination image %s for the "
                                "hardlink partition (err %d)", dst, verr);
            }
        }
    }
    if (fuse_mount(dst, tmpl, &dmp, &fail) != 0) { rc = XV_ERR; goto out; }
    {
        struct stat st;
        if (lstat(dmp.root, &st) != 0) { sv_push(&fail, "%s: lstat: %s", dmp.root, strerror(errno)); rc = XV_ERR; goto out; }
    }
    collect_tree(dmp.root, "", &dv, &fail);
    t_coll = now_s() - t0;
    qsort(sv.v, sv.n, sizeof *sv.v, vent_cmp);
    qsort(dv.v, dv.n, sizeof *dv.v, vent_cmp);

    /* ---- pass A: presence + metadata ---- */
    {
        size_t is = 0, id = 0;
        while (is < sv.n || id < dv.n) {
            int c;
            if (is == sv.n) c = 1;
            else if (id == dv.n) c = -1;
            else c = strcmp(sv.v[is].path, dv.v[id].path);
            if (c < 0) {
                diff(&ds, "missing", sv.v[is].path, "(the source has it, the volume does not)");
                is++;
                continue;
            }
            if (c > 0) {
                diff(&ds, "extra", dv.v[id].path, "(the volume has it, the source does not)");
                id++;
                continue;
            }
            {
                vent *s = &sv.v[is], *d = &dv.v[id];
                int is_root = (s->path[0] == 0);
                if (s->type != d->type) {
                    diff(&ds, "type", s->path, "src=0%o dst=0%o",
                         (unsigned)s->type, (unsigned)d->type);
                } else if (is_root) {
                    /* The daemon's getattr answers "/" with a fixed
                     * 0755/root/now (fuse_fs.c invf_getattr), so the volume
                     * root's stored mode/owner/mtime is not presented. Named
                     * once, in the report, not as a per-path difference. */
                } else {
                    if (s->type != S_IFLNK && s->mode != d->mode)
                        diff(&ds, "mode", s->path, "src=0%o dst=0%o",
                             (unsigned)s->mode, (unsigned)d->mode);
                    if (s->uid != d->uid)
                        diff(&ds, "uid", s->path, "src=%u dst=%u", s->uid, d->uid);
                    if (s->gid != d->gid)
                        diff(&ds, "gid", s->path, "src=%u dst=%u", s->gid, d->gid);
                    if (s->mtime_sec != d->mtime_sec || s->mtime_nsec != d->mtime_nsec)
                        diff(&ds, "mtime", s->path, "src=%lld.%09d dst=%lld.%09d",
                             (long long)s->mtime_sec, s->mtime_nsec,
                             (long long)d->mtime_sec, d->mtime_nsec);
                }
                if (s->has_size && s->size != d->size)
                    diff(&ds, "size", s->path, "src=%llu dst=%llu",
                         (unsigned long long)s->size, (unsigned long long)d->size);
                if (s->type != d->type && s->type == S_IFREG)
                    diff(&ds, "content", s->path, "(not a regular file on the volume)");
                if (s->type == S_IFCHR || s->type == S_IFBLK) {
                    if (s->rdev != d->rdev)
                        diff(&ds, "rdev", s->path, "src=%llu dst=%llu",
                             (unsigned long long)s->rdev, (unsigned long long)d->rdev);
                }
            }
            is++; id++;
        }
    }

    /* ---- pass B: content, one hash per side, per-file timing ---- */
    t_cmp = now_s();
    for (i = 0; i < sv.n; i++) {
        vent *s = &sv.v[i], *d;
        uint8_t hs[BLAKE3_OUT_LEN], hd[BLAKE3_OUT_LEN];
        char xs[2 * BLAKE3_OUT_LEN + 1], xd[2 * BLAKE3_OUT_LEN + 1];
        uint64_t bs = 0, bd = 0;
        char full[P_CAP];
        double t_file;
        if (s->type != S_IFREG) continue;
        d = find_ent(&dv, s->path);
        if (!d || d->type != S_IFREG) continue;
        if (s->size != d->size) continue;   /* already reported by pass A */
        t_file = now_s();
        snprintf(full, sizeof full, "%s/%s", smp.root, s->path);
        if (hash_file(full, hs, &bs, &fail) != 0) { rc = XV_ERR; continue; }
        snprintf(full, sizeof full, "%s/%s", dmp.root, s->path);
        if (hash_file(full, hd, &bd, &fail) != 0) { rc = XV_ERR; continue; }
        t_file = now_s() - t_file;
        if (n_per == cap_per) {
            size_t nc = cap_per ? cap_per * 2 : 1024;
            double *np = (double *)realloc(per_file, nc * sizeof *np);
            if (!np) { fprintf(stderr, "invf-convert: out of memory\n"); exit(XV_ERR); }
            per_file = np; cap_per = nc;
        }
        per_file[n_per++] = t_file;
        n_reg++;
        n_bytes += bs;
        if (bs != bd || memcmp(hs, hd, sizeof hs) != 0) {
            hex(hs, 8, xs);
            hex(hd, 8, xd);
            diff(&ds, "content", s->path, "BLAKE3 src=%s… dst=%s… (%llu B read)",
                 xs, xd, (unsigned long long)bs);
        }
    }

    /* ---- pass C: hardlink identity (a PARTITION, not a count) ----
     *
     * THE MOUNT CANNOT ANSWER THIS. The daemon never fills st_ino (its getattr
     * memsets the stat and leaves ino 0), so the kernel reports libfuse's
     * per-path node ids: measured on this fixture, two names of ONE engine
     * inode (nlink=2 on both) report ino=2 and ino=3 through the mount. So a
     * partition comparison through FUSE would invent a split on every
     * hardlink, which is why the identity check reads the ENGINE's inode ids
     * (vol_find) from the image itself when the destination is one, and says
     * so when it cannot. The presented nlink is still compared per path above,
     * and convert runs invf-fsck, whose nlink audit is the fan-in == nlink
     * check (src/core/vol_btree.c) for the same property. */
    {
        hlgroup *sg = NULL, *dg = NULL;
        size_t ns = build_groups(&sv, &sg), k;
        (void)build_groups(&dv, &dg);
        for (k = 0; k < ns; k++) {
            long first_d = -1;
            size_t m;
            int split = 0, merged = 0;
            for (m = 0; m < sv.n; m++) {
                vent *e;
                long di;
                if (sv.v[m].ino != sg[k].ino || sv.v[m].type != S_IFREG) continue;
                e = find_ent(&dv, sv.v[m].path);
                if (!e) continue;                    /* reported as missing */
                di = (long)dest_ino_of(&sv.v[m], &dmap);
                if (di <= 0) continue;               /* not mapped: see the LIMIT */
                if (first_d < 0) first_d = di;
                else if (di != first_d) split = 1;
            }
            if (split) {
                diff(&ds, "hardlink-split", sv.v[sg[k].first].path,
                     "the %zu names sharing one source inode are separate inodes "
                     "on the volume", sg[k].count);
                continue;
            }
            if (first_d < 0) continue;
            /* fan-out: every name on that destination inode must be a name of
             * THIS source inode */
            for (m = 0; m < sv.n; m++) {
                if (sv.v[m].type != S_IFREG) continue;
                if ((long)dest_ino_of(&sv.v[m], &dmap) != first_d) continue;
                if (sv.v[m].ino != sg[k].ino) merged = 1;
            }
            if (merged)
                diff(&ds, "hardlink-merge", sv.v[sg[k].first].path,
                     "a destination inode carries names from more than one source inode");
        }
        if (dmap.have)
            printf("hardlinks  : %zu source group(s) of names sharing one inode; the "
                   "engine's own inode ids carry the same partition\n", ns);
        else
            printf("hardlinks  : LIMIT — the mount does not present st_ino (getattr leaves "
                   "it 0), so the partition was checked against the engine image only if "
                   "the destination is a file; per-path nlink was compared regardless\n");
        /* nlink is what `ls -l` and every backup tool read */
        for (i = 0; i < sv.n; i++) {
            vent *d;
            if (sv.v[i].type != S_IFREG) continue;
            d = find_ent(&dv, sv.v[i].path);
            if (!d || d->type != S_IFREG) continue;
            if (sv.v[i].nlink != d->nlink)
                diff(&ds, "nlink", sv.v[i].path, "src=%u dst=%u",
                     sv.v[i].nlink, d->nlink);
        }
        free(sg);
        free(dg);
    }

    /* ---- pass D: xattrs ---- */
    for (i = 0; i < sv.n; i++) {
        vent *d = find_ent(&dv, sv.v[i].path);
        char fulls[P_CAP], fulld[P_CAP];
        char *sn;
        ssize_t slen;
        const char *p;
        if (!d) continue;
        snprintf(fulls, sizeof fulls, "%s/%s", smp.root, sv.v[i].path);
        snprintf(fulld, sizeof fulld, "%s/%s", dmp.root, sv.v[i].path);
        sn = xattr_names(fulls, &slen, &fail);
        if (sn) {
            for (p = sn; p < sn + slen; p += strlen(p) + 1) {
                uint8_t *vs = NULL, *vd = NULL;
                size_t ls = 0, ld = 0;
                if (!*p) continue;
                if (xattr_engine_owned(p)) { n_xattr_skipped++; continue; }
                if (xattr_get(fulls, p, &vs, &ls) != 0) {
                    sv_push(&fail, "%s: xattr %s unreadable on the source", fulls, p);
                    continue;
                }
                if (xattr_get(fulld, p, &vd, &ld) != 0 || ld != ls ||
                    memcmp(vs, vd, ls) != 0) {
                    diff(&ds, "xattr", sv.v[i].path, "%s: src=%zu B dst=%s", p, ls,
                         vd ? "different" : "absent");
                } else {
                    n_xattr++;
                }
                free(vs);
                free(vd);
            }
            free(sn);
        }
        /* attributes on the volume with no source counterpart: engine-owned
         * ones are expected (invfs.class after a sweep), anything else is a
         * difference the other direction. */
        {
            ssize_t dlen = 0;
            char *dn = xattr_names(fulld, &dlen, &fail);
            const char *q;
            if (dn) {
                for (q = dn; q < dn + dlen; q += strlen(q) + 1) {
                    uint8_t *tmp = NULL;
                    size_t tl = 0;
                    if (!*q) continue;
                    if (xattr_engine_owned(q)) { n_engine_xattr++; continue; }
                    if (sn && strstr(sn, q) != NULL) continue;   /* compared above */
                    if (xattr_get(fulls, q, &tmp, &tl) == 0) { free(tmp); continue; }
                    diff(&ds, "xattr-extra", sv.v[i].path, "%s (the volume has it)", q);
                }
                free(dn);
            }
        }
    }
    t_cmp = now_s() - t_cmp;

    /* ---- the report ---- */
    {
        char b1[32];
        unsigned long long nc[8] = { 0 }, nd2[8] = { 0 };
        size_t k;
        for (k = 0; k < sv.n; k++) {
            switch (sv.v[k].type) {
            case S_IFDIR: nc[0]++; break;
            case S_IFREG: nc[1]++; break;
            case S_IFLNK: nc[2]++; break;
            default:      nc[3]++; break;
            }
        }
        for (k = 0; k < dv.n; k++) {
            switch (dv.v[k].type) {
            case S_IFDIR: nd2[0]++; break;
            case S_IFREG: nd2[1]++; break;
            case S_IFLNK: nd2[2]++; break;
            default:      nd2[3]++; break;
            }
        }
        printf("== invf-convert verify ==\n");
        printf("source     : %s%s\n", src, smp.is_fuse ? " (fuse)" : " (loop, read-only)");
        printf("destination: %s%s\n", dst, dmp.is_fuse ? " (fuse)" : "");
        printf("entries    : %zu (src) / %zu (dst), %s of file content compared\n",
               sv.n, dv.n, human((double)n_bytes, b1, sizeof b1));
        printf("  directories     src %-8llu dst %llu\n",
               (unsigned long long)nc[0], (unsigned long long)nd2[0]);
        printf("  regular files   src %-8llu dst %llu\n",
               (unsigned long long)nc[1], (unsigned long long)nd2[1]);
        printf("  symlinks        src %-8llu dst %llu\n",
               (unsigned long long)nc[2], (unsigned long long)nd2[2]);
        printf("  specials        src %-8llu dst %llu\n",
               (unsigned long long)nc[3], (unsigned long long)nd2[3]);
        printf("content    : %llu file(s), %llu B hashed on each side (BLAKE3, holes "
               "read as zeros)\n", n_reg, n_bytes);
        printf("xattrs     : %llu name(s)/value(s) compared, %llu engine-owned skipped "
               "on the source, %llu engine-owned on the volume\n",
               n_xattr, n_xattr_skipped, n_engine_xattr);
        printf("metadata   : type, size, mode, uid, gid, mtime (sec+nsec), rdev, "
               "nlink, symlink target\n");
        printf("timing     : collect %.2f s, content+compare %.2f s, total %.2f s\n",
               t_coll, t_cmp, now_s() - t0);
        if (n_per)
            printf("per-file   : p50 %.2f ms  p95 %.2f ms  p99 %.2f ms  max %.2f ms "
                   "(%zu regular file(s))\n",
                   pctl(per_file, n_per, 0.50) * 1e3, pctl(per_file, n_per, 0.95) * 1e3,
                   pctl(per_file, n_per, 0.99) * 1e3, pctl(per_file, n_per, 1.0) * 1e3,
                   n_per);
        printf("not compared: directory/symlink SIZE (an allocator artefact, not "
               "content) and ATIME (the read itself moves it: relatime)\n");
        printf("             the volume root's stored mode/owner/mtime: the daemon's "
               "getattr reports \"/\" as a fixed 0755/root/now\n");
        if (fail.n) {
            printf("TOOL ERRORS : %zu (the comparison is incomplete)\n", fail.n);
            for (k = 0; k < fail.n; k++) printf("  %s\n", fail.v[k]);
            rc = XV_ERR;
        }
        printf("DIFFS      : %llu%s\n", ds.n_diffs,
               ds.n_diffs > ds.n_printed ? " (only the first N are printed; see --max-diffs)" : "");
        if (ds.n_diffs == 0 && rc == XV_OK) printf("RESULT: IDENTICAL\n");
        else if (ds.n_diffs) printf("RESULT: DIFFERENT (%llu difference(s))\n", ds.n_diffs);
    }
    if (ds.n_diffs && rc == XV_OK) rc = XV_DIFF;

out:
    if (dmp.root[0]) fuse_unmount(&dmp);
    if (smp.root[0]) src_unmount(&smp);
    rm_rf(tmpl);
    if (fail.n && rc != XV_OK) {
        size_t k;
        if (sv.n == 0 && dv.n == 0) {
            fprintf(stderr, "invf-convert verify could not run:\n");
            for (k = 0; k < fail.n; k++) fprintf(stderr, "  %s\n", fail.v[k]);
        }
    }
    vv_free(&sv);
    vv_free(&dv);
    free(per_file);
    sv_free(&fail);
    return rc;
}
