/* scratch_policy_test.c — the scratch DIRECTORY DECISION.
 *
 * The thing under test is a policy, not a data path, so this asserts which
 * directory gets chosen and what the refusal says. It never allocates the
 * gigabytes a real containerpack job would: the demand is a NUMBER handed
 * to tool_tmpdir(), compared against each root's measured headroom, so a
 * decision that only shows up at 7 GB is decided here at kilobytes.
 *
 * Why this test exists. tool_tmpdir() used to be
 *
 *     static const char *roots[] = { "/dev/shm", "/tmp" };
 *     for (r = 0; r < 2; r++) if (mkdtemp(...)) return 0;
 *
 * first root that accepts a directory wins, sized by nothing, while the
 * containerpack forward path pins a whole container into it and then
 * extracts every member of that container into the same place
 * (src/core/vol_cpack.c). On a host where both roots are tmpfs the peak is
 * about twice the container, IN RAM. Measured on this platform with a
 * 734,003,768-byte container against a 1200 MiB /dev/shm: the pin plus 50
 * members is 1,258,291,768 bytes against 1,258,291,200 free -- 568 bytes
 * short -- the extract failed at member 49 and the lane declined SILENTLY
 * (rc 0, no stamp, no !mbr siblings). That is the shape a whole day of
 * "the sweep did not decompose anything" took, and INVFS_TOOL_SCRATCH was
 * the only way out.
 *
 * Every case carries the control that flips it: a policy whose decision
 * does not move when the term under test moves is not testing that term.
 *
 *   1. a job every root can hold -> the FIRST root. tmpfs stays the fast
 *      path; the case the old code also passed, and the control for the
 *      rest.
 *   2. THE DECISION. INVFS_SCRATCH_TMPFS_MAX_FRAC=0 sends the demand to
 *      the real directory; 100 sends the SAME demand back to the tmpfs.
 *      One term, two answers, no gigabytes allocated.
 *   3. at the DEFAULT fraction, a demand the tmpfs is not offered falls
 *      through to the real directory and the deviation is announced with
 *      the numbers. (Needs a window between the allocation-aware ceiling
 *      and statvfs; skipped, loudly, where the two agree.)
 *   4. a demand no root can hold -> REFUSED, and the message carries the
 *      demand, the margin, every root's number, and the remedy.
 *   5. INVFS_TOOL_SCRATCH wins over the list AND is still sized: pointed
 *      at a root that cannot hold the job it refuses, with numbers.
 *   6. the two-phase shape: a root that held the pin but not the pin plus
 *      the members. The decision is RE-taken mid-job, the scratch
 *      MIGRATES, and the carried pin must arrive intact in the new
 *      directory. Its control is the job that still fits and must not move.
 *   7. the tmpfs headroom is not statvfs: on a memory-backed root the
 *      allocation-aware ceiling is reported separately and the offered
 *      figure never exceeds it; on a real filesystem there is no
 *      allocation term at all.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
/* The filesystem-type magic lives in struct statfs. struct statvfs has no
 * f_type member, so this was
 *     error: 'struct statvfs' has no member named 'f_type'
 * and the test could never compile. Same fix, same reason, as
 * src/core/tool_scratch.c -- which means the bug existed in two files and I
 * fixed the one I happened to be shown, without grepping for the pattern. */
#include <sys/statfs.h>
#include <sys/statvfs.h>   /* fs_free() legitimately uses statvfs for free space */
#include <sys/wait.h>
#include <unistd.h>

#include "../core/tool_scratch.h"

#define TMPFS_MAGIC 0x01021994UL

static int fails;

static void child_pick(void);
static void child_grow(void);

struct envkv { const char *k, *v; };
struct outcome { int rc; char out[4096]; char err[4096]; };

static void slurp(int fd, char *dst, size_t cap)
{
    size_t used = 0;
    ssize_t r;
    while (used + 1 < cap && (r = read(fd, dst + used, cap - 1 - used)) > 0)
        used += (size_t)r;
    dst[used] = 0;
}

