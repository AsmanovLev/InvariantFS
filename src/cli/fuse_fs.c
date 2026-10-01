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
#include "vol_spt0.h"
#include "vol_walk.h"   /* WP135: a walk's status is not optional */
#include "tmpstore.h"

static invfs_volume *g_vol;
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
/* open-handle counter: background sweep only when idle */
static volatile int g_open_handles = 0;
static volatile int g_handles_warned = 0;

/* ---- open-handle accounting -------------------------------------------
 *
 * INVARIANT: g_open_handles is the number of file handles the kernel is
 * holding -- the number of .open and .create calls that returned 0 and
 * have not yet been matched by a .release. It is a CENSUS, not a refcount
 * the kernel consults; it is the only thing that tells the background
 * worker whether the volume is idle (fuse_sweep_thread).
 *
 * libfuse sends .release for a file it CREATED exactly as it does for one
 * it opened, so .create owes an acquire just as .open does. Discharge is
 * unconditional (invf_release) because libfuse pairs it with every
 * successful open and only with those -- a failed entry point gets no
 * .release, so it must not acquire.
 *
 * That leaves exactly one way to get this wrong: acquiring on a path that
 * goes on to return an error. It has been got wrong twice -- .create did
 * not acquire at all, and .open acquired before its calloc and then
 * returned -ENOMEM straight past the discharge. So the acquire is not a
 * call scattered through the body. It is ONE call, immediately before the
 * entry point's only `return 0`, which is the only way out of a
 * successful open: nothing fallible sits between an acquire and the
 * success it buys, and the pair reads off the function's tail.
 *
 * A third entry point that can hand the kernel a handle ends with the
 * same two lines. */
static void handle_acquired(void)
{
    __sync_fetch_and_add(&g_open_handles, 1);
}

/* The only discharge. Unconditional on purpose: libfuse sends .release for
 * every successful .open and .create, and for nothing else, so a release
 * with no matching acquire means an entry point failed to acquire -- which
 * the fuse_sweep_thread guard reports. Symmetric by construction: if
 * handle_acquired() has exactly one call site per entry point, so does
 * this. */
static void handle_released(void)
{
    __sync_fetch_and_sub(&g_open_handles, 1);
}

/* set at unmount: stops the background sweep thread before vol_close */
static volatile int g_shutdown = 0;
static volatile sig_atomic_t g_sweep_now = 0;
static char g_img_path[512] = "?";
static char g_mnt_path[512] = "?";  /* WP134: named in the rollback warning */
static volatile int g_sweep_busy = 0;
static double g_attr_t = 1.0;   /* -o attr_t= override; 0 = bench-honest */
/* WP26: RAW-zone fill watermark (percent, 0 = lazy default). Set by
 * -o raw_watermark=<pct> or INVFS_RAW_WATERMARK; when the RAW fill
 * exceeds it the background sweep thread kicks an early sweep. */
static int g_raw_watermark = 0;
static tmp_area_mode g_tmp_area = TMP_AREA_AUTO;
static size_t g_tmp_max_bytes = 128 * 1024 * 1024;
/* WP59: codec-policy mount gate flags (-o ignore-missing-codecs,
 * -o ignore-codec-versions) */
static int g_ignore_missing_codecs = 0;
static int g_ignore_codec_versions = 0;
static void invf_sweep_worker(int arm_ckp);   /* defined below sweep thread */
static void table_rebuild_locked(void);   /* fwd (defined below) */

/* Mutations only mark the name table stale; the next consumer pays for one
 * rebuild instead of every close paying one. tar/cp imports fire thousands
 * of closes -- rebuilding per close scanned the whole inode area each time
 * and made imports O(N^2). */
static int g_table_stale = 0;
/* WP135: set when a rebuild finished but the walk behind it did not. The
 * table is usable for what it holds; it is not authority on what the volume
 * contains, so a name it lacks is an I/O error, not an ENOENT. */
static int g_table_degraded = 0;

static void table_mark_stale(void) { g_table_stale = 1; }
static void table_upsert_locked(const char *name, uint64_t ino,
                                uint64_t size, uint64_t ctime);
static void table_remove_name(const char *name);

static int is_temp_path(const char *path)
{
    return strstr(path, "/tmp/") != NULL ||
           strstr(path, "/var/tmp/") != NULL;
}

/* WP59a: files under /.invariantfs/config/ or /.invariantfs/codecpacks/
 * must be anchored so they stay builtin-readable. */
static int is_anchored_path(const char *path)
{
    if (strncmp(path, "/.invariantfs/config/", 21) == 0)
        return 1;
    if (strncmp(path, "/.invariantfs/codecpacks/", 25) == 0)
        return 1;
    return 0;
}


