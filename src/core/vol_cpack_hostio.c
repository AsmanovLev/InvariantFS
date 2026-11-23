/* vol_cpack_hostio.c — ABI v3 host I/O for containerpacks (ADR-015).
 *
 * See vol_cpack_hostio.h for the contract. This file holds the two halves:
 *
 *   1. the PROVIDER: `ivpack_host_io` over the core's ranged read path
 *      (vol_read_range on the container's own inode). The read side is what
 *      removes the pin; the write side is deliberately absent (see below).
 *
 *   2. the DISPATCHER: load the pack's lib<name>.so in a contained forked
 *      child and call ivpack_container_cmd with `io` filled. The child is
 *      given exactly the containment the exec path gives a helper, so pack
 *      code still never runs in the daemon.
 *
 * WHY pwrite IS NULL IN PHASE 1 (a deliberate deviation from ADR-015's
 * "pread -> vol_read_range, pwrite -> vol_write_range"): the output operands
 * of a container command (out_path/recipe_path/mbr_dir) are still paths, and
 * the core has to inspect every one of them before the commit -- the size
 * guard prices the members, the map validator parses the map, the commit
 * needs the recipe in RAM. A pack that could pwrite its recipe into the
 * volume would have to be given an inode the core then reads back, which is
 * the same bytes through more machinery and no less RAM. So the host leaves
 * io->pwrite NULL, which the ABI documents as "not provided" (never a stub
 * that always fails). The read side is where the 2x-scratch and the
 * un-materializable container actually live.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifndef _WIN32

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/ivpack_api.h"   /* ivpack_host_io + the v3 gate */
#include "vol_cpack_hostio.h"
#include "helper_exec.h"
#include "ivpack_loader.h"
#include "volume_internal.h"

/* The ABI level that carries ivpack_host_io. Kept as a named constant so the
 * gate reads the same way it is documented (desc->api_version >= 3 AND the
 * pack opted in with IVPACK_F_HOST_IO). */
#define CPACK_HOSTIO_ABI 3

/* 64 MiB per callback read: vol_read_range returns int, and the core's own
 * member funnel (cpack_member_read) uses the same ceiling. A pack asking for
 * more is served in several calls, which is legal (a short read is allowed
 * only at EOF, and these are not short -- each call returns what it was
 * asked for). */
#define CPACK_HOSTIO_WINDOW (64u << 20)

/* ------------------------------------------------------------------ */
/* the provider                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    invfs_volume *v;
    uint64_t      inode;
    uint64_t      size;
    uint64_t      reads;
    uint64_t      bytes;
} cpack_hostio_handle;

/* The current decomposition's handle. Thread-local, and set by the sweep
 * before it forks: the forked child inherits the value, which is exactly how
 * the callbacks reach the pack (a function pointer cannot cross execve, but
 * a fork copies both the pointer and this state). */
static __thread cpack_hostio_handle g_io;
static __thread int g_io_on;

static ssize_t hostio_pread(void *user, void *buf, size_t n, uint64_t off)
{
    cpack_hostio_handle *h = (cpack_hostio_handle *)user;
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;

    if (!h || !h->v || !h->inode) return -1;
    if (off >= h->size) return 0;
    if (n > h->size - off) n = (size_t)(h->size - off);
    while (done < n) {
        size_t want = n - done;
        int got;
        if (want > CPACK_HOSTIO_WINDOW) want = CPACK_HOSTIO_WINDOW;
        got = vol_read_range(h->v, h->inode, off + done, want, dst + done);
        if (got <= 0 || (size_t)got != want) return -1;
        done += want;
    }
    h->reads++;
    h->bytes += n;
    return (ssize_t)n;
}

int cpack_hostio_begin(invfs_volume *v, uint64_t inode, uint64_t size)
{
    const char *e = getenv("INVFS_CPACK_HOSTIO");

    if (e && *e == '0') return -1;
    if (!v || !inode || !size) return -1;
    memset(&g_io, 0, sizeof g_io);
    g_io.v = v;
    g_io.inode = inode;
    g_io.size = size;
    g_io_on = 1;
    return 0;
}

void cpack_hostio_end(void)
{
    if (getenv("INVFS_DEBUG_PACKS") && g_io_on)
        fprintf(stderr, "[cpack-t] host_io: %llu read(s), %llu byte(s)\n",
                (unsigned long long)g_io.reads,
                (unsigned long long)g_io.bytes);
    g_io_on = 0;
    memset(&g_io, 0, sizeof g_io);
}

int cpack_hostio_active(void) { return g_io_on; }

void cpack_hostio_stats(uint64_t *reads, uint64_t *bytes)
{
    if (reads) *reads = g_io.reads;
    if (bytes) *bytes = g_io.bytes;
}

/* ------------------------------------------------------------------ */
/* pack support probe (parent side, cached)                            */
/* ------------------------------------------------------------------ */

/* The .so a directory pack carries its plugin half in. Same name the Makefile
 * builds (lib<name>.so) and the one the daemon's plugin pool looks for
 * (src/core/vol_cpack.c, invfs_codec_pack_cmd). */
