/* ivfs-stat: console space inspector for InvariantFS images.
 *
 * Draws zone bars with policy-colored cells:
 *   blue    = semantic policy applied (Shadow zone: JXL/ZSTD/...)
 *   green   = dedup policy (block referenced by >1 L2P map)
 *   cyan    = semantic + dedup
 *   gray    = As-IS (RAW, not yet processed)
 *   '█' allocated, '░' free
 *
 * Usage: ivfs-stat <image> [--files]
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "volume.h"

#ifdef _WIN32
#include <windows.h>
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


static void draw_bar(const invfs_volume *v, const invfs_superblock *sb,
                     const uint32_t *refcount,
                     uint64_t *asis_out, uint64_t *sem_out,
                     uint64_t *dedup_out, uint64_t *both_out,
                     uint64_t *used_out, uint64_t *free_out)
{
    const uint8_t *bm = vol_bitmap((invfs_volume *)v, NULL);
    uint64_t total = sb->total_blocks;
    uint64_t asis = 0, sem = 0, dedup = 0, both = 0, used = 0;
    uint64_t meta_used = 0;
    for (uint64_t b = 0; b < total; b++) {
        if (!(bm[b >> 3] & (1 << (b & 7)))) continue;
        if (b < sb->raw_zone_start) { meta_used++; continue; }  /* metadata */
        used++;
        int s = (b >= sb->shadow_zone_start) ? 1 : 0;
        int d = (refcount && refcount[b] > 1) ? 1 : 0;
        if (s && d) both++;
        else if (s) sem++;
        else if (d) dedup++;
        else        asis++;
    }
    uint64_t data_total = total - sb->raw_zone_start;
    uint64_t freeb = data_total - used;

    /* one sorted bar: [dedup][both][semantic][asis] then free
     * (dedup at left, semantic middle, raw As-IS glued to the right end) */
    /* one sorted bar: [dedup][both][semantic][asis] then free
     * (dedup at left, semantic middle, raw As-IS glued to the right end) */
    printf("  vol [");
    uint64_t rem = BAR_W;
    uint64_t groups[4] = { dedup, both, sem, asis };
    const char *cols[4] = { C_GREEN, C_CYAN, C_BLUE, C_GRAY };
    uint64_t cells[4];
    for (int g = 0; g < 4; g++)
        cells[g] = (uint64_t)((double)groups[g] / (data_total ? data_total : 1) * rem + 0.5);
    for (int g = 0; g < 4; g++) {
        if (cells[g] > rem) cells[g] = rem;
        for (uint64_t c = 0; c < cells[g] && rem > 0; c++, rem--)
            printf("%s█%s", cols[g], C_RST);
    }
    for (uint64_t c = 0; c < rem; c++) printf(C_DIM "░" C_RST);
    printf("]\n");
    (void)meta_used;

    if (asis_out)  *asis_out  = asis;
    if (sem_out)   *sem_out   = sem;
    if (dedup_out) *dedup_out = dedup;
    if (both_out)  *both_out  = both;
    if (used_out)  *used_out  = used;
    if (free_out)  *free_out  = freeb;
}

static void human(uint64_t bytes, char *buf, size_t cap)
{
    double v = (double)bytes;
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(buf, cap, "%.1f %s", v, u[i]);
}

/* One name in the namespace, with the inode and size it resolves to. `dir` is
 * what a v3 namespace walk hands over and the v2 record walk does not: on v3
 * the count is a NAME count (so it lines up with invf-ls's `N file(s)`), and a
 * directory is a name too -- it is just not a file. */
typedef struct {
    /* a v3 walk hands a MOUNT-RELATIVE PATH, not a leaf name: dir + '/' + name
     * with two full components is 2*INVFS_MAX_NAME+1 characters. The v2 record
     * carried a bare leaf, which is why 256 was enough then. Sizing this at the
     * path length is what keeps a deep entry from being reported under a name
     * it does not have -- the same shape as the truncated-deep-path bug
     * invf-ls's own `full[]` comment records. */
    char name[2 * INVFS_MAX_NAME + 2];
    uint64_t ino, fsz;
    uint8_t killed, dir;
} fent;

