#include <stdio.h>
#include <stdlib.h>
#include "invarifs.h"
#include "volume.h"
int main(int argc, char **argv) {
    int err;
    invfs_volume *v = vol_open(argv[1], &err);
    if (!v) { printf("open fail %d\n", err); return 1; }
    uint64_t inode = vol_find(v, "gost.exe");
    if (!inode) { printf("not found\n"); return 1; }
    uint64_t file_size = 13279232;
    size_t sizes[] = { 131072, 524288, 1048576 };
    for (int si = 0; si < 3; si++) {
        size_t size = sizes[si];
        printf("=== size=%zu ===\n", size);
        int fail = 0;
        for (uint64_t off = 0; off < file_size; off += size) {
            static char buf[1048576];
            int got = vol_read_range(v, inode, off, size, buf);
            if (got < 0) { printf("  FAIL at off=%llu\n", (unsigned long long)off); fail = 1; break; }
            if (got == 0 && off < file_size) { printf("  SHORT0 at off=%llu\n", (unsigned long long)off); fail = 1; break; }
        }
        if (!fail) printf("  ok\n");
    }
    vol_close(v);
    return 0;
}
