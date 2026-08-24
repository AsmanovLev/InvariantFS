/* meta_probe.c — isolate vol_apply_meta failures outside FUSE.
 * usage: meta_probe <image> <name> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0, rc;
    uint64_t id, r2;
    invfs_meta_pub m;

    if (argc < 3) { fprintf(stderr, "usage: %s <img> <name>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    v = vol_open(argv[1], &err);
    if (!v) { printf("open FAIL err=%d\n", err); return 1; }
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
