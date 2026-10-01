/*
 * otrunc_test.c -- "an O_TRUNC that could not read the file's identity
 * replaced it with 0644 root".
 *
 * THE DEFECT
 * ----------
 * invf_open's O_TRUNC branch (src/cli/fuse_fs.c) saves the file's POSIX
 * identity, truncates, and puts the identity back:
 *
 *     uint64_t ino = vol_find(g_vol, path + 1);
 *     if (ino && vol_get_meta(g_vol, ino, &keep) == 0)
 *         have_keep = 1;
 *     vol_replace_file(g_vol, path + 1, NULL, 0);   <-- return IGNORED
 *     if (have_keep) vol_apply_meta(g_vol, path + 1, &keep);
 *
 * vol_find returns a uint64_t, so 0 means both "no such name" and "the
 * lookup could not be completed" (src/core/vol_ast.c, the comment above
 * vol_find_rc). A failed lookup therefore drops the saved copy -- and then
 * the IDENTITY OF THE FILE IS DECIDED BY A READ THAT ALSO DID NOT
 * COMPLETE, inside vol_v3_create_node (src/core/vol_dirs.c).
 *
 * WHAT THAT SECOND READ COSTS, MEASURED
 * -------------------------------------
 * It is not the memset that does the damage, and that is what makes the
 * mode half of this LIVE rather than latent. INVFS_ITYP_REG is 0
 * (src/core/invarifs.h:1147) and the defaulting branch reads
 *
 *     } else if (in.type == 0) {            <-- 0 == INVFS_ITYP_REG
 *         in.mode = 0644;
 *
 * (src/core/vol_dirs.c:324-329) -- the guard CANNOT be false for a regular
 * file, so it is taken whether the row was read or zeroed, and
 * `vol_replace_file(..., NULL, 0)` (meta == NULL) rewrites mode to 0644 on
 * EVERY regular file it truncates. The mode survives only because
 * vol_apply_meta(&keep) runs afterwards. So:
 *
 *   mode  -- ONE failed read is enough, and it is LIVE. Losing the saved
 *            copy loses the mode. Legs 2 and 3 below each lose it with a
 *            single armed site.
 *   owner -- takes TWO. uid/gid are only zeroed by the memset at
 *            src/core/vol_dirs.c:299-300, which needs create_node's OWN
 *            inode read to fail on top of the save already being dropped.
 *            Leg 4 composes the two and moves uid/gid to 0.
 *
 * The POSIX ACL is in neither set: xattrs are separate 0x03 || inode keys,
 * so a file that comes out of this as 0644 root still carries the ACL that
 * was denying the mode it no longer has.
 *
 * The second finding, in the same three lines: vol_replace_file's return
 * value is not examined at all, so a truncate that FAILS leaves the old
 * content in place and open() still returns 0 (leg 5).
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE ERNO
 * --------------------------------------------
 * Every leg reads mode, owner and CONTENT back off the volume (vol_flush,
 * then vol_get_meta_rc / vol_read_file on the engine), never the return
 * code. An errno-only control passes against a fix that renamed the return
 * value and left the defaults being written. The healthy-truncate arm is
 * not optional and is asserted the same way: a truncate must still work,
 * must still CLEAR THE CONTENT, and must still leave the identity alone.
 *
 * HOW THE ROW READS ARE FAILED
 * ----------------------------
 * INVFS_FAULT="<site>:<n>" (src/core/vol_fault.h) -- one-shot, per
 * translation unit. The composed failure needs TWO SITES, because the
 * dirent row read and the inode row read are both in vol_btree.c and one
 * spec names one site, so the schedule below delivers the dirent spec to
 * the first few seam consultations and the inode spec to every one after.
 * The injection itself is entirely vol_fault.h's; the schedule is a
 * delivery mechanism, not a substitute for the seam.
 *
 * THE SCHEDULE
 * ------------
 * invfs_vol_fault re-arms when getenv returns a different POINTER and stays
 * armed when it returns the same one (src/core/vol_fault.h:93), so an
 * interposed getenv() can hand out one spec and then another with no other
 * change. The same spec twice in a row comes from the SAME buffer on
 * purpose: rotating it would re-arm on every call and turn the one-shot
 * into a permanent failure.
 *
 * Two things this deliberately does NOT rely on:
 *   - unsetenv()+setenv() is not a re-arm. The freed address is handed
 *     back, the pointer compare sees no change, the countdown stays spent,
 *     and the leg quietly measures the healthy path. The schedule avoids
 *     the question by changing the address itself; the plain-spec arms
 *     below still call invfs_vol_btree_fault_reload() /
 *     invfs_vol_dirs_fault_reload() first, because they need it.
 *   - the switch position is NOT a constant. The sites are consulted once
 *     per row read, so the position moves if the read path ever changes.
 *     OT_VICTIM_SEARCH=1 re-derives the map and prints it per position;
 *     OT_VICTIM_SWITCH pins what `make test` uses, and the trace is
 *     printed for the composed leg on every run, so a drift is visible.
 *
 * STALENESS
 * ---------
 * `make -j4` does not build the CLI/test binaries -- they exist only as
 * prerequisites of `make test` -- so a test binary can be older than the
 * code it measures, and a red control that measures a stale binary reports
 * a result about code that is not in the tree. This test refuses to
 * measure anything if its own executable is older than the sources it was
 * built from, and says so loudly.
 *
 * THE SHAPE
 * ---------
 * invf_open is static in fuse_fs.c and libfuse is the only thing that ever
 * calls it, so this file includes fuse_fs.c with main renamed and stubs the
 * libfuse and tmpstore symbols invf-fuse links. It is not modified,
 * stubbed or re-implemented: the assertions are on the shipped code.
 *
 * NOT RUN UNDER $(TESTISO). It calls the entry point directly with
 * fuse_get_context() stubbed NULL, so every permission check takes the
 * documented uid-0 bypass and the only failure injected is the one under
 * test. No real uid denial anywhere, so nothing here can be swallowed by
 * the one-entry fake-root uid map.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, O_TRUNC (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* NOTE: volume_internal.h deliberately does NOT appear here -- see the
 * header comment of src/cli/readdir_error_test.c, which includes
 * fuse_fs.c the same way for the same reason. */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"
