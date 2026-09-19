/*
 * metabuf_test.c -- WP-M2 unit harness for the v3 base-page format, the
 * blkptr verification rules and the metadata block allocator.
 *
 * Pure/format cases run with no backing store (CRC, header validation,
 * struct layout). The IO/allocator cases build a small synthetic volume in
 * a scratch image (a real blkio over a plain file) and drive it through
 * mbuf_write/mbuf_read, mbuf_alloc/mbuf_free and the RT30 double-slot
 * publish/select helpers.
 *
 *   invf-metabuf_test [scratch-dir]
 *
 * Deliberately does NOT use vol_open/mkfs: this is the substrate unit tier,
 * and the v3 write path is not wired yet (see WP-M2 scope).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "volume_internal.h"
#include "vol_metabuf.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static unsigned char pat(uint64_t i)
{
    return (unsigned char)(i * 131u + 17u);
}

/* ---- synthetic volume ------------------------------------------------- */

#define FB_TOTAL   64u
#define FB_META_START 1u
#define FB_META_BLOCKS 8u          /* blocks 1..8 */
#define FB_RAW_START 9u
#define FB_RAW_BLOCKS 8u           /* blocks 9..16 */
#define FB_SHADOW_START 17u
#define FB_SHADOW_BLOCKS (FB_TOTAL - FB_SHADOW_START)  /* 17..63 */
#define FB_MAPPER_PBA 3u
#define FB_MAPPER_BLOCKS 2u        /* root area boot pages then 5,6 */

static int image_make(const char *path, uint64_t blocks)
{
    blkio io;
    unsigned char *buf;
    uint64_t p;
    int j;
    if (blkio_open(&io, path, BLKIO_CREATE) != 0)
        return -1;
    if (blkio_chsize(&io, blocks * INVFS_BLOCK_SIZE) != 0) {
        blkio_close(&io);
        return -1;
    }
    buf = (unsigned char *)malloc(INVFS_BLOCK_SIZE);
    if (!buf) { blkio_close(&io); return -1; }
    for (p = 0; p < blocks; p++) {
        for (j = 0; j < INVFS_BLOCK_SIZE; j++)
            buf[j] = pat(p * INVFS_BLOCK_SIZE + (uint64_t)j);
        if (blkio_pwrite(&io, p * INVFS_BLOCK_SIZE, buf, INVFS_BLOCK_SIZE) != 0) {
            free(buf);
            blkio_close(&io);
            return -1;
        }
    }
    free(buf);
    blkio_close(&io);
    return 0;
}

/* Construct the synthetic v3 geometry on an already-made image. Mirrors
 * mkfs: block 0 + the whole metadata zone are allocated; the map/raw/
 * shadow fields are advisory. */
