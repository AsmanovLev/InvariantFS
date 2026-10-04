/* tool_scratch.c — the scratch DIRECTORY decision. See tool_scratch.h.
 *
 * The rule this file exists to enforce: the safe choice is the default.
 * A tmpfs scratch is genuinely right for a small transcoding job — the
 * intermediates are written once, read once, and tmpfs makes that cheap —
 * so tmpfs stays first in the preference order. What is not acceptable is
 * choosing a memory-backed root for a job whose size nobody looked at: the
 * containerpack sweep pins a whole container and extracts every member of
 * it, so a 3.5 GB rootfs container asks ~7 GB of scratch, and on a host
 * where /dev/shm and /tmp are both tmpfs that is RAM. The tmpfs fills, the
 * extract fails, the file silently falls through to the generic lane, and
 * in the field the same staging is what gets the sweep OOM-killed.
 *
 * So: the caller states how many bytes the job needs, this file picks a
 * root that can hold them, and if none can, it says so WITH THE NUMBERS
 * instead of letting the failure arrive later as an opaque ENOSPC.
 *
 * Sizing is deliberately conservative in one direction only. A root that
 * can hold the job is used (fast path preserved). A tmpfs is additionally
 * capped at INVFS_SCRATCH_TMPFS_MAX_FRAC (default 50%) of the
 * ALLOCATION-AWARE ceiling, not of statvfs: a tmpfs page is charged to the
 * writing cgroup, so on this platform a tmpfs can report gigabytes free
 * and still fail ENOSPC the moment the cgroup's memory.max is reached, and
 * a scratch that consumes every spare page is what OOM-kills the machine
 * and, with it, every other test run sharing it.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "tool_scratch.h"

#define SCRATCH_TMPFS_MAGIC 0x01021994UL

#define SCRATCH_MAX_ROOTS 8

/* Bytes left free on top of the demand. The pin, the member table, the
 * recipe, the map and the rebuilt output all land in the same directory,
 * and a scratch sized to the last byte of the demand is a scratch that
 * fails on the last file. 128 MiB, or INVFS_SCRATCH_MARGIN_MB. */
#define SCRATCH_MARGIN_MB_DEFAULT 128

/* Percent of the allocation-aware ceiling a tmpfs root may be offered. The
 * rest stays free so the staging is not the reason the host runs out. */
#define SCRATCH_TMPFS_PCT_DEFAULT 50

static uint64_t scratch_margin(void)
{
    const char *e = getenv("INVFS_SCRATCH_MARGIN_MB");
    long v = SCRATCH_MARGIN_MB_DEFAULT;

    if (e && *e) {
        v = strtol(e, NULL, 10);
        if (v < 0) v = 0;
    }
    return (uint64_t)v * 1024u * 1024u;
}

static unsigned scratch_tmpfs_pct(void)
{
    const char *e = getenv("INVFS_SCRATCH_TMPFS_MAX_FRAC");
    long v = SCRATCH_TMPFS_PCT_DEFAULT;

    if (e && *e) {
        v = strtol(e, NULL, 10);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
    }
    return (unsigned)v;
}

static void scratch_chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

/* UINT64_MAX = "no value here", which is how a cgroup that is unlimited
 * and a file that does not exist are both reported. */
static uint64_t scratch_read_u64(const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned long long v;

    if (!f) return UINT64_MAX;
    if (fscanf(f, "%llu", &v) != 1) v = UINT64_MAX;
    fclose(f);
    return (uint64_t)v;
}

static int scratch_finite(uint64_t v)
{
    /* cgroup v1 spells "unlimited" as a huge page count, not a keyword */
    return v != UINT64_MAX && v < (UINT64_MAX / 2);
}

/* headroom left under this process's cgroup memory ceiling, cgroup v2:
 * /proc/self/cgroup's "0::<path>" line names the directory under the v2
 * mount point. UINT64_MAX when no line names a cgroup with a finite limit. */
static uint64_t scratch_cgroup_v2_headroom(void)
{
    char line[512], rel[512], p[640];
    FILE *f = fopen("/proc/self/cgroup", "r");
    uint64_t best = UINT64_MAX;

    if (!f) return UINT64_MAX;
    while (fgets(line, sizeof line, f)) {
        uint64_t lim, cur, h;

        if (strncmp(line, "0::", 3) != 0) continue;
        snprintf(rel, sizeof rel, "%s", line + 3);
        scratch_chomp(rel);
        snprintf(p, sizeof p, "/sys/fs/cgroup%s/memory.max", rel);
        lim = scratch_read_u64(p);
        if (!scratch_finite(lim)) continue;      /* this cgroup: unlimited */
        snprintf(p, sizeof p, "/sys/fs/cgroup%s/memory.high", rel);
        h = scratch_read_u64(p);
        if (scratch_finite(h) && h < lim) lim = h;
        if (lim == 0) continue;                  /* charged to the parent */
        snprintf(p, sizeof p, "/sys/fs/cgroup%s/memory.current", rel);
        cur = scratch_read_u64(p);
        if (!scratch_finite(cur)) cur = 0;
        if (lim > cur && lim - cur < best) best = lim - cur;
    }
    fclose(f);
    return best;
}

