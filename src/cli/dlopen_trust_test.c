/* dlopen_trust_test.c — WP625 trust-matrix unit coverage.
 *
 * Exercises the in-process loader gates without a daemon:
 *   spec parsing (opt-in default OFF, pilot restriction),
 *   origin gate (anchored roots, relative/scratch/volume/symlink refusals),
 *   signature gate (signed/unsigned/tampered fixture packs),
 *   probe matrix (good-ABI/bad-ABI/missing-symbols/crashing/hanging .so
 *     -> load NULL + this process alive throughout),
 *   estimate routing (hit vs fallback per cell).
 *
 * Usage: invf-dlopen_trust_test <scratch> ; cwd must be the repo root
 * (resolves src/include + tools/codecpacks/jxl.codecpack/libjxlest.so).
 * $INVFS_ALLOW_INPROCESS_PLUGINS must be unset (the script guarantees it).
 * All residue stays under <scratch> (disk discipline).
 *
 * Exit 0 = every cell as expected; nonzero = first failure (names the cell).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "core/vol_plugin_inproc.h"
#include "pack_sig.h"

static char g_scratch[PATH_MAX];
static int g_fail;
static unsigned long g_checks;

#define CHECK(cond, fmt, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fail = 1; \
        printf("FAIL %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } \
} while (0)

#define PASSMSG(fmt, ...) do { printf("ok %s: " fmt "\n", __func__, ##__VA_ARGS__); } while (0)

/* worker heartbeat: hostile legs must not stall or kill this process. */
static volatile unsigned long g_heart;
static void *heart_fn(void *a)
{
    (void)a;
    for (;;) { g_heart++; usleep(1000); }
    return NULL;
}
static void heart_kick(void)
{
    unsigned long a = g_heart;
    usleep(60000);
    CHECK(g_heart > a, "heartbeat stalled (process impaired)");
}

static int writefile(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(buf, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f);
}

static int copyfile(const char *dst, const char *src)
{
    FILE *fi = fopen(src, "rb"), *fo;
    char b[65536];
    size_t n;
    if (!fi) return -1;
    fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return -1; }
    while ((n = fread(b, 1, sizeof b, fi)) > 0)
        if (fwrite(b, 1, n, fo) != n) { fclose(fi); fclose(fo); return -1; }
    fclose(fi);
    return fclose(fo);
}

/* ---- embedded hostile-.so fixtures ---- */
static const char src_badabi[] =
    "#include <stdint.h>\n#include <stddef.h>\n#include \"ivpack_api.h\"\n"
    "static const ivpack_desc d = { 99, \"jxl\", \"9.9\", \"codecpack\", 0 };\n"
    "const ivpack_desc *ivpack_get_desc(void){ return &d; }\n"
    "int ivpack_container_cmd(const ivpack_container_args *a){ (void)a; return 3; }\n"
    "int ivpack_container_estimate(const char *p, ivpack_estimate_res *r){ (void)p; (void)r; return 3; }\n";
static const char src_nosyms[] =
    "void invfs_wp625_dummy(void){}\n";
static const char src_crash[] =
    "#include <stdlib.h>\n"
    "#include <stdint.h>\n#include <stddef.h>\n#include \"ivpack_api.h\"\n"
    "__attribute__((constructor)) static void c(void){ abort(); }\n"
    "static const ivpack_desc d = { 2, \"jxl\", \"9.9\", \"codecpack\", 0 };\n"
    "const ivpack_desc *ivpack_get_desc(void){ return &d; }\n"
    "int ivpack_container_cmd(const ivpack_container_args *a){ (void)a; return 3; }\n"
    "int ivpack_container_estimate(const char *p, ivpack_estimate_res *r){ (void)p; (void)r; return 3; }\n";
