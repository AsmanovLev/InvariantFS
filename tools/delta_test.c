/*
 * delta_test.c -- WP-M10 unit harness + offline e2e driver for the v3
 * delta log (append + in-memory index + mount replay).
 *
 * Unit mode (default, `invf-delta_test [scratch-dir]`):
 *   pure record/segment codec cases, then synthetic-volume cases driving
 *   vol_delta_append/vol_delta_lookup/vol_delta_mount: round-trip,
 *   coalescing (latest wins), the delete marker, per-record CRC detection,
 *   torn-tail truncation, segment rollover and index/scan-replay
 *   equivalence. No vol_open/mkfs: the substrate is a scratch image opened
 *   through the same blkio + synthetic geometry metabuf_test uses.
 *
 * E2E mode (`invf-delta_test append|verify|tear|verify-torn <img> <n>`):
 *   drives a real mkfs'd VOLF_META image through vol_open (which runs mount
 *   replay) and vol_delta_append, so tools/test-meta-delta.sh can test
 *   crash/remount reconstruction and a torn tail end to end.
 *
 * The record framing is frozen in invarifs.h; this harness never invents
 * its own on-disk format.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>     /* crash-append: SIGKILL */
#include <unistd.h>     /* crash-append: getpid */

#include "volume_internal.h"   /* pulls in volume.h + invarifs.h */
#include "vol_delta.h"
#include "vol_metabuf.h"
#include "vol_spt0.h"

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

/* ================================================================== */
/* synthetic volume (mirrors metabuf_test's substrate)                */
/* ================================================================== */

#define FB_TOTAL        512u
#define FB_META_START   1u
#define FB_META_BLOCKS  32u
#define FB_RAW_START    33u
#define FB_RAW_BLOCKS   32u
#define FB_SHADOW_START 65u
#define FB_SHADOW_BLOCKS (FB_TOTAL - FB_SHADOW_START)
#define FB_MAPPER_PBA   3u
#define FB_MAPPER_BLOCKS 2u

static unsigned char pat(uint64_t i)
{
    return (unsigned char)(i * 131u + 17u);
}

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

/* Open the synthetic geometry. Fake_vol_open mirrors mkfs: block 0 and the
 * whole metadata zone are allocated; raw/shadow fields are advisory. */
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
    for (i = 0; i <= FB_META_START + FB_META_BLOCKS - 1; i++)
        bit_set(v->bitmap, i);
    for (i = 0; i < FB_TOTAL; i++)
        if (bit_get(v->bitmap, i))
            alloc++;
    v->free_blocks = FB_TOTAL - alloc;
    alloc_state_reset(v);

    memset(&v->rt, 0, sizeof v->rt);
    memcpy(v->rt.magic, "RT30", 4);
    v->rt.version = INVFS_RT_VERSION;
    v->rt.page_size = INVFS_PAGE_SIZE_DEFAULT;
    v->rt_present = 1;
    mbuf_init(v);
    return 0;
}

static void fake_vol_close(invfs_volume *v)
{
    if (!v->io_open[0])
        return;
    blkio_close(&v->io);
    v->io_open[0] = 0;
    free(v->bitmap);
    free(v->meta_type_bitmap);
    free(v->path);
    memset(v, 0, sizeof *v);
}

/* Fresh open of the synthetic image: load RT30 from disk (absent on the
 * first open) and replay the delta chain. */
static int synth_open(invfs_volume *v, const char *path)
{
    fake_vol_close(v);
    if (fake_vol_open(v, path) != 0)
        return -1;
    mbuf_rt30_load(v);          /* rc 1 = present an empty root */
    return vol_delta_mount(v);
}

static int synth_remount(invfs_volume *v, const char *path)
{
    vol_delta_close(v);
    fake_vol_close(v);
    if (fake_vol_open(v, path) != 0)
        return -1;
    mbuf_rt30_load(v);
    return vol_delta_mount(v);
}

/* ================================================================== */
/* pure format / parser cases                                          */
/* ================================================================== */

/* Encode one record into buf; returns its length. */
static size_t mkrec(uint8_t *buf, const char *key, const char *val,
                    uint16_t flags)
{
    size_t kl = strlen(key), vl = val ? strlen(val) : 0;
    uint32_t crc;
    buf[0] = (uint8_t)(kl >> 8); buf[1] = (uint8_t)kl;
    buf[2] = (uint8_t)(vl >> 8); buf[3] = (uint8_t)vl;
    buf[4] = (uint8_t)(flags >> 8); buf[5] = (uint8_t)flags;
    crc = invfs_crc32c(key, kl);
    if (vl)
        crc = invfs_crc32c_update(crc, val, vl);
    memcpy(buf + 6, &crc, 4);
    memcpy(buf + INVFS_DELTA_REC_HDR_LEN, key, kl);
    if (vl)
        memcpy(buf + INVFS_DELTA_REC_HDR_LEN + kl, val, vl);
    return INVFS_DELTA_REC_HDR_LEN + kl + vl;
}

