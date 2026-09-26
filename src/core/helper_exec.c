/* helper_exec.c — WP61: shared containment launcher for external helpers.
 * See helper_exec.h for the contract. The WP12(d) Landlock layer and the
 * WP11 scratch/timeout plumbing were folded here so there is exactly one
 * fork/exec child-setup path for every helper the engine runs. */

/* _GNU_SOURCE for O_PATH (the Landlock rule fds), unshare(), close_range */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "helper_exec.h"

#ifndef _WIN32

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#if defined(__linux__)
#  include <sched.h>
#  if defined(__has_include)
#    if __has_include(<linux/landlock.h>)
#      include <linux/landlock.h>
#      if defined(SYS_landlock_create_ruleset) && \
          defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self)
#        define INVFS_HAVE_LANDLOCK 1
#      endif
#    endif
#  endif
#endif

#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif

/* ------------------------------------------------------------------ */
/* tunables                                                           */
/* ------------------------------------------------------------------ */

#define HELPER_TIMEOUT_MS_DEFAULT (120ull * 1000ull)
#define HELPER_NOFILE_DEFAULT     1024u
#define HELPER_NPROC_DEFAULT      256u
#define HELPER_FSIZE_DEFAULT      (8ull << 30)      /* 8 GiB per file */
#define HELPER_NOBODY_UID         65534u
#define HELPER_NOBODY_GID         65534u

static uint64_t helper_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t helper_env_u64(const char *name, uint64_t dflt)
{
    const char *e = getenv(name);
    if (e && *e) {
        unsigned long long v = strtoull(e, NULL, 10);
        if (v) return (uint64_t)v;
    }
    return dflt;
}