#undef main

/* The cross-TU doors onto vol_btree.c's and vol_dirs.c's arming state; see
 * the declaration comment in src/core/vol_fault.h. */
extern void invfs_vol_btree_fault_reload(void);
extern void invfs_vol_dirs_fault_reload(void);

/* ---- libfuse / tmpstore stubs ---------------------------------------- */

struct fuse_context *fuse_get_context(void) { return NULL; }
int  fuse_daemonize(int foreground) { (void)foreground; return 0; }
void fuse_destroy(struct fuse *f) { (void)f; }
void fuse_unmount(struct fuse *f) { (void)f; }
struct fuse_session *fuse_get_session(struct fuse *f) { (void)f; return NULL; }
int  fuse_loop_mt_31(struct fuse *f, int clone_fd) { (void)f; (void)clone_fd; return 0; }
int  fuse_mount(struct fuse *f, const char *mp) { (void)f; (void)mp; return 0; }
struct fuse *_fuse_new_31(struct fuse_args *args, const struct fuse_operations *op,
                           size_t op_size, struct libfuse_version *ver, void *data)
{ (void)args; (void)op; (void)op_size; (void)ver; (void)data; return NULL; }
int  fuse_set_signal_handlers(struct fuse_session *se) { (void)se; return 0; }
void fuse_remove_signal_handlers(struct fuse_session *se) { (void)se; }
int  tmpstore_init(size_t max_bytes, tmp_area_mode mode)
{ (void)max_bytes; (void)mode; return 0; }
void tmpstore_destroy(void) { }

/* ---- refuse to measure a stale binary --------------------------------
 * make -j4 does not link the CLI/test binaries: they exist only as
 * prerequisites of `make test`. So `make -j4 && run the test` has run the
 * test against code that is no longer in the tree, which is the one way a
 * red control proves nothing while still printing a result. Exit non-zero,
 * with both timestamps, rather than reporting. */
static time_t path_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

static time_t newest_in(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    time_t newest = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        char p[1024];
        struct stat st;
        if (n < 3) continue;
        if (strcmp(e->d_name + n - 2, ".c") && strcmp(e->d_name + n - 2, ".h"))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0 && st.st_mtime > newest) newest = st.st_mtime;
    }
    closedir(d);
    return newest;
}