/* the same question, cgroup v1: "N:controllers:/path", and only the
 * memory controller's hierarchy says anything about a tmpfs page. */
static uint64_t scratch_cgroup_v1_headroom(void)
{
    char line[512], p[640], path[400];
    FILE *f = fopen("/proc/self/cgroup", "r");
    uint64_t best = UINT64_MAX;

    if (!f) return UINT64_MAX;
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':'), *c2;
        uint64_t lim, cur;

        if (!c1) continue;
        c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        *c2 = '\0';
        if (!strstr(c1 + 1, "memory")) continue;
        snprintf(path, sizeof path, "%s", c2 + 1);
        scratch_chomp(path);
        snprintf(p, sizeof p, "/sys/fs/cgroup/memory%s/memory.limit_in_bytes",
                 path);
        lim = scratch_read_u64(p);
        if (!scratch_finite(lim)) continue;
        snprintf(p, sizeof p, "/sys/fs/cgroup/memory%s/memory.usage_in_bytes",
                 path);
        cur = scratch_read_u64(p);
        if (!scratch_finite(cur)) cur = 0;
        if (lim > cur && lim - cur < best) best = lim - cur;
    }
    fclose(f);
    return best;
}

static uint64_t scratch_cgroup_headroom(void)
{
    uint64_t v2 = scratch_cgroup_v2_headroom();
    uint64_t v1 = scratch_cgroup_v1_headroom();

    if (v1 < v2) v2 = v1;
    return v2;
}

/* MemAvailable, in bytes. 0 when /proc/meminfo cannot be read, which
 * callers read as "no memory ceiling could be established". */
static uint64_t scratch_mem_available(void)
{
    char key[64], unit[16];
    unsigned long long v;
    FILE *f = fopen("/proc/meminfo", "r");
    uint64_t r = 0;

    if (!f) return 0;
    /* the leading space is load-bearing: a %[ conversion does NOT skip
     * whitespace, so without it every key after the first line is read
     * as "\nMemFree" and never matches. */
    while (fscanf(f, " %63[^:]: %llu %15s", key, &v, unit) == 3) {
        if (strcmp(key, "MemAvailable") == 0) { r = (uint64_t)v * 1024u; break; }
    }
    fclose(f);
    return r;
}

uint64_t tool_scratch_headroom(const char *root, uint64_t *fs_avail,
                               uint64_t *alloc)
{
    struct statfs vfs;
    uint64_t fs, a = 0;

    if (fs_avail) *fs_avail = 0;
    if (alloc) *alloc = 0;
    /* statfs, not statvfs: the filesystem-type magic lives in struct statfs.
     * `struct statvfs` has no f_type member at all, so this was
     *     error: 'struct statvfs' has no member named 'f_type'
     * on every toolchain that actually compiles it -- it only built locally
     * by never being compiled there. statfs carries everything this needs:
     * f_type for the magic, f_bavail * f_bsize for the free space. */
    if (!root || !*root || statfs(root, &vfs) != 0) return 0;
    fs = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_bsize;
    if (fs_avail) *fs_avail = fs;
    if ((unsigned long)vfs.f_type != SCRATCH_TMPFS_MAGIC) return fs;

    /* memory-backed: statvfs answers "does the mount have room", which is
     * NOT the question. The page cache charge is per-cgroup, so the
     * ceiling is the smallest of the cgroup headroom and what the machine
     * says it has left. */
    {
        uint64_t cg = scratch_cgroup_headroom();
        uint64_t mem = scratch_mem_available();
        uint64_t offered;

        if (mem && mem < cg) cg = mem;
        if (cg == UINT64_MAX) return fs;      /* nothing bounds it: fs wins */
        a = cg * scratch_tmpfs_pct() / 100u;
        offered = a < fs ? a : fs;
        if (alloc) *alloc = a;
        return offered;
    }
}

/* is `dir` inside `root`? (a bare prefix match would let a root named
 * /dev/shx claim /dev/shm/...) */
static int helper_root_is(const char *root, const char *dir)
{
    size_t n = strlen(root);
    return strncmp(root, dir, n) == 0 && (dir[n] == '\0' || dir[n] == '/');
}

/* The root list, in preference order. tmpfs first on purpose: for the
 * small jobs that dominate (a JPEG through cjxl, a FLAC header) the scratch
 * really is faster in RAM, and the size check is what stops that preference
 * from becoming a memory incident. */