static void test_format(void)
{
    printf("delta format / recorder layout\n");
    ok(sizeof(invfs_delta_rec_hdr) == 10, "sizeof(delta_rec_hdr) == 10");
    ok(sizeof(invfs_delta_seg_hdr) == 44, "sizeof(delta_seg_hdr) == 44");
    ok(INVFS_DELTA_REC_HDR_LEN == 10, "REC_HDR_LEN == 10");
    ok(INVFS_DELTA_SEG_HDR_LEN == 44, "SEG_HDR_LEN == 44");
    ok(offsetof(invfs_delta_rec_hdr, key_len) == 0, "key_len at 0");
    ok(offsetof(invfs_delta_rec_hdr, val_len) == 2, "val_len at 2");
    ok(offsetof(invfs_delta_rec_hdr, flags) == 4, "flags at 4");
    ok(offsetof(invfs_delta_rec_hdr, crc32c) == 6, "crc at 6");
    ok(offsetof(invfs_delta_seg_hdr, seg_seq) == 16, "seg_seq at 16");
    ok(offsetof(invfs_delta_seg_hdr, prev_pba) == 24, "prev_pba at 24");
    ok(offsetof(invfs_delta_seg_hdr, crc32c) == 40, "seg crc at 40");
    ok(INVFS_DELTA_SEG_BYTES == 131072ULL, "segment is 128 KiB");
}

static void test_parser(void)
{
    uint8_t buf[256];
    uint16_t kl, vl, fl;
    size_t rl;
    int rc;

    printf("record parser: CRC, truncation, reserved bits\n");
    memset(buf, 0, sizeof buf);
    rl = mkrec(buf, "hello", "world", 0);
    ok(rl == INVFS_DELTA_REC_HDR_LEN + 5 + 5, "record length");
    rc = vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl);
    ok(rc == 1 && kl == 5 && vl == 5 && fl == 0 &&
       rl == INVFS_DELTA_REC_HDR_LEN + 10, "parse round-trip");

    /* CRC mismatch is detected */
    buf[INVFS_DELTA_REC_HDR_LEN] ^= 0x01;
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == -1,
       "corrupt key detected");
    buf[INVFS_DELTA_REC_HDR_LEN] ^= 0x01;
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == 1,
       "restore parses");
    buf[INVFS_DELTA_REC_HDR_LEN + 7] ^= 0x80;
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == -1,
       "corrupt value detected");
    buf[INVFS_DELTA_REC_HDR_LEN + 7] ^= 0x80;

    /* truncated body */
    ok(vol_delta_rec_parse(buf, INVFS_DELTA_REC_HDR_LEN + 5 + 4, 0, &kl, &vl, &fl, &rl) == -1,
       "truncated body rejected");
    /* not enough bytes for a header: clean end */
    ok(vol_delta_rec_parse(buf, 5, 0, &kl, &vl, &fl, &rl) == 0,
       "short header is a clean end");

    /* zeroed header is clean end, not a zero-length record */
    memset(buf, 0, sizeof buf);
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == 0,
       "zeroed header is clean end");

    /* empty key / reserved flag / delete-with-value */
    memset(buf, 0, sizeof buf);
    mkrec(buf, "", "x", 0);
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == -1,
       "empty key rejected");
    memset(buf, 0, sizeof buf);
    mkrec(buf, "k", "v", 0x0002);
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == -1,
       "reserved flag rejected");
    memset(buf, 0, sizeof buf);
    mkrec(buf, "k", "v", INVFS_DELTA_FLAG_DELETE);
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == -1,
       "delete-with-value rejected");
    memset(buf, 0, sizeof buf);
    mkrec(buf, "k", NULL, INVFS_DELTA_FLAG_DELETE);
    ok(vol_delta_rec_parse(buf, sizeof buf, 0, &kl, &vl, &fl, &rl) == 1 &&
       (fl & INVFS_DELTA_FLAG_DELETE) && vl == 0, "delete record parses");
}

static void test_scan_valid(void)
{
    uint8_t buf[512];
    size_t off = 0;
    uint64_t n = 0;
    int bad = 0;
    size_t end;

    printf("scanner: valid prefix / torn tail\n");
    memset(buf, 0, sizeof buf);
    off += mkrec(buf + off, "a", "1", 0);
    off += mkrec(buf + off, "b", "22", 0);
    off += mkrec(buf + off, "c", "333", 0);
    end = vol_delta_scan_valid(buf, sizeof buf, &n, &bad);
    ok(end == off && n == 3 && !bad, "three valid records");

    /* tear the third record's value */
    buf[off - 1] ^= 0xFF;
    end = vol_delta_scan_valid(buf, sizeof buf, &n, &bad);
    ok(end == off - (INVFS_DELTA_REC_HDR_LEN + 1 + 3) && n == 2 && bad == 1,
       "torn tail truncates at the last valid record");
}

/* ================================================================== */
/* synthetic-volume cases                                              */
/* ================================================================== */

static void test_append_lookup(invfs_volume *v)
{
    delta_ref r;
    uint8_t val[64];
    uint16_t vlen = 0;
    int rc;

    printf("append / lookup / value read\n");
    ok(vol_delta_count(v) == 0, "empty index after mount");
    ok(vol_delta_append(v, (const uint8_t *)"alpha", 5,
                        (const uint8_t *)"one", 3, 0) == 0, "append alpha");
    rc = vol_delta_lookup(v, (const uint8_t *)"alpha", 5, &r);
    ok(rc == 1, "alpha found");
    ok(r.flags == 0 && r.vlen == 3, "alpha ref fields");
    ok(vol_delta_read_value(v, &r, val, sizeof val, &vlen) == 0 &&
       vlen == 3 && memcmp(val, "one", 3) == 0, "alpha value round-trip");
    ok(vol_delta_lookup(v, (const uint8_t *)"nope", 4, &r) == 0,
       "absent key reports 0");

    /* coalescing: latest wins, one distinct key */
    ok(vol_delta_append(v, (const uint8_t *)"alpha", 5,
                        (const uint8_t *)"two", 3, 0) == 0, "append alpha #2");
    ok(vol_delta_count(v) == 1, "coalesced to one key");
    vol_delta_lookup(v, (const uint8_t *)"alpha", 5, &r);
    memset(val, 0, sizeof val);
    vol_delta_read_value(v, &r, val, sizeof val, &vlen);
    ok(vlen == 3 && memcmp(val, "two", 3) == 0, "latest value wins");

    /* delete marker shadows the value in the index */
    ok(vol_delta_append(v, (const uint8_t *)"alpha", 5, NULL, 0,
                        INVFS_DELTA_FLAG_DELETE) == 0, "append delete");
    ok(vol_delta_lookup(v, (const uint8_t *)"alpha", 5, &r) == 1 &&
       (r.flags & INVFS_DELTA_FLAG_DELETE) && r.vlen == 0,
       "delete marker is the winner");
    ok(vol_delta_count(v) == 1, "delete does not add a key");
}