static uint64_t helper_timeout_ms(uint64_t requested)
{
    if (requested) return requested;
    return helper_env_u64("INVFS_HELPER_TIMEOUT_MS",
                          HELPER_TIMEOUT_MS_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* errors + target ids (computed in the parent, never via NSS in child) */
/* ------------------------------------------------------------------ */

static int helper_target_ids(uid_t *uid, gid_t *gid)
{
    const char *eu = getenv("INVFS_HELPER_UID");
    const char *eg = getenv("INVFS_HELPER_GID");
    struct passwd *pw = getpwnam("nobody");
    uid_t u;
    gid_t g;

    if (eu && *eu) {
        u = (uid_t)strtoul(eu, NULL, 10);
    } else {
        u = pw ? pw->pw_uid : HELPER_NOBODY_UID;
    }
    if (eg && *eg) {
        g = (gid_t)strtoul(eg, NULL, 10);
    } else if (pw) {
        g = pw->pw_gid;
    } else {
        struct passwd *pu = getpwuid(u);
        g = pu ? pu->pw_gid : HELPER_NOBODY_GID;
    }
    *uid = u;
    *gid = g;
    return 0;
}

/* ------------------------------------------------------------------ */
/* environment scrub (child side)                                     */
/* ------------------------------------------------------------------ */

/* Read one environment name into a heap copy before clearenv(). */
static char *helper_takeenv(const char *name)
{
    const char *v = getenv(name);
    return (v && *v) ? strdup(v) : NULL;
}

/* Rebuild a minimal environment. Names in INVFS_HELPER_KEEPENV are copied
 * through (except LD_*), TMPDIR is forced to the sandbox scratch, and PATH
 * is the minimal system path unless PATH itself is being kept. */
static void helper_scrub_env(const invfs_helper_sandbox *sb)
{
    static const char *always[] = { "LANG", "LC_ALL", "LC_CTYPE" };
    const char *keep = getenv("INVFS_HELPER_KEEPENV");
    char namebuf[512];
    char *names[32];
    char *saved[32];
    size_t n = 0, i;
    char pathdef[4096];
    char *parent_path = helper_takeenv("PATH");
    char *helper_path = helper_takeenv("INVFS_HELPER_PATH");
    int keep_path = 0;

    if (keep && *keep) {
        snprintf(namebuf, sizeof namebuf, "%s", keep);
        {
            char *save = NULL, *tok;
            for (tok = strtok_r(namebuf, ", ", &save);
                 tok && n < sizeof names / sizeof names[0];
                 tok = strtok_r(NULL, ", ", &save)) {
                if (strncmp(tok, "LD_", 3) == 0) continue;
                names[n] = tok;
                saved[n] = helper_takeenv(tok);
                if (strcmp(tok, "PATH") == 0) keep_path = 1;
                n++;
            }
        }
    }
    {
        size_t a;
        for (a = 0; a < sizeof always / sizeof always[0]; a++) {
            if (n >= sizeof names / sizeof names[0]) break;
            names[n] = (char *)always[a];
            saved[n] = helper_takeenv(always[a]);
            n++;
        }
    }

    clearenv();

    if (keep_path && parent_path && *parent_path)
        snprintf(pathdef, sizeof pathdef, "%s", parent_path);
    else if (helper_path && *helper_path)
        snprintf(pathdef, sizeof pathdef, "%s", helper_path);
    else
        snprintf(pathdef, sizeof pathdef, "%s",
                 "/usr/local/bin:/usr/bin:/bin");
    setenv("PATH", pathdef, 1);

    for (i = 0; i < n; i++) {
        if (saved[i]) {
            setenv(names[i], saved[i], 1);
            free(saved[i]);
        }
    }
    /* the sandbox scratch wins over any inherited TMPDIR */
    if (sb && sb->rw_dir) setenv("TMPDIR", sb->rw_dir, 1);

    free(parent_path);
    free(helper_path);
}

/* ------------------------------------------------------------------ */
/* resource limits (child side)                                       */
/* ------------------------------------------------------------------ */

static void helper_setrlimit(int what, rlim_t cur, rlim_t max)
{
    struct rlimit rl;
    rl.rlim_cur = cur;
    rl.rlim_max = max;
    (void)setrlimit(what, &rl);
}

static void helper_setrlimits(uint64_t mem_cap, uint64_t timeout_ms)
{
    unsigned long secs = (unsigned long)(timeout_ms / 1000) + 5;

    if (mem_cap) helper_setrlimit(RLIMIT_AS, (rlim_t)mem_cap,
                                  (rlim_t)mem_cap);
    helper_setrlimit(RLIMIT_CPU, (rlim_t)secs, (rlim_t)(secs + 10));
    helper_setrlimit(RLIMIT_NOFILE,
                     (rlim_t)helper_env_u64("INVFS_HELPER_NOFILE",
                                            HELPER_NOFILE_DEFAULT),
                     (rlim_t)helper_env_u64("INVFS_HELPER_NOFILE",
                                            HELPER_NOFILE_DEFAULT));
    helper_setrlimit(RLIMIT_NPROC,
                     (rlim_t)helper_env_u64("INVFS_HELPER_NPROC",
                                            HELPER_NPROC_DEFAULT),
                     (rlim_t)helper_env_u64("INVFS_HELPER_NPROC",
                                            HELPER_NPROC_DEFAULT));
    {
        uint64_t mb = helper_env_u64("INVFS_HELPER_FSIZE_MB",
                                     HELPER_FSIZE_DEFAULT >> 20);
        helper_setrlimit(RLIMIT_FSIZE, (rlim_t)(mb << 20),
                         (rlim_t)(mb << 20));
    }
}

/* ------------------------------------------------------------------ */
/* network namespace (child side, best effort)                        */
/* ------------------------------------------------------------------ */

#ifdef __linux__
static void helper_write_file(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t w = write(fd, val, strlen(val));
    (void)w;
    close(fd);
}
#endif

/* CLONE_NEWNET directly when privileged; otherwise bootstrap a user
 * namespace that maps the current uid/gid, then create the net namespace
 * inside it. Failure is ignored (older kernels / locked-down containers);
 * Landlock still applies, and the network test flags the miss. */
static void helper_netns(void)
{
#ifdef __linux__
    uid_t uid = getuid();
    gid_t gid = getgid();
    char buf[64];

    if (unshare(CLONE_NEWNET) == 0) return;
    if (errno != EPERM) return;

    if (unshare(CLONE_NEWUSER) != 0) return;
    helper_write_file("/proc/self/setgroups", "deny\n");
    snprintf(buf, sizeof buf, "0 %d 1\n", (int)uid);
    helper_write_file("/proc/self/uid_map", buf);
    snprintf(buf, sizeof buf, "0 %d 1\n", (int)gid);
    helper_write_file("/proc/self/gid_map", buf);
    (void)unshare(CLONE_NEWNET);
#endif
}

/* ------------------------------------------------------------------ */
/* privilege drop (child side, root only)                             */
/* ------------------------------------------------------------------ */

/* Chown only scratch trees under the tmpfs roots the engine uses; never
 * touch the volume or arbitrary user paths. */
static int helper_scratch_path(const char *p)
{
    if (!p) return 0;
    return strncmp(p, "/dev/shm/", 9) == 0 || strncmp(p, "/tmp/", 5) == 0;
}

static void helper_chown_tree(const char *path, uid_t uid, gid_t gid,
                              int depth)
{
    DIR *d;
    struct dirent *e;

    if (depth > 8 || !helper_scratch_path(path)) return;
    int cr = lchown(path, uid, gid);
    (void)cr;
    d = opendir(path);
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char child[4096];
        struct stat st;
        int n;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        n = snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof child) continue;
        if (lstat(child, &st) != 0) continue;
        cr = lchown(child, uid, gid);
        (void)cr;
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            helper_chown_tree(child, uid, gid, depth + 1);
    }
    closedir(d);
}

