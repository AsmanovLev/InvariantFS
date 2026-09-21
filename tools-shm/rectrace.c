/* rectrace.c — dump every record version (INOD/DELT) for name-patterns. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume_internal.h"

static int cb(void *ctx_, uint64_t rec_pos, const invfs_inode_rec *h,
              const uint8_t *rec)
{
    const char *flt = (const char *)ctx_;
    char name[257];
    size_t nl = h->name_len < 255 ? h->name_len : 255;
    (void)rec;
    memcpy(name, h->name, nl);
    name[nl] = 0;
    if (flt && *flt && !strstr(name, flt)) return 0;
    {
        /* make control bytes printable */
        char pn[300];
        size_t j = 0;
        for (size_t i = 0; name[i] && j < 280; i++) {
            if ((uint8_t)name[i] < 0x20) { pn[j++] = '\\'; pn[j++] = 'x';
                pn[j++] = "0123456789abcdef"[(uint8_t)name[i] >> 4];
                pn[j++] = "0123456789abcdef"[(uint8_t)name[i] & 15]; }
            else pn[j++] = name[i];
        }
        pn[j] = 0;
        printf("pos=%llu %s inode=%llu rec_len=%u file_size=%llu\n",
               (unsigned long long)rec_pos,
               h->magic == INODE_REC_MAGIC ? "INOD" :
               h->magic == TOMBSTONE_MAGIC ? "DELT" : "????",
               (unsigned long long)h->inode_id, h->rec_len,
               (unsigned long long)h->file_size);
    }
    return 0;
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    if (argc < 2) { fprintf(stderr, "usage: rectrace <img> [name-filter]\n"); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    vol_records_walk(v, cb, argc > 2 ? argv[2] : (char *)"");
    uint64_t id = vol_find(v, argc > 2 ? argv[2] : "");
    printf("vol_find(%s) = %llu, pos=%llu\n", argc > 2 ? argv[2] : "",
           (unsigned long long)id,
           (unsigned long long)(id ? idx_get_id(v, id) : 0));
    vol_close(v);
    return 0;
}