/* ------------------------------------------------------------------ */
/* v3: the namespace IS the dirent tree.                                  */
/* ------------------------------------------------------------------ */
/*
 * WP stat-counts-v3: this tool used to populate the table above from
 * vol_records_walk(), which scans [inode_area_start, inode_area_pos) -- the
 * v2 inode area, dead weight on a v3 volume. The walk still existed (the v2
 * L2P journal layer is deliberately still in the tree), so it still ran, still
 * found nothing, and invf-stat printed "0 live of 0 names, 0.0 B logical,
 * largest 0.0 B" for a volume with files on it. vol_open refuses anything
 * without VOLF_V3, so every volume this build opens is v3 and every count this
 * tool printed was zero.
 *
 * vol_v3_walk() is the v3 equivalent, and it is what the rest of the tool
 * already uses: the FUSE build_file_table_v3 (src/cli/fuse_fs.c:224) is built
 * on it, and invf-ls reaches the same tree through vol_list_dir ->
 * vol_v3_path_list_dir. Its callback hands (path, ino, type, size, mtime) --
 * the whole fent row plus the file/dir distinction.
 *
 * vol_v3_iter_live_inodes() was the other candidate and is the wrong shape
 * here: it fires once per INODE (not once per name, so a hardlinked volume
 * would under-report against invf-ls), and it hands no size, so fsz would cost
 * a vol_stat_full() per inode on top of a walk that already resolves a name
 * per inode.
 *
 * No name dedup is needed: a dirent tree yields each path exactly once, so the
 * hash table the v2 path needs (many versions of one name) has nothing to
 * merge. The grow-on-demand table is kept and is what keeps the 146k-file
 * image off a fixed bound; `oom` is what stops a failed growth from printing
 * the truncated count as if it were the whole one.
 */
typedef struct {
    fent *tbl;
    size_t nfiles, fcap;
    uint64_t ndirs, max_live_ino;
    int oom;
} stat_v3_ctx;