static int fake_vol_open(invfs_volume *v, const char *path)
{
    size_t bb;
    uint64_t i, alloc = 0;

    memset(v, 0, sizeof *v);
    v->path = strdup(path);
    if (!v->path)
        return -1;
    if (blkio_open(&v->io, path, 0) != 0)
        return -1;
    v->io_open[0] = 1;
    v->ndev = 1;
    v->dev0_present = 1;

    v->sb.total_blocks = FB_TOTAL;
    v->sb.block_size = INVFS_BLOCK_SIZE;
    v->sb.metadata_zone_start = FB_META_START;
    v->sb.metadata_zone_blocks = FB_META_BLOCKS;
    v->sb.raw_zone_start = FB_RAW_START;
    v->sb.raw_zone_blocks = FB_RAW_BLOCKS;
    v->sb.shadow_zone_start = FB_SHADOW_START;
    v->sb.shadow_zone_blocks = FB_SHADOW_BLOCKS;
    v->sb.reserved_blocks = 0;
    v->sb.hard_min_blocks = 0;
    v->sb.meta_reserved_pct = 0;
    v->sb.meta_mapper_pba = FB_MAPPER_PBA;
    v->sb.meta_mapper_blocks = FB_MAPPER_BLOCKS;

    v->bitmap_blocks = (FB_TOTAL / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    bb = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    v->bitmap = (uint8_t *)calloc(1, bb);
    if (!v->bitmap)
        return -1;
    /* mkfs marks [0, metadata_zone_start + metadata_zone_blocks) allocated */
    for (i = 0; i <= FB_META_START + FB_META_BLOCKS - 1; i++)
        bit_set(v->bitmap, i);
    for (i = 0; i < FB_TOTAL; i++)
        if (bit_get(v->bitmap, i))
            alloc++;
    v->free_blocks = FB_TOTAL - alloc;
    alloc_state_reset(v);

    memset(&v->rt30, 0, sizeof v->rt30);
    memcpy(v->rt30.magic, "RT30", 4);
    v->rt30.version = INVFS_RT30_VERSION;
    v->rt30.page_size = INVFS_V3_PAGE_SIZE_DEFAULT;
    v->rt30.seq = 0;
    v->rt30_present = 1;
    mbuf_init(v);
    return 0;
}

static void fake_vol_close(invfs_volume *v)
{
    blkio_close(&v->io);
    free(v->bitmap);
    free(v->meta_type_bitmap);
    free(v->path);
    memset(v, 0, sizeof *v);
}

/* ---- tests ------------------------------------------------------------ */

static void test_layout(void)
{
    printf("layout / packed structs\n");
    ok(sizeof(invfs_page_hdr) == 20, "sizeof(invfs_page_hdr) == 20");
    ok(sizeof(invfs_blkptr) == 24, "sizeof(invfs_blkptr) == 24");
    ok(sizeof(invfs_rt30) == 48, "sizeof(invfs_rt30) == 48");
    ok(offsetof(invfs_page_hdr, magic) == 0, "page magic at 0");
    ok(offsetof(invfs_page_hdr, gen) == 4, "page gen at 4");
    ok(offsetof(invfs_page_hdr, level) == 12, "page level at 12");
    ok(offsetof(invfs_page_hdr, nentries) == 14, "page nentries at 14");
    ok(offsetof(invfs_page_hdr, checksum) == 16, "page checksum at 16");
    ok(offsetof(invfs_blkptr, pba) == 0, "blkptr pba at 0");
    ok(offsetof(invfs_blkptr, checksum) == 8, "blkptr checksum at 8");
    ok(offsetof(invfs_blkptr, gen) == 12, "blkptr gen at 12");
    ok(offsetof(invfs_blkptr, flags) == 20, "blkptr flags at 20");
    ok(offsetof(invfs_rt30, root_slot) == 0xC, "rt30 root_slot at 0xC");
    ok(offsetof(invfs_rt30, seq) == 0x24, "rt30 seq at 0x24");
    ok(offsetof(invfs_rt30, crc32c) == 0x2C, "rt30 crc at 0x2C");
}

static void test_page_format(void)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_page_hdr *h;
    uint32_t crc;
    int i;

    printf("page format / CRC\n");
    memset(page, 0xAB, sizeof page);
    mbuf_page_init(page, INVFS_PAGE_LEVEL_LEAF, 42);
    h = mbuf_page_hdr(page);
    ok(memcmp(h->magic, "BPG3", 4) == 0, "magic BPG3");
    ok(h->gen == 42, "gen stamped");
    ok(h->level == INVFS_PAGE_LEVEL_LEAF, "level leaf");
    ok(h->nentries == 0, "nentries zero");
    ok(h->checksum == 0, "checksum zero until seal");

    mbuf_page_seal(page);
    crc = mbuf_page_crc(page);
    ok(h->checksum == crc, "seal stores the CRC");
    ok(mbuf_page_validate(page) == 1, "sealed page validates");

    /* round-trip the CRC convention: recomputing after a no-op is stable */
    ok(mbuf_page_crc(page) == crc, "CRC is deterministic");

    /* torn page: one flipped byte must be detected */
    page[100] ^= 0x01;
    ok(mbuf_page_validate(page) == 0, "flipped byte detected");
    page[100] ^= 0x01;
    ok(mbuf_page_validate(page) == 1, "restore validates");

    /* checksum field itself is excluded from the CRC (RDP0 rule) */
    h->checksum = 0;
    for (i = 0; i < 4; i++)
        ((uint8_t *)&h->checksum)[i] = 0x5A;
    ok(mbuf_page_crc(page) == crc, "CRC ignores the checksum field");
    h->checksum = crc;

    /* bad magic is refused even if the CRC happens to match */
    page[0] ^= 0xFF;
    mbuf_page_seal(page);
    ok(mbuf_page_validate(page) == 0, "bad magic refused even with good CRC");
}

