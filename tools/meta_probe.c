/* meta_probe.c — isolate vol_apply_meta failures outside FUSE.
 *
 * usage: meta_probe <image> <name>           legacy probe (MUTATES:
 *                                              applies a test setattr)
 *        meta_probe <image> --heat <name>    WP19 read-only dump: storage
 *                                            class + per-entry heat counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "volume.h"
#include "invarifs.h"

/* WP19: dump the file's storage class, its AST block entries (zone/algo
 * per segment) and the heat counters of every live L2P mapping it owns.
 * Read-only: nothing here touches the vol_read_* paths, so no heat
 * accrues and the volume closes clean. */
static int heat_dump(invfs_volume *v, const char *name)
{
    uint64_t id = vol_find(v, name);
    const invfs_l2p_entry *l2p;
    size_t n = 0, i, shown = 0;
    uint8_t cls = 0, algo = 0;
    uint16_t gen = 0;
    uint64_t pos, p;
    uint8_t *rec = NULL;
    uint32_t rl = 0, rec_rl = 0;

    printf("find(%s)=%llu\n", name, (unsigned long long)id);
    if (!id) return 1;
    if (vol_get_class(v, id, &cls, &algo, &gen) == 0)
        printf("class=%u algo=%u gen=%u\n", cls, algo, gen);
    else
        printf("class=absent\n");

    /* AST entries of the LIVE record (newest version with this id) */
    pos = vol_inode_area_start(v);
    while ((p = vol_inode_next(v, pos, NULL, NULL, NULL, NULL, 0, &rl)) != 0) {
        uint8_t *buf;
        invfs_inode_rec rh;
        if (vol_read_raw(v, p - rl - 4, &rh, sizeof rh) != 0) break;
        if (rh.magic == TOMBSTONE_MAGIC || rh.inode_id != id) { pos = p; continue; }
        buf = (uint8_t *)malloc(rl);
        if (buf && vol_read_raw(v, p - rl - 4, buf, rl) == 0) {
            free(rec);
            rec = buf;   /* keep the last match: newest wins */
            rec_rl = rl;
        } else {
            free(buf);
        }
        pos = p;
    }
    if (rec) {
        invfs_ast_recipe_header ah;
        size_t base = sizeof(invfs_inode_rec);
        if (rec_rl >= base + sizeof ah) {
            const invfs_ast_block_entry *ents;
            uint16_t k;
            memcpy(&ah, rec + base, sizeof ah);
            ents = (const invfs_ast_block_entry *)(rec + base + sizeof ah);
            for (k = 0; k < ah.num_blocks; k++) {
                if (base + sizeof ah + (size_t)(k + 1) * sizeof(*ents) > rec_rl)
                    break;
                printf("ast i=%u off=%llu len=%llu zone=%u algo=%u "
                       "bid=%u boff=%u\n", k,
                       (unsigned long long)ents[k].file_offset,
                       (unsigned long long)ents[k].length,
                       ents[k].zone, ents[k].algo,
                       ents[k].block_id, ents[k].block_offset);
            }
        }
        free(rec);
    }

    l2p = vol_l2p(v, &n);
    for (i = 0; i < n; i++) {
        const invfs_l2p_entry *e = &l2p[i];
        if (e->type != INVFS_JRN_MAP || e->inode != id) continue;
        printf("entry lba=%llu pba=%llu len=%u rheat=%u wheat=%u\n",
               (unsigned long long)e->lba,
               (unsigned long long)e->pba, e->length,
               vol_heat_r(e), vol_heat_w(e));
        shown++;
    }
    printf("entries=%zu\n", shown);
    return 0;
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0, rc;
    uint64_t id, r2;
    invfs_meta_pub m;

    if (argc < 3) { fprintf(stderr, "usage: %s <img> <name>|--heat <name>\n",
                            argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    v = vol_open(argv[1], &err);
    if (!v) { printf("open FAIL err=%d\n", err); return 1; }
    if (strcmp(argv[2], "--heat") == 0) {
        if (argc < 4) { vol_close(v); return 2; }
        rc = heat_dump(v, argv[3]);
        vol_close(v);
        return rc;
    }
    id = vol_find(v, argv[2]);
    printf("find(%s)=%llu\n", argv[2], (unsigned long long)id);
    if (!id) { vol_close(v); return 1; }
    rc = vol_get_meta(v, id, &m);
    printf("get_meta#1 rc=%d", rc);
    if (rc == 0) printf(" type=%d mode=%o uid=%u gid=%u target='%s'",
                        m.type, m.mode, m.uid, m.gid, m.target);
    printf("\n");

    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0755;
    m.uid = 1000;
    m.gid = 1000;
    m.mtime = m.atime = (int64_t)time(NULL);
    m.nlink = 1;
    r2 = vol_apply_meta(v, argv[2], &m);
    printf("apply_meta -> %llu (0=fail)\n", (unsigned long long)r2);

    rc = vol_get_meta(v, id, &m);
    printf("get_meta#2 rc=%d", rc);
    if (rc == 0) printf(" type=%d mode=%o uid=%u gid=%u", m.type, m.mode, m.uid, m.gid);
    printf("\n");
    vol_close(v);
    return 0;
}
