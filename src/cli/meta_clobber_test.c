/*
 * meta_clobber_test.c — "a chmod reported success and reset the file to
 * 0644 root".
 *
 * THE DEFECT
 * ----------
 * vol_get_meta returned -1 for BOTH "this inode has no meta row" and "the
 * row could not be read" (src/core/vol_records.c, the flattening is still
 * visible in the wrapper). meta_for_path (src/cli/fuse_fs.c) turned that -1
 * into meta_defaults() -- mode 0644, uid 0, gid 0 -- and returned SUCCESS.
 * meta_apply_patch then merged the caller's patch over those defaults and
 * wrote the WHOLE record back with vol_apply_meta.
 *
 * So a chmod / utimens / chown on an inode whose metadata row would not read
 * silently reset its mode to 0644 and its owner to root, and returned 0, so
 * the kernel recorded the chmod as done. That is the shape this whole family
 * has: a question whose failure cannot be told from a legitimate "no",
 * answered with a default, and the default PERSISTED. The volume is changed
 * by a call that reported success.
 *
 * WHY -1 WAS NOT ENOUGH
 * ----------------------
 * The two cases have to diverge, and only one of them is damage:
 *
 *   rc == 0 ("no such inode row") is NORMAL. volume.c sets v3_mbuf_ready
 *   unconditionally on open, so v3_ready() never refuses, and on a volume
 *   whose records predate the v3 inode row -- every pre-v3 volume -- the row
 *   will never be found. Falling back to type defaults there is the
 *   documented v1->v2 upgrade path, and letting the patch apply on top is
 *   correct: there is no recorded mode to lose.
 *
 *   rc < 0 ("the row could not be read") is DAMAGE. The recorded mode and
 *   owner are exactly what could not be read, and 0644/root is not a guess
 *   at them, it is a different inode.
 *
 * So a fix that simply refused every missing-meta case would pass every
 * clobber leg below and still be wrong. LEG 6 is the control on the fix for
 * that: it deletes the inode row out from under a live dirent, so the
 * metadata really is absent, and the chmod must still be applied.
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE ERNO
 * --------------------------------------------
 * The primary assertion is on the VOLUME. A control that only checked for
 * -EIO would go green against a "fix" that renamed the return value and left
 * meta_apply_patch writing the defaults anyway -- which is the defect, and
 * which is silent. So every clobber leg reads the mode and the owner back
 * OFF THE VOLUME (vol_get_meta_rc on the engine, after vol_flush) and
 * requires them to be the values they were before the call. The errno is
 * asserted too, but only after the bytes, and never instead of them.
 *
 * HOW THE ROW READ IS FAILED
 * --------------------------
 * INVFS_FAULT="v3_inode_row_read:<n>" (src/core/vol_fault.h), a one-shot
 * site that stands in for the row read failing and injects the same -1 the
 * real failure produces (a quarantined base page makes bt_read fail,
 * btree_search return -1, and v3_base_get pass it through).
 *
 * THE COUNT IS n, NOT 1, AND THAT IS LOAD-BEARING. invf_chmod and
 * invf_chown each read the metadata row TWICE -- once for the
 * ownership/permission test, once inside meta_apply_patch for the write-back
 * -- so the read that decides what gets written is the SECOND one. Arming
 * :1 would fail the harmless first read, leave the write-back read healthy,
 * and this test would go GREEN measuring the correct path. That is the same
 * trap as the unsetenv+setenv one documented in vol_fault.h, one level up.
 *
 * The reload is likewise not optional: the arming state is per-translation-
 * unit static and compared by POINTER, so re-setting an identical spec sees
 * no change and the countdown stays spent. invfs_vol_btree_fault_reload()
 * is the exported door onto vol_btree.c's copy of that state.
 *
 * THE SHAPE
 * ---------
 * invf_chmod / invf_chown / invf_utimens and meta_apply_patch are static in
 * fuse_fs.c and libfuse is the only thing that ever calls them, so this file
 * includes fuse_fs.c with main renamed and stubs the libfuse and tmpstore
 * symbols invf-fuse links. None of them is modified, stubbed or
 * re-implemented: the assertions are on the shipped code.
 *
 * NOT RUN UNDER $(TESTISO). This test never asks the kernel for a
 * permission; it runs the entry points directly with fuse_get_context()
 * stubbed to NULL, so every permission check takes the documented uid-0
 * bypass and the only failure being injected is the one under test. There is
 * no real uid denial anywhere in it, and therefore nothing that the
 * one-entry fake-root uid map could turn into a vacuous pass.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

/* NOTE: volume_internal.h deliberately does NOT appear here -- see the
 * header comment of src/cli/readdir_error_test.c, which includes
 * fuse_fs.c the same way for the same reason. */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"
