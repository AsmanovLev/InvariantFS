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
    uint64_t size = 0;
    /* читаем по 64KB со всех границ */
    for (uint64_t off = 0; off < 13279232; off += 65536) {
        char buf[65536];
        int got = vol_read_range(v, inode, off, 65536, buf);
        if (got < 0) { printf("FAIL at offset %llu\n", (unsigned long long)off); break; }
        if (got == 0 && off < 13279232) { printf("SHORT at offset %llu (got 0)\n", (unsigned long long)off); break; }
    }
    printf("range scan done\n");
    vol_close(v);
    return 0;
}