static int helper_drop_privs(const char *work_dir, const char *ro_path,
                             uid_t uid, gid_t gid)
{
    int rc;

    if (work_dir) helper_chown_tree(work_dir, uid, gid, 0);
    if (helper_scratch_path(ro_path)) {
        int cr = lchown(ro_path, uid, gid);
        (void)cr;
    }

    /* A user namespace created by an unprivileged process (unshare -r, a
     * rootless container) maps ONE id and the kernel then REQUIRES that
     * setgroups(2) never be called: it fails with EPERM, permanently. Every
     * runtime that drops privileges inside such a namespace skips it, and so
     * must we -- calling it and treating EPERM as fatal made the child
     * _exit(126) before it ever exec'd, so every pack command in a rootless
     * namespace reported as a tool failure. /proc/self/setgroups is not a
     * usable test here: in the forked child it is already unreadable (EACCES).
     * Any other errno still means "we really could not drop". */
    if (setgroups(0, NULL) != 0 && errno != EPERM)
        return -1;

    /* setgid/setuid answer EINVAL when the target id is simply not present in
     * this namespace's map. Then there is no second id to shed and the
     * namespace itself is the confinement, so keep the current (namespace
     * root) ids instead of refusing to run the helper at all. EPERM/EACCES
     * still mean a genuine inability to drop and stay fatal. */
    rc = setgid(gid);
    if (rc != 0 && errno != EINVAL)
        return -1;
    rc = setuid(uid);
    if (rc != 0 && errno != EINVAL)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Landlock whitelist (child side, WP12d, unchanged)                  */
/* ------------------------------------------------------------------ */

typedef struct invfs_helper_sandbox tool_sandbox;

/* 0 = off (env), 1 = Landlock absent, 2 = Landlock enforced. Probed once
 * per process in the parent, where stderr still reaches the user. */
static int pack_sandbox_mode(void)
{
    static int mode = -1;

    if (mode >= 0) return mode;
    {
        const char *e = getenv("INVFS_PACK_SANDBOX");
        if (e && !strcmp(e, "0")) {
            mode = 0;
            return mode;
        }
    }
    mode = 1;
#ifdef INVFS_HAVE_LANDLOCK
    {
        int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                               LANDLOCK_CREATE_RULESET_VERSION);
        if (abi >= 1) {
            mode = 2;
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[vol] pack sandbox: Landlock ABI v%d\n",
                        abi);
        }
    }
#endif
    if (mode == 1 && getenv("INVFS_DEBUG"))
        fprintf(stderr, "[vol] pack sandbox: Landlock unavailable — "
                "no_new_privs + RLIMIT_AS only\n");
    return mode;
}

#ifdef INVFS_HAVE_LANDLOCK
static void ll_add_rule(int rfd, uint64_t rights, const char *path)
{
    struct landlock_path_beneath_attr a;
    int pfd;

    if (!path || !*path) return;
    pfd = open(path, O_PATH | O_CLOEXEC);
    if (pfd < 0) return;
    a.allowed_access = rights;
    a.parent_fd = pfd;
    (void)syscall(SYS_landlock_add_rule, rfd, LANDLOCK_RULE_PATH_BENEATH,
                  &a, 0);
    close(pfd);
}

static void ll_add_exe(int rfd, const char *argv0)
{
    char buf[4096];
    const char *path, *p;

    if (!argv0 || !*argv0) return;
    if (strchr(argv0, '/')) {
        ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                         LANDLOCK_ACCESS_FS_EXECUTE, argv0);
        return;
    }
    path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    p = path;
    for (;;) {
        const char *c = strchr(p, ':');
        size_t dl = c ? (size_t)(c - p) : strlen(p);
        size_t nl = strlen(argv0);
        if (!dl) { p = "."; dl = 1; }
        if (dl + 1 + nl + 1 <= sizeof buf) {
            memcpy(buf, p, dl);
            buf[dl] = '/';
            memcpy(buf + dl + 1, argv0, nl + 1);
            if (access(buf, X_OK) == 0) {
                ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                                 LANDLOCK_ACCESS_FS_EXECUTE, buf);
                return;
            }
        }
        if (!c) break;
        p = c + 1;
    }
}