#undef main

/* The cross-TU door onto vol_btree.c's arming state; see arm() below and
 * the declaration comment in src/core/vol_fault.h. This test is a different
 * translation unit from the site, so it cannot reload that state by hand. */
extern void invfs_vol_btree_fault_reload(void);

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

/* ---- harness ---------------------------------------------------------- */

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* Re-arm the one-shot site.
 *
 * The reload is the load-bearing part, NOT the unsetenv/setenv. The arming
 * state is per-translation-unit static and is compared by POINTER
 * (src/core/vol_fault.h), so unsetenv() frees the old spec string and
 * setenv() with the same value very often gets the SAME address back -- the
 * engine then sees no change, the countdown stays spent, and the leg quietly
 * measures the healthy path. That is how a red control ends up proving
 * nothing, so the reload has to be explicit. invfs_vol_btree_fault_reload()
 * is the exported door onto vol_btree.c's copy of that state, because this
 * test is a different translation unit and cannot reach it by hand. */
static void arm(const char *spec)
{
    invfs_vol_btree_fault_reload();
    setenv("INVFS_FAULT", spec, 1);
}

/* Which call of the pair is the write-back read; see the header. */
static void arm_meta_row(int n) { char b[64]; snprintf(b, sizeof b, "v3_inode_row_read:%d", n); arm(b); }

/* ---- the oracle: read mode/owner back OFF THE VOLUME ------------------ */

#define VICTIM_MODE 0640
#define VICTIM_UID  4242u
#define VICTIM_GID  4243u

static invfs_volume *vol;
static uint64_t victim_ino;
static const char *victim = "/victim";

/* Read the record straight out of the engine. This is the volume's own
 * answer, not the daemon's cached view of it: vol_flush first, so nothing
 * here can be answered out of a page that was never persisted. */
static int off_volume(uint16_t *mode, uint32_t *uid, uint32_t *gid)
{
    invfs_meta_pub m;
    int rc;
    pthread_mutex_lock(&g_io_lock);
    vol_flush(g_vol);
    rc = vol_get_meta_rc(g_vol, victim_ino, &m);
    pthread_mutex_unlock(&g_io_lock);
    if (rc != 0) return rc;
    *mode = m.mode;
    *uid  = m.uid;
    *gid  = m.gid;
    return 0;
}

/* The one assertion every clobber leg makes. A wrong errno with the volume
 * intact is a lesser bug; a right errno with the volume rewritten is the
 * defect, and this is what catches it. */
static void assert_volume_untouched(const char *what)
{
    uint16_t mode = 0; uint32_t uid = 0, gid = 0;
    if (off_volume(&mode, &uid, &gid) != 0) {
        checks++; failures++;
        printf("  FAIL  %s: the record cannot be read back off the volume at "
               "all\n", what);
        return;
    }
    checks++;
    if (mode == VICTIM_MODE && uid == VICTIM_UID && gid == VICTIM_GID) {
        printf("  OK    %s -- volume still 0%o uid %u gid %u\n",
               what, mode, uid, gid);
        return;
    }
    failures++;
    printf("  FAIL  %s -- THE VOLUME CHANGED: it reads 0%o uid %u gid %u, "
           "was 0%o uid %u gid %u\n",
           what, mode, uid, gid, VICTIM_MODE, VICTIM_UID, VICTIM_GID);
}