static int stat_v3_cb(void *ctx_, const char *path, uint64_t ino,
                      uint32_t type, uint64_t size, int64_t mtime)
{
    stat_v3_ctx *c = (stat_v3_ctx *)ctx_;
    size_t nl;
    (void)mtime;

    if (c->nfiles == c->fcap) {
        size_t ncap = c->fcap ? c->fcap * 2 : 4096;
        fent *nt = (fent *)realloc(c->tbl, ncap * sizeof(fent));
        if (!nt) {
            fprintf(stderr, "out of memory at %llu names\n",
                    (unsigned long long)c->nfiles);
            c->oom = 1;
            return 1;
        }
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
    if (ino > c->max_live_ino) c->max_live_ino = ino;
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: ivfs-stat <image> [--files]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 2) {
        fprintf(stderr, "usage: ivfs-stat <image> [--files]\n");
        return 1;
    }
    int show_files = argc > 2 && strcmp(argv[2], "--files") == 0;
    int oerr = 0;
    enable_ansi();

    invfs_volume *vol = vol_open(argv[1], &oerr);
    if (!vol) {
        fprintf(stderr, "ivfs-stat: cannot open %s\n", argv[1]);
        return 1;
    }
    const invfs_superblock *sb = vol_sb(vol);
    uint64_t total = sb->total_blocks;

    /* The file table, straight from the namespace. vol_open refuses any
     * volume without VOLF_V3, so this walk is the only source of names
     * there is -- there is no record stream to fall back to. */
    stat_v3_ctx vc;
    int rc;
    memset(&vc, 0, sizeof vc);
    rc = vol_v3_walk(vol, stat_v3_cb, &vc);
    if (rc != 0 || vc.oom) {
        /* A walk that did not finish, or a table that stopped growing, has
         * no count. Printing the part that was collected would present a
         * bounded table's overflow as a plausible total. Name the failure
         * and exit. */
        fprintf(stderr, "ivfs-stat: cannot enumerate %s (%s)%s\n",
                argv[1],
                rc < 0 ? "namespace walk failed" : "out of memory",
                vc.oom ? " growing the name table" : "");
        free(vc.tbl);
        vol_close(vol);
        return 1;
    }
    fent *tbl = vc.tbl;
    const size_t nfiles = vc.nfiles;
    const uint64_t ndirs = vc.ndirs;

    printf("\n" C_BLUE "InvariantFS" C_RST " volume: " C_DIM "%s" C_RST "\n", argv[1]);
    printf("  %llu blocks x %u = ", (unsigned long long)total, INVFS_BLOCK_SIZE);
    char sz[64];
    human(total * INVFS_BLOCK_SIZE, sz, sizeof sz);
    printf("%s\n\n", sz);

    uint64_t asis, sem, dedup, both, alloc, freeb;
    draw_bar(vol, sb, NULL, &asis, &sem, &dedup, &both, &alloc, &freeb);

    printf("\n  " C_GRAY "█ As-IS (RAW)" C_RST "  " C_BLUE "█ semantic" C_RST
           "  " C_DIM "░ free" C_RST "\n"
           "  " C_DIM "dedup and semantic+dedup are not shown: this format "
           "records no per-block mappings, so refcounts are not available "
           "here." C_RST "\n\n");

    char fb[64], ab[64], ub[64], pct[32];
    human(freeb * INVFS_BLOCK_SIZE, fb, sizeof fb);
    human(alloc * INVFS_BLOCK_SIZE, ab, sizeof ab);
    human(total * INVFS_BLOCK_SIZE, ub, sizeof ub);
    snprintf(pct, sizeof pct, "%.1f%%", 100.0 * alloc / total);
    printf("  space : %s used / %s free of %s (%s)\n", ab, fb, ub, pct);
    printf("  zones : meta %llu MB | raw %llu MB | shadow %llu MB\n",
           (unsigned long long)(sb->metadata_zone_blocks * INVFS_BLOCK_SIZE >> 20),
           (unsigned long long)(sb->raw_zone_blocks * INVFS_BLOCK_SIZE >> 20),
           (unsigned long long)(sb->shadow_zone_blocks * INVFS_BLOCK_SIZE >> 20));

    uint64_t nlive = 0, total_bytes = 0, max_size = 0;
    for (size_t j = 0; j < nfiles; j++) {
        if (tbl[j].killed || tbl[j].ino == 0 || tbl[j].dir) continue;
        nlive++;
        total_bytes += tbl[j].fsz;
        if (tbl[j].fsz > max_size) max_size = tbl[j].fsz;
    }
    char mb[64], mb2[64];
    human(total_bytes, mb, sizeof mb);
    human(max_size, mb2, sizeof mb2);
    /* A directory is a name but not a file: invf-ls prints it with 0 bytes
     * and leaves it out of its `N file(s)` tally, so counting it here
     * would put the two tools back in disagreement for no gain. */
    printf("  files : %llu live of %zu names (%llu directories), %s logical, largest %s\n",
           (unsigned long long)nlive, nfiles, (unsigned long long)ndirs, mb, mb2);

    /* There is no mapping journal and no inode area on this format, so
     * there is no occupancy to report here: printing two zeroes about
     * structures that are not there, in a block an operator reads as
     * measurements, is the defect. "space :" and "zones :" are
     * bitmap-derived and are honest. */
    printf("  meta  : no mapping journal or inode area on this format\n");

    printf("\n  semantic: %llu blk (%.1f%%)  dedup: n/a  both: n/a\n",
           (unsigned long long)sem, 100.0 * sem / (alloc ? alloc : 1));

    if (show_files) {
        printf("\n  files by policy:\n");
        for (size_t j = 0; j < nfiles; j++) {
            const char *nm = tbl[j].name;
            uint64_t fsz = tbl[j].fsz;
            char fs2[24];
            human(fsz, fs2, sizeof fs2);
            /* Directories are names too; mark them the way invf-ls does
             * rather than listing them bare under a "files" heading. The
             * per-file storage policy tag is NOT printed: this format
             * records no per-block mappings, so there is nothing to compute
             * it from, and a tag the tool did not derive is the same defect
             * as a zero it did not measure. */
            char dname[sizeof tbl[0].name + 2];
            snprintf(dname, sizeof dname, "%s%s", nm, tbl[j].dir ? "/" : "");
            printf("    %s%-44s %8s" C_RST "\n", C_DIM, dname, fs2);
        }
        printf("\n  " C_DIM "[?] storage policy unknown: this format records "
               "no per-block mappings, so refcounts are not available here." C_RST "\n");
    }

    free(tbl);
    vol_close(vol);
    return 0;
}
