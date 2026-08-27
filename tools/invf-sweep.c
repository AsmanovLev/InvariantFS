/*
 * invf-sweep.c — offline sweep driver for Linux
 *
 *   invf-sweep <image> [--dry-run]
 *
 * Walks live records (same CRC-validated scan as invf-ls), feeds every
 * regular file with segments to vol_sweep_one() — the unified per-inode
 * dispatch (containers/transcodes/text-batching/generic ZSTD-19). Then the
 * per-segment dedupe pass (vol_sweep_dedupe, WP12(h)) merges identical
 * stored segments. Text candidates defer into the volume's accumulator and
 * are sealed into shared PPMd batches by vol_tz_flush() at the end of the
 * run, after the dead-batch GC (vol_tz_gc). The author's sweep.c CLI is
 * Windows-only.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "invarifs.h"
#include "volume.h"
#include "codec.h"

typedef struct sw_bucket { struct sw_bucket *next; int slot; } sw_bucket;

/* WP14b: container-part deferrals ("name!partN") are aggregated per
 * container and printed as one summary line at the end of the walk --
 * a Silesia mozilla/samba/xml run would otherwise log 1573 near-identical
 * per-part lines. */
typedef struct {
    char prefix[256];   /* container name including the '!' */
    int  n_text;        /* parts deferred to PPMd batches */
    int  n_bin;         /* parts deferred to ZSTD batches */
} part_agg;

static part_agg *g_parts;
static size_t   g_parts_n, g_parts_cap;

static void part_agg_add(const char *name, int binary)
{
    const char *bang = strchr(name, '!');
    size_t plen = bang ? (size_t)(bang - name) + 1 : 0;
    size_t i;

    if (!plen || plen >= 256) return;
    for (i = 0; i < g_parts_n; i++)
        if (strncmp(g_parts[i].prefix, name, plen) == 0 &&
            g_parts[i].prefix[plen] == 0)
            break;
    if (i == g_parts_n) {
        if (g_parts_n == g_parts_cap) {
            size_t nc = g_parts_cap ? g_parts_cap * 2 : 16;
            part_agg *na = realloc(g_parts, nc * sizeof *na);
            if (!na) return;
            g_parts = na;
            g_parts_cap = nc;
        }
        memset(&g_parts[i], 0, sizeof g_parts[i]);
        memcpy(g_parts[i].prefix, name, plen);
        g_parts_n++;
    }
    if (binary) g_parts[i].n_bin++;
    else        g_parts[i].n_text++;
}

static void part_agg_print(void)
{
    size_t i;
    for (i = 0; i < g_parts_n; i++) {
        if (g_parts[i].n_bin)
            printf("  %s*: %d parts -> ZSTD batch\n", g_parts[i].prefix,
                   g_parts[i].n_bin);
        if (g_parts[i].n_text)
            printf("  %s*: %d parts -> PPMd batch\n", g_parts[i].prefix,
                   g_parts[i].n_text);
    }
    free(g_parts);
    g_parts = NULL;
    g_parts_n = g_parts_cap = 0;
}

static uint64_t sw_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static int sw_find(sw_bucket **tab, size_t mask, char (*names)[256],
                   const char *name)
{
    const sw_bucket *b;
    if (!tab) return -1;
    for (b = tab[sw_hash(name) & mask]; b; b = b->next)
        if (strcmp(names[b->slot], name) == 0) return b->slot;
    return -1;
}

static void sw_insert(sw_bucket ***tabp, size_t *maskp, size_t *countp,
                      char (*names)[256], int slot)
{
    sw_bucket *b;
    size_t h;
    if (!*tabp) {
        *tabp = (sw_bucket **)calloc(1024, sizeof **tabp);
        *maskp = 1023;
    } else if (*countp > *maskp) {
        size_t ncap = (*maskp + 1) * 2, i;
        sw_bucket **nt = (sw_bucket **)calloc(ncap, sizeof *nt);
        if (nt) {
            for (i = 0; i <= *maskp; i++) {
                sw_bucket *e = (*tabp)[i];
                while (e) {
                    sw_bucket *nx = e->next;
                    size_t nb = sw_hash(names[e->slot]) & (ncap - 1);
                    e->next = nt[nb]; nt[nb] = e;
                    e = nx;
                }
            }
            free(*tabp);
            *tabp = nt;
            *maskp = ncap - 1;
        }
    }
    b = (sw_bucket *)malloc(sizeof *b);
    if (!b) return;
    b->slot = slot;
    h = sw_hash(names[slot]) & *maskp;
    b->next = (*tabp)[h];
    (*tabp)[h] = b;
    (*countp)++;
}

