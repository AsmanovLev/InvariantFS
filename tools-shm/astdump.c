/* astdump.c — debug helper: dump a file's AST entries (zone/algo/pba/off).
 * usage: astdump <img> <name>   (env INVFS_DEV1 honored by vol_open)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume_internal.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    if (argc < 3) { fprintf(stderr, "usage: astdump <img> <name>\n"); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    uint64_t id = vol_find(v, argv[2]);
    if (!id) { fprintf(stderr, "'%s' not found\n", argv[2]); vol_close(v); return 1; }
    uint64_t pos = idx_get_id(v, id);
    printf("inode %llu record pos %llu\n", (unsigned long long)id,
           (unsigned long long)pos);
    uint8_t pre[36];
    if (vol_read_raw(v, pos, pre, sizeof pre) != 0) {
        fprintf(stderr, "header read failed\n"); vol_close(v); return 1;
    }
    const invfs_inode_rec *rh = (const invfs_inode_rec *)pre;
    if (rh->magic != INODE_REC_MAGIC) {
        fprintf(stderr, "bad magic %08x at record pos\n", rh->magic);
        vol_close(v); return 1;
    }
    uint8_t *rb = (uint8_t *)malloc(rh->rec_len + 4);
    if (!rb || vol_read_raw(v, pos, rb, rh->rec_len) != 0) {
        fprintf(stderr, "record read failed\n"); vol_close(v); return 1;
    }
    const uint8_t *body = invfs_rec_body((const invfs_inode_rec *)rb);
    size_t avail = (size_t)rh->rec_len - (size_t)(body - rb);
    invfs_ast_hdr h;
    if (invfs_ast_hdr_parse(body, avail, &h) != 0) {
        fprintf(stderr, "recipe header parse failed\n"); vol_close(v); return 1;
    }
    printf("file_size %llu recipe v%u num_blocks %u num_children %u hdr_len %u\n",
           (unsigned long long)h.file_size, h.version, h.num_blocks,
           h.num_children, h.hdr_len);
    const invfs_ast_block_entry *e =
        (const invfs_ast_block_entry *)(body + h.hdr_len);
    for (uint32_t i = 0; i < h.num_blocks && i < 16; i++) {
        printf("  ent[%u]: foff=%llu len=%llu zone=%u algo=%u blk_id=%u "
               "blk_off=%u pba=%llu\n", i,
               (unsigned long long)e[i].file_offset,
               (unsigned long long)e[i].length, e[i].zone, e[i].algo,
               e[i].block_id, e[i].block_offset,
               (unsigned long long)e[i].pba);
    }
    /* raw segment frame at the first entry's pba */
    if (h.num_blocks) {
        uint8_t frame[16];
        uint64_t off = e[0].pba * INVFS_BLOCK_SIZE + e[0].block_offset;
        if (vol_read_raw(v, off, frame, sizeof frame) == 0) {
            uint32_t csize, crc;
            memcpy(&csize, frame, 4);
            memcpy(&crc, frame + 4, 4);
            printf("frame @%llu: csize=%u crc=%08x payload[0..4]=%02x %02x "
                   "%02x %02x\n", (unsigned long long)off, csize, crc,
                   frame[8], frame[9], frame[10], frame[11]);
        } else {
            printf("frame read @%llu FAILED\n", (unsigned long long)off);
        }
    }
    free(rb);
    vol_close(v);
    return 0;
}