static void run_case(const struct envkv *env, int nenv, struct outcome *o)
{
    int op[2], ep[2];
    pid_t pid;
    int st = 0;

    memset(o, 0, sizeof *o);
    /* the parent has been printing progress to a pipe; flush it HERE, or the
     * child inherits the buffer and its fflush() hands the parent's output
     * back through the case's capture. */
    fflush(NULL);
    if (pipe(op) != 0 || pipe(ep) != 0) { perror("pipe"); exit(2); }
    pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        int i;
        close(op[0]); close(ep[0]);
        dup2(op[1], 1); dup2(ep[1], 2);
        close(op[1]); close(ep[1]);
        for (i = 0; i < nenv; i++)
            if (setenv(env[i].k, env[i].v, 1) != 0) _exit(120);
        if (getenv("TEST_GROW") && atoi(getenv("TEST_GROW"))) child_grow();
        else child_pick();
        fflush(NULL);
        _exit(0);
    }
    close(op[1]); close(ep[1]);
    slurp(op[0], o->out, sizeof o->out);
    slurp(ep[0], o->err, sizeof o->err);
    close(op[0]); close(ep[0]);
    waitpid(pid, &st, 0);
    o->rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    if (!cond) fails++;
}

/* --- the child halves ------------------------------------------------- */

/* one call: pick a scratch for a TEST_NEED-byte job, print the directory */
static void child_pick(void)
{
    char dir[256];
    uint64_t need = (uint64_t)strtoull(getenv("TEST_NEED"), NULL, 10);

    if (tool_tmpdir(dir, sizeof dir, need) != 0) { printf("REFUSED\n"); return; }
    printf("CHOSEN %s\n", dir);
}

/* the containerpack two-phase shape: pick a scratch sized for the PIN,
 * write the pin into it, then discover the job is `need * 4` and re-price.
 * The carried file must arrive intact wherever the scratch ends up. */
static void child_grow(void)
{
    char dir[256], p[300];
    uint8_t want[64], got[64];
    scratch_file f[1];
    FILE *o;
    size_t i, n;
    uint64_t need = (uint64_t)strtoull(getenv("TEST_NEED"), NULL, 10);

    for (i = 0; i < sizeof want; i++) want[i] = (uint8_t)(i * 7 + 3);
    if (tool_tmpdir(dir, sizeof dir, need) != 0) { printf("REFUSED\n"); return; }
    snprintf(p, sizeof p, "%s/in", dir);
    if (!(o = fopen(p, "wb"))) { printf("NOWRITE\n"); return; }
    fwrite(want, 1, sizeof want, o);
    fclose(o);
    f[0].name = "in";
    f[0].data = want;
    f[0].len = sizeof want;
    if (tool_scratch_grow(dir, sizeof dir, need * 4, f, 1) != 0) {
        printf("REFUSED\n");
        return;
    }
    snprintf(p, sizeof p, "%s/in", dir);
    if (!(o = fopen(p, "rb"))) { printf("NOCARRY\n"); return; }
    n = fread(got, 1, sizeof got, o);
    fclose(o);
    if (n != sizeof got || memcmp(got, want, sizeof want) != 0) {
        printf("BADARRY\n");
        return;
    }
    printf("CHOSEN %s\n", dir);
}

/* --- helpers ---------------------------------------------------------- */

static int is_tmpfs(const char *p)
{
    struct statfs v;
    return statfs(p, &v) == 0 && (unsigned long)v.f_type == TMPFS_MAGIC;
}

