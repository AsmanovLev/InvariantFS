/* ivpack_impl.h — shared implementation glue for containerpack .so plugins
 * (ADR-007). Included at the bottom of a pack's single .c file, right before
 * its main(), together with "ivpack_api.h".
 *
 * Why this exists
 * ---------------
 * A containerpack .c is written as a CLI helper: every refusal path is a bare
 * exit(3) (decline) or exit(1) (die), estimate reports its number by printing
 * it, and some packs keep an 8 MiB streaming window in a file-scope global
 * that main() allocates. Loading that code with dlmopen() into a long-lived
 * worker means the FIRST declined image would take the whole worker down and
 * the printed estimate would land in the daemon's stdout.
 *
 * So the glue below preserves the CLI contract exactly by running each
 * command in a fork()ed child of the worker:
 *
 *   - exit()/die()/decline() kill the child only; its exit status becomes the
 *     plugin's return code, so `3` still means "decline" and `1` "error",
 *     byte-for-byte like the CLI.
 *   - stdout is redirected into a pipe for ESTIMATE and parsed there, so the
 *     number the .so reports is the number the CLI would have printed.
 *   - the child inherits the pack's globals, so main()'s window allocation is
 *     reproduced by IVPACK_GLUE()/IVPACK_UNGLUE() around the call.
 *
 * Cost: one fork() (~50-100 us) and no execve(), no ELF load, no dynamic
 * linking, no interpreter start — which is the part ADR-007 actually removes.
 * A pack that sets no_exit = 1 in its descriptor can later be called
 * in-process; nothing does yet.
 *
 * Portability: the packs are built both as CLI (`cc -std=c11 -Wall -Wextra
 * -Werror pack.c`) and as .so (`-fPIC -shared -DIVPACK_SHARED_LIB`), so this
 * header stays inside C11 + POSIX and needs no -ldl and no _GNU_SOURCE.
 */
#ifndef IVPACK_IMPL_H
#define IVPACK_IMPL_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivpack_api.h"

/* The helpers below are a menu, not a checklist: a pack uses the ones it
 * needs. Marked unused so `-Wall -Wextra -Werror` stays quiet in the packs
 * that do not (a CLI pack build never touches them at all, the whole header
 * is inert without IVPACK_SHARED_LIB). */
#if defined(__GNUC__)
#define IVPACK_UNUSED __attribute__((__unused__))
#else
#define IVPACK_UNUSED
#endif

/* The pack's own exit-code convention (all containerpacks agree). */
#define IVPACK_RC_OK       0
#define IVPACK_RC_ERROR    1
#define IVPACK_RC_USAGE    2
#define IVPACK_RC_DECLINE  3

/* ------------------------------------------------------------------ */
/* fork-guarded command run                                            */
/* ------------------------------------------------------------------ */

/* Run fn(user) in a forked child. Returns the child's exit status, or:
 *   -1  fork failed
 *   -4  the child was killed by a signal (128+signo is reported instead when
 *       it exited normally; a signal means the pack crashed — the worker
 *       survives, which is the whole point of the fork)
 * When capture != NULL, the child's stdout is redirected into a pipe and
 * captured (NUL-terminated, truncated to cap-1). */
