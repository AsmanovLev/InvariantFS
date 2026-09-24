/* ivpack_probe.c — exercise a containerpack .so plugin directly, the way
 * invf-plugin-host does, without needing the daemon or a 64 MiB /dev/shm slot.
 *
 *   ivpack-probe desc    <lib.so>
 *   ivpack-probe est     <lib.so> <image>
 *   ivpack-probe cmd <lib.so> N <in> <idx|-> <out> <recipe|-> <dir|->
 *
 * Prints the plugin's answer on stdout in a greppable form and exits with:
 *   0  the call succeeded (the plugin's own status was 0)
 *   1  usage / dlopen / dlsym failure
 *   N  the plugin returned N (its CLI exit status: 3 = decline, 1 = error)
 * 100+N when the plugin was killed by signal N (a crash must never be
 *       mistaken for a decline).
 *
 * Built by tools/test-ivpacks.sh into the suite's scratch bin/ (not a shipped
 * tool): -std=gnu11 -O2 -Wall -Wextra -Werror ivpack_probe.c -o ivpack-probe -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/ivpack_api.h"

static void *load(const char *path)
{
    void *h = NULL;

#ifdef LM_ID_NEWLM
    /* The daemon loads into an isolated link map; do the same so the suite
     * covers that path (and falls back to dlopen exactly like the daemon). */
    h = dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "ivpack-probe: load %s: %s\n", path, dlerror());
    return h;
}

static const char *dash(const char *s)
{
    return (s && strcmp(s, "-") != 0) ? s : NULL;
}

int main(int argc, char **argv)
{
    const char *op;
    void *h;
    ivpack_get_desc_fn get_desc;
    int rc;

    if (argc < 3) {
        fprintf(stderr, "usage: %s desc|est|cmd <lib.so> [...]\n", argv[0]);
        return 1;
    }
    op = argv[1];
    h = load(argv[2]);
    if (!h) return 1;

    get_desc = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
    if (!get_desc) {
        fprintf(stderr, "ivpack-probe: %s exports no ivpack_get_desc\n", argv[2]);
        return 1;
    }

    if (strcmp(op, "desc") == 0) {
        const ivpack_desc *d = get_desc();
        ivpack_container_cmd_fn c =
            (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
        ivpack_container_estimate_fn e =
            (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");

        if (!d) { fprintf(stderr, "ivpack-probe: desc is NULL\n"); return 1; }
        printf("api_version=%u name=%s version=%s pack_class=%s flags=%u "
               "cmd=%s estimate=%s\n",
               d->api_version, d->name ? d->name : "(null)",
               d->version ? d->version : "(null)",
               d->pack_class ? d->pack_class : "(null)", d->flags,
               c ? "yes" : "NO", e ? "yes" : "NO");
        return (c && e && d->api_version >= IVPACK_API_VERSION_MIN) ? 0 : 1;
    }

    if (strcmp(op, "est") == 0) {
        ivpack_container_estimate_fn est =
            (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");
        ivpack_estimate_res res;

        if (!est) { fprintf(stderr, "ivpack-probe: no estimate export\n"); return 1; }
        if (argc < 4) { fprintf(stderr, "ivpack-probe: est needs <image>\n"); return 1; }
        memset(&res, 0, sizeof res);
        rc = est(argv[3], &res);
        printf("rc=%d eligible=%d orig_size=%llu mbr_size=%llu recipe_size=%llu\n",
               rc, res.eligible, (unsigned long long)res.orig_size,
               (unsigned long long)res.mbr_size,
               (unsigned long long)res.recipe_size);
        return rc < 0 ? 100 - rc : rc;
    }

    if (strcmp(op, "cmd") == 0) {
        ivpack_container_cmd_fn cmdfn =
            (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
        ivpack_container_args a;
        char self[4096];

        if (!cmdfn) { fprintf(stderr, "ivpack-probe: no cmd export\n"); return 1; }
        if (argc < 9) {
            fprintf(stderr,
                    "usage: %s cmd <lib.so> <N> <in> <idx|-> <out> <recipe|-> <dir|->\n",
                    argv[0]);
            return 1;
        }
        memset(&a, 0, sizeof a);
        a.cmd = atoi(argv[3]);
        a.in_path = dash(argv[4]);
        a.extract_idx = dash(argv[5]);
        a.out_path = dash(argv[6]);
        a.recipe_path = dash(argv[7]);
        a.mbr_dir = dash(argv[8]);
        /* what the daemon hands an ABI v2 plugin: the loaded object's path */
        snprintf(self, sizeof self, "%s", argv[2]);
        a.self_path = self;
        rc = cmdfn(&a);
        printf("cmd=%d rc=%d\n", a.cmd, rc);
        return rc < 0 ? 100 - rc : rc;
    }

    fprintf(stderr, "ivpack-probe: unknown op %s\n", op);
    return 1;
}
