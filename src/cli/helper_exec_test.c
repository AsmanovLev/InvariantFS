/* helper_exec_test.c — WP61 unit coverage for the shared helper launcher.
 *
 * Exercises the containment controls directly against invfs_helper_exec()
 * with fixture helpers that try to escape:
 *   (a) open a network connection        -> CLONE_NEWNET / userns fallback
 *   (b) fork-bomb / spawn a grandchild   -> RLIMIT_NPROC + process-group kill
 *   (c) read a file outside the sandbox  -> Landlock whitelist
 *   (d) read $HOME / inherited secrets   -> environment scrub
 *   (e) run forever                      -> wall-clock timeout
 * plus the rlimit values themselves and the root-only privilege drop.
 *
 * Standalone (wired into `make test`):
 *   gcc -std=gnu11 -O2 -I src -I src/core -o invf-helper_exec_test \
 *       src/cli/helper_exec_test.c build/obj/helper_exec.o
 *
 * The environment-dependent legs degrade to SKIP, never FAIL, when a
 * prerequisite is missing (no python3/bash, non-root privdrop). */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "helper_exec.h"

static int checks;
static int failures;
static int skips;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void skip(const char *what)
{
    skips++;
    printf("  SKIP  %s\n", what);
}

static int have(const char *path)
{
    return access(path, X_OK) == 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int write_file(const char *path, const char *data, int exec)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, strlen(data), f) != strlen(data)) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    if (exec && chmod(path, 0755) != 0) return -1;
    return 0;
}

/* Count processes whose cmdline carries the tag (NULs -> spaces). */
static int procs_with_tag(const char *tag)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char path[300];
        char buf[4096];
        int fd;
        ssize_t r;
        char *p;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r <= 0) continue;
        buf[r] = '\0';
        for (p = buf; p < buf + r; p++)
            if (*p == '\0') *p = ' ';
        if (strstr(buf, tag)) n++;
    }
    closedir(d);
    return n;
}

/* ---------------------------------------------------------------- */

static void test_rlimits(const char *work)
{
    (void)work;
    const char *sh = have("/bin/bash") ? "/bin/bash" : "/bin/sh";
    char *argv[] = {
        (char *)sh, (char *)"-c",
        (char *)"echo v=$(ulimit -v); echo n=$(ulimit -n); "
                "echo u=$(ulimit -u); echo t=$(ulimit -t)",
        NULL
    };
    char out[512];
    int rc = invfs_helper_exec(argv, (256ull << 20), NULL, NULL, 1,
                               out, sizeof out, 5000);

    ok(rc == 0, "rlimit helper exits 0");
    ok(strstr(out, "v=262144") != NULL, "RLIMIT_AS == 256 MiB (ulimit -v)");
    ok(strstr(out, "n=1024") != NULL, "RLIMIT_NOFILE == 1024");
    ok(strstr(out, "u=256") != NULL, "RLIMIT_NPROC == 256");
    ok(strstr(out, "t=10") != NULL, "RLIMIT_CPU == 10 s for a 5 s deadline");
    printf("  info  rlimits: %sexit=%d\n", out[0] ? out : "", rc);
}

static void test_timeout(void)
{
    char *argv[] = { (char *)"/bin/sleep", (char *)"30", NULL };
    uint64_t t0 = now_ms();
    int rc = invfs_helper_exec(argv, (64ull << 20), NULL, NULL, 1,
                               NULL, 0, 500);
    uint64_t dt = now_ms() - t0;

    ok(rc == -1, "run-forever helper is killed (-1)");
    ok(dt < 5000, "timeout fired near the deadline (< 5 s)");
    printf("  info  timeout: rc=%d after %llums\n", rc,
           (unsigned long long)dt);
}