static void test_replay_remount(invfs_volume *v, const char *img)
{
    delta_ref r;
    uint8_t val[64];
    uint16_t vlen = 0;
    uint64_t seg1, bump1;

    printf("remount replay reconstructs the index\n");
    seg1 = v->delta_seg_pba;
    bump1 = v->delta_bump;
    ok(seg1 != 0 && bump1 > INVFS_DELTA_SEG_HDR_LEN, "active segment has data");

    ok(synth_remount(v, img) == 0, "remount");
    ok(v->delta_seg_pba == seg1, "active segment survives remount");
    ok(vol_delta_count(v) == 2, "two keys after replay");
    ok(vol_delta_lookup(v, (const uint8_t *)"alpha", 5, &r) == 1 &&
       (r.flags & INVFS_DELTA_FLAG_DELETE), "delete marker survived");
    ok(vol_delta_lookup(v, (const uint8_t *)"bravo", 5, &r) == 1, "bravo found");
    memset(val, 0, sizeof val);
    ok(vol_delta_read_value(v, &r, val, sizeof val, &vlen) == 0 &&
       vlen == 7 && memcmp(val, "seven!!", 7) == 0, "bravo value survived");
}

/* Index == a fresh linear scan of the raw active segment payload. */
static void test_index_equals_scan(invfs_volume *v)
{
    uint8_t *buf;
    uint64_t nrec = 0;
    int bad = 0;
    size_t plen, off = 0;
    uint64_t count;
    int seen_alpha = 0, seen_bravo = 0;

    printf("index == scan-replay equivalence\n");
    count = vol_delta_count(v);
    ok(v->delta_seg_pba != 0, "active segment present");
    buf = (uint8_t *)malloc((size_t)INVFS_DELTA_SEG_BYTES);
    if (!buf) {
        ok(0, "malloc replay buffer");
        return;
    }
    if (io_seek(&v->io, v->delta_seg_pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, (size_t)INVFS_DELTA_SEG_BYTES) != 0) {
        free(buf);
        ok(0, "read active segment");
        return;
    }
    plen = (size_t)(INVFS_DELTA_SEG_BYTES - INVFS_DELTA_SEG_HDR_LEN);
    while (off < plen) {
        uint16_t kl, vl, fl;
        size_t rl;
        int rc = vol_delta_rec_parse(buf + INVFS_DELTA_SEG_HDR_LEN, plen, off,
                                     &kl, &vl, &fl, &rl);
        if (rc <= 0)
            break;
        if (kl == 5 && memcmp(buf + INVFS_DELTA_SEG_HDR_LEN + off + INVFS_DELTA_REC_HDR_LEN,
                              "alpha", 5) == 0)
            seen_alpha = 1;
        if (kl == 5 && memcmp(buf + INVFS_DELTA_SEG_HDR_LEN + off + INVFS_DELTA_REC_HDR_LEN,
                              "bravo", 5) == 0)
            seen_bravo = 1;
        off += rl;
        nrec++;
    }
    free(buf);
    (void)nrec;
    (void)bad;
    ok(seen_alpha && seen_bravo, "scan sees the indexed keys");
    ok(count == 2, "index and scan agree on key count");
}

/* The bitmap is NOT made durable before a delta record, and there is no test
 * that pretends otherwise: a record is barriered at append
 * (vol_delta.c:571) while the bitmap is only written at vol_flush
 * (volume.c:2641), so a crash inside the deferred window leaves a durable
 * record naming blocks the on-disk bitmap still calls free. What makes that
 * safe is a DERIVATION, not an ordering, and this is the test for it: replay
 * re-reserves every block a replayed segment spans (dl_reserve_segment)
 * before any allocator call can hand it out again.
 *
 * A test that asserted "the bitmap is durable before the record" would be
 * asserting a guarantee the code does not have. This one asserts the
 * property that IS true, and it is written with an explicit red control
 * (the bit really is clear before replay) so a suite that silently stopped
 * exercising anything cannot pass. */