static const char src_hang[] =
    "#include <unistd.h>\n"
    "#include <stdint.h>\n#include <stddef.h>\n#include \"ivpack_api.h\"\n"
    "__attribute__((constructor)) static void c(void){ for (;;) pause(); }\n"
    "static const ivpack_desc d = { 2, \"jxl\", \"9.9\", \"codecpack\", 0 };\n"
    "const ivpack_desc *ivpack_get_desc(void){ return &d; }\n"
    "int ivpack_container_cmd(const ivpack_container_args *a){ (void)a; return 3; }\n"
    "int ivpack_container_estimate(const char *p, ivpack_estimate_res *r){ (void)p; (void)r; return 3; }\n";

static int build_so(const char *name, const char *src)
{
    char cpath[PATH_MAX], spath[PATH_MAX], cmd[PATH_MAX * 3];
    const char *increl = "src/include";
    snprintf(cpath, sizeof cpath, "%s/%s.c", g_scratch, name);
    snprintf(spath, sizeof spath, "%s/%s.so", g_scratch, name);
    if (writefile(cpath, src, strlen(src)) != 0) return -1;
    snprintf(cmd, sizeof cmd, "cc -shared -fPIC -O1 -I%s -o %s %s >/dev/null 2>&1",
             increl, spath, cpath);
    if (system(cmd) != 0) return -1;
    return 0;
}

/* ---- cells ---- */

static void t_spec(void)
{
    unsetenv("INVFS_ALLOW_INPROCESS_PLUGINS");
    invfs_inproc_set_allow(NULL);
    CHECK(invfs_inproc_allowed("jxl") == 0, "default must be OFF");
    invfs_inproc_set_allow("");
    CHECK(invfs_inproc_allowed("jxl") == 0, "empty spec must be OFF");
    invfs_inproc_set_allow("jxl");
    CHECK(invfs_inproc_allowed("jxl") == 1, "jxl opt-in");
    CHECK(invfs_inproc_allowed("zip") == 0, "non-pilot never allowed");
    invfs_inproc_set_allow("zip,qcow2");
    CHECK(invfs_inproc_allowed("jxl") == 0, "jxl absent from spec");
    CHECK(invfs_inproc_allowed("zip") == 0, "non-pilot even when named");
    invfs_inproc_set_allow(" png , jxl ");
    CHECK(invfs_inproc_allowed("jxl") == 1, "whitespace-tolerant");
    setenv("INVFS_ALLOW_INPROCESS_PLUGINS", "jxl", 1);
    invfs_inproc_set_allow(NULL);
    CHECK(invfs_inproc_allowed("jxl") == 1, "env opt-in for offline tools");
    invfs_inproc_set_allow("zip");
    CHECK(invfs_inproc_allowed("jxl") == 0, "explicit -o wins over env");
    unsetenv("INVFS_ALLOW_INPROCESS_PLUGINS");
    invfs_inproc_set_allow(NULL);
    PASSMSG("opt-in parsing");
}

static void t_origin(void)
{
    char reg[PATH_MAX], anchored[PATH_MAX], outside[PATH_MAX], linkp[PATH_MAX];
    snprintf(reg, sizeof reg, "%s/adminpacks", g_scratch);
    snprintf(anchored, sizeof anchored, "%s/jxl.codecpack", reg);
    snprintf(outside, sizeof outside, "%s/stray/jxl.codecpack", g_scratch);
    CHECK(mkdir(reg, 0700) == 0 || errno == EEXIST, "mkdir reg");
    CHECK(mkdir(anchored, 0700) == 0 || errno == EEXIST, "mkdir anchored");
    {
        char q[PATH_MAX];
        snprintf(q, sizeof q, "%s/stray", g_scratch);
        CHECK(mkdir(q, 0700) == 0 || errno == EEXIST, "mkdir stray");
        CHECK(mkdir(outside, 0700) == 0 || errno == EEXIST, "mkdir outside");
    }
    if (g_fail) return;
    unsetenv("INVFS_CODECPACKS");
    /* without the admin registry pointing at it, the same dir refuses. */
    CHECK(invfs_inproc_origin_anchored(anchored, 0) == 0, "unlisted dir refused");
    setenv("INVFS_CODECPACKS", reg, 1);
    CHECK(invfs_inproc_origin_anchored(anchored, 0) == 1, "admin registry entry anchored");
    CHECK(invfs_inproc_origin_anchored(anchored, 1) == 0, "volume never anchored");
    CHECK(invfs_inproc_origin_anchored(outside, 0) == 0, "scratch refused");
    CHECK(invfs_inproc_origin_anchored("rel/path/jxl.codecpack", 0) == 0, "relative refused");
    CHECK(invfs_inproc_origin_anchored(NULL, 0) == 0, "null refused");
    CHECK(invfs_inproc_origin_anchored("/nonexistent-wp625-xyz", 0) == 0, "absent refused");
    /* symlink escape: link inside the anchored root pointing outside. */
    snprintf(linkp, sizeof linkp, "%s/evil.codecpack", reg);
    unlink(linkp);
    CHECK(symlink(outside, linkp) == 0, "symlink fixture");
    CHECK(invfs_inproc_origin_anchored(linkp, 0) == 0, "symlink escape refused");
    unlink(linkp);
    unsetenv("INVFS_CODECPACKS");
    PASSMSG("origin gate");
}

