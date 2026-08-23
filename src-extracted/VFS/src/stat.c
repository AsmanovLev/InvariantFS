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

static const char *zone_name(int z)
{
    switch (z) {
    case INVFS_ZONE_RAW:     return "RAW   ";
    case INVFS_ZONE_TEXT:    return "Text  ";
    case INVFS_ZONE_BINARY:  return "Binary";
    default:                 return "?     ";
    }
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

/* One name seen in the inode area, with its newest version. */
typedef struct { char name[256]; uint64_t ino, fsz; uint8_t killed; } fent;

static uint64_t fent_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* Open-addressed name -> slot map. Buckets hold slot+1 so 0 means empty; the
   table is kept under half full, which bounds the probe run. */
static size_t fent_find(const uint32_t *hb, size_t hmask, const fent *tbl,
                        const char *nm)
{
    size_t b;
    if (!hb) return (size_t)-1;
    for (b = fent_hash(nm) & hmask; hb[b]; b = (b + 1) & hmask)
        if (strcmp(tbl[hb[b] - 1].name, nm) == 0) return (size_t)(hb[b] - 1);
    return (size_t)-1;
}

static int fent_insert(uint32_t **hbp, size_t *hmaskp, const fent *tbl,
                       size_t slot, size_t nfiles)
{
    size_t b;
    if (!*hbp || nfiles * 2 > *hmaskp + 1) {
        size_t ncap = *hbp ? (*hmaskp + 1) * 2 : 8192, i;
        uint32_t *nt = (uint32_t *)calloc(ncap, sizeof *nt);
        if (!nt) return -1;
        for (i = 0; i < nfiles; i++) {          /* rehash every live slot */
            size_t j = fent_hash(tbl[i].name) & (ncap - 1);
            while (nt[j]) j = (j + 1) & (ncap - 1);
            nt[j] = (uint32_t)(i + 1);
        }
        free(*hbp);
        *hbp = nt;
        *hmaskp = ncap - 1;
        return 0;                                /* slot went in with the rest */
    }
    b = fent_hash(tbl[slot].name) & *hmaskp;
    while ((*hbp)[b]) b = (b + 1) & *hmaskp;
    (*hbp)[b] = (uint32_t)(slot + 1);
    return 0;
}

int main(int argc, char **argv)
{
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

    /* walk inode area; version-aware: a tombstone kills only the inode
     * version it references (sweep appends create-first, then the OLD
     * tombstone — the newer live record sits BEFORE the tombstone). */
    uint64_t pos = vol_inode_area_start(vol);
    /* Was a fixed calloc(65536) with a silent `nfiles < MAX_FILES` cutoff, so
       a 146k-file image reported "65536 live of 65536 names" -- a number that
       looks like a real total and is not. Grown on demand instead.

       The per-record name lookup was also a linear strcmp over everything seen
       so far; at 146k names that is ~10^10 comparisons and stat never returns.
       Same fix ls.c already carries: hash the name to a slot. */
    fent *tbl = NULL;
    size_t nfiles = 0, fcap = 0;
    uint32_t *hb = NULL;           /* name -> slot+1, open addressed */
    size_t hmask = 0;
    uint64_t recs = 0, tombs = 0, max_live_ino = 0;
    while (1) {
        uint32_t magic; uint64_t ino, fsz; uint32_t rl; char nm[256];
        uint64_t np = vol_inode_next(vol, pos, &magic, &ino, &fsz, nm, sizeof nm, &rl);
        if (!np) break;
        pos = np;
        recs++;
        if (magic == TOMBSTONE_MAGIC) tombs++;
        if (magic == INODE_REC_MAGIC && ino > max_live_ino) max_live_ino = ino;
        {
            size_t slot = fent_find(hb, hmask, tbl, nm);
            if (slot == (size_t)-1) {
                if (nfiles == fcap) {
                    size_t ncap = fcap ? fcap * 2 : 4096;
                    fent *nt = (fent *)realloc(tbl, ncap * sizeof(fent));
                    if (!nt) { fprintf(stderr, "out of memory at %llu names\n",
                                       (unsigned long long)nfiles); break; }
                    tbl = nt; fcap = ncap;
                }
                memset(&tbl[nfiles], 0, sizeof(fent));
                strncpy(tbl[nfiles].name, nm, 255);
                slot = nfiles++;
                if (fent_insert(&hb, &hmask, tbl, slot, nfiles) != 0) {
                    fprintf(stderr, "out of memory at %llu names\n",
                            (unsigned long long)nfiles);
                    break;
                }
            }
            if (magic == INODE_REC_MAGIC) {
                tbl[slot].ino = ino; tbl[slot].fsz = fsz; tbl[slot].killed = 0;
            } else {
                /* tombstone: kills only the matching version */
                if (tbl[slot].ino == ino) tbl[slot].killed = 1;
            }
        }
    }
    /* live inode set: per-name newest version that is not killed */
    uint8_t *live = (uint8_t *)calloc((size_t)max_live_ino + 1, 1);
    for (size_t j = 0; j < nfiles; j++)
        if (!tbl[j].killed && tbl[j].ino != 0 && tbl[j].ino <= max_live_ino)
            live[tbl[j].ino] = 1;

    /* refcount per block from L2P journal — only LIVE inodes */
    uint32_t *refc = (uint32_t *)calloc(total, sizeof(uint32_t));
    size_t n_l2p = 0;
    const invfs_l2p_entry *l2p = vol_l2p(vol, &n_l2p);
    for (size_t i = 0; i < n_l2p; i++) {
        const invfs_l2p_entry *e = &l2p[i];
        if (e->type != INVFS_JRN_MAP || e->inode > max_live_ino ||
            !live[e->inode] || e->pba >= total) continue;
        uint64_t n = e->length;
        if (n > total - e->pba) n = total - e->pba;
        for (uint64_t b = e->pba; b < e->pba + n; b++)
            if (refc[b] < 0xFFFF) refc[b]++;
    }

    printf("\n" C_BLUE "InvariantFS" C_RST " volume: " C_DIM "%s" C_RST "\n", argv[1]);
    printf("  %llu blocks x %u = ", (unsigned long long)total, INVFS_BLOCK_SIZE);
    char sz[64];
    human(total * INVFS_BLOCK_SIZE, sz, sizeof sz);
    printf("%s\n\n", sz);

    uint64_t asis, sem, dedup, both, alloc, freeb;
    draw_bar(vol, sb, refc, &asis, &sem, &dedup, &both, &alloc, &freeb);

    printf("\n  " C_GRAY "█ As-IS (RAW)" C_RST "  " C_BLUE "█ semantic" C_RST
           "  " C_GREEN "█ dedup" C_RST "  " C_CYAN "█ semantic+dedup" C_RST
           "  " C_DIM "░ free" C_RST "\n\n");

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
        if (tbl[j].killed || tbl[j].ino == 0) continue;
        nlive++;
        total_bytes += tbl[j].fsz;
        if (tbl[j].fsz > max_size) max_size = tbl[j].fsz;
    }
    char mb[64], mb2[64];
    human(total_bytes, mb, sizeof mb);
    human(max_size, mb2, sizeof mb2);
    printf("  files : %llu live of %zu names (%llu tombstones), %s logical, largest %s\n",
           (unsigned long long)nlive, nfiles, (unsigned long long)tombs, mb, mb2);

    printf("  l2p   : %zu maps, journal %.1f%% (of %llu blk)\n",
           n_l2p, 100.0 * vol_journal_pos(vol) /
           (double)(INVFS_JOURNAL_BLOCKS * INVFS_BLOCK_SIZE),
           (unsigned long long)INVFS_JOURNAL_BLOCKS);
    printf("  inode : area %.1f%% used\n",
           100.0 * (vol_inode_area_pos(vol) - vol_inode_area_start(vol)) /
           (double)((vol_inode_area_end(vol) - vol_inode_area_start(vol))));

    printf("\n  semantic: %llu blk (%.1f%%)  dedup: %llu blk (%.1f%%)  both: %llu\n",
           (unsigned long long)sem, 100.0 * sem / (alloc ? alloc : 1),
           (unsigned long long)dedup, 100.0 * dedup / (alloc ? alloc : 1),
           (unsigned long long)both);

    if (show_files) {
        printf("\n  files by policy:\n");
        for (size_t j = 0; j < nfiles; j++) {
            const char *nm = tbl[j].name;
            uint64_t ino = tbl[j].ino, fsz = tbl[j].fsz;
            if (tbl[j].killed || ino == 0) {
                printf("    " C_DIM "X %-40s (deleted)" C_RST "\n", nm);
                continue;
            }
            /* check refcount over this inode's mapped blocks */
            int is_sem = 0, is_ded = 0;
            for (size_t i = 0; i < n_l2p; i++) {
                if (l2p[i].type != INVFS_JRN_MAP || l2p[i].inode != ino) continue;
                if (l2p[i].pba >= sb->shadow_zone_start) is_sem = 1;
                uint64_t n = l2p[i].length;
                if (n > total - l2p[i].pba) n = total - l2p[i].pba;
                for (uint64_t b = l2p[i].pba; b < l2p[i].pba + n; b++)
                    if (refc[b] > 1) is_ded = 1;
            }
            const char *col = C_GRAY, *tag = "A";
            if (is_sem && is_ded) { col = C_CYAN; tag = "B"; }
            else if (is_sem)     { col = C_BLUE; tag = "S"; }
            else if (is_ded)     { col = C_GREEN; tag = "D"; }
            char fs2[24];
            human(fsz, fs2, sizeof fs2);
            printf("    %s[%s] %-44s %8s" C_RST "\n", col, tag, nm, fs2);
        }
        printf("\n  " C_GRAY "[A] as-is" C_RST "  " C_BLUE "[S] semantic" C_RST
               "  " C_GREEN "[D] dedup" C_RST "  " C_CYAN "[B] both" C_RST "\n");
    }

    free(refc);
    free(tbl);
    free(hb);
    free(live);
    vol_close(vol);
    return 0;
}
