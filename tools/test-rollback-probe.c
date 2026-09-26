/* rbprobe — measure the v3 SPT0 rollback contract.
 *
 *   rbprobe <img>
 *
 * Answers the questions the retired v2 CKP0 legs used to ask, on v3:
 *   D  retention fidelity: a delete issued AFTER the savepoint, then a
 *      restore -- does the file come back?
 *   G  overwrite under a live savepoint: are the pre-overwrite bytes
 *      recoverable after a restore?
 *   C  spt0_drop is the point of no return
 * Prints one key=value per step; the caller asserts.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "volume_internal.h"
#include "vol_spt0.h"

static const char *EN(int rc)
{
    switch (rc) {
    case 0:      return "OK";
    case -ESTALE: return "ESTALE";
    case -ENOENT: return "ENOENT";
    case -ENOSPC: return "ENOSPC";
    default:     return "ERR";
    }
}

static void show(invfs_volume *v, const char *tag, const char *name)
{
    invfs_meta_pub m;
    uint64_t id = vol_find(v, name);
    uint8_t *buf = NULL;
    size_t len = 0;

    if (!id) { printf("%s_present=0\n", tag); return; }
    printf("%s_present=1\n", tag);
    memset(&m, 0, sizeof m);
    vol_get_meta(v, id, &m);
    printf("%s_size=%llu\n", tag, (unsigned long long)m.size);
    if (vol_read_inode(v, id, 0, &buf, &len) == 0) {
        printf("%s_bytes=", tag);
        for (size_t i = 0; i < len; i++)
            putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        putchar('\n');
        free(buf);
    } else {
        printf("%s_bytes=<unreadable>\n", tag);
    }
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    invfs_meta_pub m;
    invfs_spt0 sp;
    int err = 0, rc;

    if (argc < 2) return 2;
    if (!(v = vol_open(argv[1], &err))) { fprintf(stderr, "open %d\n", err); return 1; }

    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode  = 0644;
    m.nlink = 1;

    /* ---- D: delete after the savepoint must be undone by the restore ---- */
    m.size = 4;
    vol_v3_write_bulk(v, "d.txt", (const uint8_t *)"AAAA", 4, &m);
    show(v, "d_pre_capture", "d.txt");

    rc = spt0_capture(v);
    printf("d_capture_rc=%d %s\n", rc, EN(rc));

    rc = vol_v3_unlink(v, "d.txt");
    printf("d_unlink_rc=%d %s\n", rc, EN(rc));
    show(v, "d_after_unlink", "d.txt");

    rc = spt0_restore(v);
    printf("d_restore_rc=%d %s\n", rc, EN(rc));
    show(v, "d_after_restore", "d.txt");

    /* ---- G: overwrite under a live savepoint keeps the old bytes ---- */
    m.size = 6;
    vol_v3_write_bulk(v, "g.txt", (const uint8_t *)"GGGGGG", 6, &m);
    rc = spt0_capture(v);
    printf("g_capture_rc=%d %s\n", rc, EN(rc));
    m.size = 4;
    vol_v3_write_bulk(v, "g.txt", (const uint8_t *)"ZZZZ", 4, &m);
    show(v, "g_after_overwrite", "g.txt");
    rc = spt0_restore(v);
    printf("g_restore_rc=%d %s\n", rc, EN(rc));
    show(v, "g_after_restore", "g.txt");

    /* ---- C: drop is the point of no return ---- */
    m.size = 4;
    vol_v3_write_bulk(v, "c.txt", (const uint8_t *)"CCCC", 4, &m);
    rc = spt0_capture(v);
    printf("c_capture_rc=%d %s\n", rc, EN(rc));
    spt0_info(v, &sp);
    printf("c_info_present=1 base_root=%llu delta_end=%llu\n",
           (unsigned long long)sp.base_root, (unsigned long long)sp.delta_end);
    rc = spt0_drop(v);
    printf("c_drop_rc=%d %s\n", rc, EN(rc));
    rc = spt0_restore(v);
    printf("c_restore_after_drop_rc=%d %s\n", rc, EN(rc));
    show(v, "c_after_failed_restore", "c.txt");

    vol_close(v);
    return 0;
}