/* fixture pack signing helper: packdir must exist. Returns keyring path. */
static void make_keyring(char *kr, size_t cap, char *sec, size_t seccap)
{
    char err[256];
    snprintf(kr, cap, "%s/kr", g_scratch);
    mkdir(kr, 0700);
    CHECK(pack_sig_keygen(kr, "t", err, sizeof err) == 0, "keygen: %s", err);
    snprintf(sec, seccap, "%s/t.sec", kr);
}

static void sign_pack(const char *packdir, const char *sec)
{
    char err[256];
    CHECK(pack_sig_sign_dir(packdir, sec, err, sizeof err) == 0,
          "sign %s: %s", packdir, err);
}

static void t_sig_gate(void)
{
    char reg[PATH_MAX], packdir[PATH_MAX], kr[PATH_MAX], sec[PATH_MAX];
    char err[256];
    snprintf(reg, sizeof reg, "%s/adminpacks", g_scratch);
    snprintf(packdir, sizeof packdir, "%s/jxl.codecpack", reg);
    setenv("INVFS_CODECPACKS", reg, 1);
    make_keyring(kr, sizeof kr, sec, sizeof sec);
    setenv("INVFS_KEYRING", kr, 1);
    /* fixture content: manifest + the real pilot .so + sample input. */
    {
        char mp[PATH_MAX], sop[PATH_MAX], inp[PATH_MAX];
        snprintf(mp, sizeof mp, "%s/manifest", packdir);
        /* packdir already exists from t_origin; refresh it signed-clean. */
        CHECK(writefile(mp, "name = jxl\n", 10) == 0, "manifest");
        snprintf(sop, sizeof sop, "%s/libjxlest.so", packdir);
        CHECK(copyfile(sop, "tools/codecpacks/jxl.codecpack/libjxlest.so") == 0,
              "pilot .so fixture (run make plugin-so first)");
        snprintf(inp, sizeof inp, "%s/sample.bin", packdir);
        CHECK(writefile(inp, "not a jpeg, geometry unknown", 28) == 0, "sample");
    }
    sign_pack(packdir, sec);
    invfs_inproc_set_allow("jxl");
    {
        char reason[256];
        CHECK(pack_sig_verify_dir(packdir, kr, err, sizeof err) == PACK_SIG_OK,
              "verify OK: %s", err);
        CHECK(invfs_inproc_eligible("jxl", packdir, 0, reason, sizeof reason) == 1,
              "signed+local+opt-in eligible (%s)", reason);
    }
    /* tamper: one appended byte -> BAD -> refuse. */
    {
        char sop[PATH_MAX], reason[256];
        FILE *f;
        snprintf(sop, sizeof sop, "%s/libjxlest.so", packdir);
        f = fopen(sop, "ab");
        CHECK(f != NULL, "tamper open");
        if (f) { fputc(0, f); fclose(f); }
        CHECK(pack_sig_verify_dir(packdir, kr, err, sizeof err) == PACK_SIG_BAD,
              "tamper verifies BAD");
        CHECK(invfs_inproc_eligible("jxl", packdir, 0, reason, sizeof reason) == 0,
              "tampered refused (%s)", reason);
    }
    PASSMSG("signature gate");
}