static void reset_victim(void)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = VICTIM_MODE;
    m.uid  = VICTIM_UID;
    m.gid  = VICTIM_GID;
    m.nlink = 1;
    pthread_mutex_lock(&g_io_lock);
    vol_apply_meta(g_vol, victim + 1, &m);
    vol_flush(g_vol);
    pthread_mutex_unlock(&g_io_lock);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char img[512], cmd[1024];
    invfs_meta_pub meta;
    struct timespec tv[2];
    uint16_t mode; uint32_t uid, gid;
    invfs_volume *v;
    int err = 0, rc;

    printf("meta_clobber_test: a chmod that cannot read the metadata row must "
           "not rewrite it from defaults\n");

    snprintf(img, sizeof img, "%s/invf-meta-clobber-test.img", dir);
    unlink(img);

    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
             getenv("PWD") ? getenv("PWD") : ".", img);
    if (system(cmd) != 0) {
        printf("  cannot create volume with invf-mkfs\n");
        return 2;
    }
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "meta_clobber_test: vol_open(%s) failed: err=%d\n", img, err);
        return 2;
    }
    g_vol = v;
    vol = v;

    memset(&meta, 0, sizeof meta);
    meta.type  = INVFS_ITYP_REG;
    meta.mode  = VICTIM_MODE;
    meta.uid   = VICTIM_UID;
    meta.gid   = VICTIM_GID;
    meta.nlink = 1;
    victim_ino = vol_create_file_with_meta(v, victim + 1, NULL, 0, &meta);
    if (!victim_ino) { printf("  cannot create the victim\n"); return 2; }
    vol_flush(v);

    /* The daemon's file table is refreshed lazily (fuse_fs.c:326). The setup
     * write above went through the engine directly, not through an entry
     * point that marks it stale, so do what every create/unlink in
     * fuse_fs.c does. Without this meta_for_path answers "no such file" and
     * every leg below would be measuring ENOENT instead. */
    pthread_mutex_lock(&g_io_lock);
    g_table_stale = 1;
    pthread_mutex_unlock(&g_io_lock);

    /* ---- 0. the green control: the row is what we put there ---------- */
    rc = off_volume(&mode, &uid, &gid);
    ok(rc == 0 && mode == VICTIM_MODE && uid == VICTIM_UID && gid == VICTIM_GID,
       "the volume starts at mode 0640 uid 4242 gid 4243");
    if (rc != 0 || mode != VICTIM_MODE)
        { printf("  setup is wrong, every leg below would be meaningless\n"); return 2; }

    /* ---- 1. chmod on a readable row applies, and is visible ---------- */
    rc = invf_chmod(victim, 0600, NULL);
    off_volume(&mode, &uid, &gid);
    ok(rc == 0 && mode == 0600, "a chmod whose row is readable applies "
        "(volume now 0600)");
    if (rc == 0 && mode != 0600)
        printf("        ^ the chmod reported success but the volume reads 0%o\n", mode);
    reset_victim();

    /* ---- 2. THE RED: chmod with an unreadable row -------------------
     * The write-back read is the SECOND vol_v3_inode_get of the call (the
     * first is the ownership test), so the fault is armed on it. Before the
     * fix this returned 0 and left the volume at 0644/root. */
    arm_meta_row(2);
    rc = invf_chmod(victim, 0600, NULL);
    note("        (chmod returned %d = %s)\n", rc,
         rc == -EIO ? "EIO" : rc == 0 ? "0 -- REPORTED APPLIED" : "another error");
    ok(rc != 0, "a chmod that could not read the row is not reported as applied");
    checks++;   /* the volume assertion below is the load-bearing one */
    assert_volume_untouched("a chmod with an unreadable row");
    unsetenv("INVFS_FAULT");

    /* ---- 3. the same read failure through chown ---------------------- */
    arm_meta_row(2);
    rc = invf_chown(victim, 0, 0, NULL);
    note("        (chown returned %d = %s)\n", rc,
         rc == -EIO ? "EIO" : rc == 0 ? "0 -- REPORTED APPLIED" : "another error");
    ok(rc != 0, "a chown that could not read the row is not reported as applied");
    checks++;
    assert_volume_untouched("a chown with an unreadable row");
    unsetenv("INVFS_FAULT");

    /* ---- 4. and through utimens -------------------------------------- */
    tv[0].tv_sec = 1000000; tv[0].tv_nsec = 0;
    tv[1].tv_sec = 2000000; tv[1].tv_nsec = 0;
    arm_meta_row(1);      /* utimens reads the row exactly once */
    rc = invf_utimens(victim, tv, NULL);
    note("        (utimens returned %d = %s)\n", rc,
         rc == -EIO ? "EIO" : rc == 0 ? "0 -- REPORTED APPLIED" : "another error");
    ok(rc != 0, "a utimens that could not read the row is not reported as applied");
    checks++;
    assert_volume_untouched("a utimens with an unreadable row");
    unsetenv("INVFS_FAULT");

    /* ---- 5. and the inode is not left half-modified ------------------
     * Same call, but asking for a change that does not touch mode or owner,
     * so this isolates the TIMESTAMPS from the two fields the defaults
     * carry. A default-filled record has mtime = now; if the write-back had
     * happened, the volume would carry a fresh mtime instead of the
     * pre-set one. */
    {
        invfs_meta_pub m;
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_REG; m.mode = VICTIM_MODE;
        m.uid = VICTIM_UID; m.gid = VICTIM_GID; m.nlink = 1;
        m.mtime = 1111111; m.atime = 1111111;
        pthread_mutex_lock(&g_io_lock);
        vol_apply_meta(g_vol, victim + 1, &m);
        vol_flush(g_vol);
        pthread_mutex_unlock(&g_io_lock);
    }
    arm_meta_row(1);
    rc = invf_utimens(victim, tv, NULL);
    unsetenv("INVFS_FAULT");
    {
        invfs_meta_pub m;
        int grc;
        pthread_mutex_lock(&g_io_lock);
        vol_flush(g_vol);
        grc = vol_get_meta_rc(g_vol, victim_ino, &m);
        pthread_mutex_unlock(&g_io_lock);
        ok(rc != 0 && grc == 0 && m.mtime == 1111111,
           "a refused utimens leaves the recorded mtime alone too "
           "(still 1111111, not the wall clock)");
        if (grc == 0 && m.mtime != 1111111)
            printf("        ^ the recorded mtime is now %lld: a "
                   "default-filled record was written back\n",
                   (long long)m.mtime);
    }
    reset_victim();

    /* ---- 6. THE CONTROL ON THE FIX: a genuinely ABSENT row still gets
     * the defaults, and the chmod still applies.
     *
     * This is the leg a "just fail closed everywhere" fix fails. rc == 0
     * from vol_v3_inode_get is not damage: it is a record with no metadata
     * row, which is every pre-v3 volume, and the v1->v2 upgrade path is
     * built on answering it with type defaults and letting the patch apply
     * on top. Deleting the inode row out from under a live dirent
     * constructs exactly that state on a v3 image. */
    {
        uint64_t other = vol_create_file_with_meta(v, "legacy", NULL, 0, &meta);
        pthread_mutex_lock(&g_io_lock);
        vol_v3_inode_delete(g_vol, other);
        vol_flush(g_vol);
        g_table_stale = 1;
        pthread_mutex_unlock(&g_io_lock);
        if (!other) {
            printf("  cannot construct the absent-row case\n");
            return 2;
        }
        rc = invf_chmod("/legacy", 0700, NULL);
        note("        (chmod on a record with no meta row returned %d)\n", rc);
        ok(rc == 0, "a chmod on a record with NO meta row still applies "
                    "(the v1->v2 upgrade path must keep working)");
        {
            invfs_meta_pub m;
            uint64_t ino2;
            int frc = vol_find_rc(g_vol, "legacy", &ino2);
            ok(frc == 1, "and the record is still there afterwards");
            if (frc == 1) {
                /* it now HAS a row, because the upgrade path wrote one */
                ok(vol_get_meta_rc(g_vol, ino2, &m) == 0 && m.mode == 0700,
                   "the upgrade path stamped the new mode on the new row");
                if (vol_get_meta_rc(g_vol, ino2, &m) == 0 && m.mode != 0700)
                    printf("        ^ the row reads 0%o\n", m.mode);
            }
        }
    }

    /* ---- 7. and the healthy answers are all still the healthy ones --- */
    off_volume(&mode, &uid, &gid);
    ok(mode == VICTIM_MODE && uid == VICTIM_UID && gid == VICTIM_GID,
       "after every fault, /victim is back to mode 0640 uid 4242 gid 4243");
    {
        /* vol_get_meta's 0/-1 shape is unchanged for its ~100 callers, and
         * vol_get_meta_rc is the one that tells the two apart. */
        invfs_meta_pub m;
        ok(vol_get_meta_rc(g_vol, victim_ino, &m) == 0,
           "a readable row is 0 from vol_get_meta_rc");
        ok(vol_get_meta_rc(g_vol, 0xdeadbeefULL, &m) == -ENOENT,
           "an absent row is -ENOENT, not -EIO and not a default");
        ok(vol_get_meta(v, victim_ino, &m) == 0,
           "vol_get_meta still answers 0 for its existing callers");
        ok(vol_get_meta(v, 0xdeadbeefULL, &m) == -1,
           "vol_get_meta still answers -1 for its existing callers");
    }

    /* ---- 8. and the permission side of the same read -----------------
     * perm_check_cred reads the SAME row, and the same default-fill used to
     * put mode 0644 / owner root into a permission decision. A 0600 file
     * owned by uid 1000 must still refuse uid 1000 when the row cannot be
     * read -- and must still be governed by its real mode once it can. */
    {
        struct acreds c;
        memset(&c, 0, sizeof c);
        c.uid = 1000; c.gid = 1000; c.ngr = 0; c.bypass = 0;
        {
            invfs_meta_pub pm;
            memset(&pm, 0, sizeof pm);
            pm.type = INVFS_ITYP_REG; pm.mode = 0600;
            /* owned by uid 2000, NOT by the caller: 0600 on a file you own
             * correctly permits you, and this leg is about the OTHER bits
             * being consulted. The default-fill answers 0644/root, and 0644
             * permits uid 1000 -- which is exactly the fail-open. */
            pm.uid = 2000; pm.gid = 2000; pm.nlink = 1;
            /* a second file the setup does not touch later */
            {
                uint64_t p = vol_create_file_with_meta(v, "private", NULL, 0, &pm);
                pthread_mutex_lock(&g_io_lock);
                vol_flush(g_vol);
                g_table_stale = 1;
                pthread_mutex_unlock(&g_io_lock);
                if (!p) { printf("  cannot create /private\n"); return 2; }
            }
        }
        rc = perm_check_cred(&c, "/private", R_OK);
        ok(rc == -EACCES, "uid 1000 is denied on another user's 0600 file "
            "(the real mode is what governs)");
        if (rc != -EACCES)
            printf("        ^ returned %d, so the mode is not being read\n", rc);
        arm_meta_row(1);
        rc = perm_check_cred(&c, "/private", R_OK);
        ok(rc != 0, "and an unreadable row does not ALLOW it either");
        if (rc == 0)
            printf("        ^ THE FAIL-OPEN: the decision was made against "
                   "0644/root instead of the file's real mode\n");
        else
            note("        (refused with %s)\n",
                 rc == -EIO ? "-EIO" : rc == -EACCES ? "-EACCES" : "another error");
        checks++;
        unsetenv("INVFS_FAULT");
        rc = perm_check_cred(&c, "/private", R_OK);
        ok(rc == -EACCES, "and the real mode governs again once the fault is spent");
    }

    /* ---- 9. RENAME_NOREPLACE: a lookup that could not be done is not an
     * absent destination.
     *
     * The sibling shape. vol_find returns a uint64_t whose "no such name"
     * and "the lookup failed" are both the single value 0, and the
     * RENAME_NOREPLACE guard read `vol_find(g_vol, to + 1) != 0` -- so an
     * unreadable DIRENT row read as "the destination is not there", the
     * guard did not fire, and vol_rename overwrote a destination the flag
     * was explicitly told to protect. The call reported success.
     *
     * v3_dirent_row_read is the xattr WP's site, reused here because it is
     * the same seam: it injects the -1 that bt_read makes btree_search
     * return. arm_meta_row would fail the wrong read -- the guard resolves
     * the DESTINATION NAME, not an inode row.
     *
     * The count is 3 because that is where the guard's lookup falls under
     * this seam: invf_rename reads the dirent once for vol_is_dir(from) and
     * once for vol_is_dir(to) before it reaches vol_find_rc(to). The
     * assertion below is -EIO rather than "not zero" precisely so that if
     * that arithmetic ever drifts, the leg goes RED instead of quietly
     * measuring the healthy guard. */
    {
        static const char payload[] = "destination content that must survive";
        /* "src" is a file, "dst" is a file holding different bytes. */
        if (!vol_create_file_with_meta(v, "dst", (const uint8_t *)payload,
                                       sizeof payload - 1, &meta)) {
            printf("  cannot create /dst\n");
            return 2;
        }
        vol_create_file_with_meta(v, "src", (const uint8_t *)"s", 1, &meta);
        vol_flush(v);
        pthread_mutex_lock(&g_io_lock);
        g_table_stale = 1;
        pthread_mutex_unlock(&g_io_lock);

        /* the green control: the flag is honoured on a healthy lookup */
        rc = invf_rename("/src", "/dst", RENAME_NOREPLACE);
        ok(rc == -EEXIST, "RENAME_NOREPLACE reports EEXIST when the "
            "destination really is there");
        {
            uint64_t st_ino = 0;
            ok(vol_find_rc(g_vol, "dst", &st_ino) == 1,
               "and the destination is still there afterwards");
        }

        /* THE RED: the guard's own lookup fails. Before the fix this
         * returned 0 and /dst was overwritten; after it, the rename is
         * refused and /dst keeps its bytes. */
        {
            uint64_t dino = 0;
            arm("v3_dirent_row_read:3");
            rc = invf_rename("/src", "/dst", RENAME_NOREPLACE);
            unsetenv("INVFS_FAULT");
            note("        (rename returned %d = %s)\n", rc,
                 rc == -EIO ? "EIO" : rc == 0 ? "0 -- REPORTED APPLIED"
                                             : "another error");
            /* -EIO, not merely "not zero", and that is on purpose. A healthy
             * lookup of a destination that exists answers -EEXIST, and so does
             * a fault that landed on one of the two vol_is_dir probes ahead
             * of the guard. Accepting any non-zero here would let the count
             * below drift and the leg go green while measuring nothing. The
             * byte assertion underneath is the load-bearing one; this is the
             * guard that proves the fault reached the guard at all. */
            ok(rc == -EIO, "a RENAME_NOREPLACE whose destination lookup FAILED "
                           "is refused with EIO, not carried out");
            checks++;
            pthread_mutex_lock(&g_io_lock);
            vol_flush(g_vol);
            (void)vol_find_rc(g_vol, "dst", &dino);
            pthread_mutex_unlock(&g_io_lock);
            {
                uint8_t *got = NULL;
                size_t glen = 0;
                int rrc = dino ? vol_read_file(g_vol, dino, &got, &glen) : -1;
                int same = (rrc == 0 && glen == sizeof payload - 1 &&
                            got && memcmp(got, payload, glen) == 0);
                ok(same, "THE DESTINATION SURVIVED: /dst still holds its own bytes");
                if (!same)
                    printf("        ^ THE CLOBBER: /dst is now %zu bytes "
                           "starting \"%.*s\"\n",
                           glen, (int)(glen > 24 ? 24 : glen), (char *)(got ? (char *)got : ""));
                free(got);
            }
            {
                uint64_t s_ino = 0;
                ok(vol_find_rc(g_vol, "src", &s_ino) == 1,
                   "and the source was not renamed away either");
            }
        }
    }

    vol_close(v);
    unlink(img);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