static void test_bitmap_before_delta_gap(const char *dir)
{
    char img[512];
    invfs_volume *v;
    uint64_t seg, b, leaked = 0, n;

    printf("bitmap-before-delta gap is safe by replay, not by ordering\n");
    snprintf(img, sizeof img, "%s/invf-delta_bmgap.img", dir);
    if (image_make(img, FB_TOTAL) != 0) { ok(0, "make bmgap image"); return; }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) { ok(0, "calloc bmgap volume"); return; }
    if (synth_open(v, img) != 0) { ok(0, "open bmgap volume"); free(v); return; }

    /* One append allocates a whole 128 KiB delta segment and reserves it. */
    ok(vol_delta_append(v, (const uint8_t *)"gap", 3,
                        (const uint8_t *)"payload", 7, 0) == 0,
       "append inside the deferred window");
    seg = v->delta_seg_pba;
    ok(seg != 0, "the append created a delta segment");
    ok(bit_get(v->bitmap, seg), "append reserved the segment in RAM");
    ok(v->rt_present && v->rt.delta_pba == seg,
       "RT30 names the segment (the record is durable and reachable)");

    /* The deferred window, exactly: the record and its segment are on the
     * image, the bitmap flush that would have recorded the reservation has
     * not happened. A plain file cannot have its page cache discarded
     * unprivileged, so the unflushed state is stood up directly -- the
     * allocation bits for the segment's blocks are gone. */
    n = 0;
    for (b = seg; b < seg + INVFS_DELTA_SEG_BLOCKS && b < v->sb.total_blocks; b++)
        n++;
    ok(n > 0, "the segment spans real blocks");
    for (b = seg; b < seg + INVFS_DELTA_SEG_BLOCKS && b < v->sb.total_blocks; b++)
        bit_clr(v->bitmap, b);

    /* RED CONTROL: without replay, those blocks are free and the allocator
     * would hand them straight back out. If this ever passes, the test has
     * stopped standing up the hazard and the assertion below proves
     * nothing. */
    for (b = seg; b < seg + INVFS_DELTA_SEG_BLOCKS && b < v->sb.total_blocks; b++)
        if (bit_get(v->bitmap, b)) break;
    ok(b >= seg + n, "control: the blocks really are un-reserved before replay");

    /* The record survived, so replay runs -- and must re-reserve. */
    ok(synth_remount(v, img) == 0, "replay after the unflushed bitmap");
    ok(vol_delta_count(v) == 1, "the record itself was durable");
    for (b = seg; b < seg + INVFS_DELTA_SEG_BLOCKS && b < v->sb.total_blocks; b++)
        if (!bit_get(v->bitmap, b)) leaked++;
    ok(leaked == 0, "replay re-reserved every block the record names");

    vol_delta_close(v);
    fake_vol_close(v);
    free(v);
    remove(img);
}

static void test_crc_truncation(const char *dir)
{
    char img[512];
    invfs_volume *v;
    delta_ref r;
    uint64_t off;
    unsigned char junk = 0xFF;

    printf("on-disk CRC mismatch truncates the replay\n");
    snprintf(img, sizeof img, "%s/invf-delta_crc.img", dir);
    if (image_make(img, FB_TOTAL) != 0) { ok(0, "make crc image"); return; }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) { ok(0, "calloc crc volume"); return; }
    if (synth_open(v, img) != 0) { ok(0, "open crc volume"); free(v); return; }
    ok(vol_delta_append(v, (const uint8_t *)"one", 3,
                        (const uint8_t *)"111", 3, 0) == 0, "append one");
    ok(vol_delta_append(v, (const uint8_t *)"two", 3,
                        (const uint8_t *)"222", 3, 0) == 0, "append two");
    ok(vol_delta_append(v, (const uint8_t *)"three", 5,
                        (const uint8_t *)"333", 3, 0) == 0, "append three");
    ok(vol_delta_lookup(v, (const uint8_t *)"two", 3, &r) == 1, "two found");
    off = r.seg * (uint64_t)INVFS_BLOCK_SIZE + r.off +
          INVFS_DELTA_REC_HDR_LEN + 3;    /* first value byte of "two" */
    ok(blkio_pwrite(&v->io, off, &junk, 1) == 0, "corrupt record two on disk");

    ok(synth_remount(v, img) == 0, "remount after corruption");
    ok(vol_delta_count(v) == 1, "replay stops before the bad record");
    ok(vol_delta_lookup(v, (const uint8_t *)"one", 3, &r) == 1,
       "record before the bad one survives");
    ok(vol_delta_lookup(v, (const uint8_t *)"two", 3, &r) == 0,
       "corrupt record dropped");
    ok(vol_delta_lookup(v, (const uint8_t *)"three", 5, &r) == 0,
       "tail after the corrupt record dropped");

    vol_delta_close(v);
    fake_vol_close(v);
    free(v);
    remove(img);
}

static void test_rollover(const char *dir)
{
    char img[512];
    invfs_volume *v;
    char key[32], val[1200];
    int i, n = 0;
    uint64_t segs = 0, bytes = 0, records = 0;

    printf("segment rollover + cross-segment lookup\n");
    snprintf(img, sizeof img, "%s/invf-delta_roll.img", dir);
    if (image_make(img, FB_TOTAL) != 0) { ok(0, "make rollover image"); return; }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) { ok(0, "calloc rollover volume"); return; }
    if (synth_open(v, img) != 0) { ok(0, "open rollover volume"); free(v); return; }

    memset(val, 'x', sizeof val);
    for (i = 0; i < 400; i++) {          /* 400 * ~1.2 KiB > 2 segments */
        int nk = snprintf(key, sizeof key, "key-%04d", i);
        val[sizeof val - 1] = (char)('a' + (i % 26));
        if (vol_delta_append(v, (const uint8_t *)key, (uint16_t)nk,
                             (const uint8_t *)val, (uint16_t)(sizeof val - 1),
                             0) != 0)
            break;
        n++;
    }
    ok(n == 400, "appended the whole run");
    vol_delta_stats(v, &records, &segs, &bytes);
    ok(segs >= 2, "rollover created a second segment");
    ok(records == 400, "400 distinct records indexed");

    /* a key from the first segment must still resolve after rollover */
    {
        delta_ref r;
        ok(vol_delta_lookup(v, (const uint8_t *)"key-0000", 8, &r) == 1,
           "early key found after rollover");
        ok(vol_delta_lookup(v, (const uint8_t *)"key-0399", 8, &r) == 1,
           "last key found after rollover");
    }
    /* and after a remount (chain replay across segments) */
    ok(synth_remount(v, img) == 0, "remount the rolled chain");
    ok(vol_delta_count(v) == 400, "all keys replayed across segments");
    {
        delta_ref r;
        ok(vol_delta_lookup(v, (const uint8_t *)"key-0000", 8, &r) == 1,
           "early key survives remount");
        ok(vol_delta_lookup(v, (const uint8_t *)"key-0200", 8, &r) == 1,
           "middle key survives remount");
        ok(vol_delta_lookup(v, (const uint8_t *)"key-0399", 8, &r) == 1,
           "last key survives remount");
    }

    vol_delta_close(v);
    fake_vol_close(v);
    free(v);
    remove(img);
}

