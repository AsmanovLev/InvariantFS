/*
 * ls.c — list files in an InvariantFS volume
 *
 *   invf-ls <image>
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume.h"

/* name -> slot, so the per-record dedup below is a hash probe rather than a
   walk over every name seen so far. The linear version made listing quadratic:
   a 40k-file image spent minutes in strcmp before printing anything. */
typedef struct ls_bucket { struct ls_bucket *next; int slot; } ls_bucket;

static uint64_t ls_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* slot of `name`, or -1 */
static int ls_find(ls_bucket **tab, size_t mask, char (*names)[256],
                   const char *name)
{
    const ls_bucket *b;
    if (!tab) return -1;
    for (b = tab[ls_hash(name) & mask]; b; b = b->next)
        if (strcmp(names[b->slot], name) == 0) return b->slot;
    return -1;
}

/* record that names[slot] is taken; grows the table at 100% load */
static int ls_insert(ls_bucket ***tabp, size_t *maskp, size_t *countp,
                     char (*names)[256], int slot)
{
    ls_bucket *b;
    size_t h;
    if (!*tabp) {
        *tabp = (ls_bucket **)calloc(1024, sizeof **tabp);
        if (!*tabp) return -1;
        *maskp = 1023;
    } else if (*countp > *maskp) {
        size_t ncap = (*maskp + 1) * 2, i;
        ls_bucket **nt = (ls_bucket **)calloc(ncap, sizeof *nt);
        if (nt) {
            for (i = 0; i <= *maskp; i++) {
                ls_bucket *e = (*tabp)[i];
                while (e) {
                    ls_bucket *nx = e->next;
                    size_t nb = ls_hash(names[e->slot]) & (ncap - 1);
                    e->next = nt[nb]; nt[nb] = e;
                    e = nx;
                }
            }
            free(*tabp);
            *tabp = nt;
            *maskp = ncap - 1;
        }
    }
    b = (ls_bucket *)malloc(sizeof *b);
    if (!b) return -1;
    b->slot = slot;
    h = ls_hash(names[slot]) & *maskp;
    b->next = (*tabp)[h];
    (*tabp)[h] = b;
    (*countp)++;
    return 0;
}