static void test_page_io(invfs_volume *v)
{
    uint8_t page[INVFS_BLOCK_SIZE], back[INVFS_BLOCK_SIZE];
    invfs_blkptr p;
    uint64_t pba;

    printf("page IO / blkptr verification\n");
    pba = mbuf_alloc(v, 7);
    ok(pba != 0, "alloc a page for IO");
    if (!pba)
        return;
    mbuf_page_init(page, INVFS_PAGE_LEVEL_LEAF, 7);
    page[64] = pat(1);
    page[4000] = pat(2);
    ok(mbuf_write(v, pba, page) == 0, "mbuf_write");
    memset(back, 0, sizeof back);
    ok(mbuf_read(v, pba, back) == 0, "mbuf_read");
    ok(memcmp(page, back, sizeof page) == 0, "read-back is byte-identical");
    ok(mbuf_page_validate(back) == 1, "read-back validates");

    mbuf_ptr_set(&p, pba, back, INVFS_BP_LEAF);
    ok(p.pba == pba, "ptr pba");
    ok(p.gen == 7, "ptr gen from page");
    ok(p.checksum == mbuf_page_hdr(back)->checksum, "ptr checksum from page");
    ok(mbuf_verify_ptr(v, &p) == 0, "verify_ptr accepts the live page");
    ok(mbuf_read_ptr(v, &p, back) == 0, "read_ptr accepts the live page");

    {
        invfs_blkptr bad = p;
        bad.gen = 8;
        ok(mbuf_verify_ptr(v, &bad) != 0, "gen mismatch rejected");
        bad = p;
        bad.checksum ^= 0xDEADBEEFu;
        ok(mbuf_verify_ptr(v, &bad) != 0, "checksum mismatch rejected");
        bad = p;
        bad.pba = 0;
        ok(mbuf_verify_ptr(v, &bad) != 0, "null pba rejected");
    }

    /* torn on-disk page: bypass mbuf_write's reseal to simulate the tear */
    page[100] ^= 0x01;
    ok(blkio_pwrite(&v->io, pba * INVFS_BLOCK_SIZE, page,
                    INVFS_BLOCK_SIZE) == 0, "corrupt the page on disk");
    ok(mbuf_verify_ptr(v, &p) != 0, "torn page rejected by verify_ptr");
}

static void test_allocator(invfs_volume *v)
{
    uint64_t boot0, boot1, c, d, e, pba, reuse = 0, last = 0, n = 0;
    uint64_t seen[128];

    printf("allocator: bootstrap, reuse, ENOSPC\n");
    /* WP-M1 reserved the two root-area pages at mapper_pba+mapper_blocks */
    boot0 = mbuf_alloc(v, 1);
    boot1 = mbuf_alloc(v, 1);
    ok(boot0 == FB_MAPPER_PBA + FB_MAPPER_BLOCKS, "first alloc is the boot page");
    ok(boot1 == boot0 + 1, "second alloc is the next boot page");

    c = mbuf_alloc(v, 1);   /* boot exhausted -> free-space pool */
    d = mbuf_alloc(v, 1);
    ok(c != 0 && d != 0 && c != d, "free-space allocations distinct");
    ok(c >= FB_SHADOW_START || c >= FB_META_START,
       "allocation is inside the metadata or shared pool");

    mbuf_free(v, c);
    e = mbuf_alloc(v, 1);
    ok(e == c, "freed page is reused");

    mbuf_free(v, boot0);
    reuse = mbuf_alloc(v, 1);
    ok(reuse == boot0, "freed metadata-zone page is reused");

    /* allocate every remaining page; the allocator must return 0 rather
     * than abort or underflow. */
    while (n < 128) {
        pba = mbuf_alloc(v, 1);
        if (pba == 0)
            break;
        seen[n++] = pba;
    }
    ok(n < 128, "allocator terminates instead of looping");
    ok(mbuf_alloc(v, 1) == 0, "exhaustion returns 0 (ENOSPC), not abort");

    /* a free must make room again */
    if (n) {
        mbuf_free(v, seen[0]);
        last = mbuf_alloc(v, 1);
        ok(last != 0, "alloc after free succeeds");
    }
    while (n)
        mbuf_free(v, seen[--n]);
    /* return everything so the following tests start from a clean pool */
    mbuf_free(v, boot1);
    mbuf_free(v, d);
    mbuf_free(v, e);
    mbuf_free(v, reuse);
    if (last)
        mbuf_free(v, last);
}