static int IVPACK_UNUSED ivpack_run_child(int (*fn)(void *), void *user, char *capture,
                            size_t cap)
{
    int pipefd[2];
    pid_t pid;
    int status = 0;
    int rc;

    if (capture) {
        if (pipe(pipefd) != 0) return -1;
    }
    fflush(NULL);                          /* do not duplicate our own stdio */
    pid = fork();
    if (pid < 0) {
        if (capture) { close(pipefd[0]); close(pipefd[1]); }
        return -1;
    }
    if (pid == 0) {
        if (capture) {
            int devnull;
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(IVPACK_RC_ERROR);
            close(pipefd[1]);
            /* A pack's diagnostics belong to the CLI user; inside a daemon they
             * would only pollute its log, and the decline reason is already
             * implied by the status code. */
            devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                if (devnull != STDERR_FILENO) {
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
            }
        }
        rc = fn(user) & 0xff;
        /* _exit() bypasses stdio cleanup, and a pack reports its estimate by
         * PRINTING it -- so the redirected stdout has to be flushed by hand or
         * the number dies in the child's buffer. */
        fflush(NULL);
        _exit(rc);
    }
    if (capture) {
        /* Drain concurrently with the child: reading only after waitpid() would
         * deadlock as soon as a pack prints more than the pipe buffer holds. */
        size_t got = 0;
        ssize_t n;
        close(pipefd[1]);
        if (cap) capture[0] = '\0';
        while ((n = read(pipefd[0], capture + got,
                         (got + 1 < cap) ? cap - 1 - got : 1024)) > 0) {
            if (got + 1 < cap) {
                got += (size_t)n;
                capture[got] = '\0';
            }
        }
        close(pipefd[0]);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -4;
}

/* Parse the first unsigned decimal in s (the CLI estimate's stdout). */
static int IVPACK_UNUSED ivpack_parse_u64(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int digits = 0;

    if (!s || !out) return -1;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10ull + (unsigned long long)(*s - '0'); s++; digits++; }
    if (!digits) return -1;
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* glue macros                                                         */
/* ------------------------------------------------------------------ */

/* The five canonical cmd_* adapters, for packs whose signatures are
 *   int|void cmd_enumerate(in, out)      int|void cmd_strip(in, out)
 *   int|void cmd_extract(in, idx, out)   int|void cmd_map(in, out)
 *   int|void cmd_rebuild(recipe, dir, out)
 * A pack that differs (p7z takes argv0, xfs returns void everywhere) writes
 * its own five one-line adapters instead of using this macro. */
#define IVPACK_CANON_CALLS(PREFIX)                                           \
static int PREFIX##_iv_call_enumerate(const ivpack_container_args *a)         \
{ return (int)cmd_enumerate(a->in_path, a->out_path); }                      \
static int PREFIX##_iv_call_extract(const ivpack_container_args *a)           \
{ return (int)cmd_extract(a->in_path, a->extract_idx, a->out_path); }        \
static int PREFIX##_iv_call_strip(const ivpack_container_args *a)             \
{ return (int)cmd_strip(a->in_path, a->out_path); }                          \
static int PREFIX##_iv_call_rebuild(const ivpack_container_args *a)           \
{ return (int)cmd_rebuild(a->recipe_path, a->mbr_dir, a->out_path); }        \
static int PREFIX##_iv_call_map(const ivpack_container_args *a)               \
{ return (int)cmd_map(a->in_path, a->out_path); }


/* Per-call global setup/teardown, mirroring what main() does. A pack without
 * globals uses IVPACK_GLUE_NONE. */
#define IVPACK_GLUE_NONE()      do { } while (0)

/* Declare the three exported entry points for a pack whose cmd_* take the
 * canonical containerpack signatures:
 *     int|void cmd_enumerate(in, out)
 *     int|void cmd_extract(in, idx, out)
 *     int|void cmd_strip(in, out)
 *     int|void cmd_rebuild(recipe, dir, out)
 *     int|void cmd_map(in, out)
 *     int|void cmd_estimate(in)
 * GLUE is a statement run around each call in the child (window alloc).
 * EST_STMT is the estimate call as an `int rc = ...;`-shaped statement, so a
 * void-returning pack (xfs) can pass `cmd_estimate(a->in); int rc = 0;`.
 *
 * The command bodies run in the forked child; see the header comment. */
#define IVPACK_DEFINE_CONTAINER_CMD(PREFIX, GLUE)                            \
static int PREFIX##_iv_body(void *user)                                      \
{                                                                            \
    const ivpack_container_args *a = (const ivpack_container_args *)user;    \
    int rc = IVPACK_RC_USAGE;                                                \
    GLUE;                                                                    \
    switch (a->cmd) {                                                        \
    case 1: rc = PREFIX##_iv_call_enumerate(a); break;                       \
    case 2: rc = PREFIX##_iv_call_extract(a); break;                         \
    case 3: rc = PREFIX##_iv_call_strip(a); break;                           \
    case 4: rc = PREFIX##_iv_call_rebuild(a); break;                         \
    case 5: rc = PREFIX##_iv_call_map(a); break;                             \
    default: return IVPACK_RC_USAGE;                                         \
    }                                                                        \
    return rc;                                                               \
}                                                                            \
                                                                             \
int ivpack_container_cmd(const ivpack_container_args *args)                  \
{                                                                            \
    if (!args)                                                               \
        return -IVPACK_RC_ERROR;                                             \
    switch (args->cmd) {                                                     \
    case 1:                                                                  \
    case 3:                                                                  \
        if (!args->in_path || !args->out_path)                               \
            return -IVPACK_RC_USAGE;                                         \
        break;                                                               \
    case 5:                                                                  \
        if ((!args->in_path && !args->recipe_path) || (!args->out_path && !args->out_buf)) \
            return -IVPACK_RC_USAGE;                                         \
        if (args->out_buf && args->out_cap > 0)                              \
            return PREFIX##_iv_call_map(args);                               \
        break;                                                               \
    case 2:                                                                  \
        if (!args->in_path || !args->extract_idx || !args->out_path)          \
            return -IVPACK_RC_USAGE;                                         \
        break;                                                               \
    case 4:                                                                  \
        if (!args->recipe_path || !args->mbr_dir || !args->out_path)          \
            return -IVPACK_RC_USAGE;                                         \
        break;                                                               \
    default:                                                                 \
        return -IVPACK_RC_USAGE;                                             \
    }                                                                        \
    return ivpack_run_child(PREFIX##_iv_body, (void *)args, NULL, 0);        \
}

/* The estimate export: capture the CLI's stdout in the child and report it
 * through ivpack_estimate_res. mbr_size carries the number the FS admits on
 * (sum of member usizes + the pack's margin), which is what the CLI prints
 * and what invfs_codec_pack_estimate() would have parsed. */
#define IVPACK_DEFINE_CONTAINER_ESTIMATE(PREFIX, GLUE, EST_STMT)             \
static int PREFIX##_iv_est_body(void *user)                                  \
{                                                                            \
    const char *a = (const char *)user;                                      \
    int rc = IVPACK_RC_ERROR;                                                \
    GLUE;                                                                    \
    EST_STMT;                                                                \
    return rc;                                                               \
}                                                                            \
                                                                             \
int ivpack_container_estimate(const char *in_path, ivpack_estimate_res *res) \
{                                                                            \
    char cap[64];                                                            \
    unsigned long long v = 0;                                                \
    int rc;                                                                  \
                                                                             \
    if (!in_path || !res) return -IVPACK_RC_ERROR;                           \
    memset(res, 0, sizeof *res);                                             \
    rc = ivpack_run_child(PREFIX##_iv_est_body, (void *)in_path, cap,        \
                          sizeof cap);                                       \
    if (rc != IVPACK_RC_OK) {                                                \
        res->eligible = 0;                                                   \
        return rc == IVPACK_RC_DECLINE ? IVPACK_RC_DECLINE : rc;             \
    }                                                                        \
    if (ivpack_parse_u64(cap, &v) != 0) {                                    \
        res->eligible = 0;                                                   \
        return -IVPACK_RC_ERROR;                                             \
    }                                                                        \
    res->eligible = 1;                                                       \
    res->mbr_size = (uint64_t)v;                                             \
    {                                                                        \
        FILE *f = fopen(in_path, "rb");                                      \
        if (f) {                                                             \
            long sz;                                                         \
            if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0)           \
                res->orig_size = (uint64_t)sz;                               \
            fclose(f);                                                       \
        }                                                                    \
    }                                                                        \
    return IVPACK_RC_OK;                                                     \
}

/* The descriptor. NAME/VERSION are strings; a pack that can be called without
 * the fork guard would pass flags |= 1 (none can today). */
#define IVPACK_DEFINE_DESC(VAR, NAME, VERSION)                               \
static const ivpack_desc VAR = {                                             \
    IVPACK_API_VERSION, NAME, VERSION, "containerpack", 0                    \
};                                                                           \
                                                                             \
const ivpack_desc *ivpack_get_desc(void) { return &VAR; }

#endif /* IVPACK_IMPL_H */