static int check_not_stale(const char *tree)
{
    char self[1024], p[1024];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    time_t bin_mtime, src;
    const char *subs[] = { "src/core", "src/cli" };
    size_t i;

    if (n <= 0) return 0;                 /* cannot tell; do not block */
    self[n] = 0;
    bin_mtime = path_mtime(self);
    src = 0;
    for (i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        time_t t;
        snprintf(p, sizeof p, "%s/%s", tree, subs[i]);
        t = newest_in(p);
        if (t > src) src = t;
    }
    if (bin_mtime >= src) return 0;
    printf("otrunc_test: REFUSING TO MEASURE -- this binary is OLDER than the "
           "sources it was built from.\n"
           "  binary            %s\n    mtime %lld\n"
           "  newest source     mtime %lld (under %s/src)\n"
           "  `make -j4` does NOT build the CLI/test binaries; they are built "
           "only as prerequisites of `make test`.\n"
           "  Rebuild it explicitly: rm -f %s && make %s\n",
           self, (long long)bin_mtime, (long long)src, tree, self, self);
    return 1;
}

/* ---- the schedule: an ordered list of INVFS_FAULT specs ---------------
 *
 * interposed getenv(), faithful for every other name. See the header. */
extern char **environ;

#define OT_POOL      1024
#define OT_SCHED_MAX 64
static char        g_pool[OT_POOL][40];
static int         g_pool_next;
static const char *g_sched[OT_SCHED_MAX];  /* spec per seam consultation */
static int         g_sched_n;              /* 0 = no schedule: pass through */
static int         g_pos;
static int         g_trace;
static char       *g_cur_buf;              /* buffer the current spec lives in */

static char *ot_buf(const char *spec)
{
    char *b;
    /* Same spec as last time -> the SAME buffer. That is the whole point:
     * a fresh address re-arms the one-shot, and a re-armed one-shot is not
     * one-shot. */
    if (g_cur_buf && strcmp(g_cur_buf, spec) == 0)
        return g_cur_buf;
    b = g_pool[g_pool_next++ % OT_POOL];
    snprintf(b, sizeof g_pool[0], "%s", spec);
    g_cur_buf = b;
    return b;
}

char *getenv(const char *name)
{
    if (g_sched_n && name && strcmp(name, "INVFS_FAULT") == 0) {
        /* past the end of the schedule the LAST element is held: that is
         * the second site, spent after its one shot, for the rest of the
         * call. */
        const char *s = g_sched[g_pos < OT_SCHED_MAX ? g_pos : OT_SCHED_MAX - 1];
        g_pos++;
        if (g_trace)
            printf("        [seam] consultation %2d -> %s\n", g_pos, s);
        return ot_buf(s);
    }
    if (name) {
        size_t nl = strlen(name);
        char **e;
        for (e = environ; e && *e; e++)
            if (strncmp(*e, name, nl) == 0 && (*e)[nl] == '=')
                return (char *)(*e + nl + 1);
    }
    return NULL;
}

/* Arm one spec for the whole call (the calibration, single-site and
 * delta-append legs). The reloads are load-bearing, NOT the
 * unsetenv/setenv -- see the header. */
static void arm(const char *spec)
{
    g_sched_n = 0;
    g_cur_buf = NULL;
    invfs_vol_btree_fault_reload();
    invfs_vol_dirs_fault_reload();
    setenv("INVFS_FAULT", spec, 1);
}

/* Arm the composed failure: the dirent row read for the first `switch_at`
 * seam consultations, the inode row read for every one after. */
static void arm_two_sites(int switch_at)
{
    int i;
    g_cur_buf = NULL;
    g_sched_n = switch_at < 1 ? 1 : switch_at;
    for (i = 0; i < g_sched_n; i++)
        g_sched[i] = "v3_dirent_row_read:1";
    for (; i < OT_SCHED_MAX; i++)
        g_sched[i] = "v3_inode_row_read:1";
    g_pos = 0;
    invfs_vol_btree_fault_reload();
    invfs_vol_dirs_fault_reload();
    setenv("INVFS_FAULT", "v3_dirent_row_read:1", 1);
}

static void disarm(void)
{
    g_sched_n = 0;
    g_cur_buf = NULL;
    g_trace = 0;
    g_pos = 0;
    unsetenv("INVFS_FAULT");
}

/* ---- harness ---------------------------------------------------------- */

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  OK    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---- the victim ------------------------------------------------------- */