static void t_probe(void)
{
    char err[256];
    void *h;
    char good[PATH_MAX], bad[PATH_MAX], nos[PATH_MAX], cr[PATH_MAX], hg[PATH_MAX];
    snprintf(good, sizeof good, "%s/adminpacks/jxl.codecpack/libjxlest.so", g_scratch);
    CHECK(build_so("badabi", src_badabi) == 0, "build badabi");
    CHECK(build_so("nosyms", src_nosyms) == 0, "build nosyms");
    CHECK(build_so("crash", src_crash) == 0, "build crash");
    CHECK(build_so("hang", src_hang) == 0, "build hang");
    snprintf(bad, sizeof bad, "%s/badabi.so", g_scratch);
    snprintf(nos, sizeof nos, "%s/nosyms.so", g_scratch);
    snprintf(cr, sizeof cr, "%s/crash.so", g_scratch);
    snprintf(hg, sizeof hg, "%s/hang.so", g_scratch);

    h = invfs_inproc_load("jxl", good, err, sizeof err);
    CHECK(h != NULL, "good-ABI loads: %s", err);
    heart_kick();

    CHECK(invfs_inproc_load("jxl", bad, err, sizeof err) == NULL, "bad ABI refused (%s)", err);
    heart_kick();
    CHECK(invfs_inproc_load("jxl", nos, err, sizeof err) == NULL, "missing symbols refused (%s)", err);
    heart_kick();
    CHECK(invfs_inproc_load("jxl", cr, err, sizeof err) == NULL, "crashing .so refused (%s)", err);
    heart_kick();
    CHECK(invfs_inproc_load("jxl", hg, err, sizeof err) == NULL, "hanging .so refused (%s)", err);
    heart_kick();
    CHECK(invfs_inproc_load("jxl", "/nonexistent-wp625.so", err, sizeof err) == NULL,
          "absent .so refused");
    heart_kick();
    CHECK(invfs_inproc_load("zip", good, err, sizeof err) == NULL, "non-pilot never loads");
    PASSMSG("probe matrix (daemon survived every hostile leg)");
}

