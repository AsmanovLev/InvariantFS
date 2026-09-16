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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "usage: %s <volume-image|idle-block-device>\n"
                "  read-only overview: population, zones, compression ratios\n"
                "NOTE: the volume must NOT be actively mounted by invf-fuse\n"
                "      (open the image file from the host, or run this from the\n"
                "       initramfs shell before switch_root for the root device)\n",
                argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }
    if (argc != 2) {
        fprintf(stderr,
            "usage: %s <volume-image|idle-block-device>\n"
            "  read-only overview: population, zones, compression ratios\n"
            "NOTE: the volume must NOT be actively mounted by invf-fuse\n"
            "      (open the image file from the host, or run this from the\n"
            "       initramfs shell before switch_root for the root device)\n",
            argv[0]);
        return 2;
    }
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
        printf("zones detail (physical used = content class, WP-DZ):\n");
        {
            double lr = st.logic_raw_bytes / 1048576.0;
            double ls = st.logic_shadow_bytes / 1048576.0;
            double pr = st.raw_used_bytes / 1048576.0;
            double ps = st.shadow_used_bytes / 1048576.0;
            if (st.raw_used_bytes)
                printf("  RAW   : logic %8.1f MiB | used %8.1f MiB | %.2fx\n",
                       lr, pr, pr ? lr / pr : 0);
            else
                printf("  RAW   : empty\n");
            if (st.shadow_used_bytes)
                printf("  SHADOW: logic %8.1f MiB | used %8.1f MiB | %.2fx\n",
                       ls, ps, ps ? ls / ps : 0);
            else
                printf("  SHADOW: empty (not swept yet)\n");
            if (st.logic_text_bytes || st.text_used_bytes)
                printf("  TEXT  : logic %8.1f MiB | used %8.1f MiB "
                       "(shared batches, PPMd text / ZSTD binary)\n",
                       st.logic_text_bytes / 1048576.0,
                       st.text_used_bytes / 1048576.0);
            if (st.unclaimed_used_bytes)
                printf("  unclaimed: %.1f MiB (allocated, no live reference;"
                       " fsck reclaims)\n",
                       st.unclaimed_used_bytes / 1048576.0);
        }
    }
    /* WP25: two-device state -- device table, mirror freshness, the RAW
     * mirror and the dev0 tier-arena copies. Absent on single-device
     * volumes (the print is the compat surface). */
    if (vol_ndev(v) == 2) {
        uint64_t mblocks = 0, tblocks = 0, cpba = 0, dpba = 0;
        uint64_t mcnt = vol_rawm_count(v, &mblocks);
        uint64_t tcnt = vol_tier_count(v, &tblocks, &cpba, &dpba);
        printf("two-device (WP25)  :\n");
        printf("  devices          : %d%s\n", vol_ndev(v),
               vol_degraded(v) ? " (DEGRADED: dev0 absent, read-only)" : "");
        printf("  mirror           : %s\n",
               vol_mirror_stale(v) ? "STALE (resync at next flush)"
                                   : "in sync");
        printf("  raw mirror       : %" PRIu64 " segments, %" PRIu64
               " blocks\n", mcnt, mblocks);
        printf("  tier (dev0 copies): %" PRIu64 " live, %" PRIu64
               " blocks\n", tcnt, tblocks);
        if (tcnt)
            printf("  first tier copy  : canonical pba %" PRIu64
                   " -> dev0 pba %" PRIu64 "\n", cpba, dpba);
    }
    vol_close(v);
    return 0;
}
