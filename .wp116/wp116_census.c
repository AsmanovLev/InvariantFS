/* wp116_census.c — WP116 instrumentation (NOT a product tool).
 *
 * Read-only block census of an InvariantFS v3 volume. Answers the question
 * the leak raises: of the blocks the bitmap says are allocated, how many
 * are v3 COW base pages, and of those, how many belong to generations that
 * the current published root cannot reach?
 *
 * usage: wp116_census <image>
 *
 * Read-only: never writes, never calls vol_flush, so a probe cannot dirty
 * the volume it is measuring.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_metabuf.h"

/* The tool must not mutate: force the volume read-only for the census. */
static uint64_t g_page_gens_touched[64];
static int      g_page_gen_n;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    uint64_t i, total, alloc = 0, free_b;
    uint64_t page_alloc = 0, page_live = 0, page_dead = 0;
    uint64_t other_alloc = 0, rt30_pages = 0;
    uint64_t *gen_counts = NULL;
    uint64_t max_gen = 0;
    invfs_blkptr root;
    uint64_t *mark = NULL;
    uint8_t  *page = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <image>\n", argv[0]);
        return 2;
    }
    v = vol_open(argv[1], 0);
    if (!v) { fprintf(stderr, "vol_open failed\n"); return 1; }

    total = v->sb.total_blocks;
    free_b = vol_count_free(v);
    alloc = total - free_b;

    printf("image            %s\n", argv[1]);
    printf("total_blocks     %llu\n", (unsigned long long)total);
    printf("free_blocks      %llu\n", (unsigned long long)free_b);
    printf("alloc_blocks     %llu\n", (unsigned long long)alloc);
    printf("alloc_bytes      %llu\n", (unsigned long long)(alloc * INVFS_BLOCK_SIZE));
    printf("meta_zone        start=%llu blocks=%llu\n",
           (unsigned long long)v->sb.metadata_zone_start,
           (unsigned long long)v->sb.metadata_zone_blocks);
    printf("vol_flags        0x%x  (VOLF_V3=%d)\n",
           v->sb.vol_flags, !!(v->sb.vol_flags & VOLF_V3));

    /* Pass 1: classify every allocated block by its on-disk magic, and
     * bucket v3 base pages by the generation stamped in their header. */
    page = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    gen_counts = (uint64_t *)calloc(total + 1, sizeof(uint64_t));
    mark = (uint8_t *)calloc(1, (size_t)((total + 7) / 8));
    if (!page || !gen_counts || !mark) { fprintf(stderr, "oom\n"); return 1; }

    for (i = 0; i < total; i++) {
        if (!bit_get(v->bitmap, i))
            continue;
        if (io_pread(&v->io, i * (uint64_t)INVFS_BLOCK_SIZE, page,
                     INVFS_BLOCK_SIZE) != 0)
            continue;
        if (memcmp(page, INVFS_PAGE_MAGIC, 4) == 0) {
            const invfs_page_hdr *h = mbuf_page_chdr(page);
            page_alloc++;
            gen_counts[h->gen]++;
            if (h->gen > max_gen) max_gen = h->gen;
        } else if (memcmp(page, "RT30", 4) == 0) {
            rt30_pages++;
        } else {
            other_alloc++;
        }
    }
    printf("\n-- allocated-block census --\n");
    printf("v3 base pages (BPG3)  %llu\n", (unsigned long long)page_alloc);
    printf("RT30 descriptor pages %llu\n", (unsigned long long)rt30_pages);
    printf("other allocated       %llu\n", (unsigned long long)other_alloc);
    printf("max base-page gen      %llu\n", (unsigned long long)max_gen);

    /* Pass 2: mark the live tree from the published root. */
    if (vol_v3_base_root(v, &root) == 0 && root.pba != 0) {
        extern int btree_mark_count(invfs_volume *, invfs_blkptr, uint8_t *);
        page_live = 0;
        /* btree_mark_count is not exported; use the reclaim module's walker
         * through a local re-implementation of the reachability walk. */
        {
            extern int wp116_mark(invfs_volume *, invfs_blkptr, uint8_t *);
            page_live = (uint64_t)wp116_mark(v, root, mark);
        }
    } else {
        printf("no v3 base root (empty tree)\n");
    }

    /* Pass 3: a page is leaked if it is a base page NOT in the live set. */
    for (i = 0; i < total; i++) {
        if (!bit_get(v->bitmap, i))
            continue;
        if (io_pread(&v->io, i * (uint64_t)INVFS_BLOCK_SIZE, page,
                     INVFS_BLOCK_SIZE) != 0)
            continue;
        if (memcmp(page, INVFS_PAGE_MAGIC, 4) != 0)
            continue;
        if (!bit_get(mark, i))
            page_dead++;
    }
    printf("\n-- reachability --\n");
    printf("live base pages       %llu\n", (unsigned long long)page_live);
    printf("unreachable base pages %llu  (%.1f%% of allocated)\n",
           (unsigned long long)page_dead,
           alloc ? 100.0 * (double)page_dead / (double)alloc : 0.0);
    printf("unreachable bytes     %llu\n",
           (unsigned long long)(page_dead * INVFS_BLOCK_SIZE));

    /* Generation histogram, largest first. */
    printf("\n-- base pages by generation (top 20) --\n");
    {
        uint64_t g, shown = 0;
        for (g = max_gen + 1; g-- > 0 && shown < 20; ) {
            if (gen_counts[g]) {
                printf("  gen %6llu : %llu pages%s\n",
                       (unsigned long long)g,
                       (unsigned long long)gen_counts[g],
                       (g == root.gen) ? "   <- published root" : "");
                shown++;
            }
            if (g == 0) break;
        }
    }

    free(page); free(gen_counts); free(mark);
    vol_close(v);
    return 0;
}
