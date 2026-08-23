#include <stdio.h>
#include <stdlib.h>
#include "invarifs.h"
#include "volume.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("open fail\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *jpg = malloc(sz);
    fread(jpg, 1, sz, f);
    fclose(f);
    uint8_t *jxl = NULL;
    size_t jxl_len = 0;
    int rc = invfs_jxl_compress(jpg, (size_t)sz, &jxl, &jxl_len);
    printf("compress rc=%d jxl_len=%zu (orig %ld)\n", rc, jxl_len, sz);
    if (rc == 0 && jxl) {
        printf("jxl size: %zu\n", jxl_len);
        free(jxl);
    }
    free(jpg);
    return 0;
}
