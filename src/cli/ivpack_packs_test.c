/* ivpack_packs_test.c — WP71 unit coverage for the ADR-007 plugin ABI across
 * EVERY C containerpack, not just qcow2.
 *
 * What it pins (no fixtures, no daemon, no /dev/shm — it runs inside the plain
 * `make test` tier; tools/test-ivpacks.sh owns the image round trips, the
 * .ivpack bundle checks and the daemon legs):
 *
 *   1. lib<name>.so exists for all eight packs (`make plugin-so`) and loads
 *      through dlmopen(LM_ID_NEWLM) with the dlopen fallback — the exact pair
 *      invf-plugin-host uses.
 *   2. All three ABI symbols are exported.
 *   3. The descriptor agrees with the pack's own manifest: name =, type = →
 *      pack_class, and api_version >= IVPACK_API_VERSION_MIN (a plugin the
 *      host must refuse to load is caught here, not in the field).
 *   4. Argument validation happens in the plugin, before anything is executed:
 *      a NULL args, an unknown cmd, and an ENUMERATE without an out_path must
 *      all return a NEGATIVE code. Negative is what tells
 *      invfs_codec_pack_cmd() "the pool could not carry this, fall back to the
 *      CLI", so a plugin answering 0 or 3 here would silently skip a command.
 *   5. estimate on a non-image returns the pack's decline status (3) rather
 *      than crashing or succeeding: every containerpack declines with a bare
 *      exit(3), which is why the glue in ivpack_impl.h fork-guards each call —
 *      without that guard this leg would take the whole test process down.
 *
 * Standalone (wired into `make test`):
 *   gcc -std=gnu11 -O2 -I src -o invf-ivpack_packs_test \
 *       src/cli/ivpack_packs_test.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/ivpack_api.h"

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

/* The eight C containerpacks (Makefile CPACKS). The Python packs (splt_test,
 * raw_image) and the external-tool codecpack (jxl) have no .so: they are not
 * containerpack CLIs written in C, so there is nothing to dlopen. */
static const char *const PACKS[] = {
    "qcow2", "ext4fs", "fatfs", "ntfs", "rawdisk", "vdi", "xfs", "p7z"
};
#define NPACKS ((int)(sizeof PACKS / sizeof PACKS[0]))

static void *load_pack(const char *name, char *sopath, size_t cap)
{
    void *h = NULL;

    snprintf(sopath, cap, "tools/codecpacks/%s.codecpack/lib%s.so", name, name);
    if (access(sopath, R_OK) != 0) return NULL;
#ifdef LM_ID_NEWLM
    h = dlmopen(LM_ID_NEWLM, sopath, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) h = dlopen(sopath, RTLD_NOW | RTLD_LOCAL);
    return h;
}

/* manifest lookup: `key = value`, comments and blanks ignored */
static int manifest_get(const char *name, const char *key, char *out, size_t cap)
{
    char path[256], line[512];
    FILE *f;
    size_t klen = strlen(key);

    snprintf(path, sizeof path, "tools/codecpacks/%s.codecpack/manifest", name);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *eq, *v;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, key, klen) != 0) continue;
        eq = line + klen;
        while (*eq == ' ') eq++;
        if (*eq != '=') continue;
        eq++;
        while (*eq == ' ') eq++;
        v = eq + strlen(eq);
        while (v > eq && (v[-1] == '\n' || v[-1] == '\r' || v[-1] == ' ')) v--;
        *v = '\0';
        snprintf(out, cap, "%s", eq);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

