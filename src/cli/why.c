/*
 * why.c -- invf-why: read-only per-file storage inspector (P0-1 queue).
 *
 *   invf-why <image> <path>
 *
 * Prints how one file is stored: the inode row, the class stamp (WHY it
 * is stored this way), the recipe entries (offset/len/zone/algo/pba),
 * and the byte accounting -- record payloads plus the per-record
 * [u16 klen][key][u16 vlen][value] framing (vol_btree.c:195,248,255),
 * and each data segment's on-disk extent from its framed header.
 *
 * Read-only: opens the volume, never mutates. No mount, no daemon.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume.h"
#include "volume_internal.h"   /* recipe parse + v->io for segment headers */
#include "codec.h"             /* registry algo names */

/* AST-only algo tags the codec registry deliberately has no row for. */
static const char *ast_algo_name(uint32_t algo)
{
    size_t n = 0, i;
    const invfs_codec *all;
    if (algo == INVFS_ALGO_ZSTD_BCJ)
        return "zstd-bcj(batch stage, no registry row; invarifs.h)";
    if (algo == INVFS_ALGO_WINDOW_SRC)
        return "window-src(sibling reference, no registry row)";
    if (algo == INVFS_ALGO_RAWIMG)
        return "rawimg(pack-only, no registry row)";
    all = invfs_codec_all(&n);
    for (i = 0; i < n; i++)
        if (all[i].algo == algo)
            return all[i].name;
    return "?";
}

static const char *zone_name(uint32_t z)
{
    if (z == INVFS_ZONE_RAW)
        return "RAW";
    if (z == INVFS_ZONE_TEXT)
        return "TEXT";
    if (z == INVFS_ZONE_BINARY)
        return "BINARY";
    return "?";
}

static const char *class_name(uint8_t c)
{
    switch (c) {
    case INVFS_CLASS_UNCOMPRESSIBLE: return "UNCOMPRESSIBLE";
    case INVFS_CLASS_CODEC: return "CODEC";
    case INVFS_CLASS_CONTAINER: return "CONTAINER";
    case INVFS_CLASS_GENERIC: return "GENERIC";
    case INVFS_CLASS_GENERIC_MEMLIMIT: return "GENERIC_MEMLIMIT";
    case INVFS_CLASS_GENERIC_GUARD: return "GENERIC_GUARD";
    case INVFS_CLASS_TEXT: return "TEXT";
    case INVFS_CLASS_BATCHED_BIN: return "BATCHED_BIN";
    case INVFS_CLASS_DEFER_ENOSPC: return "DEFER_ENOSPC";
    case INVFS_CLASS_ANCHORED: return "ANCHORED";
    default: return "?";
    }
}

static const char *type_name(uint32_t t)
{
    if (t == INVFS_ITYP_REG)
        return "reg";
    if (t == INVFS_ITYP_DIR)
        return "dir";
    if (t == INVFS_ITYP_LNK)
        return "lnk";
    return "?";
}