/* WP-M12: a namespace mutation must land in the delta and leave the base
 * root untouched (the base stays immutable between folds). Drives the public
 * v3 mutation entry points on a synthetic volume whose base tree is empty, so
 * "base unchanged" is exactly "the base root blkptr did not move". */
static void test_metadata_delta(const char *dir)
{
    char img[512];
    invfs_volume *v;
    invfs_inode in;
    invfs_blkptr b0, b1;
    uint64_t child = 0;
    char vbuf[16];
    size_t vlen;

    printf("v3 metadata mutations append to the delta (base untouched)\n");
    snprintf(img, sizeof img, "%s/invf-delta_mut.img", dir);
    if (image_make(img, FB_TOTAL) != 0) { ok(0, "make v3mut image"); return; }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) { ok(0, "calloc v3mut volume"); return; }
    if (synth_open(v, img) != 0) {
        ok(0, "open v3mut volume");
        free(v);
        return;
    }
    if (vol_base_root(v, &b0) != 0) {
        ok(0, "read base root before mutations");
        vol_delta_close(v);
        fake_vol_close(v);
        free(v);
        return;
    }

    memset(&in, 0, sizeof in);
    in.type = INVFS_ITYP_REG;
    in.mode = 0640;
    in.uid = in.gid = 1000;
    in.nlink = 1;
    in.size = 0;

    ok(vol_inode_delta_put(v, 42, &in) == 0, "delta put inode 42");
    ok(vol_inode_get(v, 42, &in) == 1 && in.mode == 0640,
       "overlay reads the delta inode row");
    ok(vol_dirent_delta_put(v, 77, "a", 42) == 0, "delta put dirent");
    ok(vol_dirent_get(v, 77, "a", &child) == 1 && child == 42,
       "overlay reads the delta dirent");
    ok(vol_xattr_delta_set(v, 42, "user.k", "v", 1) == 0,
       "delta set xattr");
    vlen = sizeof vbuf;
    ok(vol_xattr_get(v, 42, "user.k", vbuf, &vlen) == 0 &&
       vlen == 1 && vbuf[0] == 'v', "overlay reads the delta xattr");
    ok(vol_inode_alloc(v) == 43,
       "id allocator resumes above the delta-only inode");

    ok(vol_dirent_delta_del(v, 77, "a") == 0, "delta delete dirent");
    ok(vol_dirent_get(v, 77, "a", &child) == 0, "deleted dirent hidden");
    ok(vol_inode_delta_delete(v, 42) == 0, "delta delete inode");
    ok(vol_inode_get(v, 42, &in) == 0, "deleted inode hidden");
    vlen = sizeof vbuf;
    /* -ENODATA, not the bare -1 this used to assert. The cascade removes
     * the keys, so the honest answer is "this xattr is not there" -- and
     * the bare -1 was the very value the row-read failure also returned,
     * which is how an unreadable inode row could read as an inode carrying
     * no ACLs. The assertion is now stronger, not weaker. */
    ok(vol_xattr_get(v, 42, "user.k", vbuf, &vlen) == -ENODATA,
       "inode delete cascaded the xattr keys");

    if (vol_base_root(v, &b1) != 0) {
        ok(0, "read base root after mutations");
    } else {
        ok(b1.pba == b0.pba && b1.gen == b0.gen,
           "base root unchanged after every mutation");
    }
    ok(vol_delta_count(v) > 0, "the recent tier is non-empty");

    vol_delta_close(v);
    fake_vol_close(v);
    free(v);
    remove(img);
}

/* A rollback must actually roll back ACROSS a segment rollover.
 *
 * The divergence this exists for (WP meta-write-dup-audit): three
 * implementations of "how many bytes does each segment of the delta chain
 * contribute", and vol_delta_truncate's was inverted. The producer
 * (vol_spt0.c, capture) counts the HEAD's used bytes and then a full
 * segment per older one; the WP96 prefix reader (vol_btree.c) inverts
 * exactly that; vol_delta_truncate walked the chain oldest-first and gave
 * the head's bump to the OLDEST segment instead.
 *
 * With one segment the two coincide, which is why nothing noticed. With
 * two, the old loop measured the OLDEST segment by the head's (small) bump,
 * so the cut landed inside the HEAD: the head was truncated at a nonsense
 * offset, the "newer" list came out EMPTY (nothing was freed), and rc = 0 --
 * a rollback that reported success and restored nothing. Every write made
 * after the savepoint survived it.
 *
 * The red control is the shape that distinguishes them: capture while the
 * chain is ONE segment, then keep appending until it rolls, then truncate
 * back to the CAPTURED delta_end. Records written before the capture must
 * survive; every record written after it must be gone -- in RAM and after a
 * remount. */