#define VICTIM_MODE 0640
#define VICTIM_UID  4242u
#define VICTIM_GID  4243u
static const char VICTIM_BODY[] = "the bytes O_TRUNC is supposed to remove";
#define VICTIM_LEN ((size_t)(sizeof VICTIM_BODY - 1))

static uint64_t victim_ino;

/* The oracle: the volume's own answer, after a flush. */
static void off_volume(uint16_t *mode, uint32_t *uid, uint32_t *gid,
                       size_t *len)
{
    invfs_meta_pub m;
    uint8_t *out = NULL;
    size_t olen = 0;

    pthread_mutex_lock(&g_io_lock);
    vol_flush(g_vol);
    if (vol_get_meta_rc(g_vol, victim_ino, &m) == 0) {
        *mode = m.mode; *uid = m.uid; *gid = m.gid;
    } else {
        *mode = 0; *uid = 0; *gid = 0;
    }
    if (vol_read_file(g_vol, victim_ino, &out, &olen) == 0) {
        *len = olen;
        free(out);
    } else {
        *len = (size_t)-1;
    }
    pthread_mutex_unlock(&g_io_lock);
}

static const char *body_state(size_t len)
{
    if (len == 0) return "TRUNCATED";
    if (len == (size_t)-1) return "unreadable";
    if (len == VICTIM_LEN) return "the ORIGINAL body";
    return "SOMETHING ELSE";
}

static void show_volume(const char *what)
{
    uint16_t mode; uint32_t uid, gid; size_t len;
    off_volume(&mode, &uid, &gid, &len);
    printf("        %s: volume reads mode 0%o uid %u gid %u, %zd content "
           "bytes (%s)\n", what, mode, uid, gid,
           len == (size_t)-1 ? (ssize_t)-1 : (ssize_t)len, body_state(len));
}

/* The load-bearing assertion. A truncate that cannot preserve the file's
 * identity must not truncate: the file has to be byte-for-byte and
 * field-for-field what it was, INCLUDING its content, because the caller
 * was told the open failed. */
static void assert_untouched(const char *what)
{
    uint16_t mode; uint32_t uid, gid; size_t len;
    off_volume(&mode, &uid, &gid, &len);
    checks++;
    if (mode == VICTIM_MODE && uid == VICTIM_UID && gid == VICTIM_GID &&
        len == VICTIM_LEN) {
        printf("  OK    %s -- volume still 0%o uid %u gid %u and the "
               "original %zd bytes\n",
               what, mode, uid, gid, (ssize_t)len);
    } else {
        failures++;
        printf("  FAIL  %s -- THE VOLUME CHANGED: it reads mode 0%o uid %u "
               "gid %u, %zd content bytes (%s); it was 0%o uid %u gid %u and "
               "%zd\n", what, mode, uid, gid,
               len == (size_t)-1 ? (ssize_t)-1 : (ssize_t)len, body_state(len),
               VICTIM_MODE, VICTIM_UID, VICTIM_GID, (ssize_t)VICTIM_LEN);
    }
}

/* put the victim back exactly as it started, content included */
static void reset_victim(void)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = VICTIM_MODE;
    m.uid = VICTIM_UID;
    m.gid = VICTIM_GID;
    m.nlink = 1;
    pthread_mutex_lock(&g_io_lock);
    if (vol_replace_file(g_vol, "victim", (const uint8_t *)VICTIM_BODY,
                         VICTIM_LEN) == 0) {
        printf("  SETUP BROKEN: cannot restore the victim's content\n");
        failures++;
    }
    victim_ino = vol_apply_meta(g_vol, "victim", &m);
    vol_flush(g_vol);
    /* Rebuild the daemon's file table from a HEALTHY volume, so the first
     * seam consultation inside invf_open is the lookup the O_TRUNC branch
     * does and not the listing snapshot_entry() does when the table is
     * stale. Left stale, the listing consumes the fault, the table is
     * marked degraded, and open() returns -EIO at the WP135 check long
     * before the truncate -- a leg that measures neither. */
    g_table_stale = 1;
    table_rebuild_locked();
    pthread_mutex_unlock(&g_io_lock);
    if (!victim_ino) {
        printf("  SETUP BROKEN: cannot restore the victim's identity\n");
        failures++;
    }
}

/* The call under test. fuse_get_context() is NULL, so the permission gate
 * takes the documented uid-0 bypass and nothing here depends on a real uid
 * denial. */