int main(void)
{
    char junk[] = "/tmp/ivpack_packs_test_junk.bin";
    FILE *jf;
    int i;

    printf("ivpack plugin ABI tests (ADR-007, %d containerpacks)\n", NPACKS);

    if (access("tools/codecpacks/qcow2.codecpack/libqcow2.so", R_OK) != 0) {
        if (system("make -s plugin-so >/dev/null 2>&1") != 0) {
            skip("plugin .so not built and `make plugin-so` failed");
            printf("%d checks, %d failure(s), %d skip(s)\n", checks, failures, skips);
            printf("SKIP\n");
            return 0;
        }
    }

    /* A file that is nobody's image: every containerpack must decline it. */
    jf = fopen(junk, "wb");
    if (jf) {
        unsigned char buf[4096];
        int k;
        for (k = 0; k < 16; k++) {
            size_t b;
            for (b = 0; b < sizeof buf; b++) buf[b] = (unsigned char)((k * 31 + b) & 0xff);
            fwrite(buf, 1, sizeof buf, jf);
        }
        fclose(jf);
    }

    for (i = 0; i < NPACKS; i++) {
        const char *name = PACKS[i];
        char sopath[256], want[128], what[256];
        void *h = load_pack(name, sopath, sizeof sopath);
        ivpack_get_desc_fn get_desc;
        ivpack_container_cmd_fn cmd;
        ivpack_container_estimate_fn est;
        const ivpack_desc *d;

        snprintf(what, sizeof what, "%s: lib%s.so loads (dlmopen/dlopen)", name, name);
        if (!h) {
            ok(0, what);
            printf("        %s\n", dlerror() ? dlerror() : "(no dlerror)");
            continue;
        }
        ok(1, what);

        get_desc = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
        cmd = (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
        est = (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");
        snprintf(what, sizeof what, "%s: exports the three ABI symbols", name);
        ok(get_desc && cmd && est, what);
        if (!get_desc || !cmd || !est) { dlclose(h); continue; }

        d = get_desc();
        snprintf(what, sizeof what, "%s: api_version %u >= %u",
                 name, d ? d->api_version : 0, (unsigned)IVPACK_API_VERSION_MIN);
        ok(d && d->api_version >= IVPACK_API_VERSION_MIN, what);
        if (!d) { dlclose(h); continue; }

        /* name = in the manifest */
        if (manifest_get(name, "name", want, sizeof want) == 0) {
            snprintf(what, sizeof what, "%s: desc name matches the manifest", name);
            ok(d->name && strcmp(d->name, want) == 0, what);
        }
        /* type = container -> pack_class "containerpack" */
        if (manifest_get(name, "type", want, sizeof want) == 0) {
            char wantclass[64];
            snprintf(wantclass, sizeof wantclass, "%spack", want);
            snprintf(what, sizeof what, "%s: pack_class matches manifest type", name);
            ok(d->pack_class && strcmp(d->pack_class, wantclass) == 0, what);
        }

        /* operand validation must answer NEGATIVE (the CLI-fallback cue) */
        snprintf(what, sizeof what, "%s: NULL args is refused (<0)", name);
        ok(cmd(NULL) < 0, what);

        {
            ivpack_container_args a;
            memset(&a, 0, sizeof a);
            a.cmd = 99;
            a.in_path = junk;
            a.out_path = "/dev/null";
            snprintf(what, sizeof what, "%s: unknown cmd is refused (<0)", name);
            ok(cmd(&a) < 0, what);

            memset(&a, 0, sizeof a);
            a.cmd = 1;                     /* ENUMERATE without an out_path */
            a.in_path = junk;
            snprintf(what, sizeof what, "%s: ENUMERATE without out_path refused", name);
            ok(cmd(&a) < 0, what);
        }

        /* decline parity on a non-image: 3, exactly like the CLI's exit(3), and
         * the test process must survive it (the fork guard's job). */
        if (access(junk, R_OK) == 0) {
            ivpack_estimate_res res;
            int rc;
            memset(&res, 0, sizeof res);
            rc = est(junk, &res);
            snprintf(what, sizeof what,
                     "%s: estimate declines a non-image with 3 (got %d)", name, rc);
            ok(rc == 3, what);   /* 3 = decline, the containerpack convention */
            snprintf(what, sizeof what, "%s: a declined estimate reports eligible=0", name);
            ok(res.eligible == 0, what);
        }

        dlclose(h);
    }

    unlink(junk);
    printf("%d checks, %d failure(s), %d skip(s)\n", checks, failures, skips);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
