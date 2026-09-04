/* Exercise the overwrite path the way the mount does.
   argv[3] = "old" reproduces the delete-then-create order, anything else
   (or nothing) uses vol_replace_file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v = vol_open(argv[1], &err);
    if (!v) { printf("open fail\n"); return 1; }
    const char *nm = argv[2];
    int old_order = argc > 3 && strcmp(argv[3], "old") == 0;
    static char buf[64];
    size_t len = (size_t)snprintf(buf, sizeof buf, "plain text now\n");
    uint64_t nid;
    if (old_order) {
        if (vol_find(v, nm) != 0) vol_delete_file(v, nm);
        nid = vol_create_file(v, nm, (const uint8_t *)buf, len);
    } else {
        nid = vol_replace_file(v, nm, (const uint8_t *)buf, len);
    }
    printf("overwrite %s (%s) -> inode %llu\n", nm,
           old_order ? "delete-first" : "replace", (unsigned long long)nid);
    vol_flush(v);
    vol_close(v);
    return 0;
}