static void t_estimate(void)
{
    char reg[PATH_MAX], packdir[PATH_MAX], inp[PATH_MAX], kr[PATH_MAX], sec[PATH_MAX];
    char reason[256];
    uint64_t a = 0, b = 0;
    snprintf(reg, sizeof reg, "%s/adminpacks", g_scratch);
    snprintf(packdir, sizeof packdir, "%s/jxl.codecpack", reg);
    snprintf(inp, sizeof inp, "%s/sample.bin", packdir);
    snprintf(kr, sizeof kr, "%s/kr", g_scratch);
    snprintf(sec, sizeof sec, "%s/t.sec", kr);
    setenv("INVFS_CODECPACKS", reg, 1);
    setenv("INVFS_KEYRING", kr, 1);
    /* re-sign after the tamper leg (t_sig_gate appended a byte). */
    sign_pack(packdir, sec);

    invfs_inproc_set_allow("jxl");
    {
        uint64_t h0 = invfs_inproc_count_hit(), f0 = invfs_inproc_count_fallback();
        CHECK(invfs_inproc_estimate("jxl", packdir, 0, inp, &a) == 0,
              "signed+opt-in estimate served in-process (%s)", invfs_inproc_last_route());
        CHECK(invfs_inproc_count_hit() == h0 + 1, "hit counter");
        CHECK(a == 0, "unknown geometry admits as 0 (got %llu)", (unsigned long long)a);
        /* determinism: same answer twice. */
        CHECK(invfs_inproc_estimate("jxl", packdir, 0, inp, &b) == 0 && b == a,
              "in-process estimate deterministic");
        (void)f0;
    }
    /* opt-out -> fallback. */
    invfs_inproc_set_allow(NULL);
    unsetenv("INVFS_ALLOW_INPROCESS_PLUGINS");
    {
        uint64_t f0 = invfs_inproc_count_fallback();
        CHECK(invfs_inproc_estimate("jxl", packdir, 0, inp, &b) == -1,
              "opt-out falls back (%s)", invfs_inproc_last_route());
        CHECK(invfs_inproc_count_fallback() == f0 + 1, "fallback counter");
    }
    /* unsigned pack -> fallback even when opted in. */
    {
        char ureg[PATH_MAX], upack[PATH_MAX], mf[PATH_MAX];
        snprintf(ureg, sizeof ureg, "%s/unsigned", g_scratch);
        snprintf(upack, sizeof upack, "%s/jxl.codecpack", ureg);
        mkdir(ureg, 0700);
        mkdir(upack, 0700);
        snprintf(mf, sizeof mf, "%s/manifest", upack);
        CHECK(writefile(mf, "name = jxl\n", 10) == 0, "unsigned manifest");
        invfs_inproc_set_allow("jxl");
        setenv("INVFS_CODECPACKS", ureg, 1);
        CHECK(invfs_inproc_eligible("jxl", upack, 0, reason, sizeof reason) == 0,
              "unsigned refused (%s)", reason);
        CHECK(invfs_inproc_estimate("jxl", upack, 0, inp, &b) == -1,
              "unsigned estimate falls back");
        setenv("INVFS_CODECPACKS", reg, 1);
    }
    /* volume pack -> fallback. */
    CHECK(invfs_inproc_estimate("jxl", packdir, 1, inp, &b) == -1, "volume pack falls back");
    /* non-pilot -> fallback. */
    CHECK(invfs_inproc_estimate("zip", packdir, 0, inp, &b) == -1, "non-pilot falls back");
    unsetenv("INVFS_CODECPACKS");
    PASSMSG("estimate routing");
}

int main(int argc, char **argv)
{
    pthread_t th;
    if (argc != 2) {
        fprintf(stderr, "usage: invf-dlopen_trust_test <scratch>\n");
        return 2;
    }
    snprintf(g_scratch, sizeof g_scratch, "%s", argv[1]);
    /* disk discipline: the caller names a fresh dir per leg; create it
     * (mkdir -p shape) so a missing parent fails loud, not silent. */
    {
        char tmp[PATH_MAX], *q;
        snprintf(tmp, sizeof tmp, "%s", g_scratch);
        for (q = tmp + 1; *q; q++) {
            if (*q == '/') {
                *q = '\0';
                if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                    fprintf(stderr, "cannot mkdir %s: %s\n", tmp, strerror(errno));
                    return 2;
                }
                *q = '/';
            }
        }
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "cannot mkdir %s: %s\n", tmp, strerror(errno));
            return 2;
        }
    }
    if (getenv("INVFS_ALLOW_INPROCESS_PLUGINS")) {
        fprintf(stderr, "refusing: INVFS_ALLOW_INPROCESS_PLUGINS is set in the environment\n");
        return 2;
    }
    if (pthread_create(&th, NULL, heart_fn, NULL) != 0) {
        fprintf(stderr, "no threads\n");
        return 2;
    }
    t_spec();
    t_origin();
    t_sig_gate();
    t_probe();
    t_estimate();
    invfs_inproc_unload_all();
    printf("%s: %lu checks, %s (hit=%llu fallback=%llu last=%s)\n",
           argv[0], g_checks, g_fail ? "FAIL" : "PASS",
           (unsigned long long)invfs_inproc_count_hit(),
           (unsigned long long)invfs_inproc_count_fallback(),
           invfs_inproc_last_route());
    return g_fail ? 1 : 0;
}
