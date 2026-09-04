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
    /* случайные смещения (как FUSE: произвольные offset+size) */
    unsigned long long offs[] = { 0, 511, 4096, 65535, 65536, 65537, 70000,
                                  1048576, 6291456, 6291457, 13279231, 13279200 };
    size_t sizes[] = { 512, 4096, 65536, 131072, 1 };
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 5; j++) {
            char buf[131072];
            int got = vol_read_range(v, inode, offs[i], sizes[j], buf);
            printf("off=%llu size=%zu -> %d\n", offs[i], sizes[j], got);
            if (got < 0) return 1;
        }
    }
    printf("done\n");
    vol_close(v);
    return 0;
}
