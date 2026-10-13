/* vol_plugin_inproc.c — WP625 in-process codec plugin loader.
 *
 * See the header for the trust model. Structure:
 *   §1 opt-in spec (allowlist, default OFF)
 *   §2 origin gate (anchored admin roots)
 *   §3 signature gate (pack_sig, load-time re-verify for in-process only)
 *   §4 forked probe (hostile .so fails closed, daemon survives)
 *   §5 cached dlopen + ABI re-check + estimate dispatch
 *   §6 route counters
 */
#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "core/vol_plugin_inproc.h"
#include "include/ivpack_api.h"

/* pack_sig lives in src/cli but is linked into CORE_O for exactly this
 * gate (WP625): load-time re-verify exists ONLY for the in-process path.
 * The declaration is repeated here instead of including the cli header so
 * core keeps its header discipline (codec.h is the only cli->core bridge
 * the other way). Values must match src/cli/pack_sig.h. */
typedef enum {
    INPROC_PACK_SIG_OK = 0,
    INPROC_PACK_SIG_UNSIGNED = 1,
    INPROC_PACK_SIG_BAD = 2
} inproc_pack_sig_status;
inproc_pack_sig_status pack_sig_verify_dir(const char *packdir,
                                           const char *keyring_dir,
                                           char *err, size_t errcap);
int pack_sig_default_keyring(char *out, size_t outcap,
                             char *err, size_t errcap);

/* ---- §1 opt-in spec (default OFF) ---- */

static pthread_mutex_t g_allow_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_allow_spec[256] = "";
/* last routing: short static string + reason, for stats. */
static char g_last_route[320] = "ipc: loader idle";

static void set_last_route(const char *s)
{
    pthread_mutex_lock(&g_allow_lock);
    snprintf(g_last_route, sizeof g_last_route, "%s", s ? s : "?");
    pthread_mutex_unlock(&g_allow_lock);
}

void invfs_inproc_set_allow(const char *spec)
{
    pthread_mutex_lock(&g_allow_lock);
    if (!spec || !*spec)
        g_allow_spec[0] = '\0';
    else
        snprintf(g_allow_spec, sizeof g_allow_spec, "%s", spec);
    pthread_mutex_unlock(&g_allow_lock);
}

const char *invfs_inproc_allow_spec(void)
{
    return g_allow_spec;
}