static int hostio_soname(const invfs_codec *c, char *buf, size_t cap)
{
    const char *hd = invfs_codec_pack_host_dir(c);
    int n;

    if (!c || !c->name || !hd || !*hd) return -1;
    /* an .ivpack archive would have to be unpacked to load its .so; the
     * archive path keeps the exec flow, which fexecve's the member */
    if (ivpack_is_archive_file(hd)) return -1;
    n = snprintf(buf, cap, "%s/lib%s.so", hd, c->name);
    if (n < 0 || (size_t)n >= cap) return -1;
    if (access(buf, R_OK) != 0) return -1;
    return 0;
}

/* Probe outcome, cached by file identity so a sweep that decomposes a thousand
 * containers probes once. -1 = cannot; 0 = cannot (recorded); 1 = can. */
typedef struct {
    char  soname[1024];
    dev_t dev;
    ino_t ino;
    int   ok;
} hostio_probe;

#define HOSTIO_PROBE_MAX 8
static hostio_probe g_probe[HOSTIO_PROBE_MAX];
static size_t g_probe_n;

/* Probe exit codes (child -> parent). */
#define HIO_PROBE_NO   20   /* no v3 host-I/O descriptor */
#define HIO_PROBE_DL   21   /* dlopen failed */
#define HIO_PROBE_SYM  22   /* missing exports */
#define HIO_PROBE_OK   0