static int scratch_roots(char *buf, size_t cap, const char **out,
                         int max)
{
    static const char *def[] = { "/dev/shm", "/tmp", "/var/tmp" };
    const char *e = getenv("INVFS_SCRATCH_ROOTS");
    char *p, *save;
    int n = 0;

    if (e && *e && cap >= 2) {
        snprintf(buf, cap, "%s", e);
        for (p = strtok_r(buf, ":", &save); p && n < max;
             p = strtok_r(NULL, ":", &save))
            if (*p) out[n++] = p;
        if (n) return n;
    }
    for (n = 0; n < (int)(sizeof def / sizeof def[0]) && n < max; n++)
        out[n] = def[n];
    return n;
}

/* The one diagnostic an operator gets when the scratch cannot hold the
 * job. It names the demand, the margin, every root with the number that
 * ruled it out, and what to do about it -- a bare ENOSPC from a tmpfs has
 * already cost this project hours. */
static int scratch_refuse(const char **roots, int nroots, uint64_t need,
                          const char *forced)
{
    uint64_t margin = scratch_margin();
    char msg[2048];
    size_t m = 0;
    int i;

    m += (size_t)snprintf(msg + m, sizeof msg - m,
        "tool_tmpdir: REFUSED: this job needs %llu B of scratch and a "
        "%llu B margin, and no configured root can hold that.\n",
        (unsigned long long)need, (unsigned long long)margin);

    if (forced) {
        uint64_t fs = 0, a = 0, hr = tool_scratch_headroom(forced, &fs, &a);
        m += (size_t)snprintf(msg + m, sizeof msg - m,
            "  INVFS_TOOL_SCRATCH=%s: %llu B offered (statvfs %llu B%s)\n",
            forced, (unsigned long long)hr, (unsigned long long)fs,
            a ? ", allocation ceiling scaled" : "");
    }
    for (i = 0; i < nroots; i++) {
        uint64_t fs = 0, a = 0, hr = tool_scratch_headroom(roots[i], &fs, &a);
        m += (size_t)snprintf(msg + m, sizeof msg - m,
            "  %s: %llu B offered (statvfs %llu B%s)%s\n",
            roots[i], (unsigned long long)hr, (unsigned long long)fs,
            a ? ", allocation ceiling scaled" : "",
            (hr >= need + margin) ? " -- ENOUGH ROOM, but the directory "
                                    "could not be created" : "");
    }
    m += (size_t)snprintf(msg + m, sizeof msg - m,
        "  nothing is written; the file is left to its other lanes.\n"
        "  fix: point the scratch at a directory with room, e.g.\n"
        "    INVFS_TOOL_SCRATCH=/srv/invfs-scratch <command>\n"
        "  or widen the root list with INVFS_SCRATCH_ROOTS=/a:/b (a ':' "
        "separated list, searched in order), or lower\n"
        "  INVFS_SCRATCH_TMPFS_MAX_FRAC (currently %u%%) if the machine's "
        "memory is genuinely free.\n",
        scratch_tmpfs_pct());
    fputs(msg, stderr);
    return -1;
}

/* mkdtemp under `root` for a `need`-byte job, with the decision logged.
 * `skipped` is the pre-formatted list of roots that were passed over, so
 * the line says not just where the scratch went but what ruled the earlier
 * preferences out -- "say what decided it" is the whole point. */
static int scratch_make(const char *root, char *dir, size_t cap, uint64_t need,
                        int announce, const char *skipped)
{
    uint64_t fs = 0, a = 0, hr = tool_scratch_headroom(root, &fs, &a);
    int n = snprintf(dir, cap, "%s/invfs-tool-XXXXXX", root);

    if (n <= 0 || (size_t)n >= cap) return -1;
    if (mkdtemp(dir) == NULL) return -1;
    if (announce) {
        fprintf(stderr,
                "tool_tmpdir: need %llu B (+%llu B margin) -> %s "
                "(%llu B offered: statvfs %llu B%s)\n",
                (unsigned long long)need,
                (unsigned long long)scratch_margin(), dir,
                (unsigned long long)hr, (unsigned long long)fs,
                a ? ", allocation-aware tmpfs ceiling" : "");
        if (skipped && *skipped) fputs(skipped, stderr);
    }
    return 0;
}

/* one "  <root>: <offered> B offered (statvfs <n> B...)" line */
static void scratch_note(char *buf, size_t cap, const char *root)
{
    uint64_t fs = 0, a = 0, hr = tool_scratch_headroom(root, &fs, &a);
    size_t used = strlen(buf);

    if (used + 8 >= cap) return;
    snprintf(buf + used, cap - used,
             "  %s not offered: %llu B (statvfs %llu B%s)\n",
             root, (unsigned long long)hr, (unsigned long long)fs,
             a ? ", allocation-aware tmpfs ceiling applied" : "");
}