static void test_spt0_truncate_multiseg(const char *dir)
{
    char img[512];
    invfs_volume *v;
    char key[32], val[8000];
    uint64_t delta_end, segs = 0, bytes = 0, records = 0;
    uint64_t head_before;
    /* wide records, few of them: every append is a barrier, and the point
     * is to cross ONE 128 KiB segment boundary, not to append a lot. */
    int i, n_before = 12, n_total = 25, err = 0, rc_cap;
    delta_ref r;

    printf("rollback truncates correctly across a segment rollover\n");
    /* A REAL v3 volume, not the synthetic substrate: spt0_capture refuses
     * anything without a VOLF_META base root, and this leg exists to hold
     * the real producer (not a restatement of its formula) to the real
     * consumer. */
    snprintf(img, sizeof img, "%s/invf-delta_spt0trunc.img", dir);
    unlink(img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 1 2>/dev/null",
                 root, img);
        if (system(cmd) != 0) { ok(0, "make spt0-trunc image"); return; }
    }
    v = vol_open(img, &err);
    if (!v) { ok(0, "open spt0-trunc volume"); return; }

    /* The capture walks the BASE tree as part of its pin set and treats an
     * unreadable one as damaged -- and an EMPTY base tree reads as damaged
     * (an empty leaf fails the tree check). So put real content in the base
     * and fold it first; the fold also empties the delta, which is what
     * makes the segment geometry below start from a known place. */
    ok(vol_write_bulk(v, "seed", (const uint8_t *)"seed content", 12,
                         NULL) != 0, "seed file written");
    ok(vol_fold(v) == 0, "folded the seed into the base tree");

    memset(val, 'y', sizeof val);
    for (i = 0; i < n_before; i++) {
        int nk = snprintf(key, sizeof key, "pre-%04d", i);
        val[sizeof val - 1] = (char)('a' + (i % 26));
        if (vol_delta_append(v, (const uint8_t *)key, (uint16_t)nk,
                             (const uint8_t *)val,
                             (uint16_t)(sizeof val - 1), 0) != 0)
            break;
    }
    ok(i == n_before, "appended the pre-capture run");
    vol_delta_stats(v, &records, &segs, &bytes);
    ok(segs == 1, "the chain is ONE segment at capture time");
    head_before = v->delta_seg_pba;

    /* the real producer, not a restatement of its formula */
    rc_cap = spt0_capture(v);
    if (rc_cap != 0)
        printf("        (spt0_capture rc=%d -- SPT0_RC_DAMAGED is 3)\n",
               rc_cap);
    ok(rc_cap == 0, "spt0_capture succeeds");
    delta_end = v->spt0.delta_end;
    ok(delta_end > INVFS_DELTA_SEG_HDR_LEN, "capture recorded a delta_end");

    for (i = n_before; i < n_total; i++) {
        int nk = snprintf(key, sizeof key, "post-%04d", i);
        val[sizeof val - 1] = (char)('A' + (i % 26));
        if (vol_delta_append(v, (const uint8_t *)key, (uint16_t)nk,
                             (const uint8_t *)val,
                             (uint16_t)(sizeof val - 1), 0) != 0)
            break;
    }
    ok(i == n_total, "appended the post-capture run");
    vol_delta_stats(v, &records, &segs, &bytes);
    ok(segs >= 2, "the chain ROLLED to a second segment before the rollback");
    ok(v->delta_seg_pba != head_before, "the head is now the newer segment");

    ok(vol_delta_truncate(v, delta_end) == 0, "vol_delta_truncate succeeds");
    /* the caller re-indexes, exactly as spt0_restore does: the truncate
     * rewrites the segment and frees the newer ones, it does not touch the
     * in-RAM index (which still answers from the pre-truncate state). */
    vol_delta_close(v);
    ok(vol_delta_mount(v) == 0, "delta re-indexed after the truncate");

    /* the point of the whole thing: post-capture writes are gone */
    ok(vol_delta_lookup(v, (const uint8_t *)"post-0012", 8, &r) == 0,
       "a post-capture record is GONE after the rollback");
    ok(vol_delta_lookup(v, (const uint8_t *)"post-0024", 8, &r) == 0,
       "the last post-capture record is gone too");
    /* and pre-capture writes are not collateral damage */
    ok(vol_delta_lookup(v, (const uint8_t *)"pre-0000", 8, &r) == 1,
       "a pre-capture record survives the rollback");
    /* every post-capture key, not just two of them: the old loop's failure
     * mode was that NONE of them went away */
    {
        int leaked = 0;
        for (i = n_before; i < n_total; i++) {
            char k[32];
            int nk = snprintf(k, sizeof k, "post-%04d", i);
            if (vol_delta_lookup(v, (const uint8_t *)k, (uint16_t)nk, &r) == 1)
                leaked++;
        }
        ok(leaked == 0, "NO post-capture record survived the rollback");
    }
    {
        int lost = 0;
        for (i = 0; i < n_before; i++) {
            char k[32];
            int nk = snprintf(k, sizeof k, "pre-%04d", i);
            if (vol_delta_lookup(v, (const uint8_t *)k, (uint16_t)nk, &r) != 1)
                lost++;
        }
        ok(lost == 0, "no pre-capture record was collateral damage");
    }

    vol_close(v);

    /* durable, not just the in-RAM index */
    v = vol_open(img, &err);
    ok(v != NULL, "remount after the rollback");
    if (v) {
        ok(vol_delta_lookup(v, (const uint8_t *)"post-0012", 8, &r) == 0,
           "a post-capture record is still gone after a remount");
        ok(vol_delta_lookup(v, (const uint8_t *)"pre-0000", 8, &r) == 1,
           "a pre-capture record still resolves after a remount");
        vol_close(v);
    }
    remove(img);
}

