/* invf-stats: space inspector and overview of an InvariantFS volume.
 *
 * Usage: invf-stats [-v|--verbose] [-n|--no-bar] [--files] <image|device>
 *   Default: draws colorful zone bar + summary overview
 *   -v, --verbose: detailed numerical metrics and zone breakdown
 *   -n, --no-bar: omit graphical block bar
 *   --files: list files by storage policy
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "volume.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define BAR_W 110      /* cells per zone bar */

/* ANSI colors */
#define C_RST   "\033[0m"
#define C_BLUE  "\033[94m"
#define C_GREEN "\033[92m"
#define C_CYAN  "\033[96m"
#define C_GRAY  "\033[90m"
#define C_DIM   "\033[2m"

static void enable_ansi(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    GetConsoleMode(h, &m);
    SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

static void human(uint64_t bytes, char *buf, size_t cap)
{
    double v = (double)bytes;
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(buf, cap, "%.1f %s", v, u[i]);
}

static void draw_bar(const invfs_volume_stats *st, uint64_t total_bytes,
                     uint64_t free_bytes)
{
    uint64_t raw_b     = st->raw_used_bytes;
    uint64_t shadow_b  = st->shadow_used_bytes;
    uint64_t text_b    = st->text_used_bytes;
    uint64_t unclaim_b = st->unclaimed_used_bytes;
    uint64_t used_b    = raw_b + shadow_b + text_b + unclaim_b;
    uint64_t data_total = used_b + free_bytes;
    if (data_total == 0) data_total = total_bytes ? total_bytes : 1;

    printf("  vol [");
    uint64_t rem = BAR_W;
    uint64_t groups[4] = { shadow_b, text_b, raw_b, unclaim_b };
    const char *cols[4] = { C_BLUE, C_CYAN, C_GRAY, C_GREEN };
    uint64_t cells[4];
    for (int g = 0; g < 4; g++)
        cells[g] = (uint64_t)((double)groups[g] / (double)data_total * rem + 0.5);
    for (int g = 0; g < 4; g++) {
        if (cells[g] > rem) cells[g] = rem;
        for (uint64_t c = 0; c < cells[g] && rem > 0; c++, rem--)
            printf("%s█%s", cols[g], C_RST);
    }
    for (uint64_t c = 0; c < rem; c++) printf(C_DIM "░" C_RST);
    printf("]\n");
}

typedef struct {
    char name[2 * INVFS_MAX_NAME + 2];
    uint64_t ino, fsz;
    uint8_t dir;
} fent;

typedef struct {
    fent *tbl;
    size_t nfiles, fcap;
    uint64_t ndirs;
    int oom;
} files_ctx;

static int files_cb(void *ctx_, const char *path, uint64_t ino,
                    uint32_t type, uint64_t size, int64_t mtime)
{
    files_ctx *c = (files_ctx *)ctx_;
    size_t nl;
    (void)mtime;

    if (c->nfiles == c->fcap) {
        size_t ncap = c->fcap ? c->fcap * 2 : 4096;
        fent *nt = (fent *)realloc(c->tbl, ncap * sizeof(fent));
        if (!nt) { c->oom = 1; return 1; }
        c->tbl = nt; c->fcap = ncap;
    }
    nl = strlen(path);
    if (nl >= sizeof c->tbl[0].name) nl = sizeof c->tbl[0].name - 1;
    memset(&c->tbl[c->nfiles], 0, sizeof(fent));
    memcpy(c->tbl[c->nfiles].name, path, nl);
    c->tbl[c->nfiles].name[nl] = 0;
    c->tbl[c->nfiles].ino = ino;
    c->tbl[c->nfiles].fsz = (type == INVFS_ITYP_DIR) ? 0 : size;
    c->tbl[c->nfiles].dir = (type == INVFS_ITYP_DIR) ? 1 : 0;
    c->nfiles++;
    if (type == INVFS_ITYP_DIR) c->ndirs++;
    return 0;
}

int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v;
    const invfs_superblock *sb;
    invfs_volume_stats st = {0};
    uint64_t image_allocated = 0;
    int verbose = 0;
    int no_bar = 0;
    int show_files = 0;
    const char *img_path = NULL;

#if !defined(_WIN32)
    struct stat image_stat;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "usage: %s [-v|--verbose] [-n|--no-bar] [--files] <image|device>\n"
                "  overview and space inspector for InvariantFS volumes\n"
                "  -v, --verbose  detailed numerical metrics and zone breakdown\n"
                "  -n, --no-bar   omit visual block bar\n"
                "  --files        list files in namespace\n"
                "NOTE: the volume must NOT be actively mounted by invf-fuse\n",
                argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-bar") == 0) {
            no_bar = 1;
        } else if (strcmp(argv[i], "--files") == 0) {
            show_files = 1;
        } else if (argv[i][0] != '-') {
            img_path = argv[i];
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
            return 2;
        }
    }

    if (!img_path) {
        fprintf(stderr, "usage: %s [-v|--verbose] [-n|--no-bar] [--files] <image|device>\n", argv[0]);
        return 2;
    }

    enable_ansi();
    v = vol_open(img_path, &err);
    if (!v) { fprintf(stderr, "open failed err=%d\n", err); return 1; }
    sb = vol_sb(v);

#if !defined(_WIN32)
    if (stat(img_path, &image_stat) == 0 && image_stat.st_blocks > 0)
        image_allocated = (uint64_t)image_stat.st_blocks * 512ULL;
