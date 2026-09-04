#include <stdio.h>
#include "invarifs.h"
#include "volume.h"
int main(int argc, char **argv) {
    int err;
    invfs_volume *v = vol_open(argv[1], &err);
    if (!v) return 1;
    int rc = vol_delete_file(v, argv[2]);
    printf("delete %s: %d\n", argv[2], rc);
    vol_flush(v);
    vol_close(v);
    return 0;
}