static int do_trunc_open(void)
{
    struct fuse_file_info fi;
    int rc;
    memset(&fi, 0, sizeof fi);
    fi.flags = O_WRONLY | O_TRUNC;
    rc = invf_open("/victim", &fi);
    if (rc == 0 && fi.fh)
        invf_release("/victim", &fi);   /* the handle a success owes */
    return rc;
}

static void report_open(int rc)
{
    note("        (open returned %d = %s)\n", rc,
         rc == -EIO ? "EIO" : rc == 0 ? "0 -- REPORTED SUCCESS"
         : rc == -ENOSPC ? "ENOSPC" : "another error");
}

/*
 * The switch position between the two sites, pinned because it is a
 * property of the CURRENT read path and not of the property under test.
 * OT_VICTIM_SEARCH=1 re-derives the map and prints it per position. The
 * boundary matters: below it the two failures compose (owner AND mode
 * move), at or above it only the saved copy is lost (mode moves, the
 * owner survives).
 */
#ifndef OT_VICTIM_SWITCH
#define OT_VICTIM_SWITCH 3
#endif

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    const char *tree = getenv("PWD");
    const char *search;
    int do_search;
    char img[512], cmd[1024];
    invfs_meta_pub meta;
    invfs_volume *v;
    uint64_t ino;
    int err = 0, rc;

    if (!tree) tree = ".";
    if (check_not_stale(tree)) return 2;

    search = getenv("OT_VICTIM_SEARCH");
    do_search = search && *search && strcmp(search, "0") != 0;

    printf("otrunc_test: an O_TRUNC that cannot read the file's identity must "
           "not replace it with 0644 root\n");

    snprintf(img, sizeof img, "%s/invf-otrunc-test.img", dir);
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null", tree, img);
    if (system(cmd) != 0) { printf("  cannot create volume\n"); return 2; }
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "vol_open failed err=%d\n", err); return 2; }
    g_vol = v;

    memset(&meta, 0, sizeof meta);
    meta.type = INVFS_ITYP_REG;
    meta.mode = VICTIM_MODE;
    meta.uid = VICTIM_UID;
    meta.gid = VICTIM_GID;
    meta.nlink = 1;
    victim_ino = vol_create_file_with_meta(v, "victim",
                                           (const uint8_t *)VICTIM_BODY,
                                           VICTIM_LEN, &meta);
    if (!victim_ino) { printf("  cannot create the victim\n"); return 2; }
    vol_flush(v);

    if (do_search) {
        int s, clobbered = 0, tried = 0;
        printf("\n  -- OT_VICTIM_SEARCH: the per-position map --\n"
               "  (the read order inside invf_open is: the lookup's dirent\n"
               "   read, then vol_get_meta's inode read, then create_node's\n"
               "   dirent read, then create_node's inode read. Each inode\n"
               "   read consults the seam TWICE -- vol_btree.c:3757 and\n"
               "   :3777 -- so the positions are not one-per-read.)\n");
        for (s = 1; s <= 10; s++) {
            uint16_t mode; uint32_t uid, gid; size_t len;
            reset_victim();
            arm_two_sites(s);
            rc = do_trunc_open();
            disarm();
            off_volume(&mode, &uid, &gid, &len);
            tried++;
            if (mode == 0644 && uid == 0 && gid == 0) clobbered++;
            printf("  switch@%-2d open=%-4d mode 0%-4o uid %-5u gid %-5u "
                   "content %zd (%s)%s\n", s, rc, mode, uid, gid,
                   len == (size_t)-1 ? (ssize_t)-1 : (ssize_t)len,
                   body_state(len),
                   (mode == 0644 && uid == 0 && gid == 0)
                     ? "   <== OWNER AND MODE CLOBBERED" : "");
        }
        /* at-least-one AND not-all: a map in which every position behaves
         * alike would prove nothing about WHICH read decides the
         * identity. */
        ok(clobbered > 0,
           "at least one switch position composes the failure (owner AND "
           "mode both move)");
        ok(clobbered < tried,
           "and not every position does -- the position decides which read "
           "fails, so the map is not flat");
        vol_close(v);
        unlink(img);
        return failures ? 1 : 0;
    }

    /* ---- 0. the green control: the volume is what we put there ------- */
    {
        uint16_t mode; uint32_t uid, gid; size_t len;
        off_volume(&mode, &uid, &gid, &len);
        ok(mode == VICTIM_MODE && uid == VICTIM_UID && gid == VICTIM_GID &&
           len == VICTIM_LEN, "the volume starts 0640 uid 4242 gid 4243 with "
           "the original body");
    }

    /* ---- 0b. CALIBRATION: the seams are live in this build -----------
     * Not assertions about the fix -- they are the reason the legs below
     * cannot go green by the fault quietly not arming. */
    ino = vol_find(g_vol, "victim");
    ok(ino == victim_ino,
       "the victim resolves to its inode on a healthy read");
    arm("v3_dirent_row_read:1");
    ino = vol_find(g_vol, "victim");
    ok(ino == 0, "the dirent row-read seam fires: an armed lookup resolves to "
        "0 while the file is there (0 == \"absent\" AND \"unreadable\")");
    disarm();
    arm("v3_inode_row_read:1");
    rc = vol_get_meta_rc(g_vol, victim_ino, &meta);
    ok(rc < 0, "the inode row-read seam fires: an armed read of the row "
        "reports an error");
    disarm();

    /* ---- 1. THE GREEN CONTROL: a healthy truncate still works --------
     * Not optional. A "fix" that refused every truncate would pass every
     * leg below; this requires the truncate to SUCCEED, to CLEAR THE
     * CONTENT, and to leave the identity alone. */
    reset_victim();
    rc = do_trunc_open();
    show_volume("healthy O_TRUNC");
    ok(rc == 0, "a healthy open(O_TRUNC) succeeds");
    checks++;
    {
        uint16_t mode; uint32_t uid, gid; size_t len;
        off_volume(&mode, &uid, &gid, &len);
        if (rc == 0 && len == 0 && mode == VICTIM_MODE && uid == VICTIM_UID &&
            gid == VICTIM_GID) {
            printf("  OK    ... and it CLEARED the content while keeping the "
                   "identity (mode 0%o uid %u gid %u, 0 bytes)\n",
                   mode, uid, gid);
        } else {
            failures++;
            printf("  FAIL  ... a healthy truncate did not do its job: open "
                   "returned %d, volume reads mode 0%o uid %u gid %u with %zd "
                   "content bytes (%s)\n", rc, mode, uid, gid,
                   len == (size_t)-1 ? (ssize_t)-1 : (ssize_t)len,
                   body_state(len));
        }
    }

    /* ---- 2. ONE failed read: the lookup's DIRENT row -----------------
     * have_keep drops. create_node's own inode read is healthy, so the
     * mode and owner it finds are the real ones -- and the mode still
     * moves, because vol_replace_file(..., NULL, 0) defaults it to 0644 on
     * every REG (src/core/vol_dirs.c:324). One failure, mode gone. */
    reset_victim();
    arm("v3_dirent_row_read:1");
    rc = do_trunc_open();
    disarm();
    report_open(rc);
    ok(rc != 0, "a truncate whose identity lookup failed is not reported as "
       "performed");
    assert_untouched("one failed read (the lookup's dirent row)");

    /* ---- 3. ONE failed read: the metadata row ------------------------
     * The other half of the same single failure: the saved copy is dropped
     * because the read that produced it failed, and the truncate still
     * resets the mode. */
    reset_victim();
    arm("v3_inode_row_read:1");
    rc = do_trunc_open();
    disarm();
    report_open(rc);
    ok(rc != 0, "a truncate whose metadata row could not be read is not "
       "reported as performed");
    assert_untouched("one failed read (the metadata row)");

    /* ---- 4. THE RED: BOTH reads fail --------------------------------
     * Composed, at the pinned switch position. The saved copy is dropped
     * (the lookup failed) and the identity is then decided by a read that
     * ALSO failed -- so vol_v3_create_node memsets the row and the owner
     * goes to root as well. This is the leg the two-failure shape belongs
     * to, and the only one that moves uid/gid. */
    reset_victim();
    g_trace = 1;
    arm_two_sites(OT_VICTIM_SWITCH);
    rc = do_trunc_open();
    g_trace = 0;
    disarm();
    report_open(rc);
    ok(rc != 0, "a truncate whose identity could not be read twice over is "
       "not reported as performed");
    assert_untouched("two failed reads: the owner as well as the mode");

    /* ---- 5. THE SECOND FINDING: the ignored return ------------------
     * (added with its own fix; see the commit that carries it) */

    reset_victim();
    vol_close(v);
    unlink(img);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