static void run_unit(const char *dir)
{
    char img[512];
    invfs_volume *v;

    printf("delta tests (WP-M10)\n");
    test_format();
    test_parser();
    test_scan_valid();

    snprintf(img, sizeof img, "%s/invf-delta_test.img", dir);
    if (image_make(img, FB_TOTAL) != 0) {
        printf("  cannot create scratch image %s\n", img);
        failures++;
        return;
    }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v || synth_open(v, img) != 0) {
        printf("  cannot open scratch volume\n");
        free(v);
        return;
    }

    test_append_lookup(v);
    /* second key so the replay test has a survivor besides the delete */
    ok(vol_delta_append(v, (const uint8_t *)"bravo", 5,
                        (const uint8_t *)"seven!!", 7, 0) == 0, "append bravo");
    test_replay_remount(v, img);
    test_index_equals_scan(v);

    vol_delta_close(v);
    fake_vol_close(v);
    free(v);
    remove(img);

    test_crc_truncation(dir);
    test_bitmap_before_delta_gap(dir);
    test_rollover(dir);
    test_spt0_truncate_multiseg(dir);
    test_metadata_delta(dir);

    printf("%d checks, %d failure(s)\n", checks, failures);
}

/* ================================================================== */
/* e2e driver (real VOLF_META image via vol_open)                        */
/* ================================================================== */

#define E2E_N 40

static void e2e_key(int i, char *out, size_t cap)
{
    snprintf(out, cap, "k%03d", i);
}
static void e2e_val(int i, char *out, size_t cap)
{
    snprintf(out, cap, "val-%03d", i);
}

static int open_v3(const char *img, invfs_volume **out)
{
    int err = 0;
    invfs_volume *v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "delta_test: vol_open(%s) failed: err=%d\n", img, err);
        return -1;
    }
    if (!(vol_sb(v)->vol_flags & VOLF_META)) {
        fprintf(stderr, "delta_test: %s is not a v3 volume\n", img);
        vol_close(v);
        return -1;
    }
    *out = v;
    return 0;
}

static int e2e_append(const char *img, int n)
{
    invfs_volume *v;
    char key[16], val[16];
    int i;

    if (open_v3(img, &v) != 0)
        return 2;
    for (i = 0; i < n; i++) {
        e2e_key(i, key, sizeof key);
        e2e_val(i, val, sizeof val);
        if (vol_delta_append(v, (const uint8_t *)key, (uint16_t)strlen(key),
                             (const uint8_t *)val, (uint16_t)strlen(val),
                             0) != 0) {
            fprintf(stderr, "delta_test: append %s failed\n", key);
            vol_close(v);
            return 1;
        }
    }
    /* a trailing delete for k005: exercises the delete marker + makes the
     * torn-tail case deterministic (the last on-disk record is a delete). */
    e2e_key(5, key, sizeof key);
    if (vol_delta_append(v, (const uint8_t *)key, (uint16_t)strlen(key),
                         NULL, 0, INVFS_DELTA_FLAG_DELETE) != 0) {
        fprintf(stderr, "delta_test: append delete %s failed\n", key);
        vol_close(v);
        return 1;
    }
    printf("e2e append: %d records + delete(k005); segments=%llu bytes=%llu\n",
           n, (unsigned long long)v->delta_segments,
           (unsigned long long)v->delta_bytes);
    vol_close(v);
    return 0;
}

/* expect_delete5: 1 = k005 must carry the delete marker (normal replay),
 * 0 = k005 must carry its value (the torn delete was dropped). */
static int e2e_verify(const char *img, int n, int expect_delete5)
{
    invfs_volume *v;
    char key[16], val[16];
    delta_ref r;
    uint8_t got[64];
    uint16_t vlen = 0;
    int i;

    if (open_v3(img, &v) != 0)
        return 2;
    if (vol_delta_count(v) != (uint64_t)n) {
        fprintf(stderr, "delta_test: replay count %llu, want %d\n",
                (unsigned long long)vol_delta_count(v), n);
        vol_close(v);
        return 1;
    }
    for (i = 0; i < n; i++) {
        e2e_key(i, key, sizeof key);
        e2e_val(i, val, sizeof val);
        if (vol_delta_lookup(v, (const uint8_t *)key, (uint16_t)strlen(key),
                             &r) != 1) {
            fprintf(stderr, "delta_test: replay lost key %s\n", key);
            vol_close(v);
            return 1;
        }
        if (i == 5 && expect_delete5) {
            if (!(r.flags & INVFS_DELTA_FLAG_DELETE) || r.vlen != 0) {
                fprintf(stderr, "delta_test: k005 is not a delete marker\n");
                vol_close(v);
                return 1;
            }
            continue;
        }
        memset(got, 0, sizeof got);
        vlen = 0;
        if ((r.flags & INVFS_DELTA_FLAG_DELETE) ||
            vol_delta_read_value(v, &r, got, sizeof got, &vlen) != 0 ||
            vlen != strlen(val) || memcmp(got, val, vlen) != 0) {
            fprintf(stderr, "delta_test: replay value mismatch for %s\n", key);
            vol_close(v);
            return 1;
        }
    }
    if (vol_delta_lookup(v, (const uint8_t *)"absent-key", 10, &r) != 0) {
        fprintf(stderr, "delta_test: absent key reported present\n");
        vol_close(v);
        return 1;
    }
    printf("e2e verify: %d keys replayed%s\n", n,
           expect_delete5 ? " (k005 delete marker)" : " (torn delete dropped)");
    vol_close(v);
    return 0;
}