static void hex32(char out[65], const uint8_t h[32])
{
    static const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[2 * i] = d[h[i] >> 4];
        out[2 * i + 1] = d[h[i] & 15];
    }
    out[64] = 0;
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    const char *img, *path;
    const char *base;
    uint64_t ino;
    invfs_inode in;
    int err;
    uint8_t *blob = NULL;
    size_t blen = 0;
    long meta_records = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: invf-why <image> <path>\n");
        return 2;
    }
    img = argv[1];
    path = argv[2];

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "invf-why: cannot open volume %s (err %d)\n", img, err);
        return 1;
    }
    ino = vol_find(v, path);
    if (!ino) {
        fprintf(stderr, "invf-why: %s: no such name\n", path);
        vol_close(v);
        return 1;
    }
    if (vol_inode_get(v, ino, &in) != 1) {
        fprintf(stderr, "invf-why: %s: no inode row (directories carry"
                " no recipe; there is nothing stored to explain)\n",
                path);
        vol_close(v);
        return 1;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    printf("path: %s\n", path);
    printf("inode: %" PRIu64 "  type: %s  mode: %04o  uid: %u  gid: %u"
           "  size: %" PRIu64 "\n",
           ino, type_name(in.type), (unsigned)in.mode, (unsigned)in.uid,
           (unsigned)in.gid, in.size);
    /* inode row record: [u16][8B key][u16][114B row] */
    printf("records: inode %u\n",
           (unsigned)(2 + 8 + 2 + sizeof(invfs_inode_row)));
    meta_records += 2 + 8 + 2 + (long)sizeof(invfs_inode_row);

    /* class stamp: the WHY, from the named-xattr tree. */
    {
        invfs_class_tlv cls;
        size_t vlen = sizeof cls;
        if (vol_get_xattr(v, ino, INVFS_XATTR_CLASS, &cls, &vlen) == 0 &&
            vlen == sizeof cls) {
            size_t klen = 1 + 8 + 2 + strlen(INVFS_XATTR_CLASS);
            printf("class: %s{algo=%s(%u),gen=%u}  (xattr %s, %u B value)\n",
                   class_name(cls.cls), ast_algo_name(cls.algo),
                   (unsigned)cls.algo, (unsigned)cls.gen,
                   INVFS_XATTR_CLASS, (unsigned)vlen);
            printf("records: xattr %u\n",
                   (unsigned)(4 + klen + vlen));
            meta_records += (long)(4 + klen + vlen);
        } else {
            printf("class: none stamped\n");
        }
    }

    /* dirent record: key = parent u64 BE || name_len u16 BE || name,
     * value = inode id u64. The parent id is not resolved here; the
     * shape is fixed, so the bytes are exact anyway. */
    {
        size_t klen = 8 + 2 + strlen(base);
        printf("records: dirent %u\n", (unsigned)(4 + klen + 8));
        meta_records += (long)(4 + klen + 8);
    }

    /* recipe blob, content-addressed under 0x04 || blake3. */
    {
        char hx[65];
        static const uint8_t zero[INVFS_RECIPE_ADDR_LEN];
        if (memcmp(in.recipe_addr, zero, sizeof zero) == 0) {
            printf("recipe: none (empty file)\n");
        } else if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0) {
            printf("recipe: blob unloadable (damage? run invf-fsck)\n");
        } else {
            invfs_ast_hdr hdr;
            const invfs_ast_block_entry *ents = NULL;
            size_t nents = 0;
            hex32(hx, in.recipe_addr);
            printf("recipe: blake3:%s  blob %u B\n", hx, (unsigned)blen);
            printf("records: recipe %u\n",
                   (unsigned)(4 + (1 + INVFS_RECIPE_ADDR_LEN) + blen));
            meta_records += (long)(4 + (1 + INVFS_RECIPE_ADDR_LEN) + blen);
            if (vol_ast_recipe_parse(blob, blen, &hdr, &ents,
                                     &nents) != 0 || !ents) {
                printf("entries: not an AST recipe (symlink target?"
                       " %" PRIu64 " raw bytes)\n",
                       (uint64_t)blen);
            } else {
                size_t k;
                printf("entries: %u\n", (unsigned)nents);
                for (k = 0; k < nents; k++) {
                    const invfs_ast_block_entry *e = &ents[k];
                    /* data extent from the segment's framed header:
                     * [u32 csize LE][u32 crc32c][payload]. */
                    uint8_t fh[8];
                    if (e->algo == INVFS_ALGO_WINDOW_SRC) {
                        printf("  [%u] off=%" PRIu64 " len=%" PRIu64
                               " zone=%s algo=%s block=%u window (no blocks)\n",
                               (unsigned)k, e->file_offset, e->length,
                               zone_name(e->zone),
                               ast_algo_name(e->algo), e->block_id);
                    } else if (io_pread(&v->io,
                                        e->pba * INVFS_BLOCK_SIZE,
                                        fh, sizeof fh) != 0) {
                        printf("  [%u] off=%" PRIu64 " len=%" PRIu64
                               " zone=%s algo=%s(%u) block=%u pba=%" PRIu64
                               " header unreadable\n",
                               (unsigned)k, e->file_offset, e->length,
                               zone_name(e->zone),
                               ast_algo_name(e->algo), e->algo,
                               e->block_id, e->pba);
                    } else {
                        uint32_t cs = (uint32_t)fh[0] |
                                      ((uint32_t)fh[1] << 8) |
                                      ((uint32_t)fh[2] << 16) |
                                      ((uint32_t)fh[3] << 24);
                        uint64_t blocks =
                            (uint64_t)(cs + 8 + INVFS_BLOCK_SIZE - 1) /
                            INVFS_BLOCK_SIZE;
                        printf("  [%u] off=%" PRIu64 " len=%" PRIu64
                               " zone=%s algo=%s(%u) block=%u pba=%" PRIu64
                               " stored=%u B in %" PRIu64 " blocks\n",
                               (unsigned)k, e->file_offset, e->length,
                               zone_name(e->zone),
                               ast_algo_name(e->algo), e->algo,
                               e->block_id, e->pba, cs,
                               blocks);
                    }
                }
            }
            free(blob);
        }
    }
    printf("records: total ~%ld B of metadata (shared pages excluded)\n",
           meta_records);

    vol_close(v);
    return 0;
}
