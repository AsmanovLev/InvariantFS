/*
 * sizes.c — per-file physical blob sizes by algo (for ratio reporting)
 *
 *   invf-sizes <image>
 *
 * Enumerates LIVE inodes, reads the AST recipe, looks up L2P, reads the
 * 4-byte compressed-size header of each segment, and prints per-file:
 *   name  algo  orig_size  blob_bytes
 * algo: JXL / APE / ZSTD / LZ4 / RAW
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume.h"

/* inode record header (same layout as volume.c internals):
 * magic 4 + rec_len 4 + inode_id 8 + file_size 8 + ctime 8 +
 * name_len 4 + name[256] = 292 bytes, then AST header + entries */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t rec_len;
    uint64_t inode_id;
    uint64_t file_size;
    uint64_t ctime;
    uint32_t name_len;
    char     name[256];
} rec_hdr_t;
#pragma pack(pop)

static const char *algo_name(uint32_t a)
{
    switch (a) {
    case INVFS_ALGO_JXL:  return "JXL";
    case INVFS_ALGO_APE:  return "APE";
    case INVFS_ALGO_PMP:  return "PMP";
    case INVFS_ALGO_ZSTD: return "ZSTD";
    case INVFS_ALGO_LZ4:  return "LZ4";
    case INVFS_ALGO_PPMD: return "PPMd";
    case INVFS_ALGO_ZSTD_BCJ: return "ZSTD+BCJ";  /* WP14a */
    case INVFS_ALGO_EXER: return "EXER";          /* WP14b M2 */
    case INVFS_ALGO_NONE: return "RAW";
    default:              return "?";
    }
}

/* WP49b: per-record body fed by the bounded, index-ordered
 * vol_records_walk (the old position-driven vol_inode_next loop can cycle
 * on a non-monotonic mapper table). The walker hands a CRC-verified full
 * record buffer, so the old local read/CRC dance is gone. */
typedef struct {
    invfs_volume *vol;
    uint64_t total_blob, total_orig;
} sizes_ctx;

static int sizes_cb(void *ctx_, uint64_t rec_pos,
                    const invfs_inode_rec *h, const uint8_t *rec)
{
    sizes_ctx *c = (sizes_ctx *)ctx_;
    uint64_t ino = h->inode_id, fsz = h->file_size;
    uint32_t rl = h->rec_len;
    char nm[256];
    size_t nl;
    (void)rec_pos;

    if (h->magic != INODE_REC_MAGIC) return 0;
    /* h->name is not NUL-terminated */
    nl = h->name_len < 255 ? h->name_len : 255;
    memcpy(nm, h->name, nl);
    nm[nl] = 0;
    if (vol_find(c->vol, nm) != ino) return 0;  /* superseded version */

    /* read AST from the record buffer */
    {
        invfs_ast_hdr ast_h;
        invfs_ast_block_entry *ents;
        uint64_t blob = 0;
        const char *algo = "?";

        /* WP22a: v1/v2 recipe header — the entry offset follows the
         * parsed header length, never a fixed 16 */
        if (invfs_ast_hdr_parse(rec + sizeof(rec_hdr_t),
                                rl - sizeof(rec_hdr_t), &ast_h) != 0 ||
            (size_t)ast_h.num_blocks * sizeof(invfs_ast_block_entry) >
                rl - sizeof(rec_hdr_t) - ast_h.hdr_len) {
            return 0;
        }
        ents = (invfs_ast_block_entry *)(rec + sizeof(rec_hdr_t) +
                                         ast_h.hdr_len);
        if (ast_h.num_blocks > 0) {
            uint32_t bits;
            memcpy(&bits, (uint8_t *)&ents[0] + 16, 4);
            algo = algo_name((bits >> 2) & 0x3F);
            {
                uint64_t pba = 0, plen = 0;
                uint32_t csz = 0;
                if (vol_lookup_entry(c->vol, ino, bits >> 8, &pba, &plen) == 0) {
                    vol_read_raw(c->vol, pba * INVFS_BLOCK_SIZE, &csz, 4);
                }
                blob = csz;
            }
        }
        printf("%-40s %-4s %12llu %12llu\n", nm, algo,
               (unsigned long long)fsz, (unsigned long long)blob);
        c->total_blob += blob;
        c->total_orig += fsz;
    }
    return 0;
}

int main(int argc, char **argv)
{
    invfs_volume *vol;
    int err;
    uint64_t total_blob = 0, total_orig = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-sizes <image>\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc != 2) {
        fprintf(stderr, "usage: invf-sizes <image>\n");
        return 2;
    }
    vol = vol_open(argv[1], &err);
    if (!vol) { fprintf(stderr, "open fail (%d)\n", err); return 1; }

    {
        sizes_ctx sc;
        memset(&sc, 0, sizeof sc);
        sc.vol = vol;
        vol_records_walk(vol, sizes_cb, &sc);
        total_blob = sc.total_blob;
        total_orig = sc.total_orig;
    }
    printf("TOTAL%43s %12llu %12llu\n", "",
           (unsigned long long)total_orig, (unsigned long long)total_blob);
    vol_close(vol);
    return 0;
}