static int hostio_probe_child(const char *soname)
{
    void *h;
    ivpack_get_desc_fn get_desc;
    ivpack_container_cmd_fn cmdfn;
    const ivpack_desc *d;

    h = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (!h) _exit(HIO_PROBE_DL);
    get_desc = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
    cmdfn = (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
    if (!get_desc || !cmdfn) _exit(HIO_PROBE_SYM);
    d = get_desc();
    if (!d || d->api_version < CPACK_HOSTIO_ABI ||
        !(d->flags & IVPACK_F_HOST_IO))
        _exit(HIO_PROBE_NO);
    _exit(HIO_PROBE_OK);
    return HIO_PROBE_OK; /* unreachable */
}

static int hostio_wait(pid_t pid, int *status, long ms)
{
    long waited = 0;

    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (waited >= ms) {
            kill(pid, SIGKILL);
            while (waitpid(pid, status, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
        usleep(20000);
        waited += 20;
    }
}

int cpack_hostio_pack_supported(const invfs_codec *c)
{
    char soname[1024];
    struct stat st;
    size_t i;
    pid_t pid;
    int status = 0;
    const char *e;

    /* An operator opt-out, and the A/B control leg of the zero-scratch gate:
     * with INVFS_CPACK_HOSTIO=0 the sweep materializes the container exactly as
     * it did before this WP, which is how the report can say that the pin-free
     * run really used the callbacks and not a path. */
    e = getenv("INVFS_CPACK_HOSTIO");
    if (e && *e == '0') return 0;
    if (hostio_soname(c, soname, sizeof soname) != 0) return 0;
    if (stat(soname, &st) != 0) return 0;
    for (i = 0; i < g_probe_n; i++) {
        if (g_probe[i].dev == st.st_dev && g_probe[i].ino == st.st_ino &&
            strcmp(g_probe[i].soname, soname) == 0)
            return g_probe[i].ok;
    }
    /* Probe in a forked child: a .so whose constructors crash or hang must
     * take the child, never the sweep (the in-process loader path makes the
     * same argument, vol_plugin_inproc.c). */
    fflush(NULL);
    pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        hostio_probe_child(soname);
        _exit(HIO_PROBE_DL);
    }
    if (hostio_wait(pid, &status, 10000) != 0) return 0;
    if (!WIFEXITED(status)) return 0;
    if (g_probe_n < HOSTIO_PROBE_MAX) {
        hostio_probe *p = &g_probe[g_probe_n++];
        snprintf(p->soname, sizeof p->soname, "%s", soname);
        p->dev = st.st_dev;
        p->ino = st.st_ino;
        p->ok = (WEXITSTATUS(status) == HIO_PROBE_OK);
        if (getenv("INVFS_DEBUG_PACKS"))
            fprintf(stderr, "[cpack] host_io probe: %s -> %s\n", soname,
                    p->ok ? "v" "3 pack" : "not host-io capable");
        return p->ok;
    }
    return WEXITSTATUS(status) == HIO_PROBE_OK;
}

/* ------------------------------------------------------------------ */
/* dispatch (child side)                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *soname;
    const char *idx;
    const char *dir;
    const char *recipe;
    const char *out;
    int         cmd;         /* ABI command number, 1..5 */
    int         rc;
} hostio_job;

/* Runs in the contained child, inside the fork the glue in ivpack_impl.h
 * makes around the command body. Returns the pack's status; the parent maps
 * HIO_NOV3 to "could not dispatch". */
#define HIO_NOV3 200

static int hostio_body(void *user)
{
    hostio_job *j = (hostio_job *)user;
    ivpack_host_io io;
    ivpack_container_args a;
    void *h;
    ivpack_get_desc_fn get_desc;
    ivpack_container_cmd_fn cmdfn;
    const ivpack_desc *d;
    int rc;

    h = dlopen(j->soname, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (getenv("INVFS_DEBUG_PACKS"))
            fprintf(stderr, "[cpack] host_io: dlopen(%s): %s\n", j->soname,
                    dlerror());
        return HIO_NOV3;
    }
    get_desc = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
    cmdfn = (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
    d = get_desc ? get_desc() : NULL;
    if (!cmdfn || !d || d->api_version < CPACK_HOSTIO_ABI ||
        !(d->flags & IVPACK_F_HOST_IO))
        return HIO_NOV3;

    memset(&io, 0, sizeof io);
    io.user = &g_io;
    io.size = g_io.size;
    io.pread = hostio_pread;
    io.pwrite = NULL;       /* phase 1: outputs are still paths (see top) */

    memset(&a, 0, sizeof a);
    a.cmd = j->cmd;
    a.in_path = NULL;       /* the bytes come through io */
    a.out_path = j->out;
    a.recipe_path = j->recipe;
    a.mbr_dir = j->dir;
    a.extract_idx = j->idx;
    a.self_path = j->soname;
    a.io = &io;

    rc = cmdfn(&a);
    fflush(NULL);
    /* the glue returns the child's status (0/1/3/5); a negative value means
     * the pack refused the ARGUMENT SHAPE, which for a v3 pack with io means
     * the ABI is inconsistent -- report it as a failure, never as success */
    if (rc < 0) return (int)(-rc) & 0xff;
    return rc & 0xff;
}

int cpack_hostio_cmd(const invfs_codec *c, int cmd, const char *idx,
                     const char *dir, const char *recipe, const char *out)
{
    hostio_job j;
    invfs_helper_sandbox sb;
    char soname[1024];
    char rwbuf[4096];
    const char *rw = NULL;
    int rc;

    if (!g_io_on) return -1;
    if (hostio_soname(c, soname, sizeof soname) != 0) return -1;
    /* The same RW directory the exec path grants (invfs_codec_pack_cmd's
     * sb_dirname(out)): the command's output lives under it, and for the
     * batch-less member path that is the member dir, never its parent. */
    rw = sb_dirname(rwbuf, sizeof rwbuf, out);
    if (!rw) return -1;

    j.soname = soname;
    j.idx = idx;
    j.dir = dir;
    j.recipe = recipe;
    j.out = out;
    j.rc = -1;
    switch (cmd) {
    case INVFS_PACK_CMD_ENUMERATE: j.cmd = 1; break;
    case INVFS_PACK_CMD_EXTRACT:   j.cmd = 2; break;
    case INVFS_PACK_CMD_STRIP:     j.cmd = 3; break;
    case INVFS_PACK_CMD_REBUILD:   j.cmd = 4; break;
    case INVFS_PACK_CMD_MAP:       j.cmd = 5; break;
    default: return -1;   /* batch/grandchild verbs have no v3 wire yet */
    }

    /* The containment the exec path would apply: the pack dir RO, the scratch
     * dir RW, and NO read-only input path -- there is no input file, that is
     * the whole point. The callbacks reach the volume over the inherited
     * descriptor, which Landlock cannot and need not gate. */
    sb.pack_dir = invfs_codec_pack_host_dir(c);
    sb.ro_path = recipe;      /* REBUILD reads the recipe; harmless otherwise */
    sb.rw_dir = rw;
    sb.requires = invfs_codec_pack_def(c) ? invfs_codec_pack_def(c)->requires
                                          : NULL;

    rc = invfs_helper_run_child(hostio_body, &j, tool_mem_cap_for(c), &sb, rw,
                                soname, 0);
    if (rc < 0) {
        if (getenv("INVFS_DEBUG_PACKS"))
            fprintf(stderr, "[cpack] host_io: %s cmd=%d failed (fork/crash/"
                            "timeout)\n", c->name, cmd);
        return -1;
    }
    if (rc == HIO_NOV3) {
        if (getenv("INVFS_DEBUG_PACKS"))
            fprintf(stderr, "[cpack] host_io: %s does not declare ABI v%d "
                            "host I/O\n", c->name, CPACK_HOSTIO_ABI);
        return -1;
    }
    return rc;
}

#else  /* _WIN32: containerpacks are parked there */
#include "vol_cpack_hostio.h"
#include "volume_internal.h"

int cpack_hostio_pack_supported(const invfs_codec *c) { (void)c; return 0; }
int cpack_hostio_begin(invfs_volume *v, uint64_t inode, uint64_t size)
{ (void)v; (void)inode; (void)size; return -1; }
void cpack_hostio_end(void) { }
int cpack_hostio_active(void) { return 0; }
void cpack_hostio_stats(uint64_t *reads, uint64_t *bytes)
{ if (reads) *reads = 0; if (bytes) *bytes = 0; }
int cpack_hostio_cmd(const invfs_codec *c, int cmd, const char *idx,
                     const char *dir, const char *recipe, const char *out)
{ (void)c; (void)cmd; (void)idx; (void)dir; (void)recipe; (void)out; return -1; }
#endif /* !_WIN32 */