int main(int argc, char **argv)
{
    invfs_volume *vol;
    const invfs_superblock *sb;
    int err, dry = 0;
    uint64_t bm, area_start, area_end, p;
    int count = 0, cap = 0, swept = 0, skipped = 0, failed = 0;
    char (*names)[256] = NULL;
    uint64_t *inodes = NULL;
    uint64_t *sizes = NULL;
    const char *img;
    sw_bucket **tab = NULL;
    size_t tmask = 0, tcount = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <image> [--dry-run]\n", argv[0]);
        return 2;
    }
    img = argv[1];
    dry = (argc == 3 && strcmp(argv[2], "--dry-run") == 0);

    /* per-file lines go to stdout, the summary to stderr: unbuffered, or a
     * redirected log tears a line at every 4 KB flush boundary */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* WP16b: the codec profile rides the environment (INVFS_PROFILE).
     * Capture the setting BEFORE vol_open publishes the default into the
     * env: a default run must produce byte-identical LOGS too. */
    {
        int prof_from_env = getenv("INVFS_PROFILE") != NULL;
        vol = vol_open(img, &err);
        if (!vol) {
            fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
            return 1;
        }
        if (prof_from_env)
            fprintf(stderr, "profile: %s (generic zstd level %d)\n",
                    invfs_profile_name((int)vol_get_profile(vol)),
                    invfs_profile_zstd_level((int)vol_get_profile(vol)));
    }
    /* WP10 memory policy: same size grammar as INVFS_ARC_BYTES in volume.c;
     * unset keeps the volume default. */
    {
        const char *dl = getenv("INVFS_DEC_MEM_LIMIT");
        if (dl) {
            char *endp = NULL;
            unsigned long long want = strtoull(dl, &endp, 10);
            unsigned long long mult = 1;
            int ok = (endp != dl);
            if (ok) {
                while (*endp == ' ' || *endp == '\t') endp++;
                switch (*endp) {
                    case 'k': case 'K': mult = 1024ull; endp++; break;
                    case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
                    case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
                    default: break;
                }
                if (*endp == 'b' || *endp == 'B') endp++;
                while (*endp == ' ' || *endp == '\t') endp++;
                if (*endp != '\0') ok = 0;
                if (want > (unsigned long long)SIZE_MAX / mult) ok = 0;
            }
            if (ok)
                vol_set_dec_mem_limit(vol, (uint64_t)(want * mult));
            else
                fprintf(stderr, "[sweep] INVFS_DEC_MEM_LIMIT=\"%s\" is not a "
                                "size; ignored\n", dl);
        }
    }
    sb = vol_sb(vol);
    bm = (sb->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    area_start = (sb->metadata_zone_start + bm + INVFS_JOURNAL_BLOCKS)
                 * INVFS_BLOCK_SIZE;
    area_end = vol_inode_area_pos(vol);
    p = area_start;

    /* collect live regular files */
    while (p + sizeof(invfs_inode_rec) <= area_end) {
        invfs_inode_rec h;
        char name[257];
        uint32_t crc_stored, crc_calc;
        uint8_t *rb;

        if (vol_read_raw(vol, p, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.name_len > 256 || h.rec_len < sizeof(invfs_inode_rec) ||
            h.rec_len > INVFS_MAX_REC_LEN) break;
        if (vol_read_raw(vol, p + offsetof(invfs_inode_rec, name),
                         name, h.name_len) != 0) break;
        name[h.name_len] = 0;
        rb = (uint8_t *)malloc((size_t)h.rec_len + 4);
        if (!rb) break;
        if (vol_read_raw(vol, p, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
        memcpy(&crc_stored, rb + h.rec_len, 4);
        crc_calc = invfs_crc32c(rb, h.rec_len);
        free(rb);
        p += (uint64_t)h.rec_len + 4;
        if (crc_calc != crc_stored) continue;

        if (h.magic == TOMBSTONE_MAGIC) {
            if (h.file_size == 0) {   /* legacy kill-by-id */
                int i = sw_find(tab, tmask, names, name);
                if (i >= 0 && inodes[i] == h.inode_id) inodes[i] = 0;
            }
            continue;
        }

        {
            int i = sw_find(tab, tmask, names, name);
            if (i >= 0) inodes[i] = h.inode_id, sizes[i] = h.file_size;
            else {
                if (count == cap) {
                    int ncap = cap ? cap * 2 : 512;
                    char (*nn)[256] =
                        (char (*)[256])realloc(names, (size_t)ncap * 256);
                    uint64_t *ni =
                        (uint64_t *)realloc(inodes, (size_t)ncap * sizeof *ni);
                    uint64_t *ns =
                        (uint64_t *)realloc(sizes, (size_t)ncap * sizeof *ns);
                    if (!nn || !ni || !ns) {
                        fprintf(stderr, "out of memory\n");
                        return 1;
                    }
                    names = nn; inodes = ni; sizes = ns; cap = ncap;
                }
                strncpy(names[count], name, 256);
                names[count][255] = 0;
                inodes[count] = h.inode_id;
                sizes[count] = h.file_size;
                sw_insert(&tab, &tmask, &tcount, names, count);
                count++;
            }
        }
    }

    fprintf(stderr, "live entries: %d\n", count);

    /* sweep candidates: regular files with actual payload */
    for (int i = 0; i < count; i++) {
        if (inodes[i] == 0 || sizes[i] == 0) { skipped++; continue; }
        if (dry) { printf("would sweep %s (%llu bytes)\n",
                          names[i], (unsigned long long)sizes[i]); continue; }
        {
            /* vol_sweep_one: 0 = nothing to do, >0 = transcoded/swept,
             * 7 = JPEG->JXL, 9 = text deferred into the batch accumulator,
             * 10 = binary deferred into the WP14a binary accumulator (both
             * sealed by vol_tz_flush below), 11 = exe-as-container carve
             * (WP14b M2), >=100 = codecpack transcode (100+algo, WP13),
             * <0 = hard error */
            int rc = vol_sweep_one(vol, inodes[i], names[i]);
            if (rc == 9) {
                if (strchr(names[i], '!'))
                    part_agg_add(names[i], 0);
                else
                    printf("  %s: text -> PPMd batch\n", names[i]);
            }
            else if (rc == 10) {
                if (strchr(names[i], '!'))
                    part_agg_add(names[i], 1);
                else
                    printf("  %s: binary -> ZSTD batch\n", names[i]);
            }
            else if (rc == 11) {
                swept++;
                printf("  %s: exe media -> JXL (%u parts)\n", names[i],
                       vol_exer_last_parts(vol));
            }
            else if (rc == 7) {
                swept++;
                printf("  %s: JPEG -> JXL (lossless)\n", names[i]);
            }
            else if (rc >= 100) {
                const invfs_codec *pc = invfs_codec_by_algo((uint32_t)(rc - 100));
                swept++;
                printf("  %s: %s (codecpack)\n", names[i],
                       pc ? pc->name : "unknown-pack");
            }
            else if (rc > 0) swept++;
            else if (rc == 0) skipped++;
            else failed++;
        }
        if ((swept + skipped) % 5000 == 0)
            fprintf(stderr, "  ..%d done (swept=%d)\n", swept + skipped, swept);
    }

    /* WP14b: print the aggregated container-part deferral lines collected
     * during the walk (one line per container instead of one per part) */
    part_agg_print();

    /* WP12(h): per-segment dedupe between the walk and the text-batch GC
     * (order: walk -> dedupe -> GC -> flush). The walk's transcodes are
     * what create the duplicates worth finding -- identical content lands
     * in Shadow as identical segments -- and dedupe runs before the GC so
     * it never sees a zone==TEXT entry (WP10 §11). The pass prints its
     * own merged/freed counts. */
    if (!dry) {
        if (vol_sweep_dedupe(vol) < 0)
            fprintf(stderr, "dedupe: pass failed (sweep results are intact)\n");
    }

    /* WP10 §7 + WP14a: reclaim owner batches no live member references,
     * then seal the accumulated text AND binary candidates into shared
     * batches (one vol_tz_flush drains both accumulators). The deferred
     * counts come from the accumulators themselves: parts deferred at
     * container-explode time (WP14b) never produced a walk line. */
    if (!dry) {
        int gcrc = vol_tz_gc(vol);
        size_t tzp = vol_acc_pending(vol, 0);
        size_t bzp = vol_acc_pending(vol, 1);
        int tzrc;
        if (gcrc > 0)
            printf("text gc: %u dead batches reclaimed\n", (unsigned)gcrc);
        else if (gcrc < 0)
            fprintf(stderr, "text gc failed (rc=%d)\n", gcrc);
        tzrc = vol_tz_flush(vol);
        if (tzrc == 0) {
            if (tzp)
                printf("text batches flushed (%zu deferred)\n", tzp);
            if (bzp)
                printf("binary batches flushed (%zu deferred)\n", bzp);
        }
        else if (tzrc < 0) {
            fprintf(stderr, "batch flush failed (rc=%d)\n", tzrc);
            failed++;
        }
    }

    fprintf(stderr, "sweep done: swept=%d skipped=%d failed=%d\n",
            swept, skipped, failed);

    if (!dry) {
        if (vol_flush(vol) != 0)
            fprintf(stderr, "warning: final flush failed\n");
    }
    vol_close(vol);
    return failed ? 1 : 0;
}