static void ll_add_argv_files(int rfd, char *const argv[])
{
    char rb[4096];
    size_t i;

    for (i = 0; argv[i]; i++) {
        if (!strchr(argv[i], '/')) continue;
        if (realpath(argv[i], rb))
            ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                             LANDLOCK_ACCESS_FS_EXECUTE, rb);
    }
}

static void landlock_apply(const tool_sandbox *sb, char *const argv[])
{
    static const char *rt_dirs[] = {
        "/usr/lib", "/usr/lib64", "/lib", "/lib64",
        "/usr/bin", "/bin", "/usr/local/bin",
    };
    struct landlock_ruleset_attr attr;
    uint64_t handled, ro, rw;
    int abi, rfd;
    size_t i;

    abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                       LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return;

    handled = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |
              LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_WRITE_FILE |
              LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
              LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
              LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
              LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
              LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) handled |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    memset(&attr, 0, sizeof attr);
    attr.handled_access_fs = handled;
    rfd = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
    if (rfd < 0) return;

    ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
         LANDLOCK_ACCESS_FS_EXECUTE;
    rw = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
         LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
         LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
         LANDLOCK_ACCESS_FS_REMOVE_DIR;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) rw |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) rw |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif

    for (i = 0; i < sizeof rt_dirs / sizeof rt_dirs[0]; i++)
        ll_add_rule(rfd, ro, rt_dirs[i]);
    ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE, "/etc/ld.so.cache");
    ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE, "/etc/localtime");
    ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                     LANDLOCK_ACCESS_FS_WRITE_FILE, "/dev/null");
    {
        const char *td = getenv("INVFS_TOOLS");
        if (td && *td) ll_add_rule(rfd, ro, td);
    }
    ll_add_rule(rfd, ro, sb->pack_dir);
    ll_add_exe(rfd, argv[0]);
    ll_add_argv_files(rfd, argv);
    if (sb->requires) {
        char req[512];
        char *tok, *save = NULL;
        snprintf(req, sizeof req, "%s", sb->requires);
        for (tok = strtok_r(req, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            char buf[4096];
            const char *td = getenv("INVFS_TOOLS");
            int n;
            if (td && *td) {
                n = snprintf(buf, sizeof buf, "%s/%s", td, tok);
                if (n > 0 && (size_t)n < sizeof buf &&
                    access(buf, X_OK) == 0) {
                    ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                                LANDLOCK_ACCESS_FS_EXECUTE, buf);
                    continue;
                }
            }
            n = snprintf(buf, sizeof buf, "/usr/lib/invfs/tools/%s", tok);
            if (access(buf, X_OK) == 0) {
                ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                            LANDLOCK_ACCESS_FS_EXECUTE, buf);
                continue;
            }
            n = snprintf(buf, sizeof buf, "%s/bin/%s",
                         sb->pack_dir ? sb->pack_dir : "", tok);
            if (sb->pack_dir && access(buf, X_OK) == 0) {
                ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                            LANDLOCK_ACCESS_FS_EXECUTE, buf);
                continue;
            }
            ll_add_exe(rfd, tok);
        }
    }
    ll_add_rule(rfd, LANDLOCK_ACCESS_FS_READ_FILE, sb->ro_path);
    ll_add_rule(rfd, rw, sb->rw_dir);

    (void)syscall(SYS_landlock_restrict_self, rfd, 0);
    close(rfd);
}
#endif /* INVFS_HAVE_LANDLOCK */

/* ------------------------------------------------------------------ */
/* child setup + wait                                                 */
/* ------------------------------------------------------------------ */

static void helper_close_inherited(void)
{
#if defined(SYS_close_range)
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    {
        long max = sysconf(_SC_OPEN_MAX);
        int fd;
        if (max < 0 || max > 65536) max = 65536;
        for (fd = 3; fd < (int)max; fd++) close(fd);
    }
}