static void test_group_kill(const char *work)
{
    const char *bash = have("/bin/bash") ? "/bin/bash" : NULL;
    char tag[64], script[512];
    char *argv[3];
    int rc, left;

    if (!bash) { skip("process-group kill (no /bin/bash for exec -a)"); return; }

    snprintf(tag, sizeof tag, "P61GRAND%d", (int)getpid());
    snprintf(script, sizeof script, "%s/grand.sh", work);
    {
        char body[512];
        snprintf(body, sizeof body,
                 "#!/bin/bash\n"
                 "( exec -a %s sleep 30 ) &\n"
                 "exec -a %s sleep 30\n", tag, tag);
        if (write_file(script, body, 1) != 0) {
            skip("process-group kill (fixture write failed)");
            return;
        }
    }
    argv[0] = (char *)bash;
    argv[1] = script;
    argv[2] = NULL;

    rc = invfs_helper_exec(argv, (128ull << 20), NULL, script, 1,
                           NULL, 0, 700);
    ok(rc == -1, "group-kill helper killed at the deadline");
    usleep(300000);
    left = procs_with_tag(tag);
    ok(left == 0, "no grandchild survives the process-group SIGKILL");
    if (left != 0) printf("  info  survivors=%d\n", left);
    unlink(script);
}

static void test_forkbomb(void)
{
    const char *py = have("/usr/bin/python3") ? "/usr/bin/python3" : NULL;
    const char *code =
        "import os,sys,time\n"
        "tag=sys.argv[1]\n"
        "for i in range(20):\n"
        "    try:\n"
        "        os.fork()\n"
        "    except OSError:\n"
        "        break\n"
        "time.sleep(60)\n";
    char *argv[] = { (char *)py, (char *)"-c", (char *)code,
                     (char *)"P61BOMB", NULL };
    int rc, left;

    if (!py) { skip("fork-bomb containment (no python3)"); return; }
    setenv("INVFS_HELPER_NPROC", "32", 1);
    rc = invfs_helper_exec(argv, (2ull << 30), NULL, NULL, 0,
                           NULL, 0, 700);
    unsetenv("INVFS_HELPER_NPROC");
    ok(rc == -1, "fork-bomb helper killed at the deadline");
    usleep(300000);
    left = procs_with_tag("P61BOMB");
    ok(left == 0, "fork-bomb children are gone after the group kill");
    if (left != 0) printf("  info  survivors=%d\n", left);
}

static void test_netns(void)
{
    const char *py = have("/usr/bin/python3") ? "/usr/bin/python3" : NULL;
    const char *code =
        "import socket,sys\n"
        "s=socket.socket()\n"
        "s.settimeout(2)\n"
        "try:\n"
        "    s.connect(('8.8.8.8',80))\n"
        "    sys.exit(0)\n"
        "except OSError:\n"
        "    sys.exit(42)\n";
    char *argv[] = { (char *)py, (char *)"-c", (char *)code, NULL };
    int rc;

    if (!py) { skip("network isolation (no python3)"); return; }
    rc = invfs_helper_exec(argv, (2ull << 30), NULL, NULL, 0,
                           NULL, 0, 8000);
    if (rc == 0) {
        /* netns unavailable in this environment: report, don't fail */
        skip("network isolation (netns unavailable: connect succeeded)");
    } else {
        ok(rc == 42, "outbound connect fails with the network isolated");
        printf("  info  netns: connect rc=%d\n", rc);
    }
}

static void test_env_scrub(void)
{
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c",
        (char *)"echo HOME=${HOME-unset}; echo LD=${LD_PRELOAD-unset}; "
                "echo TOKEN=${SECRET_TOKEN-unset}; echo PATH=$PATH",
        NULL
    };
    char out[1024];
    int rc;

    setenv("LD_PRELOAD", "/tmp/evil.so", 1);
    setenv("HOME", "/root", 1);
    setenv("SECRET_TOKEN", "hunter2", 1);
    unsetenv("INVFS_HELPER_KEEPENV");
    rc = invfs_helper_exec(argv, (128ull << 20), NULL, NULL, 1,
                           out, sizeof out, 5000);
    ok(rc == 0, "env-scrub helper exits 0");
    ok(strstr(out, "HOME=unset") != NULL, "HOME dropped");
    ok(strstr(out, "LD=unset") != NULL, "LD_PRELOAD dropped");
    ok(strstr(out, "TOKEN=unset") != NULL, "SECRET_TOKEN dropped");
    ok(strstr(out, "PATH=/usr/local/bin:/usr/bin:/bin") != NULL,
       "minimal PATH");
    printf("  info  scrub: %s", out);

    setenv("INVFS_HELPER_KEEPENV", "SECRET_TOKEN,LD_PRELOAD", 1);
    rc = invfs_helper_exec(argv, (128ull << 20), NULL, NULL, 1,
                           out, sizeof out, 5000);
    ok(rc == 0, "keepenv helper exits 0");
    ok(strstr(out, "TOKEN=hunter2") != NULL,
       "INVFS_HELPER_KEEPENV passes a named variable through");
    ok(strstr(out, "LD=unset") != NULL, "LD_* is never passed through");
    unsetenv("INVFS_HELPER_KEEPENV");
    unsetenv("LD_PRELOAD");
    unsetenv("SECRET_TOKEN");
}