#endif

    if (vol_compute_stats(v, &st) != 0) {
        fprintf(stderr, "walk failed\n");
        vol_close(v);
        return 1;
    }

    uint64_t total_b = (uint64_t)sb->total_blocks * INVFS_BLOCK_SIZE;
    uint64_t free_b  = (uint64_t)vol_count_free(v) * INVFS_BLOCK_SIZE;
    uint64_t used_b  = (total_b > free_b) ? (total_b - free_b) : 0;
    double ratio = used_b ? (double)st.logical_bytes / (double)used_b : 0;

    printf("\n" C_BLUE "InvariantFS" C_RST " volume : " C_DIM "%s" C_RST "\n", img_path);
    printf("  state            : 0x%02X (%s)\n", sb->state,
           sb->state == INVFS_STATE_CLEAN ? "CLEAN" :
           sb->state == INVFS_STATE_DIRTY ? "DIRTY" : "other");

    char cap_str[64];
    human(total_b, cap_str, sizeof cap_str);
    printf("  capacity         : %llu blocks x %u = %s\n\n",
           (unsigned long long)sb->total_blocks, INVFS_BLOCK_SIZE, cap_str);

    if (!no_bar) {
        draw_bar(&st, total_b, free_b);
        printf("\n  " C_GRAY "█ RAW (verbatim / as-is)" C_RST
               "  " C_BLUE "█ Shadow (consolidated / recipes)" C_RST
               "  " C_CYAN "█ Text (batch)" C_RST
               "  " C_GREEN "█ Unclaimed" C_RST
               "  " C_DIM "░ Free" C_RST "\n\n");
    }

    if (verbose) {
        uint64_t meta_blocks = sb->metadata_zone_blocks;
        uint64_t raw_blocks  = sb->shadow_zone_start - sb->raw_zone_start;
        uint64_t shadow_blocks = sb->total_blocks - sb->shadow_zone_start;

        printf("  zones            : meta %.1f MiB | raw %d blk | shadow %d blk\n",
               meta_blocks * 4096 / 1048576.0, (int)raw_blocks, (int)shadow_blocks);
        printf("  used             : %.1f MiB | free: %.1f MiB\n",
               used_b / 1048576.0, free_b / 1048576.0);
        if (image_allocated)
            printf("  image allocated  : %.1f MiB (host filesystem blocks)\n",
                   image_allocated / 1048576.0);
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
                printf("  TEXT  : logic %8.1f MiB | used %8.1f MiB (shared batches)\n",
                       st.logic_text_bytes / 1048576.0, st.text_used_bytes / 1048576.0);
            if (st.unclaimed_used_bytes)
                printf("  unclaimed: %.1f MiB (allocated, no live reference)\n",
                       st.unclaimed_used_bytes / 1048576.0);
        }
        if (vol_ndev(v) == 2) {
            uint64_t mblocks = 0, tblocks = 0, cpba = 0, dpba = 0;
            uint64_t mcnt = vol_rawm_count(v, &mblocks);
            uint64_t tcnt = vol_tier_count(v, &tblocks, &cpba, &dpba);
            printf("two-device (WP25)  :\n");
            printf("  devices          : %d%s\n", vol_ndev(v),
                   vol_degraded(v) ? " (DEGRADED: dev0 absent, read-only)" : "");
            printf("  mirror           : %s\n",
                   vol_mirror_stale(v) ? "STALE (resync at next flush)" : "in sync");
            printf("  raw mirror       : %" PRIu64 " segments, %" PRIu64 " blocks\n", mcnt, mblocks);
            printf("  tier (dev0 copies): %" PRIu64 " live, %" PRIu64 " blocks\n", tcnt, tblocks);
            if (tcnt)
                printf("  first tier copy  : canonical pba %" PRIu64 " -> dev0 pba %" PRIu64 "\n", cpba, dpba);
        }
    } else {
        char fb[64], ab[64], ub[64], pct[32];
        human(free_b, fb, sizeof fb);
        human(used_b, ab, sizeof ab);
        human(total_b, ub, sizeof ub);
        snprintf(pct, sizeof pct, "%.1f%%", total_b ? (100.0 * used_b / total_b) : 0);

        printf("  space            : %s used / %s free of %s (%s)\n", ab, fb, ub, pct);
        printf("  zones            : meta %llu MB | raw %llu MB | shadow %llu MB\n",
               (unsigned long long)(sb->metadata_zone_blocks * INVFS_BLOCK_SIZE >> 20),
               (unsigned long long)(sb->raw_zone_blocks * INVFS_BLOCK_SIZE >> 20),
               (unsigned long long)(sb->shadow_zone_blocks * INVFS_BLOCK_SIZE >> 20));

        char lb[64];
        human(st.logical_bytes, lb, sizeof lb);
        printf("  files            : %" PRIu64 " live files (%" PRIu64 " dirs), %s logical (%.2fx ratio)\n",
               st.files, st.dirs, lb, ratio);
        if (vol_ndev(v) == 2) {
            printf("  devices          : %d (%s)\n", vol_ndev(v),
                   vol_degraded(v) ? "DEGRADED" : "in sync");
        }
    }

    if (show_files) {
        files_ctx fc;
        memset(&fc, 0, sizeof fc);
        if (vol_walk(v, files_cb, &fc) == 0 && !fc.oom) {
            printf("\n  files by namespace:\n");
            for (size_t j = 0; j < fc.nfiles; j++) {
                char fs2[24];
                human(fc.tbl[j].fsz, fs2, sizeof fs2);
                char dname[sizeof fc.tbl[0].name + 2];
                snprintf(dname, sizeof dname, "%s%s", fc.tbl[j].name, fc.tbl[j].dir ? "/" : "");
                printf("    %s%-44s %8s" C_RST "\n", C_DIM, dname, fs2);
            }
        }
        free(fc.tbl);
    }
    printf("\n");

    vol_close(v);
    return 0;
}
