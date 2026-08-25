/* invf-stats: read-only overview of an InvariantFS volume. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "volume.h"

int main(int argc, char **argv)
{
    int err;
    invfs_volume *v;
    const invfs_superblock *sb;
    invfs_volume_stats st = {0};
    uint64_t meta_blocks, raw_blocks, shadow_blocks;

    if (argc != 2) { fprintf(stderr, "usage: %s <volume>\n", argv[0]); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open failed err=%d\n", err); return 1; }
    sb = vol_sb(v);
    if (vol_compute_stats(v, &st) != 0) { fprintf(stderr, "walk failed\n"); return 1; }

    meta_blocks   = sb->metadata_zone_blocks;
    raw_blocks    = sb->shadow_zone_start - sb->raw_zone_start;
    shadow_blocks = sb->total_blocks - sb->shadow_zone_start;
    {
        uint64_t total_b = (uint64_t)sb->total_blocks * INVFS_BLOCK_SIZE;
        uint64_t free_b  = (uint64_t)vol_count_free(v) * INVFS_BLOCK_SIZE;
        uint64_t used_b  = total_b - free_b;
        double ratio = used_b ? (double)st.logical_bytes / (double)used_b : 0;

        printf("InvariantFS volume : %s\n", argv[1]);
        printf("  state            : 0x%02X (%s)\n", sb->state,
               sb->state == INVFS_STATE_CLEAN ? "CLEAN" :
               sb->state == INVFS_STATE_DIRTY ? "DIRTY" : "other");
        printf("  capacity         : %d blocks x 4K = %.1f GiB\n",
               (int)sb->total_blocks, total_b / 1073741824.0);
        printf("  zones            : meta %.1f MiB | raw %d blk | shadow %d blk\n",
               meta_blocks * 4096 / 1048576.0,
               (int)raw_blocks, (int)shadow_blocks);
        printf("  used             : %.1f MiB | free: %.1f MiB\n",
               used_b / 1048576.0, free_b / 1048576.0);
        printf("population (live)  :\n");
        printf("  regular files    : %" PRIu64 "\n", st.files);
        printf("  directories      : %" PRIu64 "\n", st.dirs);
        printf("  symlinks         : %" PRIu64 "\n", st.links);
        printf("  special nodes    : %" PRIu64 "\n", st.special);
        printf("  churn            : %" PRIu64 " tombstones, %" PRIu64 " bad records\n",
               st.tombstones, st.bad_records);
        printf("data               :\n");
        printf("  logical bytes    : %.1f MiB\n", st.logical_bytes / 1048576.0);
        printf("  biggest file     : %.1f KiB %s\n",
               st.biggest_size / 1024.0, st.biggest_name);
        printf("  est. ratio       : %.2fx logical/on-disk\n", ratio);
    }
    vol_close(v);
    return 0;
}