static void table_refresh_if_stale_locked(void)
{
    if (g_table_stale && g_vol) {
        table_rebuild_locked();
        /* WP135: build_file_table_v3 leaves the flag SET when the walk did
         * not finish, and only on a rebuild that completed is the
         * question answered. Clearing it unconditionally here is what made
         * the daemon stop retrying after the one walk that failed -- the
         * missing subtree was gone for the rest of the mount, and nothing
         * ever re-asked. */
        if (!g_table_degraded)
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



/* WP49: pass-1 collector fed by the namespace walk (a position-driven
 * vol_inode_next loop can cycle when the mapper table is not pba
 * monotonic). */
typedef struct {
    fs_entry *recs;
    uint64_t nrecs, caprecs;
    uint64_t *tpos, *tid;
    uint64_t ntomb, captomb;
    int oom;
} bft_ctx;



/* WP-M6: a v3 volume has no v2 record stream, so the table is built from
 * the dirent tree via vol_v3_walk. Files land under their path; directories
 * additionally land under "path/" so meta_for_path's anchor lookup (shared
 * with the v2 path) finds them. pos is unused after the build. */
typedef struct { fs_entry *v; int n, cap; } v3_bft_ctx;

static int v3_bft_add(v3_bft_ctx *c, const char *name, uint64_t ino,
                      uint64_t size, uint64_t ctime)
{
    if (c->n == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 1024;
        fs_entry *ne = (fs_entry *)realloc(c->v, (size_t)ncap * sizeof *ne);
        if (!ne)
            return -1;
        c->v = ne;
        c->cap = ncap;
    }
    memset(&c->v[c->n], 0, sizeof c->v[c->n]);
    strncpy(c->v[c->n].name, name, sizeof c->v[c->n].name - 1);
    c->v[c->n].inode_id = ino;
    c->v[c->n].size = size;
    c->v[c->n].ctime = ctime;
    c->n++;
    return 0;
}

static int v3_bft_cb(void *ctx_, const char *path, uint64_t ino,
                     uint32_t type, uint64_t size, int64_t mtime)
{
    v3_bft_ctx *c = (v3_bft_ctx *)ctx_;
    if (v3_bft_add(c, path, ino, type == INVFS_ITYP_DIR ? 0 : size,
                   (uint64_t)mtime) != 0)
        return 1;
    if (type == INVFS_ITYP_DIR) {
        char anchor[300];
        snprintf(anchor, sizeof anchor, "%s/", path);
        if (v3_bft_add(c, anchor, ino, 0, (uint64_t)mtime) != 0)
            return 1;
    }
    return 0;
}

/* WP135: the walk's status was dropped here, and that is what made whole
 * SUBTREES vanish from a live mount. v3_walk_dir used to `continue` past an
 * entry it could not resolve, so one unreadable dirent took its directory and
 * everything under it out of the table -- silently, with a return value of 0
 * meaning COMPLETE. Every path under a lost directory then missed in
 * snapshot_entry(), and the three call sites turned that into -ENOENT: the
 * user's `ls` says the files are not there, on a volume where they are.
 *
 * The table is still published -- a short table is strictly better than none
 * for the paths it does contain -- but the volume is marked DEGRADED until a
 * rebuild completes, and a degraded volume answers -EIO rather than -ENOENT
 * for a name the table does not have. -ENOENT is a claim about the disk; on a
 * volume that could not finish reading itself, it is a claim the daemon has
 * no standing to make. The rebuild is also left marked stale, so the next
 * op retries instead of the one-shot clear at :132 retiring the question. */
static void build_file_table_v3(void)
{
    v3_bft_ctx c;
    vol_walk_t w;
    int rc;

    memset(&c, 0, sizeof c);
    vol_walk_init(&w, g_vol, "build_file_table_v3");
    /* WP135: the STRICT walk. A name table is a partial view of the
     * namespace presented to the kernel as a whole one, and the lenient
     * walk's `continue` is what takes a whole subtree out of it. See
     * vol_v3_walk_strict's comment: it names pba_ref_ensure as the one
     * caller that must not get a partial view, and this is a second. */
    rc = vol_v3_walk_strict(g_vol, v3_bft_cb, &c);
    vol_walk_result(&w, rc, (size_t)(c.n > 0 ? c.n : 0), (size_t)(c.n > 0 ? c.n : 0));
    if (c.n > 1)
        qsort(c.v, (size_t)c.n, sizeof *c.v, cmp_entry_name_pos);
    g_entries = c.v;
    g_nentries = c.n;
    g_cap = c.cap;
    if (vol_walk_commit(&w) != 0) {
        g_table_degraded = 1;
        fprintf(stderr, "invf-fuse: the namespace walk did not complete; the "
                        "name table is missing at least one subtree, and "
                        "lookups for names it does not hold now answer EIO "
                        "rather than ENOENT until a rebuild succeeds\n");
    }
}

static void build_file_table(void)
{
    build_file_table_v3();
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
/* same as table_sync_one but caller already holds g_io_lock
 *
 * WP140: a lookup that could not be COMPLETED must not evict anything.
 *
 * This used to be `id = vol_find(...); if (!id) { table_remove_name(name);
 * return; }`, and vol_find returns a uint64_t -- so "there is no such name"
 * and "the dirent row on the path would not read" were both the value 0, and
 * this function acted on the union of them. It is called after EVERY mutation,
 * so one transient read error -- a quarantined base page, a fold racing the
 * read -- took a live file out of the name table and the mount answered ENOENT
 * for it until something unrelated marked the table stale, or the next mount.
 * Nothing is written to the volume, so no data was at risk; but `ls` still
 * listed the file (invf_readdir is engine-backed) while open/stat/read said it
 * was not there, which is the shape that reaches a user.
 *
 * The three answers, from vol_find_rc (src/core/vol_ast.c), and what each may
 * do to the table:
 *
 *   1  the name is there      -> refresh its row.
 *   0  the name is NOT there  -> evict. This is the ONLY eviction, and it is
 *                                the only one that is a fact about the volume.
 *  -1  the lookup failed      -> KEEP whatever the table already says, mark it
 *                                stale, and say so.
 *
 * Why KEEP is the right answer and not the cautious-looking EVICT: the two
 * errors are not symmetric. An entry dropped for a LIVE name is a lie about
 * presence -- `ls` hides a file that is there -- but it is SELF-HEALING (a
 * rebuild puts it back) and the bytes were never touched: the engine still
 * resolves the name and reads them (that is what `invf-cat` does, and it never
 * consults this table). An entry KEPT for a DELETED name is a lie about
 * absence, and it is the worse of the two: unlink already freed the file's
 * blocks to the allocator (AGENTS.md 2.4), so that entry hands out an inode id
 * whose storage now belongs to somebody else. On a filesystem whose whole
 * invariant is bit-exactness, returning another file's recycled blocks is a
 * worse outcome than a file the mount temporarily cannot find.
 *
 * What can still be stale in a kept entry is size and mtime, never existence
 * and never the inode id: the row was written by the previous mutation, and
 * every mutation -- including unlink, create and rename -- re-establishes it,
 * so there is no window in which a kept id can name a freed inode.
 *
 * The stale mark is what makes this total. Without it a name that was not
 * already in the table (a create or mknod that syncs a name the table has
 * never seen) would still be invisible until some unrelated op marked the
 * table dirty; with it, snapshot_entry's existing miss-then-rebuild path heals
 * on the very next lookup.
 *
 * vol_find is deliberately NOT widened. 102 call sites test it against 0 and
 * most only skip work, which is fail-safe under the collapse; changing the
 * meaning for all of them at once would change what every one of them does,
 * and the finding is that this one DECIDES something. Hence vol_find_rc. */
static void table_sync_one_locked(const char *name)
{
    uint64_t id = 0, sz = 0, ct = 0;
    int rc;
    if (!g_vol) return;
    rc = vol_find_rc(g_vol, name, &id);
    if (rc < 0) {
        table_mark_stale();
        fprintf(stderr, "invf-fuse: table: lookup did not complete for \"%s\"; "
                        "keeping the entry the table already has, and marking "
                        "the table for a rebuild\n", name);
        return;
    }
    if (rc == 0) { table_remove_name(name); return; }
    if (vol_stat_full(g_vol, name, &id, &sz, &ct) == 0)
        table_upsert_locked(name, id, sz, ct);
    else
        table_mark_stale();   /* found, but not stat-able: same "keep, and ask again later" */
}


/* Race-safe lookup (audit H1): g_entries is freed and rebuilt by every
 * flush/create/unlink/sweep, so callers must never hold the pointer across
 * a rebuild. Copy the fields out under g_io_lock instead. */
/* WP135: THREE answers now, and the third is the one this whole WP is
 * about. 1 = found. 0 = the table is authoritative and the name is not
 * there, which is a real ENOENT. -1 = the name is not there AND the last
 * rebuild's namespace walk did not finish, so the table is not authority on
 * what the volume contains: a name it lacks may be a name it never read.
 * Mapping that to ENOENT is how "a damaged volume deletes a subtree" --
 * the walk used to skip an unresolvable entry AND its recursion, so the
 * lost part was a whole directory tree, and the operator's only evidence
 * was `ls` saying the files were never written. */
static int snapshot_entry(const char *name, uint64_t *ino_out, uint64_t *size_out,
                          uint64_t *ctime_out)
{
    int found = 0;
    int degraded = 0;
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
        } else if (g_table_degraded) {
            degraded = 1;
        }
    }
    pthread_mutex_unlock(&g_io_lock);
    return found ? 1 : (degraded ? -1 : 0);
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
 *
 * THE RETURN CONTRACT IS THREE ANSWERS, and the middle one is the whole
 * point of this comment. It used to be 1 / 0, and both 0 and a failed
 * vol_get_meta collapsed into "use meta_defaults()", which is mode 0644 and
 * owner root. That is harmless for a caller that only READS the answer --
 * a stat built from a wrong mode is wrong, but nothing changed -- and it is
 * a volume-clobbering lie for every caller that WRITES it back, which is
 * what meta_apply_patch and its chmod/utimens/chown callers do.
 *
 *   1  resolved. *ename and *m are filled. *m may still be the type
 *      defaults (see below) -- that is the documented v1->v2 upgrade path,
 *      and a caller writing *m back is then doing the right thing.
 *   0  there is nothing at this path.
 *  -1  the name resolved but its metadata row COULD NOT BE READ (quarantine,
 *      an undecodable value). *m is NOT written. A caller must fail; it must
 *      not answer with defaults, because the defaults are a different
 *      inode's mode and owner rather than a guess at this one's. */
static int meta_for_path(const char *path, char *ename, size_t ecapsz,
                         invfs_meta_pub *m)
{
    const char *name = path[0] == '/' ? path + 1 : path;
    uint64_t ino = 0, size = 0, ctime = 0;

    /* WP135: == 1, not truthiness: snapshot_entry has three answers now
     * (found / absent / absent-on-a-degraded-table) and -1 must not be read
     * as found. Upstream's three-valued refactor is what made that a
     * question worth asking. */
    if (snapshot_entry(name, &ino, &size, &ctime) == 1) {
        int grc;
        pthread_mutex_lock(&g_io_lock);
        grc = g_vol ? vol_get_meta_rc(g_vol, ino, m) : -EIO;
        pthread_mutex_unlock(&g_io_lock);
        if (grc == -EIO) return -1;
        if (grc == -ENOENT) meta_defaults(name, size, m);
        snprintf(ename, ecapsz, "%s", name);
        return 1;
    }
    /* not a file entry: a directory's anchor record is "name/" */
    {
        char anchor[300];
        int is_dir = 0, grc = -ENOENT;
        pthread_mutex_lock(&g_io_lock);
        is_dir = g_vol ? vol_is_dir(g_vol, name) : 0;
        pthread_mutex_unlock(&g_io_lock);
        if (!is_dir) return 0;
        snprintf(anchor, sizeof anchor, "%s/", name);
        if (snapshot_entry(anchor, &ino, &size, &ctime) == 1) {
            pthread_mutex_lock(&g_io_lock);
            grc = g_vol ? vol_get_meta_rc(g_vol, ino, m) : -EIO;
            pthread_mutex_unlock(&g_io_lock);
        }
        if (grc == -EIO) return -1;
        if (grc == -ENOENT) {
            /* the anchor record predates its metadata row (a pre-v3 volume):
             * report sane DIR defaults, which is the v1->v2 upgrade path --
             * there is no recorded mode to lose. A row that could not be
             * READ is the other case and returned above. */
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
    case INVFS_ITYP_CHR:  st->st_mode = S_IFCHR | (m->mode & 07777);
                          st->st_rdev = (dev_t)m->rdev; break;
    case INVFS_ITYP_BLK:  st->st_mode = S_IFBLK | (m->mode & 07777);
                          st->st_rdev = (dev_t)m->rdev; break;
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
/* merge a partial patch over current/default metadata and persist it.
 *
 * THIS IS THE WRITE-BACK, so it is where the two "no metadata" cases have to
 * come apart. `cur` is merged field-by-field over the whole record and the
 * whole record is written back by vol_apply_meta, so anything the read did
 * not establish is not a missing field, it is a field this call is about to
 * invent:
 *
 *   cur from type defaults (mmeta: rc 1 with the defaults applied)  -- the
 *     record has no metadata row at all, a pre-v3 volume. Writing the
 *     defaults plus the patch is the documented v1->v2 upgrade path and is
 *     the ONE case where a default belongs in the row.
 *
 *   cur unreadable (meta_for_path -1) -- damage. The recorded mode and owner
 *     are exactly what could not be read, and 0644/root is not a guess at
 *     them, it is a different inode's. So this refuses: -EIO, and NOTHING is
 *     written. A chmod that could not be applied must not be reported as
 *     applied, and must not leave the inode half-modified -- which is what
 *     merging a patch onto a default-filled record did: it returned 0, the
 *     kernel recorded the chmod as done, and the volume's mode and owner had
 *     been reset underneath it. */
static int meta_apply_patch(const char *path, unsigned mask,
                            const invfs_meta_pub *patch)
{
    char ename[300];
    invfs_meta_pub cur;
    uint64_t nid;
    int rc = meta_for_path(path, ename, sizeof ename, &cur);
    if (rc == 0) return -ENOENT;
    if (rc <  0) return -EIO;      /* unreadable row: write nothing at all */
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
 * (0 = existence only). 0 ok, -EACCES denied, -ENOENT missing, -EIO when
 * the access ACL could not be READ.
 *
 * WHY -EIO AND NOT A VERDICT. The mount does not negotiate
 * default_permissions (AGENTS.md 2.9), so this function is the only
 * object-level permission authority there is -- there is no second opinion
 * from the kernel to fall back on. That makes "I could not read the ACL" a
 * decision the mount has to make deliberately rather than by accident.
 *
 * It used to make it by accident. vol_get_xattr returned -1 both for "this
 * inode has no access ACL" and for "the ACL row could not be read" (a
 * quarantined base page, or any other read failure), and `== 0` collapsed
 * both into aclp = NULL -- which acl_eval answers with the plain mode triad.
 * One unreadable inode row therefore removed that file's POSIX ACLs from
 * the mount entirely and the mount began ALLOWING what they had denied.
 * vol_get_xattr now separates -ENODATA from -EIO (see volume.h), and the
 * -EIO is propagated: the caller gets a refusal, not a permit. Refusing is
 * the right way to be wrong here. Guessing "probably no ACL" turns a
 * damaged volume into a permissive one, which is the one direction a
 * permission bug must never go.
 */
static int perm_check_cred(const struct acreds *c, const char *path,
                           unsigned want)
{
    invfs_meta_pub m;
    char ename[300];
    int is_root = strcmp(path, "/") == 0;
    int mrc;

    if (is_root) {
        root_meta(&m);
    } else if ((mrc = meta_for_path(path, ename, sizeof ename, &m)) != 1) {
        /* -1 as well as 0: an inode whose metadata row could not be read
         * used to come back here as mode 0644 / owner root, and this
         * function then evaluated the mode triad and the ownership against
         * THOSE -- so one unreadable row removed the file's real
         * permissions from the decision and replaced them with the most
         * permissive default in the file. The xattr fix above closed the
         * ACL half of this; the mode half is here. Both fail closed now. */
        return mrc < 0 ? -EIO : -ENOENT;
    }
    if (c->bypass || !want)
        return 0;
    /* fetch the access ACL (the root has no record, hence none) */
    {
        uint8_t acl[INVFS_META_XATTR_MAX];
        size_t alen = 0;
        const uint8_t *aclp = NULL;
        int arc = 0;
        if (!is_root) {
            uint64_t ino = 0;
            size_t vlen = sizeof acl;
            int frc;
            pthread_mutex_lock(&g_io_lock);
            frc = g_vol ? vol_find_rc(g_vol, ename, &ino) : -1;
            if (frc == 1) {
                arc = vol_get_xattr(g_vol, ino, XATTR_ACL_ACCESS, acl, &vlen);
                if (arc == 0) {
                    alen = vlen;
                    aclp = acl;
                } else if (arc == -EIO) {
                    /* the ACL row could not be read: we do not know what it
                     * says, so we must not answer as though it said
                     * "nothing". -EIO is a refusal the application sees. */
                    pthread_mutex_unlock(&g_io_lock);
                    return -EIO;
                }
                /* -ENODATA: the inode genuinely has no access ACL, and the
                 * mode triad is the whole truth. -ERANGE cannot happen
                 * (INVFS_META_XATTR_MAX is the TLV budget) and -EINVAL is a
                 * caller bug; both fall through to the mode triad exactly
                 * as they did before, so a name-length problem cannot turn
                 * into a spurious EIO storm on every open. */
            } else if (frc < 0) {
                /* Resolving the name failed, so the ACL is never even read.
                 * This is the same fail-open one call upstream of the xattr
                 * read above, and it is why vol_find is not used here: its
                 * return is a uint64_t whose "not found" and "could not look"
                 * are both 0, and a 0 would have dropped us into the mode
                 * triad below exactly as an absent ACL would. */
                pthread_mutex_unlock(&g_io_lock);
                return -EIO;
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
        if ((rc = meta_for_path(comp, ename, sizeof ename, &m)) != 1)
            return rc < 0 ? -EIO : -ENOENT;  /* unreadable is a denial too */
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
    int rc;

    if (c->bypass) return 0;
    /* The sticky bit lives in the parent's mode, and an unreadable row used
     * to answer with meta_defaults() -- 0644, no 01000 -- so the test below
     * read "this directory is not sticky" and the rename or unlink was let
     * through. The bit missing from a default is exactly the bit that was
     * protecting somebody else's file. */
    if ((rc = meta_for_path(path, ename, sizeof ename, &em)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
    if (!s) {
        root_meta(&pm);
    } else {
        char parent[300];
        size_t n = (size_t)(s - p);
        if (n == 0 || n >= sizeof parent) return -ENAMETOOLONG;
        memcpy(parent, p, n);
        parent[n] = 0;
        if ((rc = meta_for_path(parent, ename, sizeof ename, &pm)) != 1)
            return rc < 0 ? -EIO : -ENOENT;
    }
    if (!(pm.mode & 01000)) return 0;
    if (c->uid == pm.uid || c->uid == em.uid) return 0;
    return -EPERM;
}

/* fetch the default ACL of path's parent directory.
 *   1   = present (valid blob in buf, *len bytes)
 *   0   = there is none, and that is a fact about the volume
 *  -EIO = it could not be READ
 * The root has no default ACL. Call without g_io_lock held.
 *
 * The third answer is new and the two callers that matter used to get 0 for
 * it. A directory's default ACL is what restricts the files created inside
 * it: reading it as "absent" when the row could not be read creates the new
 * file with no inherited restriction at all, which is the create-time twin
 * of the perm_check_cred fail-open -- the object lands on disk already
 * carrying less restriction than the directory says it should. So the
 * callers below refuse the create instead.
 */
static int parent_default_acl(const char *path, uint8_t *buf, size_t *len)
{
    char anchor[300];
    const char *p = path[0] == '/' ? path + 1 : path;
    const char *s = strrchr(p, '/');
    uint64_t ino = 0;
    size_t vlen;
    int rc, frc;

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
    frc = g_vol ? vol_find_rc(g_vol, anchor, &ino) : -1;
    if (frc < 0) {
        /* the anchor record could not be resolved, so the default ACL was
         * never read. Same reasoning as the -EIO below: an unreadable
         * restriction is not an absent one, and treating it as absent
         * creates the new file with less restriction than the directory
         * declares. */
        pthread_mutex_unlock(&g_io_lock);
        return -EIO;
    }
    if (frc == 0) {
        pthread_mutex_unlock(&g_io_lock);
        return 0;                        /* no anchor record: no default ACL */
    }
    rc = vol_get_xattr(g_vol, ino, XATTR_ACL_DEFAULT, buf, &vlen);
    pthread_mutex_unlock(&g_io_lock);
    if (rc == -EIO)
        return -EIO;                     /* unreadable: NOT the same as none */
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
        /* WP-M20: base reads are lock-free (immutable base, delta published atomically) */
        {
            /* WP135: -1 = absent on a table that is not authority. A name
             * the degraded table does not hold may be one it never read, so
             * ENOENT here would be a claim about the disk that the daemon is
             * not in a position to make. The vol_is_dir() fallback below is
             * a real lookup against the engine, so it is allowed to have
             * the last word on what IS there. */
            int se = snapshot_entry(name, NULL, &size, &ctime);
            if (se <= 0) {
                int is_dir = 0;
                is_dir = g_vol ? vol_is_dir(g_vol, name) : 0;
                if (!is_dir) return (se < 0) ? -EIO : -ENOENT;
            }
        }
        /* The racy fallback below is for a name the snapshot has not caught
         * up with yet, and it is a READ -- nothing here is written back, so
         * answering from defaults misreports the mode but does not change
         * the volume. A row that could not be READ is a different thing
         * (meta_for_path -1) and is reported as the I/O error it is, rather
         * than as a confident 0644/root. */
        {
            int mrc = meta_for_path(path, ename, sizeof ename, &m);
            if (mrc < 0) return -EIO;
            if (mrc == 0) {
                int isd;
                isd = g_vol ? vol_is_dir(g_vol, name) : 0;
                meta_defaults(name, size, &m);
                if (isd) { m.type = INVFS_ITYP_DIR; m.nlink = 2; }
            }
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
    /* WP-M20: base reads are lock-free (immutable base, delta published atomically) */
    if (!g_vol) return -EIO;
    if (dir[0] && !vol_is_dir(g_vol, dir)) {
        return -ENOENT;
    }
    for (;;) {
        free(ents);
        ents = (invfs_dirent *)malloc((size_t)cap * sizeof(invfs_dirent));
        if (!ents) return -ENOMEM;
        n = vol_list_dir(g_vol, dir, ents, cap);
        /* A failed listing is NOT an empty one. `break`ing out and falling
         * through to `return 0` is how a directory that could not be read
         * reached ls(1) as a directory with nothing in it and exit 0 -- a
         * silently wrong answer to the question the user asked, and a hole
         * in the tree for anything walking it. Propagate the errno.
         *
         * Why not emit what we have and then fail: this entry point ignores
         * `offset` and re-lists from scratch on every call, so the kernel
         * has no resume point -- a short list would be indistinguishable
         * from a directory that really is short. Emitting nothing and
         * failing is the only answer a caller can tell from a complete one. */
        if (n < 0) { free(ents); return n; }
        if (n < cap) break;
        cap *= 2;   /* possibly truncated: retry with a bigger buffer */
    }
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

/* WP135: '!' is reserved -- it separates a container from the internal
 * sibling inodes its payload lives in ("x.tar" -> "x.tar!part0"). Core refuses
 * such a name where it is introduced (name_is_internal_ns,
 * src/core/volume_internal.h: vol_v3_create_node, vol_v3_mkdir,
 * vol_v3_rename, vol_v3_hardlink, vol_write_begin), and that check is what
 * makes the hole unreachable. This is not that check -- it is the ERRNO.
 *
 * Every core create returns 0 for every kind of failure, and these callers
 * turn a 0 into -ENOSPC (invf_create below is the clearest case: "create
 * empty file immediately so getattr-after-create works"). So without this the
 * operator sees "No space left on device" on a volume with gigabytes free and
 * goes looking for a full disk. EINVAL is the honest errno for a name the
 * filesystem will not accept, and the message says which byte and why, so the
 * remedy is in the error rather than in a source file. */
static int fuse_reserved_name(const char *path)
{
    if (!path || !strchr(path, '!'))
        return 0;
    fprintf(stderr,
            "invf: '!' is reserved: it separates a container from its "
            "internal siblings ('x.tar' -> 'x.tar!part0'), so '%s' is refused. "
            "Rename it without the '!'.\n", path);
    return 1;
}

static int invf_mkdir(const char *path, mode_t mode)
{
    int rc;
    if (fuse_reserved_name(path)) return -EINVAL;
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
    rc = parent_default_acl(path, dacl, &dlen);
    if (rc < 0) return rc;               /* the default ACL is unreadable */
    if (rc) {
        unsigned mm = cmode;
        memcpy(aacl, dacl, dlen);
        if (acl_create_masq(aacl, dlen, &mm) > 0)
            aalen = dlen;              /* named entries remain: store */
        cmode = (mode_t)(mm & 0777);
    }
    pthread_mutex_lock(&g_io_lock);
    vol_ensure_path(g_vol, path + 1);
    uint64_t d = vol_mkdir(g_vol, path + 1);
    int acldeny = 0;
    if (d) {
        /* stamp real owner/mode on the "dir/" anchor record */
        invfs_meta_pub m;
        char anchor[300];
        uint64_t anchor_ino;
        snprintf(anchor, sizeof anchor, "%s/", path + 1);
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_DIR;
        m.mode = cmode;
        if (!m.mode) m.mode = 0755;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 2;
        m.mtime = m.atime = (int64_t)time(NULL);
        anchor_ino = vol_apply_meta(g_vol, anchor, &m);
        if (!anchor_ino)
            fprintf(stderr, "invf: mkdir stamp FAILED %s (area full?)\n", anchor);
        if (dlen) {
            /* The ACL goes on the anchor record, and vol_apply_meta ALREADY
             * RESOLVED that name and handed the id back -- it is the same
             * record vol_find would have gone looking for, one line later, for
             * no new information.
             *
             * That second lookup WAS the defect. vol_find returns a uint64_t,
             * so its "there is no such name" and its "the lookup could not be
             * completed" are both 0, and `if (ino2)` acted on their union: an
             * unreadable dirent row on a name this function had written
             * microseconds earlier produced a directory that EXISTS, carries
             * the ACL-masked mode triad, and carries NO ACL AT ALL.
             *
             * On this mount that is a widening and not a mislabel. It does not
             * negotiate default_permissions (AGENTS.md 2.9), so perm_check_cred
             * is the sole object-level permission authority, and an object with
             * no access ACL is evaluated on its mode triad alone (acl_eval,
             * :796-800). The triad reproduces u::, the group class and o::;
             * every NAMED entry in the inherited ACL is a per-identity decision
             * the triad cannot express. Dropping the ACL therefore hands the
             * owning group what the ACL denied a member of it, and drops the
             * child's own default ACL, so nothing below it inherits the
             * restriction either.
             *
             * So the id comes from the write. The lookup is now only the
             * fallback for a stamp that returned nothing at all. */
            uint64_t ino2 = anchor_ino;
            if (!ino2) {
                int frc = vol_find_rc(g_vol, anchor, &ino2);
                if (frc < 0) {
                    /* Not "this directory has no ACL": the lookup did not
                     * COMPLETE, and an error is not entitled to assert a
                     * negative about the volume. The create is refused -- and
                     * the directory removed again, because leaving it behind
                     * is the fail-open with a louder log attached. The undo is
                     * exact and provable: this call created it, under
                     * g_io_lock, and it is empty by construction, so there is
                     * nothing of anybody else's to destroy. */
                    fprintf(stderr, "invf: mkdir: could not resolve \"%s\" to "
                            "attach the ACL inherited from \"%s\"; the "
                            "directory is removed again rather than left "
                            "without it\n", anchor, path + 1);
                    if (vol_rmdir(g_vol, path + 1) == 0) {
                        table_remove_name(anchor);
                        d = 0;
                        acldeny = 1;
                    } else {
                        fprintf(stderr, "invf: mkdir: the rollback of \"%s\" "
                                "FAILED -- it is on the volume with no ACL\n",
                                anchor);
                    }
                }
            }
            if (!acldeny && ino2) {
                if (aalen &&
                    vol_set_xattr(g_vol, ino2, XATTR_ACL_ACCESS, aacl, aalen) != 0)
                    fprintf(stderr, "invf: mkdir ACL inherit FAILED %s\n", anchor);
                if (vol_set_xattr(g_vol, ino2, XATTR_ACL_DEFAULT, dacl, dlen) != 0)
                    fprintf(stderr, "invf: mkdir defACL inherit FAILED %s\n", anchor);
            }
        }
        if (d)
            table_sync_one_locked(anchor);
    }
    pthread_mutex_unlock(&g_io_lock);
    if (acldeny)
        return -EIO;              /* never reported to the caller as success */
    rc = d ? 0 : -EEXIST;
    return rc;
}

static int invf_rmdir(const char *path)
{
    int rc;
    struct acreds c;
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

/* engine write-session rc -> FUSE errno. WP85: -ESTALE (this handle's
 * generation was retired by a rollback) must reach userspace AS ESTALE --
 * `cp`/`dd` must report an error, never a short write. Every other mapping
 * is exactly what it was before. */
static int wrc_eno(int rc)
{
    if (rc == -2) return -ENOSPC;
    if (rc == -ESTALE) return -ESTALE;
    return -EIO;
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
    /* WP-M20: base reads are lock-free (immutable base, delta published atomically).
     * The lock still protects the dirty session list and data path. */
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
        return got == -ESTALE ? -ESTALE : (got < 0 ? -EIO : got);
    }
    pthread_mutex_unlock(&g_io_lock);
    /* Metadata read: lock-free base path */
    {
        /* WP135: EIO, not ENOENT, when the table is degraded -- see the
         * three-answer contract on snapshot_entry(). */
        int se = snapshot_entry(path + 1, &ino, &size64, &ctime);
        if (se < 0)
            return -EIO;
        if (se == 0)
            return -ENOENT;
    }
    if ((uint64_t)offset >= size64)
        return 0;
    /* Data read: lock-free, and that means EVERY structure vol_read_range
     * touches must be safe on its own. The io_pread claim is true and it is
     * not the whole story: the block I/O is stateless, but the path above it
     * is not. THREE structures have been found racing here because this
     * comment read as a blanket guarantee that there was nothing left to
     * lock --
     *   the ARC cache            (fixed in 40ad1e3; see arc.h, arc.mu)
     *   the read-heat table      (src/core/vol_heat.c, heat_mu)
     *   the parsed !mbrmap cache (src/core/vol_cpack.c, cpacks_mu)
     * All three are strict-leaf mutexes now, and that -- not the pread -- is
     * what makes this line safe. The third is the sharpest case of the three:
     * its grow REALLOCs the array, which invalidates every pointer into it,
     * and what it handed the reader was a borrow into that array. If you add
     * a cache, a counter or an index to vol_read_range, it needs its own
     * locking; "the pread is thread-safe" is not an answer. And if it hands
     * anything out, hand out a copy or a reference -- never a pointer into a
     * structure another thread can move. */
    got = vol_read_range(g_vol, ino, (uint64_t)offset, size, buf);
    if (got < 0)
        return -EIO;
    return got;
}

static void table_rebuild_locked(void)
{
    /* free old table, rescan */
    free(g_entries);
    g_entries = NULL; g_nentries = 0; g_cap = 0;
    /* WP135: clear the degraded mark BEFORE the rebuild, so it is this
     * rebuild's verdict and not the last one's. A rebuild that completes
     * leaves it 0 and the table is authority again; one that does not sets
     * it in build_file_table_v3. */
    g_table_degraded = 0;
    build_file_table();
}

static int invf_open(const char *path, struct fuse_file_info *fi)
{
    struct acreds c;
    if (strcmp(path, "/") == 0)
        return -EISDIR;
    {
        /* WP135: EIO, not ENOENT, when the table is degraded. */
        int se = snapshot_entry(path + 1, NULL, NULL, NULL);
        if (se < 0)
            return -EIO;
        if (se == 0)
            return -ENOENT;
    }
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
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        /* WP22d: refuse a write open on a non-writable volume (latched /
         * read-only) HERE, at open: with WRITEBACK_CACHE, a write() buffers
         * payload in the kernel page cache before this daemon is asked, and a
         * later-refused write leaves exactly those un-acked bytes in the cache
         * -- the next read would serve the phantom. Failing the open is the
         * POSIX EROFS shape and keeps the phantom out of the cache entirely. */
        if (!vol_write_enabled(g_vol))
            return -EROFS;
        /* read-write open: allocate a write context. Content is NOT loaded:
         * the engine session (begun lazily at the first write) forks the
         * file's segment layout incrementally, so memory stays bounded no
         * matter how large the file is (WP4b). A failed calloc returns
         * straight out of here, before any handle exists to release. The
         * O_TRUNC work lives INSIDE this block so that the context is in
         * scope on its refusal path -- see below. */
        {
            wctx *c = (wctx *)calloc(1, sizeof(wctx));
            if (!c) return -ENOMEM;
            strncpy(c->name, path + 1, 255);
            fi->fh = (uint64_t)(uintptr_t)c;
            fi->keep_cache = 1;
            if (fi->flags & O_TRUNC) {
                /* truncate to empty; the old file's transcode siblings describe
                 * bytes that are gone, so vol_replace_file drops them with it */
                invfs_meta_pub keep;
                uint64_t ino = 0;
                int frc;

                pthread_mutex_lock(&g_io_lock);
                /* WP otrunc-keeps-the-old-identity: the file's POSIX identity
                 * has to be IN HAND before anything is replaced.
                 *
                 * This used to save it with vol_find, whose uint64_t makes
                 * "no such name" and "the lookup could not be completed" the
                 * same 0, and then truncate regardless, writing the saved
                 * copy back only if one had been taken. Losing the save did
                 * not merely skip a write-back: vol_replace_file(v, name,
                 * NULL, 0) reaches vol_v3_create_node with meta == NULL, and
                 * the "in.type == 0" defaulting branch there (vol_dirs.c:324
                 * -- INVFS_ITYP_REG is 0, invarifs.h:1147) cannot be false
                 * for a regular file, so the mode is rewritten to 0644 on
                 * EVERY truncate. Only the write-back restored it. A single
                 * unreadable dirent row was therefore enough to land a file
                 * at 0644; two unreadable rows -- that one, and the inode
                 * read inside create_node whose memset also zeroed uid/gid
                 * -- landed it at 0644 root. The POSIX ACL survived both,
                 * because xattrs are separate 0x03 || inode keys, so the
                 * file came out WIDER than it went in.
                 *
                 * REFUSE, and the reason is the ORDER: this lookup runs
                 * before vol_replace_file, so when the identity cannot be
                 * read nothing has been mutated and there is nothing to undo.
                 * A truncate that cannot preserve identity must not truncate.
                 * The caller gets -EIO, the honest answer for a truncate the
                 * filesystem could not perform correctly, and the same answer
                 * perm_check_cred already gives for an unreadable row.
                 *
                 * This is NOT the create/mkdir asymmetry. Those two differ
                 * because invf_create writes the record FIRST and resolves
                 * the object's own name afterwards, so there a rollback
                 * would take the name away from a caller already holding a
                 * handle. Here the lookup is strictly first: no object to
                 * roll back, no name to unlink, one shape -- refuse, do not
                 * truncate -- and the difference from those sites is the
                 * ordering, not an omission.
                 *
                 * Both non-zero answers of vol_get_meta_rc fail closed:
                 * -EIO is "the row could not be read" and -ENOENT is "there
                 * is no such row". The second is not expected on a v3 volume
                 * (VOLF_V3 is the only format there is), and if it happens
                 * then the file has no recorded identity to preserve, which
                 * is not a licence to replace it with 0644 root. */
                frc = vol_find_rc(g_vol, path + 1, &ino);
                if (frc != 1 || !ino ||
                    vol_get_meta_rc(g_vol, ino, &keep) != 0) {
                    fprintf(stderr, "invf: truncate of \"%s\" REFUSED: the "
                            "file's identity could not be read (lookup rc=%d, "
                            "ino=%llu). The file is untouched.\n",
                            path, frc, (unsigned long long)ino);
                    /* No table_sync_one_locked here: nothing changed, and the
                     * lookup that just failed is the same read that sync
                     * would use to decide the entry's fate. A refusal is
                     * only honest if it changes nothing at all. */
                    pthread_mutex_unlock(&g_io_lock);
                    free(c);
                    /* The handle never existed as far as the kernel is
                     * concerned -- a failed open is sent no .release -- so
                     * this carries the same contract as the failed calloc
                     * above, and handle_acquired() below is not reached. */
                    return -EIO;
                }
                vol_replace_file(g_vol, path + 1, NULL, 0);
                vol_apply_meta(g_vol, path + 1, &keep);
                table_sync_one_locked(path + 1);
                pthread_mutex_unlock(&g_io_lock);
            }
        }
    } else {
        /* WP17: keep page cache across open/close. Every content change
         * flows through this kernel mount (single-mount model; sweeps keep
         * logical bytes identical), so the kernel always knows when to
         * invalidate -- same argument as the pre-existing RDWR branch. */
        fi->keep_cache = 1;
    }
    /* the one success exit, and the one acquire -- see handle_acquired() */
    handle_acquired();
    return 0;
}

static int invf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int temp;
    if (fuse_reserved_name(path)) return -EINVAL;
    temp = is_temp_path(path);
    wctx *c;
    int acldeny = 0;          /* the inherited ACL could not be attached */
    struct fuse_context *ctx = fuse_get_context();
    uint8_t aacl[INVFS_META_XATTR_MAX];
    size_t aalen = 0;
    mode_t cmode = mode & 07777;
    (void)temp;
    if (!vol_write_enabled(g_vol))
        return -EROFS;
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
            if (snapshot_entry(path + 1, NULL, NULL, NULL) == 1) {
                rc = perm_check_cred(&cr, path, W_OK);
                if (rc) return rc;
            }
        }
    }
    if ((fi->flags & O_EXCL) && snapshot_entry(path + 1, NULL, NULL, NULL) == 1)
        return -EEXIST;
    /* default-ACL inheritance: child access ACL = parent's default,
     * masked by the create mode (posix_acl_create_masq) */
    {
        size_t dlen = 0;
        int drc = parent_default_acl(path, aacl, &dlen);
        if (drc < 0) return drc;         /* unreadable: do not create */
        if (drc) {
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
        uint64_t file_ino;
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_REG;
        m.mode = cmode;
        m.uid = ctx ? ctx->uid : 0;
        m.gid = ctx ? ctx->gid : 0;
        m.nlink = 1;
        m.mtime = m.atime = (int64_t)time(NULL);
        file_ino = vol_apply_meta(g_vol, path + 1, &m);
        if (!file_ino)
            fprintf(stderr, "invf: mknod stamp FAILED %s\n", path);
        if (aalen) {
            /* The create-side twin of invf_mkdir's stamp, and the same
             * defect: the id of the record just written is what
             * vol_apply_meta returns, and it used to be thrown away in favour
             * of a second name resolution whose failure read as "no such
             * name". See the long form at the mkdir site above. */
            uint64_t ino2 = file_ino;
            if (!ino2) {
                int frc = vol_find_rc(g_vol, path + 1, &ino2);
                if (frc < 0) {
                    /* Refuse -- the same conclusion as mkdir, but WITHOUT the
                     * rollback, and the difference is the argument rather than
                     * an omission. vol_replace_file above has already replaced
                     * whatever the name referred to, so any pre-existing
                     * content is gone before this decision is reached;
                     * unlinking now would take the NAME as well and leave a
                     * caller holding a handle to a deleted inode, which is
                     * strictly more loss than an empty file that was never
                     * going to be trusted. The volume is left as an O_CREAT
                     * that failed after the record was written, the caller is
                     * told so, and the condition is unmissable in the log. */
                    fprintf(stderr, "invf: create: could not resolve \"%s\" to "
                            "attach the ACL inherited from its parent; the "
                            "create is FAILED and \"%s\" is on the volume "
                            "WITHOUT that ACL -- it is empty and its mode is "
                            "the ACL-masked triad, so it is WIDER than its "
                            "parent's ACL. Remove it before anything is "
                            "written to it.\n", path, path);
                    acldeny = 1;
                }
            }
            if (!acldeny && ino2 &&
                vol_set_xattr(g_vol, ino2, XATTR_ACL_ACCESS, aacl, aalen) != 0)
                fprintf(stderr, "invf: create ACL inherit FAILED %s\n", path);
        }
        /* WP59a: pin /.invariantfs/config/ and /.invariantfs/codecpacks/
         * so they stay builtin-readable (never pack-container coded). */
        if (is_anchored_path(path)) {
            uint8_t anchor_val = 1;
            uint64_t ino2 = vol_find(g_vol, path + 1);
            if (ino2 &&
                vol_set_xattr(g_vol, ino2, INVFS_XATTR_ANCHOR,
                              &anchor_val, sizeof anchor_val) != 0)
                fprintf(stderr, "invf: anchor set FAILED %s\n", path);
        }
        table_sync_one_locked(path + 1);
    }
    pthread_mutex_unlock(&g_io_lock);
    /* Same contract as the calloc failure below: returning here is BEFORE the
     * handle exists, so nothing is acquired and no .release is owed. */
    if (acldeny)
        return -EIO;
    /* From here the handle is ours to release. A failed calloc above returns
     * before this point, so it acquires nothing -- and so is sent no
     * .release. */
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
    /* the one success exit, and the one acquire -- see handle_acquired().
     * libfuse pairs every successful .create with a .release exactly as it
     * does a .open, so the census owed this handle is owed here too; not
     * acquiring is what took reclaim off a mount that merely created a
     * file (see the guard in fuse_sweep_thread). */
    handle_acquired();
    return 0;
}

static int invf_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    wctx *c = (wctx *)(uintptr_t)fi->fh;
    size_t done = 0;
    (void)is_temp_path;
    /* EROFS, not ENOSPC. Every other entry point that refuses a mutation
     * on a read-only volume returns -EROFS -- mkdir, create, unlink, rmdir,
     * symlink, link, mknod, setxattr, removexattr, truncate, rename -- and
     * AGENTS.md 2.10 documents EROFS for this case by name. The two errnos
     * are not interchangeable to a caller: ENOSPC means "retry, space may
     * come back", EROFS means "stop", and a writer that retries ENOSPC (a
     * cp -r onto a latched volume, a package script) never gives up on a
     * volume that cannot accept the write at all. */
    if (!vol_write_enabled(g_vol))
        return -EROFS;
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
            return wrc_eno(rc);
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

/* WP134: a USR1 / user.invfs.sweep pass arms the same rollback window the
 * offline and watermark passes arm, so say so -- and say how to use it --
 * while the operator is still in front of the terminal that asked for it.
 * Before WP134 this pass rewrote the data with nothing behind it and
 * invf-rollback's "no save point" arrived too late to matter. */
static void sweep_rollback_warning(const char *img, const char *mnt)
{
    fprintf(stderr,
        "[sweep] ROLLBACK WINDOW ARMED: this pass re-encodes data, and a\n"
        "[sweep] save point was captured first, so the volume can be put\n"
        "[sweep] back the way it was. To undo THIS pass:\n"
        "[sweep]     1. unmount the filesystem:  fusermount3 -u %s\n"
        "[sweep]     2. restore the save point: invf-rollback %s\n"
        "[sweep] That consumes the window -- afterwards the volume has no\n"
        "[sweep] save point again. Until you spend it the next pass\n"
        "[sweep] REPLACES this one, it does not add to it.\n"
        "[sweep] Proof it is there: the '[spt0] save point: pinned N blocks'\n"
        "[sweep] line just above, or `getfattr -n user.invfs -m- %s` ->\n"
        "[sweep] savepoint=live.\n",
        mnt, img, mnt);
}

/* Manual full pass: collect every data file and sweep it with per-file
 * locking (system stays responsive). Progress to stderr (= console in
 * the guest init context). Trigger: kill -USR1 $(pidof invf-fuse) or
 * /usr/local/bin/invf-sweep. INVFS_SWEEP_INTERVAL=<seconds> additionally
 * enables the periodic mode.
 *
 * full_pass (WP26): 0 = the manual pass (kill -USR1 / the user.invfs.sweep
 * xattr), which drives each file through the plain vol_sweep_file floor;
 * 1 = the watermark-triggered pass, which drives them through the
 * policy/pack-aware vol_sweep_one. That driver choice -- and the heat
 * decay pass that goes with it -- is a SWEEP DATA DECISION and is
 * deliberately NOT tied to the rollback window below.
 *
 * The rollback window (WP77, extended by WP134 to EVERY in-FUSE pass): on
 * v3 it is the SPT0 save point; a live previous pass's save point is
 * dropped (K=1) and spt0_capture records {base_root, delta_end} in
 * prepare, i.e. under g_io_lock and still before vol_collect_sweepables
 * (collect) -- the same position the offline invf-sweep takes
 * (tools/invf-sweep.c:1689, between sw_stage_begin "prepare" and
 * sw_stage_begin "collect"). It stays live as the window after the walk.
 *
 * WP134: the USR1/xattr path used to pass 0 here and arm nothing, so an
 * operator who swept that way and then reached for invf-rollback got
 * "no save point" -- AFTER the data had been rewritten. It now arms the
 * same save point the offline and watermark paths arm, and it arms it
 * FAIL-CLOSED: if the capture is refused the pass is abandoned, because a
 * rewrite with no way back is worse than no rewrite. The watermark path
 * keeps its pre-existing fail-open behaviour.
 *
 * There is no second arm: the v3 save point above is the only window this
 * build has. The coarse WP21 sweep checkpoint it replaced had no writer,
 * so there was never a v3 path to it. */
static void invf_sweep_worker(int full_pass)
{
    uint64_t *ids = NULL;
    size_t cap = 0, n = 0, found = 0, i;
    long swept = 0, skipped = 0, failed = 0;
    int complete;
    const char *tag = full_pass ? "watermark" : "manual";

    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); free(ids); return; }
    {
        /* WP77: the rollback window is the SPT0 save point. K=1: a
         * previous pass's save point is dropped first, so the live
         * window is always the LAST sweep. */
        invfs_spt0 sp;
        if (spt0_info(g_vol, NULL))
            (void)spt0_drop(g_vol);
        if (spt0_capture(g_vol) == 0) {
            if (spt0_info(g_vol, &sp))
                fprintf(stderr, "[%s] save point captured "
                                "(base_root=%llu delta_end=%llu)\n", tag,
                        (unsigned long long)sp.base_root,
                        (unsigned long long)sp.delta_end);
            if (!full_pass)
                /* WP134: the operator asked for this pass in the
                 * foreground, so tell them -- before the walk, while it
                 * is still true -- how to take it back. */
                sweep_rollback_warning(g_img_path, g_mnt_path);
        } else {
            fprintf(stderr, "[%s] save point capture failed; %s\n", tag,
                    full_pass ? "sweeping without one"
                              : "REFUSING the pass (the volume is unchanged)");
            if (!full_pass) {
                /* fail closed: no window, no rewrite */
                pthread_mutex_unlock(&g_io_lock);
                free(ids);
                return;
            }
        }
    }
    if (full_pass)
        vol_heat_sweep_begin(g_vol);   /* one decay pass per sweep run */
    /* WP-fuse-sweep-inode-cap: GROW, and know whether the list is whole.
     *
     * This used to be a fixed `malloc(300000 * 8)` and a bare
     * vol_collect_sweepables(), so above 300,000 sweepable inodes the pass
     * swept a strict PREFIX and then printed an ordinary successful DONE
     * line. Nothing compared the stored count against what the walk saw, so
     * the truncation was invisible -- and this is the pass a live volume
     * depends on: AGENTS.md 2.5 makes the watermark ladder take the capture
     * itself, and 2.6 makes USR1 / the user.invfs.sweep xattr the daemon's
     * own recovery path. A silently truncated pass on a live volume means
     * dead segments accumulate forever behind a clean log.
     *
     * The 300,000 was not a memory budget and not a fixed allocation
     * chosen for a reason: it is 2.4 MB of ids against a process that
     * already caches 256 MB of ARC by default, it is a bare literal with no
     * comment, and `git log -S "max = 300000"` reaches the initial import
     * with no commit message offering a rationale. It is a guess that was
     * never checked against the volume geometry, so the buffer now grows
     * the way the offline sweep's own collector already does
     * (tools/invf-sweep.c:1203-1221).
     *
     * cap is 0 here on purpose: the seed lives inside
     * vol_collect_sweepables_grow(), so there is no ceiling on this path to
     * be tempted back into a literal. It doubles and re-collects until a
     * collect comes back complete, and returns 0 only then. The re-walks it
     * costs are bounded by log2(files/seed) and are lost against the
     * per-file usleep(500) further down this same loop. */
    complete = (vol_collect_sweepables_grow(g_vol, &ids, &cap, &n, &found) == 0);
    if (!complete) {
        /* WP135: -1 has two causes now and they need different words.
         * found > n is the buffer-full case (the growth ran out of memory);
         * found == n is the WALK case, where the collector could not
         * enumerate the live set at all -- a quarantined base page, and the
         * delta pass that vol_v3_iter_live_inodes skips after a failed base
         * scan, so the list is missing every inode created since the last
         * fold. Before the fix the second case reached here as `complete`
         * with a matching count and printed an ordinary DONE line: the
         * sharpest of the five, because a damaged volume then accumulates
         * dead segments forever behind a clean log (AGENTS.md 2.5). */
        if (found > n) {
            fprintf(stderr,
                    "[%s] INCOMPLETE: could not allocate a sweep list for this "
                    "volume; collected %zu of at least %zu inodes. The pass "
                    "below is a PARTIAL sweep, not a whole one.\n", tag, n, found);
        } else {
            fprintf(stderr,
                    "[%s] INCOMPLETE: the live-set WALK did not complete (%zu "
                    "inodes collected, and the walk stopped rather than "
                    "finished). The volume is damaged or unreadable; the pass "
                    "below would rewrite only a subset of it.\n", tag, n);
        }
        if (!full_pass) {
            /* Fail closed, the same rule the save-point capture follows
             * above: the operator asked for this in the foreground, a
             * subset is not what they asked for, and the volume is
             * unchanged. Say what would actually collect it all. */
            fprintf(stderr,
                    "[%s] REFUSING the pass (the volume is unchanged). Sweep "
                    "it offline instead -- invf-sweep on the unmounted image "
                    "grows its list the same way and has no memory ceiling "
                    "here.\n", tag);
            pthread_mutex_unlock(&g_io_lock);
            free(ids);
            return;
        }
        /* The watermark pass keeps its pre-existing fail-open behaviour:
         * that pass exists to relieve fill pressure, and abandoning it
         * leaves the volume worse. It sweeps what it got -- loudly, and
         * the DONE line below carries the marker. */
    }
    fprintf(stderr, "[sweep] %s pass started: %zu files%s\n", tag, n,
            complete ? "" : " (PARTIAL -- see INCOMPLETE above)");
    for (i = 0; i < n; i++) {
        int rc;
        if (g_shutdown || !g_vol) break;
        if (full_pass) {
            /* the daemon's own driver (same as the pending drain):
             * policy/pack-aware -- the plain vol_sweep_file floor defers
             * everything on small images (WP16b margin) and would never
             * relieve the watermark's pressure */
            char nm[256];
            /* WP135: == 1, and -1 counted as a FAILURE. The name lookup
             * now reports a walk that stopped as -1 rather than 0; truthiness
             * would have handed that -1 to vol_sweep_one together with an
             * empty name, and "skipped" would have told the operator the
             * file was deleted between collect and sweep. It was not. */
            int nrc = vol_sweep_name_of(g_vol, ids[i], nm, sizeof nm);
            if (nrc == 1) {
                rc = vol_sweep_one(g_vol, ids[i], nm);
                /* one rc: 0 = nothing to do, >0 = swept/deferred, <0 = err */
                if (rc > 0)       { swept++;   }
                else if (rc == 0) { skipped++; }
                else              { failed++; }
            } else if (nrc == 0) {
                skipped++;   /* deleted between collect and walk */
            } else {
                failed++;   /* the namespace could not be read for this inode */
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
        if (full_pass)
            vol_heat_promote(g_vol);   /* extract read-hot batch members */
        vol_tz_flush(g_vol);   /* seal anything the walk deferred */
        vol_flush(g_vol);
    }
    pthread_mutex_unlock(&g_io_lock);
    /* WP-fuse-sweep-inode-cap: the two counts the operator needs, and the
     * marker that says whether they are the same number.
     *
     * `found` is what the collection walk saw on the volume; `files` is what
     * this pass actually had a list entry for and therefore swept. On a
     * complete collect they are equal by construction -- the growing
     * collector only returns success when found == files -- so the line
     * states the identity rather than leaving it implied. On a short collect
     * they differ, found is a LOWER BOUND (the walk stops one entry past the
     * cap), and the line says PARTIAL so the pass cannot be read as a
     * clean one. swept+skipped+failed is the per-file disposition and
     * reconciles against `files` (the loop breaks early only on g_shutdown).
     *
     * This is the checkable form: `files`, `swept+skipped+failed` and
     * `found` are all in one line, and a truncated pass is the only way to
     * see them disagree. */
    fprintf(stderr,
            "[sweep] DONE found=%zu files=%zu swept=%ld skipped=%ld "
            "failed=%ld%s\n",
            found, n, swept, skipped, failed,
            (complete && found == n) ? "" : " INCOMPLETE");
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
     * pass; the next kick waits for the fill to rise above it. 0 = no kick
     * yet (or the fill dropped below the mark: re-armed). */
    uint64_t wm_floor = 0;
    /* WP137: the ladder OWES the volume one capture.
     *
     * The floor alone is a self-deadlock. A pass's rollback window is a HOLD:
     * the capture pins every block the pre-sweep generation's recipes named
     * (spt0_pin_take -> vol_v3_iter_inodes_at, vol_spt0.c:795), and those
     * are exactly the blocks the pass superseded -- so the fill the floor
     * records at the pass's exit is the fill WITH THE HOLD, not the fill the
     * pass produced. The reclaim that actually gives them back runs at the
     * NEXT capture (spn_reclaim, vol_spt0.c:672, called from spt0_pin_take
     * at :814), and the floor's own re-arm rule ("fill > wm_floor") can never
     * fire again, because the hold is what is keeping the fill up. So the
     * pass that creates the debt is the last pass that ever runs, and the
     * volume stays short until an operator sweeps it by hand.
     *
     * This flag is the discharge the floor could not express: the pass that
     * armed a window owes the volume the one capture that collects that
     * window's debt, and it is spent on the next pass whether or not the
     * fill moves. It does not weaken the window -- the held blocks are still
     * the ones invf-rollback restores onto, and they are still freed by a
     * CAPTURE, never at the end of the pass that armed it. See the cycle
     * note at the decision below for why it cannot re-pin forever. */
    int wm_owed = 0;
    /* WP137: no pass has been taken in this session yet. The one exception
     * to the mark gate is that first pass -- a mount is an operator action,
     * and a window inherited from the previous session is exactly what it
     * should go and collect. Session-scoped, so it lives out here with the
     * rest of the ladder state and not in the per-tick block below. */
    int wm_first = 1;
    /* A save point left live by a previous mount (watermark pass, USR1
     * pass, xattr pass, or the offline invf-sweep) still holds its
     * superseded blocks, so the fill reads high from the start. Kicking on
     * that stale reading would run a no-op walk whose arm drops the old
     * window -- the rollback window would evaporate on a plain remount.
     * Seed the floor with the current fill instead: the next pass needs
     * genuinely NEW pressure.
     *
     * The debt flag rides along: a window inherited from a previous
     * mount holds that mount's debt exactly as a pass this session ran would,
     * so this session owes the volume the same one discharge capture. */
    if (g_raw_watermark > 0) {
        pthread_mutex_lock(&g_io_lock);
        if (g_vol) {
            int live = spt0_info(g_vol, NULL) != 0;
            if (live) {
                uint64_t rf = 0, rt = 0;
                vol_zone_free(g_vol, &rf, &rt, NULL, NULL);
                if (rt) wm_floor = rt - rf;
                wm_owed = 1;
            }
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
            /* Defer while a handle is open, because a pass rewrites block
             * layout under it. Note the test is "> 0", not "!= 0": this
             * guard's subject is OCCUPANCY, and a count below zero is not
             * a claim that a handle exists. Reading a bad count as "busy"
             * is precisely what turned a one-line accounting asymmetry
             * into a mount that silently lost background reclaim for its
             * whole lifetime -- the worker re-reads the same wrong answer
             * every tick, so nothing ever recovers and nothing ever says
             * so. A wrong count is a bug to REPORT, not a reason to
             * switch off the thing that recovers space, so say it once,
             * loudly, and keep working. handle_acquired() makes the count
             * negative-unreachable; this is the tripwire for the day
             * someone adds a fourth entry point and forgets. */
            if (g_open_handles < 0) {
                if (!g_handles_warned) {
                    fprintf(stderr,
                            "invf: open-handle count is %d, which is "
                            "impossible (handle_acquired/handle_released are "
                            "unbalanced); clamping to 0 and continuing. This "
                            "is a daemon accounting bug -- report it.\n",
                            g_open_handles);
                    g_handles_warned = 1;
                }
                g_open_handles = 0;
            }
            if (g_shutdown || g_open_handles > 0) continue;
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
         * kick rearms on RISING fill (a new high above the last pass's exit
         * fill) OR when the pass it just ran owes the volume its discharge
         * capture (wm_owed, above). Default (no raw_watermark): lazy, this
         * block is inert. */
        if (g_raw_watermark > 0 && !g_shutdown && !g_sweep_busy) {
            uint64_t rf = 0, rt = 0, fill = 0, after = 0, reclaimed = 0;
            int over, fresh, window = 0;
            pthread_mutex_lock(&g_io_lock);
            if (g_vol) vol_zone_free(g_vol, &rf, &rt, NULL, NULL);
            pthread_mutex_unlock(&g_io_lock);
            if (rt) fill = rt - rf;
            over = rt && (fill * 100 > (uint64_t)g_raw_watermark * rt);
            if (rt && !over)
                wm_floor = 0;               /* below the mark: re-arm */
            if (rt && (wm_owed ? (over || wm_first)
                               : (over && fill > wm_floor))) {
                fresh = !wm_owed;    /* kicked by pressure, not by a debt */
                if (fresh)
                    fprintf(stderr, "[watermark] RAW fill %llu/%llu over %d%%: "
                                    "kicking a sweep pass\n",
                            (unsigned long long)fill, (unsigned long long)rt,
                            g_raw_watermark);
                else
                    fprintf(stderr, "[watermark] the pass just run armed a "
                                    "rollback window; taking the one capture "
                                    "that discharges it (its held blocks are "
                                    "why the fill is still high)\n");
                wm_first = 0;
                g_sweep_busy = 1;
                invf_sweep_worker(1);
                g_sweep_busy = 0;
                pthread_mutex_lock(&g_io_lock);
                if (g_vol) {
                    rf = rt = 0;
                    vol_zone_free(g_vol, &rf, &rt, NULL, NULL);
                    if (rt) after = rt - rf;
                    /* WP137: the cycle, and why it ends.
                     *
                     * A pass ARMS a window and, by re-encoding, supersedes
                     * recipes; the blocks those recipes named stay allocated
                     * because the window the pass just armed is holding them
                     * -- that is the rollback guarantee, and it is not
                     * weakened here. What the pass created is DEBT, and the
                     * only collector of debt is a CAPTURE (spn_reclaim).
                     * So the ladder owes exactly one capture per window, and
                     * the sequence on a volume that needs reclaiming is
                     *
                     *     pass 1  over the mark: arms window 1, supersedes N
                     *     pass 2  capture reclaims N  -> fill back under the
                     *             mark, and the mark is the first stop
                     *     pass 3  only if the fill is STILL over the mark
                     *             (live data): capture reclaims 0, the
                     *             EMPTY reclaim is the second stop
                     *
                     * The measurement is lag-by-one and cannot be otherwise:
                     * a pass creates debt while it re-encodes, and only the
                     * NEXT capture can see it. That is why pass 1 is owed a
                     * capture in advance (fresh) rather than being trusted
                     * to have left nothing behind.
                     *
                     * Two stops, both measured, and neither a constant:
                     *   - the MARK. An owed capture is not taken on a volume
                     *     that is not over its own watermark. Without that
                     *     gate a workload that keeps rewriting one file
                     *     leaves a live window with something dead in it on
                     *     every capture, every reclaim comes back non-empty,
                     *     and the ladder would sweep an idle volume forever.
                     *     Under the mark there is no pressure to relieve, and
                     *     the debt waits for the next capture of any kind --
                     *     the next episode, a USR1, or a maintenance sweep.
                     *     wm_first is the one exception: the first pass of a
                     *     session runs even under the mark, because a mount
                     *     is an operator action and a window inherited from
                     *     the previous session is exactly what it is for.
                     *   - the EMPTY RECLAIM. Above the mark, where the fill
                     *     is live data rather than a hold, the chain ends on
                     *     the first capture that frees nothing: a pass that
                     *     superseded nothing created no debt, so an empty
                     *     reclaim proves the window it armed is holding only
                     *     live blocks. Re-arming "whenever the fill is high"
                     *     instead is the churn this design exists to avoid --
                     *     every such pass re-pins and no empty reclaim is
                     *     ever reached. */
                    window  = spt0_info(g_vol, NULL) != 0;
                    reclaimed = spt0_reclaim_last(g_vol);
                }
                wm_floor = after;
                wm_owed = window && (fresh || reclaimed);
                pthread_mutex_unlock(&g_io_lock);
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
         * state, so a failed commit never leaves a torn file. WP85: for a
         * stale handle the abort is the WHOLE point -- the session's
         * segments belong to a retired generation, so retiring it here is
         * what hands their blocks back instead of leaking them. */
        fprintf(stderr, "invf: commit %s failed (%s)\n", c->name,
                rc == -2 ? "ENOSPC" : (rc == -ESTALE ? "ESTALE" : "io"));
        vol_write_abort(c->ws);
        c->ws = NULL;
        dirty_del_locked(c);
        vol_flush(g_vol);
        table_sync_one_locked(c->name);
        pthread_mutex_unlock(&g_io_lock);
        return wrc_eno(rc);
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
    handle_released();
    return 0;
}

/* rename is fully implemented in the engine (sibling-safe, dir-prefix
 * aware); audit PB8: it was simply never wired into this ops table */
static int invf_rename(const char *from, const char *to, unsigned int flags)
{
    int rc, noreplace = 0;
    struct acreds c;
    if (flags & ~(unsigned int)RENAME_NOREPLACE)
        return -EOPNOTSUPP;   /* RENAME_EXCHANGE / RENAME_WHITEOUT */
    /* WP135: only `to` introduces a name; `from` must stay usable so a file
     * already on the reserved namespace can be moved OFF it. */
    if (fuse_reserved_name(to)) return -EINVAL;
    if (flags & RENAME_NOREPLACE) noreplace = 1;
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
        /* an overwritten victim is a delete: same sticky rule.
         * `!= 0` rather than `== 1` on purpose. meta_for_path answers -1 for
         * a name that resolved but whose row could not be read, and that is
         * NOT "there is no victim": it is a victim whose sticky bit is
         * unknown, which is the one case the rule below exists for. Take the
         * check; perm_check_sticky refuses on the unreadable row itself. */
        if (meta_for_path(to, (char[300]){0}, 300, &(invfs_meta_pub){0}) != 0) {
            rc = perm_check_sticky(&c, to);
            if (rc) return rc;
        }
    }
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    {
        int was_dir = vol_is_dir(g_vol, from + 1);
        if (noreplace && strcmp(from, to) != 0) {
            /* WP65: RENAME_NOREPLACE must not clobber the destination.
             * Checked under g_io_lock so it is atomic with the move.
             *
             * vol_find_rc, NOT vol_find. vol_find returns a uint64_t whose
             * "no such name" and "the lookup failed" are both the single
             * value 0, so the `!= 0` below read an unreadable dirent row as
             * "the destination is absent" -- and then vol_rename
             * overwrote a destination this flag was explicitly told to
             * protect, and the call returned success. A name that could
             * not be resolved is not an absent name; refuse instead.
             * This is the same substitution the three permission paths
             * already made (perm_check_cred, parent_default_acl and
             * invf_chmod) when vol_find_rc was introduced. */
            uint64_t dino = 0;
            int frc;
            if (vol_is_dir(g_vol, to + 1)) {
                pthread_mutex_unlock(&g_io_lock);
                return -ENOTEMPTY;
            }
            frc = vol_find_rc(g_vol, to + 1, &dino);
            if (frc < 0) {
                pthread_mutex_unlock(&g_io_lock);
                return -EIO;
            }
            if (frc == 1) {
                pthread_mutex_unlock(&g_io_lock);
                return -EEXIST;
            }
        }
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
    /* WP-M20: base reads are lock-free (immutable base, delta published atomically) */
    if (!g_vol) return -EIO;
    sb = vol_sb(g_vol);
    free_blocks = vol_free_blocks_cached(g_vol);
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
    return rc == 0 ? 0 : wrc_eno(rc);
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
    return rc == 0 ? 0 : wrc_eno(rc);
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
    if ((rc = meta_for_path(path, ename, sizeof ename, &cur)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
    /* ownership rule (notify_change enforces it at VFS level already;
     * kept here as the daemon is the sole authority on everything else) */
    acreds_get(&c);
    if (!c.bypass && c.uid != cur.uid)
        return -EPERM;
    /* POSIX.1e: fold the new mode into a stored access ACL -- USER_OBJ
     * and OTHER are set from the mode, the group class (MASK if present,
     * else GROUP_OBJ) gets the mode's group bits; a now-trivial ACL is
     * dropped. No ACL -> nothing to do.
     *
     * A row read that FAILS is not "no ACL to fold into". Skipping the
     * fold leaves the previous ACL standing while the mode below changes,
     * so the file's effective permissions stop matching what the owner just
     * asked for -- in the permissive direction whenever the chmod was
     * removing access, which the named-user and named-group entries are
     * exactly the ones that decide. The chmod is refused instead: the
     * caller is told the ACL could not be updated, and nothing is left half
     * applied. */
    pthread_mutex_lock(&g_io_lock);
    if (g_vol) {
        uint64_t ino = 0;
        /* vol_find_rc, not vol_find: a name that could not be resolved has to
         * be refused, not treated as an inode with no ACL to fold into --
         * otherwise the mode below is changed while the old ACL keeps
         * granting what the owner just took away. */
        int frc = vol_find_rc(g_vol, ename, &ino);
        if (frc < 0) {
            pthread_mutex_unlock(&g_io_lock);
            return -EIO;
        }
        if (frc == 1) {
            uint8_t acl[INVFS_META_XATTR_MAX];
            size_t alen = sizeof acl;
            int arc = vol_get_xattr(g_vol, ino, XATTR_ACL_ACCESS, acl, &alen);
            if (arc == -EIO) {
                pthread_mutex_unlock(&g_io_lock);
                return -EIO;
            }
            if (arc == 0) {
                int keep = acl_chmod_masq(acl, alen, (unsigned)(mode & 0777)) > 0;
                int wrc = keep
                    ? vol_set_xattr(g_vol, ino, XATTR_ACL_ACCESS, acl, alen)
                    : vol_remove_xattr(g_vol, ino, XATTR_ACL_ACCESS);
                /* SAME REASONING AS THE READ ABOVE, ON THE WRITE SIDE.
                 * Discarding this return is what made a chmod a permission
                 * WIDENING: the mode below is applied, the chmod returns 0,
                 * and the ACL keeps its old ACL_MASK and ACL_OTHER -- and
                 * because this mount does not negotiate default_permissions
                 * (AGENTS.md 2.9) the ACL, not the mode, IS the permission
                 * decision perm_check_cred makes. A chmod 0600 that only
                 * narrows therefore left the file exactly as readable as
                 * before while reporting success.
                 *
                 * The fold already runs BEFORE meta_apply_patch, which is the
                 * load-bearing part of this fix: the mode has not been touched
                 * yet, so refusing here leaves nothing to undo. The reverse
                 * order is not merely worse, it is unrecoverable in the
                 * permissive direction. (The other interleaving -- ACL written,
                 * meta_apply_patch then fails -- is the safe one: the ACL
                 * already encodes the new permissions and the stale mode is
                 * only the fallback nobody takes.)
                 *
                 * What the caller sees: -EIO, the same answer invf_setxattr
                 * gives for a failed xattr write, plus a line on the daemon's
                 * stderr naming the path. NOT a silent 0, and not a partial
                 * application. */
                if (wrc != 0) {
                    fprintf(stderr, "invf: chmod %s: access ACL %s failed (%d) "
                                    "-- refusing, mode NOT changed\n",
                            path, keep ? "write" : "remove", wrc);
                    pthread_mutex_unlock(&g_io_lock);
                    return -EIO;
                }
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
    if ((rc = meta_for_path(path, ename, sizeof ename, &cur)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
    /* POSIX chown rules, daemon-side: on this kernel the VFS does NOT
     * police chown for a default_permissions-less FUSE mount (verified:
     * a foreign uid could chown away someone else's file). Owner change
     * is privileged (CAP_CHOWN); a group change is allowed for the owner
     * into a group they belong to.
     *
     * `cur` is read against the caller's own uid here, so a row that could
     * not be read must NOT answer as uid 0: with the defaults, "am I the
     * owner?" is asked of root and the rule below is evaluated against a
     * different inode's ownership. Refuse instead. */
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
    if (fuse_reserved_name(linkpath)) return -EINVAL;
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
    if ((rc = meta_for_path(path, ename, sizeof ename, &m)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
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
    if (fuse_reserved_name(dest)) return -EINVAL;
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
        /* v2 needs this patch: both names share blocks, so record nlink=2
         * to make unlinking ONE name take the name-only path instead of
         * retiring blocks. On v3 vol_hardlink already maintains the shared
         * row's nlink; forcing it back to 2 here would corrupt a third or
         * later link. */
        if (!(vol_sb(g_vol)->vol_flags & VOLF_V3)) {
            const char *names[2] = { from + 1, dest + 1 };
            for (int q = 0; q < 2; q++) {
                invfs_meta_pub mm;
                uint64_t nid2 = vol_find(g_vol, names[q]);
                if (nid2 && vol_get_meta(g_vol, nid2, &mm) == 0) {
                    mm.nlink = 2;
                    vol_apply_meta(g_vol, names[q], &mm);
                }
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
{
    uint8_t typ;
    uint64_t nid;
    struct fuse_context *ctx = fuse_get_context();
    uint8_t aacl[INVFS_META_XATTR_MAX];
    size_t aalen = 0, dlen = 0;
    mode_t cmode = mode & 07777;
    int rc;
    if (fuse_reserved_name(path)) return -EINVAL;
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
    rc = parent_default_acl(path, aacl, &dlen);
    if (rc < 0) return rc;               /* unreadable: do not create */
    if (rc) {
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
        char buf[768];
        int n;
        invfs_spt0 sp;
        int live = 0;
        pthread_mutex_lock(&g_io_lock);
        /* WP134: a rollback window nobody can see is useless. `savepoint=`
         * is the operator's proof that a sweep armed one -- the offline
         * invf-sweep, the watermark pass and the USR1/xattr pass all leave
         * it live. Read-only: spt0_info() just reads the loaded SPT0. */
        live = g_vol ? spt0_info(g_vol, &sp) : 0;
        n = snprintf(buf, sizeof buf,
                     "volume=%s\n"
                     "entries=%d\n"
                     "free_blocks=%llu\n"
                     "pending_sweep=%llu\n"
                     "sweep_busy=%d\n"
                     "savepoint=%s\n"
                     "savepoint_base_root=%llu\n"
                     "savepoint_delta_end=%llu\n",
                     g_img_path, g_nentries,
                     (unsigned long long)(g_vol ? vol_count_free(g_vol) : 0),
                     (unsigned long long)(g_vol ? vol_pending_count(g_vol) : 0),
                     g_sweep_busy,
                     live ? "live" : "none",
                     (unsigned long long)(live ? sp.base_root : 0),
                     (unsigned long long)(live ? sp.delta_end : 0));
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
    if ((rc = meta_for_path(path, ename, sizeof ename, &m)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) {
        pthread_mutex_unlock(&g_io_lock);
        return meta_for_path(path, ename, sizeof ename, &m) == 1
               ? -ENODATA : -ENOENT;
    }
    {
        size_t vlen = size;
        rc = vol_get_xattr(g_vol, ino, name, value, &vlen);
        pthread_mutex_unlock(&g_io_lock);
        if (rc == 0) return (int)vlen;
        /* Pass the engine's answer through. This used to be
         * `rc == -1 ? -ENODATA : -ERANGE`, which turned a row that could not
         * be read into "this attribute does not exist" -- a confidently
         * wrong answer to a question the application asked. -ENODATA now
         * means the attribute is genuinely not there and -EIO means the
         * store could not be read, which is what the caller has to act on
         * differently (retry, repair, or stop trusting the answer).
         * -EINVAL cannot reach here (name comes from the kernel and
         * vol_get_xattr already rejected it), and is passed through
         * unchanged rather than being flattened into ENODATA. */
        return rc;
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
    /* Refuses an unreadable row rather than answering as uid 0: the
     * ownership test below is `c.uid != m.uid`, and a default-filled m is
     * owned by root, so a failed read used to turn "is the caller the
     * owner of this ACL?" into "is the caller root?". */
    if ((rc = meta_for_path(path, ename, sizeof ename, &m)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
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
    if ((rc = meta_for_path(path, ename, sizeof ename, &m)) != 1)
        return rc < 0 ? -EIO : -ENOENT;
    pthread_mutex_lock(&g_io_lock);
    if (!g_vol) { pthread_mutex_unlock(&g_io_lock); return -EIO; }
    ino = vol_find(g_vol, ename);
    if (!ino) { pthread_mutex_unlock(&g_io_lock); return 0; }
    /* engine semantics: positive = bytes written (= total needed);
     * -2 = buffer too small; 0 = no xattrs; -EIO = the xattr store could
     * not be read. `0` and an error used to both land on `return 0`, so a
     * scan failure told getfattr(1) the object carries no xattrs. */
    rc = vol_list_xattr(g_vol, ino, list, size);
    pthread_mutex_unlock(&g_io_lock);
    if (rc >= 0) return rc;
    if (rc == -2) return -ERANGE;
    return rc;
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
    if ((rc = meta_for_path(path, ename, sizeof ename, &m)) != 1)
        return rc < 0 ? -EIO : -ENOENT;   /* same "answered as root" trap as setxattr */
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
    /* Pass the engine-s errno through. The engine used to answer a bare -1
     * for both "no such xattr" and "the row could not be read", so this
     * mapping had to guess -- and it guessed ENODATA. vol_v3_xattr_delta_del
     * now returns real errnos, which makes the guess actively wrong: -ENODATA
     * is not -1, so a genuinely absent attribute was reported as EIO. The
     * control on the regression test is what caught it: the leg asserting a
     * truly absent xattr is still ENODATA. */
    if (rc == 0)
        return 0;
    if (rc == -1)            /* a caller that has not been converted yet */
        return -EIO;
    return rc;
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
    /* A delete is deliberately allowed under the ordinary
     * read-only space latch (H5, it is the way out). */
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
        /* nlink > 1 selects the name-only unlink, so this decides whether
         * the OTHER hard links survive. An unreadable row must not be read
         * as nlink == 1: that retires the inode and every link to it. */
        if (have_meta < 0) return -EIO;
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
    }
    /* There is deliberately no "index ghost" recovery here any more.
     *
     * It used to ask vol_forget_name() whether the name was a live entry or a
     * ghost left behind by a killed process, and answer ENOENT only for a
     * live one. But the v2 name index that could leave a ghost is GONE
     * (0c82a7a), so vol_forget_name() is a stub that returns 0 for
     * EVERYTHING -- the function says "forgotten" about a name that is still
     * perfectly live. The branch therefore always fired, and it CONVERTED
     * EVERY FAILED UNLINK INTO A SUCCESS: rm of a file that does not exist
     * returned 0, the entry was dropped from the table anyway, and a
     * subsequent lookup reported ENOENT for a file still on the volume.
     *
     * A stub that always agrees is not a recovery path, it is the absence of
     * a check. rm(2) must fail when nothing was removed. */
    pthread_mutex_unlock(&g_io_lock);
    if (rc == 0) return 0;
    return vol_write_enabled(g_vol) ? -ENOENT : -EROFS;
}

/* WP66 H3: fallocate stub. journald calls fallocate() on its journal
 * files; without this the kernel returns EOPNOTSUPP and journald gives
 * up. We return 0 (no-op) — the volume doesn't support hole-punch or
 * preallocation, but callers that only need "file exists and is long
 * enough" are satisfied.
 *
 * WP101: that answer was honest for mode 0 only. PUNCH_HOLE and
 * ZERO_RANGE also got 0, which is a LIE: the caller is told blocks were
 * released (or zeroed) and no block ever was. A caller that punches
 * holes to shrink a file — SQLite, the trim-style reclaimers, log
 * truncators — silently leaks, and df never moves. EOPNOTSUPP is the
 * answer those callers already know how to fall back from, so say it.
 * FALLOC_FL_* is not defined on every libc, hence the literals. */
static int invf_fallocate(const char *path, int mode, off_t offset,
                          off_t length, struct fuse_file_info *fi)
{
    (void)path; (void)offset; (void)length; (void)fi;
    /* 0x02 = PUNCH_HOLE, 0x10 = ZERO_RANGE, 0x01 = KEEP_SIZE pairs with
     * either; none of them is a preallocation. */
    if (mode & 0x02 || mode & 0x10)
        return -EOPNOTSUPP;
    return 0;
}

/* WP66 H3: ioctl stub for FS_IOC_GETFLAGS / FS_IOC_SETFLAGS.
 * systemd-journald uses chattr +C (FS_IOC_SETFLAGS, clears
 * FS_NOCOMP_FL) on newly created journal files; without this the
 * kernel returns ENOTTY and journald falls back — but some versions
 * abort the journal entirely. We silently ignore the flags. */
#include <linux/fs.h>
static int invf_ioctl(const char *path, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned int flags,
                      void *data)
{
    (void)path; (void)fi; (void)flags;
    switch (cmd) {
    case FS_IOC_GETFLAGS:
    case FS_IOC_SETFLAGS:
        /* Accept silently; flags are not stored on the volume. */
        memset(data, 0, sizeof(long));
        return 0;
    default:
        return -ENOTTY;
    }
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
            (conn->want & FUSE_CAP_WRITEBACK_CACHE) ? '+' : '-',
            (int)g_tmp_area);
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
    .fallocate = invf_fallocate,
    .ioctl = invf_ioctl,
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
                size_t need = strlen(opts) + strlen(argv[i]) + 2;
                i++;
                if (need > sizeof obuf) {
                    /* fuse_main would silently lose the tail options */
                    static char *obuf_big;
                    obuf_big = (char *)realloc(obuf_big, need);
                    if (obuf_big) {
                        snprintf(obuf_big, need, "%s,%s", opts, argv[i]);
                        opts = obuf_big;
                    }
                } else {
                    snprintf(obuf, sizeof obuf, "%s,%s", opts, argv[i]);
                    opts = obuf;
                }
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
            } else if (strcmp(tok, "ignore-missing-codecs") == 0) {
                g_ignore_missing_codecs = 1;
            } else if (strcmp(tok, "ignore-codec-versions") == 0) {
                g_ignore_codec_versions = 1;
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

    g_vol = vol_open(img, &err);
    if (!g_vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }

    /* WP59: codec-policy mount gate.
     * - No PCK0 (legacy volume): refuse unless -o ignore-missing-codecs.
     * - BASIC_ONLY: pass (builtin codecs only, no packs required).
     * - Non-BASIC_ONLY with n_codecs > 0: refuse if codec packs are not
     *   installed (resolution deferred to WP60; for now refuse loudly).
     */
    {
        const invfs_pck0 *pk = vol_pck0(g_vol);
        if (!pk) {
            if (!g_ignore_missing_codecs) {
                fprintf(stderr, "invf: no codec policy (PCK0) on volume %s; "
                        "use -o ignore-missing-codecs to mount anyway\n", img);
                g_shutdown = 1;
                vol_close(g_vol);
                g_vol = NULL;
                return 1;
            }
            fprintf(stderr, "invf: WARNING: no codec policy; some reads "
                    "may fail with EIO\n");
        } else if (pk->policy_flags & INVFS_PCK0_BASIC_ONLY) {
            /* BASIC_ONLY: no packs required; mount succeeds */
        } else if (pk->n_codecs > 0 && !g_ignore_missing_codecs) {
            fprintf(stderr, "invf: volume %s requires %u codec pack%s "
                    "(policy not BASIC_ONLY); use -o ignore-missing-codecs "
                    "to mount anyway\n", img,
                    (unsigned)pk->n_codecs,
                    pk->n_codecs == 1 ? "" : "s");
            g_shutdown = 1;
            vol_close(g_vol);
            g_vol = NULL;
            return 1;
        }
    }
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
    snprintf(g_mnt_path, sizeof g_mnt_path, "%s", mnt);
    setvbuf(stderr, NULL, _IONBF, 0);
    build_file_table();
    fprintf(stderr, "InvariantFS mounted: %d files\n", g_nentries);

    /* The background on-demand sweep thread USED to be created here, twelve
     * lines above the fuse_daemonize() further down, and that was the whole
     * bug: fork keeps exactly ONE thread, so the daemon -- the process an
     * operator signals with `kill -USR1 $(pidof invf-fuse)` -- was left with
     * main and libfuse's workers and NOBODY consuming g_sweep_now. Both
     * documented triggers only set that flag (on_sweep_signal below, the
     * user.invfs.sweep xattr), so on a default mount they were silent no-ops:
     * the watermark ladder never evaluated, the pending drain never ran, and
     * no rollback window was ever armed -- none of which AGENTS.md 2.5/2.6
     * had stopped promising. It is created in the daemon, after the fork. */

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

        /* The sweep thread belongs to the DAEMON, so it is created here --
         * after the fork above, which is the entire point (see the note where
         * it used to be created). Everything it touches was already set
         * before the mount: g_vol (:3565), g_raw_watermark (:3620),
         * g_img_path/g_mnt_path (:3623-3624), all inherited across the fork
         * unchanged, so its watermark-seed block behaves identically.
         *
         * SIGUSR1 is installed HERE, by main, not by the thread. The handler
         * used to live only in fuse_sweep_thread, and the daemon inherited it
         * solely because that thread was scheduled before fuse_daemonize
         * forked -- a race, and the losing side of the race is a daemon whose
         * SIGUSR1 disposition is the default, i.e. one that DIES on the
         * documented `kill -USR1 $(pidof invf-fuse)`. main is the process that
         * survives the fork, so main is what owns the handler. After
         * fuse_set_signal_handlers, so libfuse cannot overwrite it; and
         * idempotent with the thread's own signal() at the top of its body,
         * which stays.
         *
         * If the thread cannot be created the daemon says so, loudly, rather
         * than accepting USR1 forever: without a worker g_sweep_now is a flag
         * nobody reads, and that is exactly the failure this placement fixes. */
        signal(SIGUSR1, on_sweep_signal);
        {
            pthread_t tid;
            if (pthread_create(&tid, NULL, fuse_sweep_thread, NULL) == 0) {
                pthread_detach(tid);
            } else {
                fprintf(stderr,
                        "invf: FATAL: cannot start the background sweep "
                        "thread. `kill -USR1` and the user.invfs.sweep xattr "
                        "would be accepted and do nothing, the raw_watermark "
                        "ladder would never run, and no sweep pass could arm "
                        "a rollback window. Unmount; a daemon without this "
                        "thread is a daemon with no background reclaim.\n");
                fuse_remove_signal_handlers(se);
                fuse_unmount(f);
                fuse_destroy(f);
                if (g_vol) { g_shutdown = 1; vol_close(g_vol); g_vol = NULL; }
                return 1;
            }
        }

        /* WP17: multithreaded loop. When this was written the claim was that
         * the core has NO internal locks, so all engine/table state stays
         * serialized by the single g_io_lock -- "every op already takes it".
         * BOTH halves of that are false now, and finding out cost three
         * silent-corruption bugs: the core DOES carry its own leaf mutexes
         * (arc.mu, heat.mu, cpacks_mu), and g_io_lock does NOT cover every
         * op -- invf_read releases it and THEN calls vol_read_range (:1407,
         * :1423), under the comment above. So "g_io_lock serializes the
         * engine" is not a safety argument for a new structure; the lock-free
         * read path is the counterexample and it is the busiest one. mt then
         * overlaps kernel<->daemon IPC (request dispatch, splice copies)
         * with the locked engine work.
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
