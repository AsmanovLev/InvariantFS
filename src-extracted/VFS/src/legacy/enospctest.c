/* Fill the volume, then try to overwrite a file with more than fits.
   The old contents must still be readable afterwards. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, i;
    invfs_volume *v = vol_open(argv[1], &err);
    if (!v) { printf("open fail\n"); return 1; }
    size_t big = 8u << 20;
    uint8_t *blob = (uint8_t *)malloc(big);
    for (size_t k = 0; k < big; k++) blob[k] = (uint8_t)(k * 2654435761u >> 13);

    const char *keep = "keep.bin";
    if (vol_create_file(v, keep, blob, 4096) == 0) { printf("seed fail\n"); return 1; }
    printf("seeded %s (4096 bytes)\n", keep);

    /* eat the free space */
    for (i = 0; i < 100000; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "filler%05d", i);
        if (vol_create_file(v, nm, blob, big) == 0) break;
    }
    printf("volume full after %d fillers\n", i);

    uint64_t nid = vol_replace_file(v, keep, blob, big);
    printf("replace %s with %zu bytes -> inode %llu (0 = refused)\n",
           keep, big, (unsigned long long)nid);

    uint8_t *back = NULL; size_t blen = 0;
    uint64_t ino = vol_find(v, keep);
    printf("%s still present: inode %llu\n", keep, (unsigned long long)ino);
    if (ino && vol_read_file(v, ino, &back, &blen) == 0)
        printf("readback %zu bytes, first byte %02x (expect 4096 / %02x)\n",
               blen, back[0], blob[0]);
    else
        printf("READBACK FAILED -- data lost\n");
    vol_flush(v);
    vol_close(v);
    return 0;
}