/* 1 iff the comma list names `name` (case-sensitive, like the manifest). */
static int spec_names(const char *spec, const char *name)
{
    const char *p = spec;
    size_t nl = strlen(name);
    if (!spec || !name || !nl) return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        {
            const char *comma = strchr(p, ',');
            size_t tl = comma ? (size_t)(comma - p) : strlen(p);
            while (tl && (p[tl - 1] == ' ' || p[tl - 1] == '\t')) tl--;
            if (tl == nl && memcmp(p, name, nl) == 0) return 1;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return 0;
}

static int is_pilot(const char *name)
{
    return name && strcmp(name, INVFS_INPROC_PILOT_JXL) == 0;
}

int invfs_inproc_allowed(const char *name)
{
    int ok;
    char envbuf[256];
    const char *spec;
    if (!is_pilot(name)) return 0;
    pthread_mutex_lock(&g_allow_lock);
    /* Offline tools (invf-sweep, invf-import) share this TU but take no
     * mount options: $INVFS_ALLOW_INPROCESS_PLUGINS is their explicit
     * opt-in (same comma syntax). An explicit -o (set_allow during
     * option parse, before any codec use) wins: env is consulted only
     * while no -o arrived. The daemon's own environment is
     * operator-controlled, so this does not widen the trust boundary. */
    if (!g_allow_spec[0]) {
        const char *e = getenv("INVFS_ALLOW_INPROCESS_PLUGINS");
        if (e && *e) {
            snprintf(envbuf, sizeof envbuf, "%s", e);
            spec = envbuf;
        } else spec = g_allow_spec;
    } else spec = g_allow_spec;
    ok = spec[0] && spec_names(spec, name);
    pthread_mutex_unlock(&g_allow_lock);
    return ok;
}

/* ---- §2 origin gate ---- */

/* Anchored admin roots. These are the exact install/scan roots the
 * codebase owns (src/cli/pack.c HOST_ROOT/LEGACY_ROOT, the packaging
 * system dir, and the explicit $INVFS_CODECPACKS registry): an admin
 * put the pack there through the install gate or the packaging.
 * Everything else (tool scratch /dev/shm:/tmp:/var/tmp staging,
 * volume materialization, fetched-at-runtime trees, cwd-relative pack
 * dirs) is not anchored. */
static int path_under(const char *path, const char *root)
{
    size_t rl = strlen(root);
    if (!rl || strncmp(path, root, rl) != 0) return 0;
    return path[rl] == '\0' || path[rl] == '/';
}

/* realpath both sides before comparing (a symlink pointing out of an
 * anchored root must not pass, and a symlinked root itself must still
 * match when it resolves inside). Bounded stack buffers: PATH_MAX. */
static int real_under(const char *rp_path, const char *root)
{
    char rr[PATH_MAX];
    if (!root || !*root) return 0;
    if (!realpath(root, rr)) {
        /* root absent: fall back to the literal (a missing system dir
         * simply never matches a real pack path). */
        return path_under(rp_path, root);
    }
    return path_under(rp_path, rr);
}

int invfs_inproc_origin_anchored(const char *packdir, int is_volume)
{
    char rp[PATH_MAX];
    const char *cp;

    if (!packdir || !*packdir) return 0;
    if (is_volume) return 0;   /* volume content is never an admin root */
    if (packdir[0] != '/') return 0;  /* relative = runtime tree, refuse */
    if (!realpath(packdir, rp)) return 0;  /* absent/unreadable: refuse */

    /* 1. host root: the invf-pack install destination (signature-gated
     * at install time by pack_sig_gate_install, WP203). */
    if (real_under(rp, "/.invariantfs/codecpacks")) return 1;
    /* 2. legacy root. */
    if (real_under(rp, "/.invfs/codecpacks")) return 1;
    /* 3. system root (packaging installs root-owned packs here). */
    if (real_under(rp, "/usr/lib/invfs/codecpacks")) return 1;
    /* 4. explicit admin registry: every $INVFS_CODECPACKS entry. */
    cp = getenv("INVFS_CODECPACKS");
    while (cp && *cp) {
        const char *colon = strchr(cp, ':');
        size_t dl = colon ? (size_t)(colon - cp) : strlen(cp);
        if (dl && dl < (size_t)PATH_MAX) {
            char entry[PATH_MAX], packparent[PATH_MAX];
            memcpy(entry, cp, dl);
            entry[dl] = '\0';
            /* entries name pack COLLECTIONS (dirs of *.codecpack dirs):
             * the packdir is anchored when it is an immediate child of
             * an entry, or the entry itself. */
            if (real_under(rp, entry)) return 1;
            /* also accept entry == the pack dir's parent spelled with a
             * trailing slash or equivalent (covered by real_under). */
            (void)packparent;
        }
        if (!colon) break;
        cp = colon + 1;
    }
    return 0;
}

/* ---- §3+§4+§5: eligibility, probe, load ---- */

int invfs_inproc_eligible(const char *packname, const char *packdir,
                          int is_volume, char *reason, size_t reasoncap)
{
    char err[256];
    char keyring[1024];
    char kerr[256];
    inproc_pack_sig_status st;
#define SAY(fmt, ...) do { \
        if (reason && reasoncap) snprintf(reason, reasoncap, fmt, ##__VA_ARGS__); \
    } while (0)

    if (!is_pilot(packname)) {
        SAY("not a pilot pack (only '%s' may load in-process)", INVFS_INPROC_PILOT_JXL);
        return 0;
    }
    if (!invfs_inproc_allowed(packname)) {
        SAY("not opted in (-o allow_inprocess_plugins=%s absent)", packname);
        return 0;
    }
    if (!invfs_inproc_origin_anchored(packdir, is_volume)) {
        SAY("origin not local/anchored (%s)", packdir ? packdir : "(null)");
        return 0;
    }
    /* Signature re-verify at LOAD time (in-process path only). UNSIGNED
     * refuses here even though the install gate may have prompted past
     * it: a prompted install runs out-of-process, where a tampered
     * helper is contained; in-process there is no containment left. */
    if (pack_sig_default_keyring(keyring, sizeof keyring, kerr, sizeof kerr) != 0) {
        SAY("no keyring (%s)", kerr);
        return 0;
    }
    st = pack_sig_verify_dir(packdir, keyring, err, sizeof err);
    if (st != INPROC_PACK_SIG_OK) {
        SAY("signature %s (%s)",
            st == INPROC_PACK_SIG_UNSIGNED ? "missing" : "BAD", err);
        return 0;
    }
    SAY("ok");
    return 1;
#undef SAY
}

/* ---- forked probe ---- */

#ifdef _WIN32
void *invfs_inproc_load(const char *packname, const char *soname,
                        char *err, size_t errcap)
{
    (void)packname; (void)soname;
    if (err && errcap) snprintf(err, errcap, "in-process plugins parked on Windows");
    return NULL;
}
void invfs_inproc_unload_all(void) { }
#else

/* Probe exit codes (child -> parent). */
#define PROBE_OK 0
#define PROBE_DLOPEN 11
#define PROBE_SYMBOLS 12
#define PROBE_DESC 13
#define PROBE_ABI 14
#define PROBE_NAME 15

/* What the probe checks, in the child. No daemon state touched: pure
 * dlopen + dlsym + descriptor read. A constructor that segfaults kills
 * the CHILD (WIFSIGNALED below); one that hangs trips the parent's
 * timeout (SIGKILL below). Either way the daemon never maps the .so. */
static int probe_child(const char *packname, const char *soname)
{
    void *h;
    ivpack_get_desc_fn get_desc;
    ivpack_container_cmd_fn cmd;
    ivpack_container_estimate_fn est;
    const ivpack_desc *d;

    h = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (!h) _exit(PROBE_DLOPEN);
    get_desc = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
    cmd = (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
    est = (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");
    if (!get_desc || !cmd || !est) _exit(PROBE_SYMBOLS);
    d = get_desc();
    if (!d || !d->name) _exit(PROBE_DESC);
    /* Major check: exact equality with the loader major. The v1..v2
     * append-only history lets an OLD host tolerate a new plugin over
     * IPC, but in-process there is no marshaling layer to absorb field
     * drift — the major must match exactly, else refuse + IPC fallback. */
    if (d->api_version != (uint32_t)INVFS_CODEC_ABI_MAJOR)
        _exit(PROBE_ABI);
    if (strcmp(d->name, packname) != 0) _exit(PROBE_NAME);
    /* NOTE: no dlclose here — _exit skips atexit/dlclose; the mapping
     * dies with the child. The parent dlopens its own copy after. */
    _exit(PROBE_OK);
    return PROBE_OK; /* unreachable */
}

/* waitpid with a ceiling (ms). 0 = reaped into *status; -1 = timeout
 * (child SIGKILLed and reaped) or wait error. */
static int wait_ceiling(pid_t pid, int *status, long ms)
{
    long waited = 0;
    const long step = 20; /* ms */
    struct timespec ts;
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (waited >= ms) {
            kill(pid, SIGKILL);
            /* reap the kill for real (blocking now: it must die). */
            while (waitpid(pid, status, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
        ts.tv_sec = 0;
        ts.tv_nsec = step * 1000000L;
        nanosleep(&ts, NULL);
        waited += step;
    }
}

#define INPROC_PROBE_TIMEOUT_MS 10000L

/* ---- cached handles ---- */

#define INPROC_CACHE_MAX 8
typedef struct {
    char soname[1024];
    dev_t dev;
    ino_t ino;
    void *handle;
    ivpack_container_estimate_fn est;
} inproc_slot;
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static inproc_slot g_cache[INPROC_CACHE_MAX];
static size_t g_cache_n;

static int ident_of(const char *soname, dev_t *dev, ino_t *ino)
{
    struct stat st;
    if (stat(soname, &st) != 0) return -1;
    *dev = st.st_dev;
    *ino = st.st_ino;
    return 0;
}

void invfs_inproc_unload_all(void)
{
    size_t i;
    pthread_mutex_lock(&g_cache_lock);
    for (i = 0; i < g_cache_n; i++) {
        if (g_cache[i].handle) dlclose(g_cache[i].handle);
        memset(&g_cache[i], 0, sizeof g_cache[i]);
    }
    g_cache_n = 0;
    pthread_mutex_unlock(&g_cache_lock);
}

void *invfs_inproc_load(const char *packname, const char *soname,
                        char *err, size_t errcap)
{
    pid_t pid;
    int status = 0;
    int wr;
    void *h;
    ivpack_get_desc_fn get_desc;
    ivpack_container_estimate_fn est;
    const ivpack_desc *d;
    dev_t dev = 0;
    ino_t ino = 0;
    size_t i;
#define LERR(fmt, ...) do { \
        if (err && errcap) snprintf(err, errcap, fmt, ##__VA_ARGS__); \
    } while (0)

    if (!packname || !soname) { LERR("null pack/soname"); return NULL; }
    if (!is_pilot(packname)) { LERR("not a pilot pack"); return NULL; }
    if (ident_of(soname, &dev, &ino) != 0) {
        LERR("stat %s: %s", soname, strerror(errno));
        return NULL;
    }

    /* cache hit (same path + same file identity)? */
    pthread_mutex_lock(&g_cache_lock);
    for (i = 0; i < g_cache_n; i++) {
        if (g_cache[i].dev == dev && g_cache[i].ino == ino &&
            strcmp(g_cache[i].soname, soname) == 0) {
            h = g_cache[i].handle;
            pthread_mutex_unlock(&g_cache_lock);
            return h;
        }
    }
    pthread_mutex_unlock(&g_cache_lock);

    /* §4: probe in a forked child FIRST. The daemon must survive a
     * constructor that crashes or hangs: those show up as a dead child
     * (signal / timeout+SIGKILL), never as a dead daemon. */
    pid = fork();
    if (pid < 0) { LERR("fork: %s", strerror(errno)); return NULL; }
    if (pid == 0) {
        /* child: no daemon locks held across this fork that matter (the
         * caller holds none; the cache lock is re-taken only in the
         * parent path below). */
        probe_child(packname, soname);
        _exit(99); /* unreachable */
    }
    wr = wait_ceiling(pid, &status, INPROC_PROBE_TIMEOUT_MS);
    if (wr != 0) {
        LERR("probe timeout/hang (%s): SIGKILLed, falling back to IPC", soname);
        return NULL;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != PROBE_OK) {
        const char *why;
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (!WIFEXITED(status))
            why = "crashed (signal)";
        else switch (code) {
        case PROBE_DLOPEN: why = "dlopen failed"; break;
        case PROBE_SYMBOLS: why = "missing ivpack exports"; break;
        case PROBE_DESC: why = "null descriptor"; break;
        case PROBE_ABI: why = "ABI major mismatch"; break;
        case PROBE_NAME: why = "pack name mismatch"; break;
        default: why = "unknown probe failure"; break;
        }
        LERR("probe refused %s: %s%s", soname, why,
             (!WIFEXITED(status)) ? " — daemon survives, IPC fallback" : "");
        return NULL;
    }

    /* Probe clean: map it here and re-check (TOCTOU between probe and
     * load is contained by the dev+ino identity: a swapped file misses
     * the cache and re-probes next call — worst case one stale mapping
     * for a file the admin replaced, same as any dlopen race). */
    h = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (!h) { LERR("dlopen %s: %s", soname, dlerror()); return NULL; }
    get_desc = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
    est = (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");
    d = get_desc ? get_desc() : NULL;
    if (!get_desc || !est || !d || d->api_version != (uint32_t)INVFS_CODEC_ABI_MAJOR ||
        !d->name || strcmp(d->name, packname) != 0) {
        LERR("post-load re-check failed for %s", soname);
        dlclose(h);
        return NULL;
    }
    pthread_mutex_lock(&g_cache_lock);
    if (g_cache_n < INPROC_CACHE_MAX) {
        snprintf(g_cache[g_cache_n].soname, sizeof g_cache[g_cache_n].soname,
                 "%s", soname);
        g_cache[g_cache_n].dev = dev;
        g_cache[g_cache_n].ino = ino;
        g_cache[g_cache_n].handle = h;
        g_cache[g_cache_n].est = est;
        g_cache_n++;
    }
    /* cache full: return the uncached handle (caller uses it for this
     * call; it leaks one mapping — bounded by call count, documented). */
    pthread_mutex_unlock(&g_cache_lock);
    return h;
#undef LERR
}

/* ---- §5 pilot estimate dispatch ---- */

/* ---- bounded in-process call (fork-without-exec guard) ---- */

/* The estimate body must finish fast (a streaming marker walk over a
 * bounded window); 5 s is orders of magnitude past any honest file and
 * exists only to convert a wedged child into an IPC fallback. */
#define INPROC_EST_TIMEOUT_MS 5000

typedef struct {
    ivpack_container_estimate_fn est;
    const char *in_path;
    ivpack_estimate_res *res;
    int rc;
} est_job;

static void *est_worker(void *arg)
{
    est_job *j = (est_job *)arg;
    j->rc = j->est(j->in_path, j->res);
    return NULL;
}

/* Run the .so estimate on a worker thread and join it with a ceiling.
 * Returns the estimate rc, or -99 on timeout/error (caller treats any
 * nonzero as IPC fallback). The worker holds no loader locks, so an
 * abandoned worker is self-draining: when/if its child exits it frees
 * its (detached) thread and is gone. One thread spawn per call (~10 us)
 * is the price of never wedging the sweep. */
static int est_call_bounded(ivpack_container_estimate_fn est,
                            const char *in_path, ivpack_estimate_res *res)
{
    est_job j;
    pthread_t th;
    struct timespec dl;
    int cr;
    j.est = est;
    j.in_path = in_path;
    j.res = res;
    j.rc = -99;
    if (pthread_create(&th, NULL, est_worker, &j) != 0)
        return -99;
    if (clock_gettime(CLOCK_REALTIME, &dl) != 0) {
        pthread_detach(th);
        return -99;
    }
    dl.tv_sec += INPROC_EST_TIMEOUT_MS / 1000;
    dl.tv_nsec += (INPROC_EST_TIMEOUT_MS % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    cr = pthread_timedjoin_np(th, NULL, &dl);
    if (cr != 0) {
        /* timeout (or join error): abandon the worker, fail closed. */
        pthread_detach(th);
        return -99;
    }
    return j.rc;
}

static uint64_t g_hit, g_fallback;

uint64_t invfs_inproc_count_hit(void) { return __sync_fetch_and_add(&g_hit, 0); }
uint64_t invfs_inproc_count_fallback(void) { return __sync_fetch_and_add(&g_fallback, 0); }
const char *invfs_inproc_last_route(void) { return g_last_route; }

/* The pilot .so filename inside an anchored pack dir. Never overridden
 * by the manifest: a manifest-controlled dlopen path would let a signed
 * manifest smuggle an arbitrary .so name past review (the sig binds the
 * manifest bytes, and the name would BE manifest bytes — self-licensing).
 * Fixed name = the name the Makefile builds (JXLEST_SO). */
#define INPROC_JXL_SONAME "libjxlest.so"

int invfs_inproc_estimate(const char *packname, const char *packdir,
                          int is_volume, const char *in_path,
                          uint64_t *out_bytes)
{
    char reason[256], err[256], soname[2048];
    void *h;
    ivpack_container_estimate_fn est;
    ivpack_estimate_res res;
    int rc;
    char route[300];

    if (!is_pilot(packname) || !packdir || !in_path) {
        __sync_fetch_and_add(&g_fallback, 1);
        set_last_route("ipc: not a pilot pack (or null args)");
        return -1;
    }
    if (!invfs_inproc_eligible(packname, packdir, is_volume,
                               reason, sizeof reason)) {
        __sync_fetch_and_add(&g_fallback, 1);
        snprintf(route, sizeof route, "ipc: %s", reason);
        set_last_route(route);
        return -1;
    }
    if (snprintf(soname, sizeof soname, "%s/%s", packdir,
                 INPROC_JXL_SONAME) >= (int)sizeof soname) {
        __sync_fetch_and_add(&g_fallback, 1);
        set_last_route("ipc: soname too long");
        return -1;
    }
    h = invfs_inproc_load(packname, soname, err, sizeof err);
    if (!h) {
        __sync_fetch_and_add(&g_fallback, 1);
        snprintf(route, sizeof route, "ipc: %s", err);
        set_last_route(route);
        return -1;
    }
    /* NOTE: dlsym on every call (cheap, no lock): never cache raw fn
     * pointers outside the loader lock — the slot owns them. */
    est = (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");
    if (!est) {
        __sync_fetch_and_add(&g_fallback, 1);
        set_last_route("ipc: estimate export vanished post-load");
        return -1;
    }
    /* The .so's own estimate runs its body in a forked child
     * (ivpack_run_child under IVPACK_DEFINE_CONTAINER_ESTIMATE), so even
     * the in-process call keeps a process boundary around the parsing.
     * rc==0 + eligible==1 carries mbr_size (the SOF pixel count, 0 when
     * geometry unknown = admit, same convention as the CLI helper).
     *
     * That fork is WITHOUT exec, inside OUR multithreaded daemon: if a
     * sibling thread holds a libc lock (malloc arena) at the fork
     * instant, the child can deadlock on its first allocation and the
     * .so's internal waitpid never returns. A stuck estimate must not
     * wedge the sweep, so the call below runs bounded (est_call_bounded):
     * on timeout we abandon the worker and take the IPC fallback, exactly
     * like any other decline. The abandoned worker owns its hung child;
     * both drain when/if the child exits (detached thread, no loader
     * lock held), so the only cost of a hit is one leaked thread — and a
     * hit only happens when the fork instant was already unlucky. */
    memset(&res, 0, sizeof res);
    rc = est_call_bounded(est, in_path, &res);
    if (rc != 0 || !res.eligible) {
        __sync_fetch_and_add(&g_fallback, 1);
        set_last_route("ipc: in-process estimate declined");
        return -1;
    }
    if (out_bytes) *out_bytes = res.mbr_size;
    __sync_fetch_and_add(&g_hit, 1);
    set_last_route("inprocess: jxl estimate via libjxlest.so");
    /* Observability for the trust-matrix/perf legs (and operators):
     * $INVFS_INPROC_LOG=1 prints one line per in-process estimate.
     * Off unless asked — this is a hot path. */
    if (getenv("INVFS_INPROC_LOG"))
        fprintf(stderr, "[inproc] jxl estimate %s -> %llu (in-process)\n",
                in_path, (unsigned long long)res.mbr_size);
    return 0;
}
#endif /* _WIN32 */