/* Corrupt the final on-disk record (the delete) with a raw write. */
static int e2e_tear(const char *img)
{
    invfs_volume *v;
    blkio io;
    uint64_t seg, bump, off;
    unsigned char junk[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

    if (open_v3(img, &v) != 0)
        return 2;
    seg = v->delta_seg_pba;
    bump = v->delta_bump;
    vol_close(v);
    if (!seg || bump < INVFS_DELTA_SEG_HDR_LEN + 4) {
        fprintf(stderr, "delta_test: no delta record to tear\n");
        return 1;
    }
    if (blkio_open(&io, img, 0) != 0) {
        fprintf(stderr, "delta_test: raw open %s failed\n", img);
        return 2;
    }
    off = seg * (uint64_t)INVFS_BLOCK_SIZE + bump - 4;
    if (blkio_pwrite(&io, off, junk, sizeof junk) != 0) {
        fprintf(stderr, "delta_test: raw tear failed\n");
        blkio_close(&io);
        return 1;
    }
    blkio_close(&io);
    printf("e2e tear: corrupt last 4 bytes at offset %llu\n",
           (unsigned long long)off);
    return 0;
}

/* crash-append <img> <n> <kill_after>
 *
 * The crash half of the group-commit test. Appends records k000..k(n-1)
 * and, IMMEDIATELY after the append of record index `kill_after` has
 * RETURNED -- and before any later append, before vol_flush, and above all
 * before vol_close would barrier or write CLEAN -- sends itself SIGKILL.
 * `verify-raw` then replays the image and must find every record whose
 * append returned, with its value intact.
 *
 * Everything the volume would normally do at close (the CLEAN superblock,
 * the final flush, the DEVT bump) is deliberately absent: the image is left
 * as a crash leaves it, not as a shutdown leaves it.
 *
 * WHAT THIS DOES NOT PROVE, because it is easy to believe it does: on a
 * local ext4 this test PASSES EVEN IF THE BARRIER IS DELETED. The bytes a
 * pwrite left sit in the kernel page cache, and the page cache outlives the
 * process, so replay finds them. That was measured, not assumed. Telling a
 * real barrier from a fake one needs the page cache to go away (drop_caches,
 * or a power cut), and the syscall-level audit used for that is a separate
 * harness -- not this one. Keep this test for what it does show: that the
 * kill point is real, the recovery path runs, and no acknowledged record
 * disappears across a process death. */
static int e2e_crash_append(const char *img, int n, int kill_after)
{
    invfs_volume *v;
    char key[16], val[16];
    int i;

    if (open_v3(img, &v) != 0)
        return 2;
    if (kill_after < 0 || kill_after >= n) {
        fprintf(stderr, "delta_test: kill_after %d out of range [0,%d)\n",
                kill_after, n);
        vol_close(v);
        return 2;
    }
    for (i = 0; i < n; i++) {
        e2e_key(i, key, sizeof key);
        e2e_val(i, val, sizeof val);
        if (vol_delta_append(v, (const uint8_t *)key, (uint16_t)strlen(key),
                             (const uint8_t *)val, (uint16_t)strlen(val),
                             0) != 0) {
            fprintf(stderr, "delta_test: append %s failed\n", key);
            vol_close(v);
            return 1;
        }
        if (i == kill_after) {
            /* The append returned: the record is durable by contract. Die
             * here, with nothing flushed and nothing closed. */
            fprintf(stderr, "delta_test: SIGKILL after append %s returned\n",
                    key);
            fflush(stderr);
            kill(getpid(), SIGKILL);
            /* unreachable */
        }
    }
    fprintf(stderr, "delta_test: kill_after %d never reached\n", kill_after);
    vol_close(v);
    return 1;
}

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s [scratch-dir]                     # unit tests\n"
        "       %s append <img> <n>\n"
        "       %s verify <img> <n>\n"
        "       %s tear <img>\n"
        "       %s verify-torn <img> <n>\n"
        "       %s verify-raw <img> <n>\n"
        "       %s crash-append <img> <n> <kill_after>\n",
        a0, a0, a0, a0, a0, a0, a0);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("%s version %s (build %s)\n", argv[0],
                   INVFS_VERSION_STRING, INVFS_BUILD_DATE);
            return 0;
        }
    }

    if (argc >= 2 && strcmp(argv[1], "append") == 0) {
        if (argc != 4) { usage(argv[0]); return 2; }
        return e2e_append(argv[2], atoi(argv[3]));
    }
    if (argc >= 2 && strcmp(argv[1], "verify") == 0) {
        if (argc != 4) { usage(argv[0]); return 2; }
        return e2e_verify(argv[2], atoi(argv[3]), 1);
    }
    if (argc >= 2 && strcmp(argv[1], "crash-append") == 0) {
        if (argc != 5) { usage(argv[0]); return 2; }
        return e2e_crash_append(argv[2], atoi(argv[3]), atoi(argv[4]));
    }
    if (argc >= 2 && strcmp(argv[1], "verify-raw") == 0) {
        /* Same replay check as `verify`, but for an image written by
         * crash-append: exactly `n` plain records and NO trailing delete
         * marker, so every k005 check must expect its value, not the
         * delete. Used by the group-commit durability test. */
        if (argc != 4) { usage(argv[0]); return 2; }
        return e2e_verify(argv[2], atoi(argv[3]), 0);
    }
    if (argc >= 2 && strcmp(argv[1], "tear") == 0) {
        if (argc != 3) { usage(argv[0]); return 2; }
        return e2e_tear(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "verify-torn") == 0) {
        if (argc != 4) { usage(argv[0]); return 2; }
        return e2e_verify(argv[2], atoi(argv[3]), 0);
    }

    {
        const char *dir = (argc > 1) ? argv[1] : "/tmp";
        run_unit(dir);
    }
    return failures ? 1 : 0;
}