int main(int argc, char **argv)
{
    invfs_volume *vol;
    const invfs_superblock *sb;
    int err;
    uint64_t bm, inode_area_start, inode_area_end, p;
    int count = 0, cap = 0;
    const char *img;
    /* Grown on demand. These used to be fixed 512-entry arrays with a silent
       `count < 512` cutoff, so a volume with more files than that listed the
       first 512 and reported that as the total -- a 6241-file image printed
       "512 file(s)" and looked like data loss. */
    char (*names)[256] = NULL;
    uint64_t *sizes = NULL;
    uint64_t *inodes = NULL;
    uint64_t *poss = NULL;   /* each name's current record position
                                (v2 position-kill matching) */
    ls_bucket **tab = NULL;
    size_t tmask = 0, tcount = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-ls <image>\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc != 2) {
        fprintf(stderr, "usage: invf-ls <image>\n");
        return 2;
    }
    img = argv[1];

    vol = vol_open(img, &err);
    if (!vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }
    sb = vol_sb(vol);
    (void)sb;
    inode_area_start = vol_inode_area_start(vol);  /* WP30: respects active extent */
    inode_area_end = vol_inode_area_pos(vol);      /* CRC-validated extent */
    p = inode_area_start;

    printf("files in %s:\n", img);
    while (p + sizeof(invfs_inode_rec) <= inode_area_end) {
        invfs_inode_rec h;
        char name[257];
        uint32_t crc_stored, crc_calc;
        if (vol_read_raw(vol, p, &h, sizeof(h)) != 0) break;
        /* WP30: a mapper extent can start with zeros (gap between the
         * legacy inode-area start and the first record position). Skip
         * whole blocks past zero regions rather than aborting the walk. */
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) {
            uint64_t next = (p + INVFS_BLOCK_SIZE) & ~(uint64_t)(INVFS_BLOCK_SIZE - 1);
            if (next <= p || next >= inode_area_end) break;
            p = next;
            continue;
        }
        /* rec_len must at least cover the header. Without the lower bound a
           record claiming 0 advanced p by 4 bytes and the walk crawled the
           whole area at 4 bytes a step. vol_open has the same guard. */
        if (h.name_len > 256 || h.rec_len < sizeof(invfs_inode_rec) ||
            h.rec_len > INVFS_MAX_REC_LEN) break;
        if (vol_read_raw(vol, p + offsetof(invfs_inode_rec, name), name, h.name_len) != 0) break;
        name[h.name_len] = 0;
        /* internal control-prefixed names (the WP10 batch owner "\x01tzb")
         * are not directory content -- same filter as vol_list_dir */
        if ((uint8_t)name[0] == 0x01) { p += (uint64_t)h.rec_len + 4; continue; }
        /* torn-write guard: stop at the first CRC-broken record */
        if (vol_read_raw(vol, p + h.rec_len, &crc_stored, 4) != 0) break;
        crc_calc = 0;
        /* recompute over the full record (header + ast) */
        {
            uint8_t *rb = (uint8_t *)malloc((size_t)h.rec_len);
            if (!rb) break;
            if (vol_read_raw(vol, p, rb, h.rec_len) != 0) { free(rb); break; }
            crc_calc = invfs_crc32c(rb, h.rec_len);
            free(rb);
        }
        if (crc_calc != crc_stored) {
            /* corrupt record: skip past it, keep listing */
            p += h.rec_len + 4;
            continue;
        }
        if (h.magic == TOMBSTONE_MAGIC) {  /* tombstone: remove only its version */
            int i = ls_find(tab, tmask, names, name);
            if (i >= 0) {
                if (h.file_size != 0) {
                    /* v2 position kill: retires exactly the record at that
                     * position. Kill the name only when its CURRENT version
                     * is that record: a meta_rewrite's kill names the OLD
                     * version (already superseded by the combo's INOD), but
                     * an unlink/rename kill (vol_unlink_name) names the LAST
                     * one -- skipping those listed renamed-away files as
                     * live (WP22c: the flakey soak's GHOST). */
                    if (poss[i] == h.file_size) {
                        sizes[i] = 0;
                        inodes[i] = 0;
                    }
                } else if (inodes[i] == h.inode_id) {
                    sizes[i] = 0;
                    inodes[i] = 0;
                }
            }
        } else {
            int i = ls_find(tab, tmask, names, name);
            if (i >= 0) {
                sizes[i] = h.file_size;
                inodes[i] = h.inode_id;
                poss[i] = p;
            } else {
                if (count == cap) {
                    int ncap = cap ? cap * 2 : 512;
                    char (*nn)[256] = (char (*)[256])realloc(names,
                        (size_t)ncap * 256);
                    uint64_t *ns = (uint64_t *)realloc(sizes,
                        (size_t)ncap * sizeof *ns);
                    uint64_t *ni = (uint64_t *)realloc(inodes,
                        (size_t)ncap * sizeof *ni);
                    uint64_t *np = (uint64_t *)realloc(poss,
                        (size_t)ncap * sizeof *np);
                    if (nn) names = nn;
                    if (ns) sizes = ns;
                    if (ni) inodes = ni;
                    if (np) poss = np;
                    if (!nn || !ns || !ni || !np) {
                        fprintf(stderr, "out of memory at %d names\n", count);
                        break;
                    }
                    cap = ncap;
                }
                strncpy(names[count], name, 255);
                names[count][255] = 0;
                sizes[count] = h.file_size;
                inodes[count] = h.inode_id;
                poss[count] = p;
                if (ls_insert(&tab, &tmask, &tcount, names, count) != 0) {
                    fprintf(stderr, "out of memory at %d names\n", count);
                    break;
                }
                count++;
            }
        }
        p += (uint64_t)h.rec_len + 4;  /* + trailing crc */
    }
    for (int i = 0; i < count; i++) {
        if (inodes[i] != 0) {
            /* WP22d: the walk above sees every record version; the live
             * answer is the name index's consistent cut (a torn newest
             * version is hidden, the name falls back to an older one).
             * Defer to it for both liveness and the reported id/size. */
            uint64_t id = vol_find_ex(vol, names[i], &sizes[i], NULL);
            if (!id) { inodes[i] = 0; continue; }
            inodes[i] = id;
            printf("  %8llu bytes  inode %llu  %s\n",
                   (unsigned long long)sizes[i],
                   (unsigned long long)inodes[i], names[i]);
        }
    }
    /* container members (virtual windows into the original archive) */
    {
        int mcount = 0;
        for (int i = 0; i < count; i++) {
            if (inodes[i] == 0) continue;
            invfs_ast_child_entry *ch = NULL;
            size_t nch = 0;
            if (vol_get_children(vol, inodes[i], &ch, &nch) != 0 || nch == 0)
                continue;
            for (size_t j = 0; j < nch; j++) {
                printf("  %8llu bytes  member   %s!%s  (%s)\n",
                       (unsigned long long)ch[j].usize, names[i], ch[j].name,
                       ch[j].method == 0 ? "stored" :
                       ch[j].method == 8 ? "deflate" : "?");
                mcount++;
            }
            free(ch);
        }
        if (mcount)
            printf("%d container member(s) (virtual)\n", mcount);
    }
    /* `count` is names seen, including ones a tombstone later killed or
       the consistent cut hid; those are skipped above, so reporting it as
       the total contradicted the list. */
    {
        int live = 0, i;
        for (i = 0; i < count; i++)
            if (inodes[i] != 0 && vol_find(vol, names[i]) != 0) live++;
        printf("%d file(s)\n", live);
    }
    free(names);
    free(sizes);
    free(inodes);
    free(poss);
    if (tab) {
        size_t i;
        for (i = 0; i <= tmask; i++) {
            ls_bucket *b = tab[i];
            while (b) { ls_bucket *nx = b->next; free(b); b = nx; }
        }
        free(tab);
    }
    vol_close(vol);
    return 0;
}