static uint64_t fs_free(const char *p)
{
    struct statvfs v;
    if (statvfs(p, &v) != 0) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

static void cleanup(const char *p)
{
    char cmd[640];
    if (!p || !*p) return;
    snprintf(cmd, sizeof cmd, "rm -rf '%.*s'", (int)(sizeof cmd - 16), p);
    if (system(cmd) != 0) { /* nothing there */ }
}

static const char *chosen_of(const struct outcome *o)
{
    return strncmp(o->out, "CHOSEN ", 7) == 0 ? o->out + 7 : NULL;
}

static int under(const char *path, const char *root)
{
    return path && strncmp(path, root, strlen(root)) == 0;
}

/* headroom of `root` with INVFS_SCRATCH_TMPFS_MAX_FRAC pinned to `pct` */
static uint64_t headroom_at(const char *root, int pct)
{
    char b[8];
    uint64_t h;
    snprintf(b, sizeof b, "%d", pct);
    setenv("INVFS_SCRATCH_TMPFS_MAX_FRAC", b, 1);
    h = tool_scratch_headroom(root, NULL, NULL);
    unsetenv("INVFS_SCRATCH_TMPFS_MAX_FRAC");
    return h;
}

int main(void)
{
    char base[224], shm[288], disk[288], roots[700], num[32];
    struct outcome o;
    struct envkv e[8];
    int n, groups = 0;
    uint64_t shm_fs, disk_fs, shm_max, shm_default, need, impossible;
    uint64_t fs_a = 0, alloc_a = 0, offered;

    /* One tmpfs root -- the memory-backed branch has to be real -- and one
     * root on a filesystem that is NOT tmpfs: the fall-through target. The
     * build's own directory is the usual answer; a host where every
     * writable root is tmpfs is reported, not faked. */
    snprintf(base, sizeof base, "./.invfs-scratch-policy-%ld", (long)getpid());
    snprintf(shm, sizeof shm, "/dev/shm/invfs-scratch-policy-%ld", (long)getpid());
    snprintf(disk, sizeof disk, "%s/disk", base);
    cleanup(base); cleanup(shm);
    if (mkdir(base, 0700) != 0 || mkdir(shm, 0700) != 0 ||
        mkdir(disk, 0700) != 0) {
        fprintf(stderr, "FAIL: cannot build the fixture roots under %s: %s\n",
                base, strerror(errno));
        return 2;
    }
    shm_fs = fs_free(shm);
    disk_fs = fs_free(disk);
    printf("fixture: tmpfs root %s (%llu B free, tmpfs=%d)\n"
           "         disk root %s (%llu B free, tmpfs=%d)\n",
           shm, (unsigned long long)shm_fs, is_tmpfs(shm),
           disk, (unsigned long long)disk_fs, is_tmpfs(disk));
    if (!is_tmpfs(shm)) {
        fprintf(stderr, "SKIP: %s is not a tmpfs here, so the memory-backed "
                "branch cannot be exercised\n", shm);
        cleanup(base); cleanup(shm);
        return 0;
    }
    if (is_tmpfs(disk)) {
        fprintf(stderr, "SKIP: every writable root on this host is tmpfs, so "
                "the fall-through-to-a-real-filesystem branch cannot be "
                "exercised (build root %s)\n", disk);
        cleanup(base); cleanup(shm);
        return 0;
    }
    snprintf(roots, sizeof roots, "%s:%s", shm, disk);
    shm_max = headroom_at(shm, 100);
    shm_default = headroom_at(shm, 50);
    impossible = (shm_fs > disk_fs ? shm_fs : disk_fs) * 4 + (1u << 30);
    printf("tmpfs offered %llu B at the default fraction / %llu B at 100%%; "
           "disk free %llu B\n", (unsigned long long)shm_default,
           (unsigned long long)shm_max, (unsigned long long)disk_fs);

    /* --- 1. everything fits: the first root wins --------------------- */
    snprintf(num, sizeof num, "4096");
    n = 0;
    e[n].k = "TEST_NEED";               e[n++].v = num;
    e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
    e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
    e[n].k = "INVFS_SCRATCH_VERBOSE";   e[n++].v = "1";
    run_case(e, n, &o);
    ok(o.rc == 0 && under(chosen_of(&o), shm),
       "1. a 4 KiB job goes to the FIRST root (tmpfs fast path kept): %s",
       chosen_of(&o) ? chosen_of(&o) : o.out);
    ok(strstr(o.err, "4096") != NULL && strstr(o.err, shm) != NULL,
       "1b. the decision names the demand and the directory: %.90s", o.err);
    cleanup(chosen_of(&o));
    groups++;

    /* --- 2. THE DECISION, with the flip as its control --------------- */
    if (shm_max > 8192 && disk_fs > shm_max) {
        need = shm_max / 2;
        snprintf(num, sizeof num, "%llu", (unsigned long long)need);
        n = 0;
        e[n].k = "TEST_NEED";               e[n++].v = num;
        e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
        e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
        e[n].k = "INVFS_SCRATCH_TMPFS_MAX_FRAC"; e[n++].v = "0";
        run_case(e, n, &o);
        ok(o.rc == 0 && under(chosen_of(&o), disk),
           "2.  TMPFS_MAX_FRAC=0: a %llu B job goes to the real directory",
           (unsigned long long)need);
        cleanup(chosen_of(&o));

        n = 0;
        e[n].k = "TEST_NEED";               e[n++].v = num;
        e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
        e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
        e[n].k = "INVFS_SCRATCH_TMPFS_MAX_FRAC"; e[n++].v = "100";
        run_case(e, n, &o);
        ok(o.rc == 0 && under(chosen_of(&o), shm),
           "2b. CONTROL, the same job at 100%%: back to the tmpfs fast path: "
           "%s", chosen_of(&o) ? chosen_of(&o) : o.out);
        cleanup(chosen_of(&o));
        groups++;
    } else {
        printf("  (case 2 needs a demand between the tmpfs's ceiling and the "
               "disk root's free space; skipped on this host)\n");
    }

    /* --- 3. at the DEFAULT fraction, the fall-through --------------- */
    if (shm_default > 8192 && disk_fs > shm_default &&
        shm_default < shm_max) {
        need = (shm_default + disk_fs) / 2;
        snprintf(num, sizeof num, "%llu", (unsigned long long)need);
        n = 0;
        e[n].k = "TEST_NEED";               e[n++].v = num;
        e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
        e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
        run_case(e, n, &o);
        ok(o.rc == 0 && under(chosen_of(&o), disk),
           "3. a %llu B job the tmpfs is not OFFERED falls through to the "
           "real directory: %s", (unsigned long long)need,
           chosen_of(&o) ? chosen_of(&o) : o.out);
        ok(strstr(o.err, shm) != NULL && strstr(o.err, disk) != NULL &&
           strstr(o.err, num) != NULL,
           "3b. the deviation is announced with the numbers: %.150s", o.err);
        cleanup(chosen_of(&o));
        groups++;
    } else {
        printf("  (case 3 needs the allocation ceiling to bind below "
               "statvfs; on this host both give %llu B, so it is skipped)\n",
               (unsigned long long)shm_default);
    }

    /* --- 4. a demand no root can hold: REFUSED, with numbers -------- */
    snprintf(num, sizeof num, "%llu", (unsigned long long)impossible);
    n = 0;
    e[n].k = "TEST_NEED";               e[n++].v = num;
    e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
    e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "8";
    run_case(e, n, &o);
    ok(o.rc == 0 && strcmp(o.out, "REFUSED\n") == 0,
       "4. a %llu B demand no root can hold is REFUSED, not attempted",
       (unsigned long long)impossible);
    ok(strstr(o.err, "REFUSED") != NULL && strstr(o.err, num) != NULL &&
       strstr(o.err, "8388608") != NULL,
       "4b. the refusal names the demand and the 8 MiB margin: %.200s", o.err);
    ok(strstr(o.err, shm) != NULL && strstr(o.err, disk) != NULL,
       "4c. the refusal names every root it tried: %.200s", o.err);
    ok(strstr(o.err, "INVFS_TOOL_SCRATCH") != NULL,
       "4d. the refusal says what to do about it: %.130s", o.err);
    cleanup(chosen_of(&o));
    groups++;

    /* --- 5. INVFS_TOOL_SCRATCH wins over the list, and is still sized */
    n = 0;
    e[n].k = "TEST_NEED";               e[n++].v = num;
    e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
    e[n].k = "INVFS_TOOL_SCRATCH";      e[n++].v = disk;
    e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
    run_case(e, n, &o);
    ok(o.rc == 0 && strcmp(o.out, "REFUSED\n") == 0,
       "5.  an explicit scratch that cannot hold the job is refused too "
       "(explicit is not a waiver)");
    ok(strstr(o.err, disk) != NULL && strstr(o.err, num) != NULL,
       "5b. and the refusal names it: %.200s", o.err);

    snprintf(num, sizeof num, "4096");
    n = 0;
    e[n].k = "TEST_NEED";               e[n++].v = num;
    e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = shm;   /* tmpfs only */
    e[n].k = "INVFS_TOOL_SCRATCH";      e[n++].v = disk;  /* disk wins */
    e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
    run_case(e, n, &o);
    ok(o.rc == 0 && under(chosen_of(&o), disk),
       "5c. INVFS_TOOL_SCRATCH overrides the root list: %s",
       chosen_of(&o) ? chosen_of(&o) : o.out);
    cleanup(chosen_of(&o));
    groups++;

    /* --- 6. the two-phase re-decision, with the pin carried across --- */
    if (!(shm_max > 262144 && disk_fs > need * 4 + (128ull * 1048576)))
        printf("  (case 6 needs a job that fits the real root but not the "
               "tmpfs; disk free %llu B vs whole job %llu B -- skipped on "
               "this host)\n", (unsigned long long)disk_fs,
               (unsigned long long)(need * 4));
    need = shm_max / 2 + 4096;
    /* The window this case needs: the pin fits the tmpfs, the whole job does
     * NOT fit the tmpfs, and the whole job DOES fit the real root -- that is
     * the exact shape of a containerpack forward pass, where the member total
     * is only known after enumerate and the scratch has to MIGRATE.
     *
     * The original guard only checked `shm_max > 262144` and silently assumed
     * a real root four times larger than the tmpfs. That is not a property
     * of every host: on a machine where /var/tmp and /tmp are tmpfs, or where
     * the disk root lives on a small root filesystem, there is no such window
     * and the correct verdict is REFUSED. A host-dependent assumption in a
     * guard is what made this case fail on a machine that had merely run out
     * of room elsewhere -- so the assumption is now stated and checked, and
     * the case skips loudly instead of failing, the same way cases 2 and 3
     * already do. */
    if (shm_max > 262144 && disk_fs > need * 4 + (128ull * 1048576)) {
        snprintf(num, sizeof num, "%llu", (unsigned long long)need);
        n = 0;
        e[n].k = "TEST_NEED";               e[n++].v = num;
        e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
        e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
        e[n].k = "INVFS_SCRATCH_TMPFS_MAX_FRAC"; e[n++].v = "100";
        e[n].k = "TEST_GROW";               e[n++].v = "1";
        run_case(e, n, &o);
        ok(o.rc == 0 && strncmp(o.out, "CHOSEN", 6) == 0 &&
           strstr(o.err, "grew") != NULL,
           "6.  a job that outgrew its tmpfs scratch (pin %llu B, whole job "
           "%llu B) MIGRATES off it, carrying the pin: %.120s",
           (unsigned long long)need, (unsigned long long)(need * 4), o.err);
        ok(under(chosen_of(&o), disk),
           "6b. and it lands on the real directory: %s",
           chosen_of(&o) ? chosen_of(&o) : o.out);
        cleanup(chosen_of(&o));

        /* the control: a job that does NOT outgrow it must not move */
        need = shm_max / 16 + 4096;   /* need * 4 still fits: no move */
        snprintf(num, sizeof num, "%llu", (unsigned long long)need);
        n = 0;
        e[n].k = "TEST_NEED";               e[n++].v = num;
        e[n].k = "INVFS_SCRATCH_ROOTS";     e[n++].v = roots;
        e[n].k = "INVFS_SCRATCH_MARGIN_MB"; e[n++].v = "1";
        e[n].k = "INVFS_SCRATCH_TMPFS_MAX_FRAC"; e[n++].v = "100";
        e[n].k = "TEST_GROW";               e[n++].v = "1";
        run_case(e, n, &o);
        ok(o.rc == 0 && under(chosen_of(&o), shm) &&
           strstr(o.err, "moved") == NULL,
           "6c. CONTROL, a job that still fits does NOT migrate: %s",
           chosen_of(&o) ? chosen_of(&o) : o.out);
        cleanup(chosen_of(&o));
        groups++;
    }

    /* --- 7. the headroom figure itself ------------------------------ */
    offered = tool_scratch_headroom(shm, &fs_a, &alloc_a);
    ok(alloc_a > 0 && offered <= fs_a && offered <= alloc_a,
       "7.  a tmpfs root is offered min(statvfs, allocation ceiling): "
       "offered %llu B, statvfs %llu B, ceiling %llu B",
       (unsigned long long)offered, (unsigned long long)fs_a,
       (unsigned long long)alloc_a);
    {
        uint64_t fs_b = 0, alloc_b = 1;
        uint64_t off_b = tool_scratch_headroom(disk, &fs_b, &alloc_b);
        ok(alloc_b == 0 && off_b == fs_b && fs_b > 0,
           "7b. a real filesystem has no allocation term: offered %llu B == "
           "free %llu B", (unsigned long long)off_b, (unsigned long long)fs_b);
    }
    groups++;

    cleanup(base); cleanup(shm);
    printf("%s: %d failing assertion(s), %d case group(s) run\n",
           fails ? "FAIL" : "PASS", fails, groups);
    return fails ? 1 : 0;
}
