/*
 * ophelper.c — tools/fuzz helper for the ops the CLI tools do not expose
 * (there is deliberately no `invf-rm`; the e2e scripts use the same
 * tzrm-style throwaway helper convention). Public volume.h API only.
 *
 *   ophelper <image> rm <name> [name...]        delete files, flush, close
 *   ophelper <image> rm-nonexistent <name>      delete must FAIL (rc 2)
 *
 * Build: tools/test-fuzz.sh does it, from the Makefile's own lists --
 * `build/core_objs.txt` (the whole engine) and `make print-incdirs` for
 * the include path. Do not copy a build line from here: an abbreviated
 * one is what let this harness drift out of the link entirely.
 */
#include <stdio.h>
#include <string.h>
#include "volume.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0, rc = 0, i;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <image> rm <name>...\n", argv[0]);
        return 2;
    }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    for (i = 3; i < argc; i++) {
        if (vol_delete_file(v, argv[i]) != 0) {
            fprintf(stderr, "rm %s failed\n", argv[i]);
            rc = 1;
        }
    }
    if (rc == 0 && vol_flush(v) != 0) {
        fprintf(stderr, "flush failed\n");
        rc = 1;
    }
    vol_close(v);
    return rc;
}
