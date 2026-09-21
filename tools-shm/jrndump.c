/* jrndump.c — dump the replayed in-memory L2P table + owner ids. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume_internal.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    if (argc < 2) { fprintf(stderr, "usage: jrndump <img>\n"); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    uint64_t par = vol_find(v, "\x01parity");
    uint64_t par2 = vol_find(v, "\x01parity2");
    uint64_t tzb = vol_find(v, "\x01tzb");
    uint64_t reten = vol_find(v, "\x01reten");
    printf("owners: \\x01parity=%llu \\x01parity2=%llu \\x01tzb=%llu "
           "\\x01reten=%llu\n",
           (unsigned long long)par, (unsigned long long)par2,
           (unsigned long long)tzb, (unsigned long long)reten);
    printf("l2p_count=%zu j_slotted=%d j_slot=%u\n",
           v->l2p_count, v->j_slotted, v->j_slot);
    for (size_t i = 0; i < v->l2p_count && i < 80; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        const char *who = e->inode == par ? "PARITY" :
                          e->inode == par2 ? "PARITY2" :
                          e->inode == tzb ? "TZB" :
                          e->inode == reten ? "RETEN" : "";
        printf("  [%zu] type=%02x inode=%llu lba=%llu pba=%llu len=%u %s\n",
               i, e->type, (unsigned long long)e->inode,
               (unsigned long long)e->lba, (unsigned long long)e->pba,
               e->length, who);
    }
    vol_close(v);
    return 0;
}