/* announce the choice when it is worth a line: a deviation from the first
 * preference is news, and INVFS_SCRATCH_VERBOSE asks for all of them. */
static int scratch_announce(int index)
{
    return index != 0 || getenv("INVFS_SCRATCH_VERBOSE") != NULL;
}

int tool_tmpdir(char *dir, size_t cap, uint64_t need)
{
    const char *forced = getenv("INVFS_TOOL_SCRATCH");
    const char *roots[SCRATCH_MAX_ROOTS];
    char rbuf[512], skipped[1024];
    int nroots, i;

    if (forced && *forced) {
        /* Explicit means explicit: no other root is searched. But it is
         * still SIZED -- an operator who points the scratch at a 100 MB
         * directory and hands the pipeline a 4 GB job is told so here, not
         * by an ENOSPC from the middle of an extract. */
        if (tool_scratch_headroom(forced, NULL, NULL) <
            need + scratch_margin())
            return scratch_refuse(NULL, 0, need, forced);
        return scratch_make(forced, dir, cap, need, 1, NULL);
    }

    nroots = scratch_roots(rbuf, sizeof rbuf, roots, SCRATCH_MAX_ROOTS);
    skipped[0] = 0;
    for (i = 0; i < nroots; i++) {
        if (tool_scratch_headroom(roots[i], NULL, NULL) <
            need + scratch_margin()) {
            scratch_note(skipped, sizeof skipped, roots[i]);
            continue;                     /* too small: not a candidate */
        }
        if (scratch_make(roots[i], dir, cap, need,
                         scratch_announce(i), skipped) == 0)
            return 0;
    }
    return scratch_refuse(roots, nroots, need, NULL);
}

/* write one carried file into `dir`; 0 on success */
static int scratch_put(const char *dir, const scratch_file *f)
{
    char p[640];
    FILE *o;
    int n = snprintf(p, sizeof p, "%s/%s", dir, f->name);

    if (n <= 0 || (size_t)n >= sizeof p) return -1;
    o = fopen(p, "wb");
    if (!o) return -1;
    if (f->len && fwrite(f->data, 1, f->len, o) != f->len) {
        fclose(o);
        unlink(p);
        return -1;
    }
    if (fclose(o) != 0) { unlink(p); return -1; }
    return 0;
}

int tool_scratch_grow(char *dir, size_t cap, uint64_t need,
                      const scratch_file *files, int nfiles)
{
    const char *roots[SCRATCH_MAX_ROOTS];
    char rbuf[512], ndir[sizeof rbuf], p[640], old[sizeof rbuf];
    const char *chosen = NULL;
    int nroots, i, k;

    if (tool_scratch_headroom(dir, NULL, NULL) >= need + scratch_margin())
        return 0;                 /* still fits where it is: nothing to do */

    nroots = scratch_roots(rbuf, sizeof rbuf, roots, SCRATCH_MAX_ROOTS);
    for (i = 0; i < nroots; i++) {
        if (helper_root_is(roots[i], dir)) continue;  /* we are here */
        if (tool_scratch_headroom(roots[i], NULL, NULL) <
            need + scratch_margin())
            continue;
        if (scratch_make(roots[i], ndir, sizeof ndir, need, 0, NULL) == 0) {
            chosen = roots[i];
            break;
        }
    }
    if (!chosen)
        return scratch_refuse(roots, nroots, need, NULL);

    for (k = 0; k < nfiles; k++) {
        if (scratch_put(ndir, &files[k]) != 0) {
            for (; k >= 0; k--) {
                snprintf(p, sizeof p, "%s/%s", ndir, files[k].name);
                unlink(p);
            }
            rmdir(ndir);
            return scratch_refuse(roots, nroots, need, NULL);
        }
    }
    /* the old directory goes only once the new one holds the files */
    snprintf(old, sizeof old, "%s", dir);
    for (k = 0; k < nfiles; k++) {
        snprintf(p, sizeof p, "%s/%s", old, files[k].name);
        unlink(p);
    }
    if (rmdir(old) != 0)
        fprintf(stderr, "tool_tmpdir: migrated %s -> %s but the old scratch "
                        "directory did not empty (%s); remove it by hand\n",
                old, ndir, strerror(errno));
    memcpy(dir, ndir, strlen(ndir) + 1);
    fprintf(stderr,
            "tool_tmpdir: the job grew to %llu B (+%llu B margin), which "
            "%s could not hold; scratch moved to %s\n",
            (unsigned long long)need, (unsigned long long)scratch_margin(),
            old, ndir);
    return 0;
}