static void helper_child_setup(const invfs_helper_sandbox *sb, int sbmode,
                               char *const argv[], uint64_t mem_cap,
                               uint64_t timeout_ms, int is_root,
                               uid_t h_uid, gid_t h_gid,
                               const char *work_dir)
{
    (void)argv;

    (void)setpgid(0, 0);
    helper_netns();
    helper_setrlimits(mem_cap, timeout_ms);

#ifdef __linux__
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif
#ifdef INVFS_HAVE_LANDLOCK
    if (sb && sbmode == 2) landlock_apply(sb, argv);
#else
    (void)sbmode;
#endif
    helper_scrub_env(sb);
    if (is_root) {
        const char *wd = work_dir ? work_dir : (sb ? sb->rw_dir : NULL);
        if (helper_drop_privs(wd, sb ? sb->ro_path : NULL,
                              h_uid, h_gid) != 0)
            _exit(126);
    }
    helper_close_inherited();
}

static void helper_kill_group(pid_t pid)
{
    (void)setpgid(pid, pid);
    (void)kill(-pid, SIGKILL);
    (void)kill(pid, SIGKILL);
}

static int helper_wait(pid_t pid, int capture, int rfd, char *buf,
                       size_t cap, uint64_t timeout_ms, const char *argv0)
{
    int st = 0, exited = 0;
    uint64_t t0 = helper_now_ms();
    size_t got = 0;

    if (cap) buf[0] = '\0';
    for (;;) {
        if (capture) {
            for (;;) {
                char junk[256];
                char *dst = (got + 1 < cap) ? buf + got : junk;
                size_t room = (dst == junk) ? sizeof junk : cap - 1 - got;
                ssize_t r = read(rfd, dst, room);
                if (r <= 0) break;
                if (dst != junk) got += (size_t)r;
            }
        }
        if (exited) break;
        {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) { exited = 1; continue; }
            if (w < 0 && errno != EINTR) {
                if (capture) close(rfd);
                return -1;
            }
        }
        if (helper_now_ms() - t0 > timeout_ms) {
            helper_kill_group(pid);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
            if (capture) close(rfd);
            fprintf(stderr, "helper_exec: %s killed after %llums\n",
                    argv0 ? argv0 : "?", (unsigned long long)timeout_ms);
            return -1;
        }
        usleep(2000);
    }
    if (capture) close(rfd);
    if (cap) buf[got < cap ? got : cap - 1] = '\0';
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int invfs_helper_exec(char *const argv[], uint64_t mem_cap,
                      const invfs_helper_sandbox *sb,
                      const char *work_dir,
                      int no_path_search,
                      char *out, size_t out_cap,
                      uint64_t timeout_ms)
{
    pid_t pid;
    int pfd[2] = { -1, -1 };
    int capture;
    int sbmode;
    int is_root;
    uid_t h_uid = 0;
    gid_t h_gid = 0;
    uint64_t tmo;

    if (!argv || !argv[0]) return -1;
    tmo = helper_timeout_ms(timeout_ms);
    sbmode = sb ? pack_sandbox_mode() : 0;
    is_root = (getuid() == 0);
    if (is_root) (void)helper_target_ids(&h_uid, &h_gid);

    capture = (out != NULL && out_cap > 0);
    if (capture && pipe(pfd) != 0) return -1;

    pid = fork();
    if (pid < 0) {
        if (capture) { close(pfd[0]); close(pfd[1]); }
        return -1;
    }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (capture) {
            if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); }
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[0]);
            close(pfd[1]);
        } else if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
        }
        if (dn > STDERR_FILENO) close(dn);

        helper_child_setup(sb, sbmode, argv, mem_cap, tmo, is_root,
                           h_uid, h_gid, work_dir);
        if (no_path_search) execv(argv[0], argv);
        else execvp(argv[0], argv);
        _exit(127);
    }
    (void)setpgid(pid, pid);
    if (capture) {
        close(pfd[1]);
        fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    }
    return helper_wait(pid, capture, pfd[0], out, out_cap, tmo, argv[0]);
}

#else  /* _WIN32: the POSIX launcher does not exist; callers keep their
        * own CreateProcessW paths. */
int invfs_helper_exec(char *const argv[], uint64_t mem_cap,
                      const invfs_helper_sandbox *sb,
                      const char *work_dir,
                      int no_path_search,
                      char *out, size_t out_cap,
                      uint64_t timeout_ms)
{
    (void)argv; (void)mem_cap; (void)sb; (void)work_dir;
    (void)no_path_search; (void)out; (void)out_cap; (void)timeout_ms;
    return -1;
}
#endif /* !_WIN32 */