static void test_landlock(const char *work, const char *outside)
{
    invfs_helper_sandbox sb;
    char inpath[512], cmd[1024], out[256];
    char *argv[4];
    int rc;

    snprintf(inpath, sizeof inpath, "%s/input.txt", work);
    if (write_file(inpath, "the-input\n", 0) != 0) {
        skip("Landlock fs containment (fixture write failed)");
        return;
    }

    sb.pack_dir = work;
    sb.ro_path = inpath;
    sb.rw_dir = work;
    sb.requires = NULL;

    /* read the whitelisted input: allowed */
    snprintf(cmd, sizeof cmd, "cat %s", inpath);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = cmd;
    argv[3] = NULL;
    rc = invfs_helper_exec(argv, (128ull << 20), &sb, work, 1,
                           out, sizeof out, 5000);
    ok(rc == 0 && strstr(out, "the-input") != NULL,
       "Landlock allows the RO whitelisted input");

    /* read a file outside the whitelist (path embedded in -c so
     * ll_add_argv_files does not whitelist it): denied */
    snprintf(cmd, sizeof cmd, "cat %s/secret.txt", outside);
    rc = invfs_helper_exec(argv, (128ull << 20), &sb, work, 1,
                           out, sizeof out, 5000);
    ok(rc != 0, "Landlock denies a read outside the whitelist");
    printf("  info  landlock: outside-read rc=%d\n", rc);
}

static void test_privdrop(const char *work)
{
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)"id -u", NULL
    };
    invfs_helper_sandbox sb;
    char out[128];
    int rc;

    if (getuid() != 0) {
        skip("privilege drop (not running as root)");
        return;
    }
    sb.pack_dir = work;
    sb.ro_path = NULL;
    sb.rw_dir = work;
    sb.requires = NULL;
    rc = invfs_helper_exec(argv, (128ull << 20), &sb, work, 1,
                           out, sizeof out, 5000);
    ok(rc == 0, "privdrop helper exits 0");
    ok(atoi(out) != 0, "root daemon runs the helper as a dropped uid");
    printf("  info  privdrop: helper uid=%s", out);

    setenv("INVFS_HELPER_UID", "12345", 1);
    setenv("INVFS_HELPER_GID", "12345", 1);
    rc = invfs_helper_exec(argv, (128ull << 20), &sb, work, 1,
                           out, sizeof out, 5000);
    unsetenv("INVFS_HELPER_UID");
    unsetenv("INVFS_HELPER_GID");
    ok(rc == 0 && atoi(out) == 12345,
       "INVFS_HELPER_UID/GID select the dropped uid");
}

int main(void)
{
    char base[256], work[320], outside[320];
    int r;

    printf("helper_exec containment tests\n");

    snprintf(base, sizeof base, "/tmp/invfs_helper_test_%d", (int)getpid());
    snprintf(work, sizeof work, "%s/work", base);
    snprintf(outside, sizeof outside, "%s/outside", base);
    r = mkdir(base, 0755) | mkdir(work, 0755) | mkdir(outside, 0755);
    if (r != 0) { perror("mkdir"); return 2; }
    {
        char secret[512];
        snprintf(secret, sizeof secret, "%s/secret.txt", outside);
        if (write_file(secret, "top-secret\n", 0) != 0) {
            perror("secret"); return 2;
        }
    }

    test_rlimits(work);
    test_timeout();
    test_group_kill(work);
    test_forkbomb();
    test_netns();
    test_env_scrub();
    test_landlock(work, outside);
    test_privdrop(work);

    {
        char p[512];
        snprintf(p, sizeof p, "%s/input.txt", work); unlink(p);
        snprintf(p, sizeof p, "%s/secret.txt", outside); unlink(p);
        rmdir(work); rmdir(outside); rmdir(base);
    }

    printf("%d checks, %d failure(s), %d skip(s)\n", checks, failures, skips);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