static void test_rt30(invfs_volume *v)
{
    uint8_t p1[INVFS_BLOCK_SIZE], p2[INVFS_BLOCK_SIZE];
    uint64_t rp = 0, rg = 0, a, b;

    printf("RT30 store / double-slot root publish\n");
    v->rt30.seq = 0;
    v->rt30.root_slot[0] = v->rt30.root_slot[1] = 0;
    v->rt30_present = 1;
    ok(mbuf_rt30_store(v) == 0, "rt30 store");
    v->rt30_present = 0;
    ok(mbuf_rt30_load(v) == 0, "rt30 load validates");
    ok(v->rt30_present == 1, "rt30 marked present");
    ok(v->rt30.page_size == 4096, "rt30 page_size 4096");

    ok(mbuf_root_read(v, &rp, &rg) == 1, "empty root reports 1");

    a = mbuf_alloc(v, 1);
    b = mbuf_alloc(v, 2);
    ok(a != 0 && b != 0, "alloc roots");
    mbuf_page_init(p1, INVFS_PAGE_LEVEL_LEAF, 1);
    mbuf_page_init(p2, INVFS_PAGE_LEVEL_LEAF, 2);
    ok(mbuf_write(v, a, p1) == 0, "write root page 1");
    ok(mbuf_write(v, b, p2) == 0, "write root page 2");

    ok(mbuf_root_publish(v, a, 1) == 0, "publish root gen 1");
    ok(mbuf_root_read(v, &rp, &rg) == 0 && rp == a && rg == 1,
       "root read returns gen-1 root");
    ok(mbuf_root_publish(v, b, 2) == 0, "publish root gen 2");
    ok(mbuf_root_read(v, &rp, &rg) == 0 && rp == b && rg == 2,
       "root read returns gen-2 root (higher gen wins)");
    ok(v->rt30.seq == 2, "seq advanced once per publish");
    ok(v->rt30.root_slot[0] == a && v->rt30.root_slot[1] == b,
       "slots alternate");

    /* a rejected publish (gen mismatch) must not move seq */
    {
        uint64_t seq_before = v->rt30.seq;
        ok(mbuf_root_publish(v, a, 99) != 0, "gen mismatch refused");
        ok(v->rt30.seq == seq_before, "refused publish leaves seq alone");
    }

    /* tear the newer slot's page: the older root must still be selected */
    p2[50] ^= 0x01;
    ok(blkio_pwrite(&v->io, b * INVFS_BLOCK_SIZE, p2,
                    INVFS_BLOCK_SIZE) == 0, "tear the newer root page");
    ok(mbuf_root_read(v, &rp, &rg) == 0 && rp == a && rg == 1,
       "torn newer slot falls back to the older root");
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-metabuf_test [scratch-dir]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE,
                    INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    printf("metabuf tests (WP-M2)\n");

    test_layout();
    test_page_format();

    snprintf(img, sizeof img, "%s/invf-metabuf_test.img", dir);
    if (image_make(img, FB_TOTAL) != 0) {
        printf("  cannot create scratch image %s\n", img);
        return 1;
    }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) { printf("  out of memory\n"); return 1; }
    if (fake_vol_open(v, img) != 0) {
        printf("  cannot open scratch volume\n");
        return 1;
    }

    test_allocator(v);
    test_page_io(v);
    test_rt30(v);

    fake_vol_close(v);
    free(v);
    remove(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
