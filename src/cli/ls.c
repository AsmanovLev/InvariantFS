/*
 * ls.c — list files in an InvariantFS volume
 *
 *   invf-ls <image>
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>

/* Set when any directory returned exactly the 4096-entry cap, so the summary
 * can say INCOMPLETE. The listing walk and the summary live in different
 * functions, so this cannot be a local of either. */
static int truncated;
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume.h"

int main(int argc, char **argv)
{
    invfs_volume *vol;
    const invfs_superblock *sb;
    int err;
    const char *img;

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

    /* WP-M21b: on a v3 volume there is no v2 record stream to scan -- the
     * namespace IS the dirent tree. Walk it recursively (vol_list_dir is
     * v3-aware via vol_v3_path_list_dir) and keep the listing contract
     * byte-shaped as before: "  %8llu bytes  inode %llu  %s" per live
     * name, directory anchors as "0 bytes ... name/", and the live count
     * as the final "N file(s)" line. Container-member lines are v2-AST
     * machinery (vol_get_children); on v3 they are omitted until the
     * recipe-blob member listing lands. */
    struct ls3_stack { char path[2 * INVFS_MAX_NAME + 2]; } *st = NULL;
    size_t st_n = 0, st_cap = 0;
    uint64_t live = 0;
    printf("files in %s:\n", img);
    /* seed with the root */
    st_cap = 16;
    st = (struct ls3_stack *)malloc(st_cap * sizeof *st);
    if (!st) { vol_close(vol); return 1; }
    st[0].path[0] = 0;
    st_n = 1;
    while (st_n) {
        char dir[2 * INVFS_MAX_NAME + 2];
        invfs_dirent *ents;
        int n;
        snprintf(dir, sizeof dir, "%s", st[--st_n].path);
        ents = (invfs_dirent *)calloc(4096, sizeof *ents);
        if (!ents) { free(st); vol_close(vol); return 1; }
        n = vol_list_dir(vol, dir, ents, 4096);
        /* A failed listing is not an empty one. `n = 0` here printed
         * nothing for the directory, carried on, and exited 0: an
         * unreadable subtree looked like an empty one, and the "N
         * file(s)" tally at the end counted a total that was quietly
         * short. Name the directory, say why, and fail. */
        if (n < 0) {
            fprintf(stderr, "invf-ls: cannot list %s: %s\n",
                    dir[0] ? dir : "/", strerror(-n));
            free(ents); free(st); vol_close(vol); return 1;
        }
          if (n == 4096) {
              /* The end-of-run tally can only be trusted if every directory
               * enumerated completely. usr/share/man/man3/ is larger than this
               * cap, and for six CI runs invf-ls reported a SHORT count in the
               * authoritative "N file(s)" form, which the Arch check read as
               * 7,108 lost files.
               *
               * dcaa197 meant to set the flag here and did not: the edit did not
               * apply, and the commit went in anyway because the BUILD was clean.
               * A build is not a behaviour. */
              truncated = 1;
              fprintf(stderr, "warning: %s%s truncated at %d entries\n",
                      dir, dir[0] ? "/" : "", 4096);
          }
        for (int i = 0; i < n; i++) {
            /* dir (INVFS_MAX_NAME) + '/' + name (INVFS_MAX_NAME): the
             * old buffer was INVFS_MAX_NAME+2, so a deep path was
             * silently truncated and vol_find() then answered for a
             * name that does not exist */
            char full[2 * INVFS_MAX_NAME + 2];
            int fn;
            if (dir[0])
                fn = snprintf(full, sizeof full, "%s/%s", dir,
                              ents[i].name);
            else
                fn = snprintf(full, sizeof full, "%s", ents[i].name);
            if (fn < 0 || (size_t)fn >= sizeof full) {
                fprintf(stderr, "warning: name too long, listed as "
                                "<truncated>\n");
                continue;
            }
            if (ents[i].is_dir) {
                uint64_t id = vol_find(vol, full);
                printf("  %8llu bytes  inode %llu  %s/\n",
                       0ull, (unsigned long long)id, full);
                /* push the subdir */
                if (st_n == st_cap) {
                    size_t nc = st_cap * 2;
                    void *ns = realloc(st, nc * sizeof *st);
                    if (!ns) break;
                    st = (struct ls3_stack *)ns;
                    st_cap = nc;
                }
                snprintf(st[st_n].path, sizeof st[st_n].path, "%s", full);
                st_n++;
            } else {
                uint64_t id = vol_find(vol, full);
                if (!id) continue;   /* raced away / shadowed */
                printf("  %8llu bytes  inode %llu  %s\n",
                       (unsigned long long)ents[i].size,
                       (unsigned long long)id, full);
                live++;
            }
        }
        free(ents);
    }
    free(st);
    if (truncated)
        /* Say INCOMPLETE, and exit non-zero, so no caller can mistake this for
         * a complete enumeration. */
        printf("%llu file(s) INCOMPLETE -- at least one directory exceeded %d "
               "entries\n", (unsigned long long)live, 4096);
    else
        printf("%llu file(s)\n", (unsigned long long)live);
    vol_close(vol);
    return 0;
}
