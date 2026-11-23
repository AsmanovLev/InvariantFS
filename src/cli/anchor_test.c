/* anchor_test.c — the ANC0 tail anchor driver for test-meta-anchor.sh.
 *
 * RT30 (block 0, 0x9D0) and SPT0 (0xA00) are the only copies of the two
 * descriptors that decide whether a metadata store volume can be opened at all.
 * RT30's "double slot" is two POINTERS, not two copies, so it survives a
 * stale root and nothing else; parity over block 0's contents dies with block
 * 0. The answer is a redundant LOCATION, and the only location that works is
 * one computable from the device size alone -- total_blocks - 1 -- because a
 * recovery path that has to read block 0 to find its backup is not a recovery
 * path.
 *
 * This driver builds the volume through the public write path, probes the
 * anchor, and does the damage/refusal surgery the shell cannot do portably.
 * Every command prints KEY=VALUE lines the suite asserts on; nothing here
 * decides whether the feature works, the suite does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "volume_internal.h"
#include "vol_metabuf.h"
#include "vol_anchor.h"
#include "blkio.h"

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 1;
}

/* ---- deterministic content (shared by build / readback) --------------- */

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static size_t content_len(int i)
{
    return (size_t)(701 + (size_t)((i * 2654435761u) % 3600u));
}

static void content_fill(int i, uint8_t *buf, size_t len)
{
    static const char *w[] = { "alpha", "bravo", "charlie", "delta", "echo",
                               "foxtrot", "golf", "hotel", "india", "juliet" };
    uint32_t s = 0x9E3779B9u ^ (uint32_t)(i * 2654435761u);
    size_t o = 0;
    int rep = 0;

    if (!s)
        s = 1;
    while (o < len) {
        const char *t = w[xs32(&s) % 10];
        size_t tl = strlen(t);
        if (o) {
            if (o + 1 > len) break;
            buf[o++] = ' ';
        }
        if (o + tl > len) {
            size_t room = len - o;
            memcpy(buf + o, t, room);
            o = len;
            break;
        }
        memcpy(buf + o, t, tl);
        o += tl;
        if ((++rep & 7) == 0 && o + 1 <= len) {
            buf[o++] = '\n';
            rep = 0;
        }
    }
    while (o < len)
        buf[o++] = '.';
}

static void fname(int i, char *buf, size_t n)
{
    snprintf(buf, n, "wp_anchor_file_%04d.txt", i);
}

/* ---- raw block-0 / tail-block surgery ---------------------------------- */

static int raw_open(blkio *io, const char *img)
{
    return blkio_open(io, img, 0) == 0 ? 0 : -1;
}

/* Read n bytes at an absolute offset of a single-file image, through the
 * volume's own block device handle so the same mapping rules apply. */
static int raw_read_at(blkio *io, uint64_t off, void *buf, size_t len)
{
    if (blkio_pread(io, off, buf, len) != 0)
        return -1;
    return 0;
}

static int img_size(const char *img, uint64_t *bytes)
{
    FILE *f = fopen(img, "rb");
    long n;
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    fclose(f);
    if (n <= 0)
        return -1;
    *bytes = (uint64_t)n;
    return 0;
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_build(const char *img, int nfiles)
{
    invfs_volume *v;
    int err, i;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i);
        uint8_t *buf = (uint8_t *)malloc(len);
        uint64_t id;
        if (!buf) { vol_close(v); return fail("out of memory"); }
        content_fill(i, buf, len);
        fname(i, name, sizeof name);
        id = vol_replace_file(v, name, buf, len);
        free(buf);
        if (id == 0) { vol_close(v); return fail("vol_replace_file(%s) failed", name); }
    }
    /* Fold so the base tree holds the keys and RT30 names a real root --
     * otherwise the recovery legs would be recovering an EMPTY base, which
     * they would pass whether or not the anchor works. */
    if (vol_fold(v) != 0) { vol_close(v); return fail("vol_fold failed"); }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }
    printf("FILES=%d ROOT_SLOT0=%llu ROOT_SLOT1=%llu SEQ=%llu\n", nfiles,
           (unsigned long long)v->rt.root_slot[0],
           (unsigned long long)v->rt.root_slot[1],
           (unsigned long long)v->rt.seq);
    /* A base that is still empty makes every damage leg vacuous. */
    if (v->rt.root_slot[0] == 0 && v->rt.root_slot[1] == 0) {
        vol_close(v);
        return fail("leg 0: the base tree is still empty after the build -- "
                    "the damage legs would recover nothing and pass anyway");
    }
    vol_close(v);
    return 0;
}

static int cmd_readback_gen(const char *img, int nfiles, int gen)
{
    invfs_volume *v;
    int err, i, bad = 0;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i) + (size_t)gen, got = 0;
        uint8_t *want = (uint8_t *)malloc(len);
        uint8_t *got_buf = NULL;
        fname(i, name, sizeof name);
        if (!want) { vol_close(v); return fail("out of memory"); }
        content_fill(i, want, len);
        /* Generation 1 differs from generation 0 ONLY in the last byte, so a
         * file is rewritten with identical length -- the checker can then
         * serve both without a size-dependent branch. */
        if (gen)
            want[len - 1] = (uint8_t)('a' + (i % 26));
        if (vol_read_named(v, name, &got_buf, &got) != 0) {
            fprintf(stderr, "  MISSING %s\n", name);
            bad++;
        } else if (got != len || memcmp(want, got_buf, len) != 0) {
            fprintf(stderr, "  MISMATCH %s (%zu bytes, expected %zu)\n",
                    name, got, len);
            bad++;
        }
        free(got_buf);
        free(want);
    }
    printf("READBACK=%d/%d GEN=%d\n", nfiles - bad, nfiles, gen);
    vol_close(v);
    return bad ? fail("%d file(s) did not read back byte-identical", bad) : 0;
}

static int cmd_readback(const char *img, int nfiles)
{
    return cmd_readback_gen(img, nfiles, 0);
}

/* Write a cycle, unlink a cycle (the real free path), and check the tail
 * block's allocation bit after EVERY step. The allocator can only hand the
 * anchor block out if its bit goes clear, so a bit that never clears is the
 * proof the reservation holds. */
static int cmd_reserve(const char *img, int nfiles)
{
    invfs_volume *v;
    uint64_t apba;
    int err, i;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    apba = anchor_pba(v);
    if (!apba) { vol_close(v); return fail("no anchor pba for this volume"); }
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i);
        uint8_t *buf = (uint8_t *)malloc(len);
        if (!buf) { vol_close(v); return fail("out of memory"); }
        content_fill(i, buf, len);
        fname(i, name, sizeof name);
        if (vol_replace_file(v, name, buf, len) == 0) {
            free(buf);
            vol_close(v);
            return fail("vol_replace_file(%s) failed", name);
        }
        free(buf);
        if (!bit_get(v->bitmap, apba)) {
            vol_close(v);
            return fail("the anchor block %llu went FREE during write %d",
                        (unsigned long long)apba, i);
        }
    }
    if (vol_fold(v) != 0) { vol_close(v); return fail("vol_fold failed"); }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }
    for (i = 0; i < nfiles; i++) {
        char name[64];
        fname(i, name, sizeof name);
        if (vol_unlink(v, name) != 0) {
            /* an unlink can legitimately refuse on a pinned window; the bit
             * is what this leg is about, not the unlink */
        }
        if (!bit_get(v->bitmap, apba)) {
            vol_close(v);
            return fail("the anchor block %llu went FREE during unlink %d",
                        (unsigned long long)apba, i);
        }
    }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }
    printf("RESERVE=ok ANCHOR_PBA=%llu CHECKS=%d\n",
           (unsigned long long)apba, nfiles * 2 + 2);
    vol_close(v);
    return 0;
}

/* A second write cycle on an existing volume, used by the backward-compat leg
 * to prove a full store cycle never touches a tail block it did not write. */
static int cmd_writecycle(const char *img, int nfiles)
{
    invfs_volume *v;
    int err, i;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i) + 1;  /* gen 1 */
        uint8_t *buf = (uint8_t *)malloc(len);
        if (!buf) { vol_close(v); return fail("out of memory"); }
        content_fill(i, buf, len);
        buf[len - 1] = (uint8_t)('a' + (i % 26));
        fname(i, name, sizeof name);
        if (vol_replace_file(v, name, buf, len) == 0) {
            free(buf);
            vol_close(v);
            return fail("vol_replace_file(%s) failed", name);
        }
        free(buf);
    }
    if (vol_fold(v) != 0) { vol_close(v); return fail("vol_fold failed"); }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }
    printf("WRITECYCLE=%d\n", nfiles);
    vol_close(v);
    return 0;
}

static const char *state_name(int st)
{
    switch (st) {
    case INVFS_ANCHOR_OK:                return "ok";
    case INVFS_ANCHOR_ABSENT:            return "absent";
    case INVFS_ANCHOR_REFUSED_GEOMETRY:  return "refused-geometry";
    case INVFS_ANCHOR_REFUSED_DAMAGE:    return "refused-damage";
    default:                             return "io-error";
    }
}

static int cmd_probe(const char *img)
{
    invfs_volume *v;
    uint64_t bytes, tb = 0, apba = 0;
    invfs_anc0 a;
    blkio io;
    int err, st, fp = 0, have = 0, adopted = 0;

    if (img_size(img, &bytes) != 0)
        return fail("cannot size %s", img);
    tb = bytes / INVFS_BLOCK_SIZE;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    apba = anchor_pba(v);
    adopted = anchor_adopted(v);
    /* ANCHOR_STATE is the VOLUME's verdict, not a second opinion computed
     * here: anchor_state_of() only sees magic/version/crc, so a foreign
     * anchor that is internally perfect would read "ok" if the suite asked
     * it directly. The refusal lives in anchor_probe(), i.e. in vol_open. */
    st = v->anchor_state;
    vol_close(v);
    if (!apba) {
        printf("ANCHOR_STATE=absent ANCHOR_PBA=0 TAIL_BIT=0 TAIL_FREE=1 "
               "FP_MATCH=0 MIRROR_SEQ=0 ADOPTED=%d\n", adopted);
        return 0;
    }
    memset(&a, 0, sizeof a);
    if (raw_open(&io, img) == 0) {
        have = raw_read_at(&io, apba * (uint64_t)INVFS_BLOCK_SIZE, &a, sizeof a) == 0;
        blkio_close(&io);
    }
    if (!have) st = INVFS_ANCHOR_ABSENT;
    if (have) {
        /* against the IMAGE's own superblock, which is what the open path
         * uses -- a placeholder here would make the check meaningless */
        invfs_superblock sb;
        if (raw_open(&io, img) == 0) {
            if (raw_read_at(&io, 0, &sb, sizeof sb) == 0)
                fp = anchor_fp_matches(&a, sb.total_blocks, sb.block_size,
                                       sb.format_version,
                                       (const uint8_t *)sb.uuid) ? 1 : 0;
            blkio_close(&io);
        }
    }

    /* The bitmap, read the way the allocator reads it. */
    {
        int bit = 0, isfree = 1;
        uint64_t bmpba = 1, bmpbytes, b;
        uint8_t *bm = NULL;
        blkio io2;
        if (raw_open(&io2, img) == 0) {
            bmpbytes = ((tb / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE) *
                       INVFS_BLOCK_SIZE;
            bm = (uint8_t *)calloc(1, (size_t)bmpbytes);
            if (bm && raw_read_at(&io2, bmpba * (uint64_t)INVFS_BLOCK_SIZE,
                                  bm, (size_t)bmpbytes) == 0) {
                b = apba / 8;
                if (b < bmpbytes) {
                    bit = (bm[b] >> (apba % 8)) & 1;
                    isfree = !bit;
                }
            }
            free(bm);
            blkio_close(&io2);
        }
        printf("ANCHOR_STATE=%s ANCHOR_PBA=%llu TAIL_BIT=%d TAIL_FREE=%d "
               "FP_MATCH=%d MIRROR_SEQ=%llu ADOPTED=%d\n",
               state_name(st), (unsigned long long)apba, bit, isfree, fp,
               (unsigned long long)a.rt.seq, adopted);
    }
    return 0;
}

static int cmd_corrupt_rt30(const char *img, const char *what)
{
    blkio io;
    invfs_rt rt;

    if (raw_open(&io, img) != 0)
        return fail("cannot open %s", img);
    if (raw_read_at(&io, INVFS_RT_OFF, &rt, sizeof rt) != 0) {
        blkio_close(&io);
        return fail("cannot read RT30");
    }
    if (!strcmp(what, "magic"))
        rt.magic[0] = 'X';
    else if (!strcmp(what, "crc"))
        rt.crc32c ^= 0xFFFFFFFFu;
    else if (!strcmp(what, "seq")) {
        rt.seq = 0xDEADBEEF;
        rt.crc32c = 0;
        rt.crc32c = invfs_crc32c(&rt, offsetof(invfs_rt, crc32c));
    } else {
        blkio_close(&io);
        return fail("corrupt-rt30: bad selector '%s'", what);
    }
    if (blkio_pwrite(&io, INVFS_RT_OFF, &rt, sizeof rt) != 0) {
        blkio_close(&io);
        return fail("RT30 rewrite failed");
    }
    blkio_flush(&io);
    blkio_close(&io);
    printf("CORRUPTED=rt:%s\n", what);
    return 0;
}

/* Copy src's anchor block onto dst's anchor block, addressed the way the
 * VOLUME addresses them: (total_blocks - 1) from each image's own superblock.
 * See cmd_taildump for why "the last 4096 bytes of the file" is not that. */
static int cmd_teleport_anchor(const char *src, const char *dst)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    invfs_superblock ssb, dsb;
    uint64_t soff, doff;
    blkio a, b;

    if (raw_open(&a, src) != 0 || raw_open(&b, dst) != 0) {
        blkio_close(&a);
        blkio_close(&b);
        return fail("cannot open the images");
    }
    if (raw_read_at(&a, 0, &ssb, sizeof ssb) != 0 ||
        raw_read_at(&b, 0, &dsb, sizeof dsb) != 0) {
        blkio_close(&a);
        blkio_close(&b);
        return fail("cannot read the superblocks");
    }
    soff = (ssb.total_blocks - 1) * INVFS_BLOCK_SIZE;
    doff = (dsb.total_blocks - 1) * INVFS_BLOCK_SIZE;
    if (blkio_pread(&a, soff, blk, sizeof blk) != 0 ||
        blkio_pwrite(&b, doff, blk, sizeof blk) != 0) {
        blkio_close(&a);
        blkio_close(&b);
        return fail("anchor copy failed");
    }
    blkio_flush(&b);
    blkio_close(&a);
    blkio_close(&b);
    printf("TELEPORTED=1 SRC_OFF=%llu DST_OFF=%llu\n",
           (unsigned long long)soff, (unsigned long long)doff);
    return 0;
}

/* Dump the anchor block to a file, at the offset the VOLUME uses.
 *
 * Not "the last 4096 bytes of the file": invf-mkfs derives total_blocks by
 * flooring size/4096, so an image whose size is not a whole number of blocks
 * (0.3 GB is not) ends in a PARTIAL block. The anchor sits at
 * (total_blocks-1)*4096, which is inside that partial block, and the file
 * stops before the block's end -- so filesize-4096 is a different block
 * entirely. Read it the way the volume reads it or the backward-compatibility
 * byte-identity check compares two unrelated blocks and proves nothing. */
static int cmd_taildump(const char *img, const char *out)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    invfs_superblock sb;
    uint64_t pba;
    blkio io;
    FILE *f;

    if (raw_open(&io, img) != 0) return fail("cannot open %s", img);
    if (raw_read_at(&io, 0, &sb, sizeof sb) != 0) {
        blkio_close(&io);
        return fail("cannot read the superblock");
    }
    pba = sb.total_blocks - 1;
    memset(blk, 0, sizeof blk);
    if (raw_read_at(&io, pba * INVFS_BLOCK_SIZE, blk, sizeof blk) != 0) {
        blkio_close(&io);
        return fail("cannot read the tail block");
    }
    blkio_close(&io);
    f = fopen(out, "wb");
    if (!f) return fail("cannot create %s", out);
    fwrite(blk, 1, sizeof blk, f);
    fclose(f);
    printf("TAIL_PBA=%llu DUMPED=%s\n", (unsigned long long)pba, out);
    return 0;
}

/* The same check, on a second handle: copying an anchor onto an image whose
 * fingerprint already matches it would make the refusal leg VACUOUS -- the
 * probe would adopt it, correctly, and the test would read as a failure of
 * the refusal. invf-mkfs's UUID is test-grade (time + rand, and rand() is
 * unseeded so it repeats per process), so two images made in the same second
 * at the same size really can collide; say so rather than fail obscurely. */
static int cmd_teleport_check(const char *src, const char *dst)
{
    invfs_superblock ssb, dsb;
    blkio a, b;
    int same;

    if (raw_open(&a, src) != 0 || raw_open(&b, dst) != 0) {
        blkio_close(&a);
        blkio_close(&b);
        return fail("cannot open the images");
    }
    if (raw_read_at(&a, 0, &ssb, sizeof ssb) != 0 ||
        raw_read_at(&b, 0, &dsb, sizeof dsb) != 0) {
        blkio_close(&a);
        blkio_close(&b);
        return fail("cannot read the superblocks");
    }
    blkio_close(&a);
    blkio_close(&b);
    same = (ssb.total_blocks == dsb.total_blocks) &&
           (ssb.format_version == dsb.format_version) &&
           (memcmp(ssb.uuid, dsb.uuid, 16) == 0);
    printf("SRC_BLOCKS=%llu DST_BLOCKS=%llu SAME_UUID=%d\n",
           (unsigned long long)ssb.total_blocks,
           (unsigned long long)dsb.total_blocks, same);
    return same ? fail("the two images have IDENTICAL fingerprints, so the "
                       "refusal leg would be vacuous -- make the foreign "
                       "image a different size") : 0;
}

/* Patch ONE arm of the anchor's geometry fingerprint, so each arm is shown to
 * be load-bearing on its own rather than as part of a bundle. The crc32c is
 * recomputed so the ONLY thing wrong with the descriptor is the arm under
 * test -- otherwise this would just re-run the "damaged anchor" leg. */
/* Byte offset of the anchor block, from the image's own superblock.
 *
 * NOT filesize-4096. invf-mkfs derives total_blocks by flooring size/4096,
 * so an image whose size is not a whole number of blocks (0.3 GB is not)
 * ends in a PARTIAL block: the file holds 322,122,547 bytes, 78643 blocks
 * is 322,121,728, and block 78642 runs to 322,125,824 -- past EOF. The
 * anchor lives at (total_blocks-1)*4096, inside that partial block, so the
 * last 4096 bytes of the FILE are a different block. Every tool here that
 * touches the anchor must use this offset or it is editing a stranger. */
static int tail_off(blkio *io, uint64_t *off)
{
    invfs_superblock sb;
    if (raw_read_at(io, 0, &sb, sizeof sb) != 0)
        return -1;
    if (sb.total_blocks < 1)
        return -1;
    *off = (sb.total_blocks - 1) * INVFS_BLOCK_SIZE;
    return 0;
}

static int cmd_patch_fingerprint(const char *img, const char *arm)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    uint64_t bytes, toff = 0;
    invfs_anc0 a;
    blkio io;

    if (img_size(img, &bytes) != 0)
        return fail("cannot size %s", img);
    if (raw_open(&io, img) != 0)
        return fail("cannot open %s", img);
    if (tail_off(&io, &toff) != 0 ||
        blkio_pread(&io, toff, blk, sizeof blk) != 0) {
        blkio_close(&io);
        return fail("cannot read the tail block");
    }
    memcpy(&a, blk, sizeof a);
    a.crc32c = 0;
    if (!strcmp(arm, "total_blocks"))
        a.total_blocks += 1;
    else if (!strcmp(arm, "block_size"))
        a.block_size = 8192;
    else if (!strcmp(arm, "format_version"))
        a.format_version = 77;
    else if (!strcmp(arm, "uuid"))
        a.vol_uuid[0] ^= 0xFF;
    else {
        blkio_close(&io);
        return fail("patch-fingerprint: bad arm '%s'", arm);
    }
    a.crc32c = anchor_crc(&a);
    memcpy(blk, &a, sizeof a);
    if (tail_off(&io, &toff) != 0 ||
        blkio_pwrite(&io, toff, blk, sizeof blk) != 0) {
        blkio_close(&io);
        return fail("anchor rewrite failed");
    }
    blkio_flush(&io);
    blkio_close(&io);
    printf("PATCHED=%s\n", arm);
    return 0;
}

static int cmd_corrupt_anchor_crc(const char *img)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    uint64_t bytes, toff = 0;
    invfs_anc0 a;
    blkio io;

    if (img_size(img, &bytes) != 0)
        return fail("cannot size %s", img);
    if (raw_open(&io, img) != 0)
        return fail("cannot open %s", img);
    if (tail_off(&io, &toff) != 0 ||
        blkio_pread(&io, toff, blk, sizeof blk) != 0) {
        blkio_close(&io);
        return fail("cannot read the tail block");
    }
    memcpy(&a, blk, sizeof a);
    a.crc32c ^= 0xFFFFFFFFu;
    memcpy(blk, &a, sizeof a);
    if (tail_off(&io, &toff) != 0 ||
        blkio_pwrite(&io, toff, blk, sizeof blk) != 0) {
        blkio_close(&io);
        return fail("anchor rewrite failed");
    }
    blkio_flush(&io);
    blkio_close(&io);
    printf("CORRUPTED=anchor-crc\n");
    return 0;
}

/* Put a recognisable pattern in the tail block WITHOUT going through the
 * volume: this is the backward-compat leg's red control, the shape a
 * pre-anchor volume's last block actually has (an ordinary data block nobody
 * reserved). */
static int cmd_fill_tail(const char *img, const char *pattern)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    uint64_t bytes, toff = 0;
    size_t n = strlen(pattern);
    blkio io;

    if (img_size(img, &bytes) != 0)
        return fail("cannot size %s", img);
    memset(blk, 0, sizeof blk);
    memcpy(blk, pattern, n);
    if (raw_open(&io, img) != 0)
        return fail("cannot open %s", img);
    if (tail_off(&io, &toff) != 0 ||
        blkio_pwrite(&io, toff, blk, sizeof blk) != 0) {
        blkio_close(&io);
        return fail("tail write failed");
    }
    blkio_flush(&io);
    blkio_close(&io);
    printf("TAIL_FILLED=%s\n", pattern);
    return 0;
}

/* ---- WP-ANCHOR-EXTEND: the metadata-extent grow path ------------------
 *
 * extend_meta_extent() (volume.c:2639) grows a metadata extent in place when
 * the adjacent run "is free", and it used to decide that from the clear bits
 * in v->bitmap alone. A CLEAR BIT IS NOT PROOF OF FREEDOM: the ANC0 tail
 * anchor sits inside the shadow zone's range, and its bit is legitimately
 * clear on a volume whose bitmap was rebuilt from replay (no delta record
 * names the block -- see leg 10 of test-meta-anchor.sh). This command builds
 * that geometry in RAM -- one mapper entry whose extent ends next to the
 * anchor, its blocks marked the way alloc_meta_extent marks them -- calls the
 * real extend_meta_extent(), and then performs the metadata record write the
 * merge path would perform if the extent had grown. It prints KEY=VALUE; the
 * suite decides, the driver only reports.
 *
 * Three modes, all with the same class bump (1 -> 2, 32 -> 64 blocks):
 *
 *   edge     the adjacent run STARTS at the anchor  -> the guard's 1st test
 *   runlast  the adjacent run ENDS at the anchor    -> the guard's 2nd test
 *   shifted  the SAME CALL one block further out: the run is [apba-32, apba),
 *            free and unprotected, so it MUST succeed
 *
 * `shifted` is what keeps the two refusing modes honest: a function that
 * refused every extension would pass `edge` and `runlast` while breaking
 * every metadata write on the volume.
 *
 * The setup is RAM-only -- no meta_mapper_flush, no MET0 persist, no
 * vol_flush, and every injected bit is restored before close -- so the image
 * is untouched by the leg, EXCEPT in the one place it must not be: the anchor
 * block, when the call wrongly succeeds and the record write lands on it.
 */
#define XM_OLD_CLASS 1      /* 128 KiB = 32 blocks */
#define XM_NEW_CLASS 2      /* 256 KiB = 64 blocks, so adj = 32 blocks */

static void xm_bit_clear(uint8_t *bm, uint64_t i)
{
    bm[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static int cmd_extend_meta(const char *img, const char *mode)
{
    invfs_volume *v;
    uint64_t apba, old_blocks, new_blocks, adj_blocks, ext_pba, run_pba;
    uint64_t idx, saved_entry, saved_n, saved_free, saved_meta_free;
    uint64_t k, claim_pba = 0, claim_blocks = 0;
    uint8_t before[INVFS_BLOCK_SIZE], after[INVFS_BLOCK_SIZE];
    int err, edge, runlast, shifted, ok;
    int anchor_bit_orig, anchor_bit_after, anchor_meta_after, class_after;
    int rc = 0, claimed_anchor = 0, rec_wrote = 0, bytes_changed = 0;
    int run_bits_after = 0;

    edge    = !strcmp(mode, "edge");
    runlast = !strcmp(mode, "runlast");
    shifted = !strcmp(mode, "shifted");
    if (!edge && !runlast && !shifted)
        return fail("extend-meta: unknown mode '%s' "
                    "(edge|runlast|shifted)", mode);

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    apba = anchor_pba(v);
    old_blocks = (65536ULL << XM_OLD_CLASS) / INVFS_BLOCK_SIZE;   /* 32 */
    new_blocks = (65536ULL << XM_NEW_CLASS) / INVFS_BLOCK_SIZE;   /* 64 */
    adj_blocks = new_blocks - old_blocks;                         /* 32 */
    /* The geometry must fit below the anchor, or there is nothing to place. */
    if (!apba || apba < new_blocks + adj_blocks) {
        vol_close(v);
        return fail("no anchor pba with room below it for this volume "
                    "(anchor_pba=%llu)", (unsigned long long)apba);
    }

    /* Place the extent so the adjacent run sits where this mode needs it. */
    if (edge)
        ext_pba = apba - old_blocks;                        /* run starts AT the anchor */
    else if (runlast)
        ext_pba = apba - old_blocks - (adj_blocks - 1);     /* run ends AT the anchor */
    else
        ext_pba = apba - new_blocks;                        /* run ends 1 block below it */
    run_pba = ext_pba + old_blocks;

    /* A leg that refuses for the ORDINARY reason ("the run is allocated")
     * proves nothing about the guard, so check the geometry first and say so
     * loudly rather than report a pass. */
    ok = 1;
    for (k = 0; k < old_blocks; k++)
        if (bit_get(v->bitmap, ext_pba + k)) ok = 0;
    if (!v->meta_mapper || v->meta_mapper_n >= INVFS_META_EXT_ENTRIES) ok = 0;
    if (!ok) {
        vol_close(v);
        return fail("extend-meta %s: the %llu blocks below the anchor "
                    "(from %llu) are not free on this image, so the leg "
                    "would be vacuous", mode,
                    (unsigned long long)old_blocks,
                    (unsigned long long)ext_pba);
    }
    /* Every block the extension would claim counts as free BEFORE the call,
     * apart from the anchor bit -- which is the whole point, and which is
     * cleared below to reproduce a rebuilt bitmap. */
    for (k = 0; k < adj_blocks; k++) {
        if (run_pba + k != apba && bit_get(v->bitmap, run_pba + k)) ok = 0;
    }
    if (!ok) {
        vol_close(v);
        return fail("extend-meta %s: a block in the adjacent run "
                    "[%llu, %llu) is already allocated, so the leg would "
                    "be vacuous", mode,
                    (unsigned long long)run_pba,
                    (unsigned long long)(run_pba + adj_blocks));
    }

    idx = v->meta_mapper_n;
    saved_entry = v->meta_mapper[idx];
    saved_n = v->meta_mapper_n;
    saved_free = v->free_blocks;
    saved_meta_free = v->meta_free_blocks;
    anchor_bit_orig = bit_get(v->bitmap, apba);

    /* THE WORLD A BITMAP REBUILD LEAVES: the anchor's bit is clear, the rest
     * of the volume exactly as it is. This is leg 10's finding, and it is the
     * only world in which the bug is reachable. */
    xm_bit_clear(v->bitmap, apba);

    /* alloc_meta_extent's own marking: the extent's blocks are allocated and
     * typed META. */
    for (k = 0; k < old_blocks; k++) {
        bit_set(v->bitmap, ext_pba + k);
        if (v->meta_type_bitmap)
            bit_set(v->meta_type_bitmap, ext_pba + k);
    }
    v->meta_mapper[idx] = invfs_meta_ext_encode(ext_pba, XM_OLD_CLASS);
    v->meta_mapper_n = idx + 1;

    if (io_pread(&v->io, apba * (uint64_t)INVFS_BLOCK_SIZE, before,
                 sizeof before) != 0) {
        vol_close(v);
        return fail("cannot read the anchor block");
    }

    /* THE CALL UNDER TEST -- the same one vol_meta_merge.c:170 makes. */
    rc = extend_meta_extent(v, idx, XM_NEW_CLASS);

    if (v->meta_mapper) {
        uint64_t e = v->meta_mapper[idx];
        claim_pba = invfs_meta_ext_pba(e);
        class_after = invfs_meta_ext_class(e);
        claim_blocks = invfs_meta_ext_size(e) / INVFS_BLOCK_SIZE;
        claimed_anchor = (apba >= claim_pba &&
                          apba < claim_pba + claim_blocks) ? 1 : 0;
    } else {
        class_after = -1;
    }
    anchor_bit_after = bit_get(v->bitmap, apba);
    anchor_meta_after = v->meta_type_bitmap
                        ? bit_get(v->meta_type_bitmap, apba) : -1;
    for (k = 0; k < adj_blocks; k++)
        if (bit_get(v->bitmap, run_pba + k)) run_bits_after++;

    /* THE RECORD WRITE. The merge path's next step after a GRANTED extension
     * is to append at the extent's old end (pba + offset, and the offset was
     * already at the old size). If -- and only if -- the mapper now claims
     * blocks past that point, the write lands in the newly claimed region,
     * and in `edge` mode its FIRST block is the anchor. So the write is not
     * staged: no claim, no write. */
    if (claim_blocks > old_blocks) {
        uint8_t rec[INVFS_BLOCK_SIZE];
        memset(rec, 0x5A, sizeof rec);
        memcpy(rec, "INVFS-META-RECORD-OVERWRITE", 28);
        if (io_pwrite(&v->io, (claim_pba + old_blocks) * (uint64_t)INVFS_BLOCK_SIZE,
                      rec, sizeof rec) == 0)
            rec_wrote = 1;
        blkio_force_flush(&v->io);
    }

    if (io_pread(&v->io, apba * (uint64_t)INVFS_BLOCK_SIZE, after,
                 sizeof after) != 0) {
        vol_close(v);
        return fail("cannot re-read the anchor block");
    }
    bytes_changed = memcmp(before, after, sizeof before) != 0;

    printf("MODE=%s ANCHOR_PBA=%llu EXT_PBA=%llu RUN_PBA=%llu RC=%d "
           "SPAN_FREE=1 RUN_FREE=1 ANCHOR_BIT_ORIGINAL=%d "
           "ANCHOR_BIT_AFTER=%d ANCHOR_METATYPE_AFTER=%d "
           "MAPPER_CLASS_AFTER=%d MAPPER_CLAIM_BLOCKS=%llu "
           "CLAIMED_ANCHOR=%d RUN_BITS_SET_AFTER=%d RECORD_WRITE=%d "
           "RECORD_WRITE_PBA=%llu ANCHOR_BYTES_CHANGED=%d "
           "FREE_BLOCKS_DELTA=%lld\n",
           mode, (unsigned long long)apba, (unsigned long long)ext_pba,
           (unsigned long long)run_pba, rc, anchor_bit_orig,
           anchor_bit_after, anchor_meta_after, class_after,
           (unsigned long long)claim_blocks, claimed_anchor, run_bits_after,
           rec_wrote, (unsigned long long)(rec_wrote ? claim_pba + old_blocks : 0),
           bytes_changed, (long long)(saved_free - v->free_blocks));

    /* Put the RAM back the way the open found it, so closing this handle
     * cannot propagate the synthetic geometry anywhere (nothing here dirties
     * the volume, so vol_close writes nothing -- this is belt and braces).
     * The anchor BLOCK is deliberately not restored: if it was written, the
     * suite has to see it. */
    for (k = 0; k < old_blocks; k++) {
        xm_bit_clear(v->bitmap, ext_pba + k);
        if (v->meta_type_bitmap)
            xm_bit_clear(v->meta_type_bitmap, ext_pba + k);
    }
    for (k = 0; k < adj_blocks; k++) {
        if (run_pba + k == apba)
            continue;
        xm_bit_clear(v->bitmap, run_pba + k);
        if (v->meta_type_bitmap)
            xm_bit_clear(v->meta_type_bitmap, run_pba + k);
    }
    if (anchor_bit_orig)
        bit_set(v->bitmap, apba);
    else
        xm_bit_clear(v->bitmap, apba);
    if (v->meta_type_bitmap)
        xm_bit_clear(v->meta_type_bitmap, apba);
    v->meta_mapper[idx] = saved_entry;
    v->meta_mapper_n = saved_n;
    v->free_blocks = saved_free;
    v->meta_free_blocks = saved_meta_free;

    vol_close(v);
    return 0;
}

/* ---- WP-METABUF-ANCHOR: the metadata base-page allocator ---------------
 *
 * mb_alloc_meta_zone() (vol_metabuf.c:253) hands out a metadata base page
 * from the first block of the metadata zone whose bitmap bit is clear, and its
 * scan is CLAMPED to total_blocks (vol_metabuf.c:262-263). So on any volume
 * whose metadata zone reaches the tail, total_blocks - 1 -- the ANC0 tail
 * anchor -- is a candidate, and its bit is legitimately clear on a volume
 * whose bitmap was rebuilt from replay (no delta record names the block).
 * It was the fourth allocator-shaped path to decide "free" from a clear bit
 * alone (fe214e2 fixed the third). Taking it marks the anchor as a metadata
 * base page, and then mbuf_alloc's own mb_write_empty erases the volume's only
 * second copy of RT30/SPT0.
 *
 * THE BUG IS UNREACHABLE ON AN IMAGE invf-mkfs BUILDS -- mkfs marks the whole
 * metadata zone allocated (vol_metabuf.c:245-248), so the scan finds nothing
 * whatever the geometry. So this command CONSTRUCTS the geometry the way the
 * anchor WP did (cmd_extend_meta above): it opens a real image and injects
 * into RAM only -- no flush, no MET0/superblock persist, every bit and every
 * field restored before close -- then calls the REAL mbuf_alloc().
 *
 * Three modes:
 *
 *   tail    the metadata zone IS the tail: start = anchor_pba, 2 blocks, so
 *           the clamp leaves exactly one candidate -- the anchor. Every other
 *           candidate is excluded by construction rather than by marking
 *           sixty thousand bits, and the CLAMP (the line the WP cites) is the
 *           code path under test. Must refuse.
 *   short   the same zone one block short of the tail: start = anchor_pba - 1,
 *           1 block, so the only candidate is the ordinary free block next to
 *           the anchor. Must SUCCEED and return that block -- otherwise a
 *           guard that refused the whole zone would pass `tail` while
 *           breaking the allocator, and the leg would prove nothing.
 *   ro      `short`'s geometry with VOLF_READONLY set. Must return 0 and move
 *           NO bit: the latch has to run before the scan's first bit_set.
 *
 * Two RAM fields beyond that geometry, both restored before close and both
 * necessary for the leg to be about the thing it claims:
 *
 *   mb_boot_cursor/mb_boot_end = 0   otherwise mbuf_alloc() hands out the two
 *       reserved root-area boot pages first (vol_metabuf.c:359-364) and the
 *       zone scan never runs -- the leg would be vacuous by construction.
 *   sb.shadow_zone_blocks = 0        otherwise a refused zone scan falls
 *       through to the shared pool (vol_metabuf.c:367-375) and SUCCEEDS, so
 *       "the metadata allocator handed out the anchor" could not be told
 *       apart from "the shared pool served the request", and the leg would
 *       write a page into the image on the refusing path as well.
 *
 * The anchor's bitmap bit is cleared first in every mode -- the world a bitmap
 * rebuild leaves, and the only world in which the bug is reachable -- so an
 * "already allocated" refusal cannot make the leg vacuous.
 */
#define AM_GEN 1

static int cmd_alloc_meta(const char *img, const char *mode)
{
    invfs_volume *v;
    uint8_t before[INVFS_BLOCK_SIZE], after[INVFS_BLOCK_SIZE];
    uint8_t *bm_snap = NULL;
    size_t bm_bytes;
    uint64_t apba, total, cand, zone_end, candidates, inj_start, inj_blocks;
    uint64_t saved_meta_start, saved_meta_blocks, saved_shadow_blocks;
    uint64_t saved_cursor, saved_fail_run, saved_boot_cur, saved_boot_end;
    uint64_t saved_free, saved_mfree, saved_shadow_free, saved_shadow_fail;
    uint64_t saved_runs, saved_iters, boot_pages, ret;
    uint32_t saved_flags;
    int err, tail, shortm, rom, rc, ret_anchor, bytes_changed, bits_changed;
    int anchor_bit_before, anchor_bit_after, anchor_meta_after;
    int cand_bit_before, cand_bit_after;

    tail   = !strcmp(mode, "tail");
    shortm = !strcmp(mode, "short");
    rom    = !strcmp(mode, "ro");
    if (!tail && !shortm && !rom)
        return fail("alloc-meta: unknown mode '%s' (tail|short|ro)", mode);

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    apba = anchor_pba(v);
    total = v->sb.total_blocks;
    if (!apba || total < 8 || apba < 4) {
        vol_close(v);
        return fail("no usable anchor pba on this image (total_blocks=%llu)",
                    (unsigned long long)total);
    }
    /* A missing bitmap or meta_type_bitmap would make the "the call moved no
     * bit" half unmeasurable, and a set fail_run would make the scan
     * short-circuit (vol_metabuf.c:279) so the leg would never reach the code
     * under test. Refuse rather than report a pass. */
    if (!v->bitmap || !v->meta_type_bitmap) {
        vol_close(v);
        return fail("alloc-meta %s: the volume has no bitmap/meta_type_bitmap "
                    "(%p/%p), so the assertions below cannot be measured",
                    mode, (void *)v->bitmap, (void *)v->meta_type_bitmap);
    }
    if (v->mb_alloc_fail_run) {
        vol_close(v);
        return fail("alloc-meta %s: mb_alloc_fail_run is already set at open, "
                    "so the zone scan would be skipped and the leg vacuous",
                    mode);
    }

    /* THE GEOMETRY. `tail` reaches the tail and relies on the clamp for the
     * rest of the zone; `short` stops one block short of it. */
    saved_meta_start = v->sb.metadata_zone_start;
    saved_meta_blocks = v->sb.metadata_zone_blocks;
    if (tail) {
        inj_start = apba;                      /* [apba, apba+2) -> clamped */
        inj_blocks = 2;
    } else {
        inj_start = apba - 1;                  /* [apba-1, apba) */
        inj_blocks = 1;
    }
    v->sb.metadata_zone_start = inj_start;
    v->sb.metadata_zone_blocks = inj_blocks;
    zone_end = inj_start + inj_blocks;
    if (zone_end > total)
        zone_end = total;
    candidates = zone_end - inj_start;
    cand = apba - 1;      /* the ordinary free block the short/ro modes target */

    /* `short`/`ro` succeed only because cand is genuinely free, so check that
     * instead of clearing it: a leg that fabricated the free bit would prove
     * that the allocator can read a bit, not that it still allocates. */
    cand_bit_before = bit_get(v->bitmap, cand);
    if (!tail && cand_bit_before) {
        vol_close(v);
        return fail("alloc-meta %s: the block next to the anchor (pba %llu) is "
                    "already allocated on this image, so the scan would refuse "
                    "it for the ordinary reason and the control would be "
                    "vacuous", mode, (unsigned long long)cand);
    }

    /* THE WORLD A BITMAP REBUILD LEAVES. */
    anchor_bit_before = bit_get(v->bitmap, apba);
    xm_bit_clear(v->bitmap, apba);

    /* Save every field this command writes, and take the bitmap snapshot the
     * "did the CALL move a bit" assertion compares against -- AFTER the
     * injection, so the injection itself is not counted. */
    saved_shadow_blocks = v->sb.shadow_zone_blocks;
    saved_cursor = v->mb_alloc_cursor;
    saved_fail_run = v->mb_alloc_fail_run;
    saved_boot_cur = v->mb_boot_cursor;
    saved_boot_end = v->mb_boot_end;
    saved_free = v->free_blocks;
    saved_mfree = v->meta_free_blocks;
    saved_shadow_free = v->shadow_free;
    saved_shadow_fail = v->shadow_fail_run;
    saved_runs = v->mb_zone_scan_runs;
    saved_iters = v->mb_zone_scan_iters;
    saved_flags = v->sb.vol_flags;
    boot_pages = (saved_boot_cur < saved_boot_end)
                 ? saved_boot_end - saved_boot_cur : 0;

    bm_bytes = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    bm_snap = (uint8_t *)malloc(bm_bytes);
    if (!bm_snap) {
        vol_close(v);
        return fail("out of memory snapshotting the bitmap");
    }
    memcpy(bm_snap, v->bitmap, bm_bytes);

    v->mb_boot_cursor = 0;              /* the boot pages are not this test */
    v->mb_boot_end = 0;
    v->sb.shadow_zone_blocks = 0;       /* no fallback pool: the scan or nothing */
    if (rom)
        v->sb.vol_flags |= VOLF_READONLY;

    if (io_pread(&v->io, apba * (uint64_t)INVFS_BLOCK_SIZE, before,
                 sizeof before) != 0) {
        free(bm_snap);
        vol_close(v);
        return fail("cannot read the anchor block");
    }

    /* THE CALL UNDER TEST. */
    ret = mbuf_alloc(v, AM_GEN);
    rc = (int)ret;                                /* pbas fit in an int here */

    ret_anchor = (ret != 0 && ret == apba) ? 1 : 0;
    anchor_bit_after = bit_get(v->bitmap, apba);
    anchor_meta_after = bit_get(v->meta_type_bitmap, apba);
    cand_bit_after = bit_get(v->bitmap, cand);
    bits_changed = memcmp(bm_snap, v->bitmap, bm_bytes) != 0;

    if (io_pread(&v->io, apba * (uint64_t)INVFS_BLOCK_SIZE, after,
                 sizeof after) != 0) {
        free(bm_snap);
        vol_close(v);
        return fail("cannot re-read the anchor block");
    }
    bytes_changed = memcmp(before, after, sizeof before) != 0;

    printf("MODE=%s ANCHOR_PBA=%llu ZONE_START=%llu ZONE_BLOCKS=%llu "
           "ZONE_CANDIDATES=%llu BOOT_PAGES_SKIPPED=%llu READONLY=%d "
           "RC=%d RETURNED_ANCHOR=%d ANCHOR_BIT_BEFORE=%d "
           "ANCHOR_BIT_AFTER=%d ANCHOR_METATYPE_AFTER=%d "
           "CAND_PBA=%llu CAND_BIT_BEFORE=%d CAND_BIT_AFTER=%d "
           "BITS_CHANGED=%d ANCHOR_BYTES_CHANGED=%d "
           "SCAN_RUNS_DELTA=%llu SCAN_ITERS_DELTA=%llu "
           "FREE_DELTA=%lld META_FREE_DELTA=%lld\n",
           mode, (unsigned long long)apba, (unsigned long long)inj_start,
           (unsigned long long)inj_blocks, (unsigned long long)candidates,
           (unsigned long long)boot_pages, rom ? 1 : 0, rc, ret_anchor,
           anchor_bit_before, anchor_bit_after, anchor_meta_after,
           (unsigned long long)cand, cand_bit_before, cand_bit_after,
           bits_changed, bytes_changed,
           (unsigned long long)(v->mb_zone_scan_runs - saved_runs),
           (unsigned long long)(v->mb_zone_scan_iters - saved_iters),
           (long long)(saved_free - v->free_blocks),
           (long long)(saved_mfree - v->meta_free_blocks));

    /* Put the RAM back the way the open found it, so closing this handle
     * cannot propagate the synthetic geometry anywhere. Nothing here dirtied
     * the volume (v->dirty stays 0, so vol_close writes nothing); the restore
     * is belt and braces, and it does NOT restore the anchor block's BYTES --
     * if the call wrote there, the suite has to see it. */
    memcpy(v->bitmap, bm_snap, bm_bytes);
    free(bm_snap);
    if (v->meta_type_bitmap)
        xm_bit_clear(v->meta_type_bitmap, apba);
    v->sb.metadata_zone_start = saved_meta_start;
    v->sb.metadata_zone_blocks = saved_meta_blocks;
    v->sb.shadow_zone_blocks = saved_shadow_blocks;
    v->sb.vol_flags = saved_flags;
    v->mb_alloc_cursor = saved_cursor;
    v->mb_alloc_fail_run = saved_fail_run;
    v->mb_boot_cursor = saved_boot_cur;
    v->mb_boot_end = saved_boot_end;
    v->free_blocks = saved_free;
    v->meta_free_blocks = saved_mfree;
    v->shadow_free = saved_shadow_free;
    v->shadow_fail_run = saved_shadow_fail;
    v->mb_zone_scan_runs = saved_runs;
    v->mb_zone_scan_iters = saved_iters;

    vol_close(v);
    return 0;
}

static int dump_block(const char *img, int at_tail, int only_anchor)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    uint64_t bytes, toff = 0;
    size_t n, i;
    blkio io;

    if (img_size(img, &bytes) != 0)
        return fail("cannot size %s", img);
    if (raw_open(&io, img) != 0)
        return fail("cannot open %s", img);
    if (at_tail && tail_off(&io, &toff) != 0) {
        blkio_close(&io);
        return fail("cannot locate the anchor block");
    }
    n = blkio_pread(&io, at_tail ? toff : 0, blk,
                    sizeof blk) == 0 ? sizeof blk : 0;
    blkio_close(&io);
    if (n == 0)
        return fail("cannot read the block");
    if (only_anchor)
        n = sizeof(invfs_anc0);
    for (i = 0; i < n; i++)
        printf("%02x", blk[i]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <build|readback|reserve|writecycle|probe|"
                        "readback-gen|corrupt-rt30|teleport-anchor|patch-fingerprint|"
                        "corrupt-anchor-crc|fill-tail|block0|anchorhex|"
                        "extend-meta|alloc-meta> <img> [args]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "build") && argc == 4)
        return cmd_build(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "readback-gen") && argc == 5)
        return cmd_readback_gen(argv[2], atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "readback") && argc == 4)
        return cmd_readback(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "reserve") && argc == 4)
        return cmd_reserve(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "writecycle") && argc == 4)
        return cmd_writecycle(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "probe") && argc == 3)
        return cmd_probe(argv[2]);
    if (!strcmp(argv[1], "corrupt-rt30") && argc == 4)
        return cmd_corrupt_rt30(argv[2], argv[3]);
    if (!strcmp(argv[1], "teleport-anchor") && argc == 4)
        return cmd_teleport_anchor(argv[2], argv[3]);
    if (!strcmp(argv[1], "taildump") && argc == 4)
        return cmd_taildump(argv[2], argv[3]);
    if (!strcmp(argv[1], "teleport-check") && argc == 4)
        return cmd_teleport_check(argv[2], argv[3]);
    if (!strcmp(argv[1], "patch-fingerprint") && argc == 4)
        return cmd_patch_fingerprint(argv[2], argv[3]);
    if (!strcmp(argv[1], "corrupt-anchor-crc") && argc == 3)
        return cmd_corrupt_anchor_crc(argv[2]);
    if (!strcmp(argv[1], "fill-tail") && argc == 4)
        return cmd_fill_tail(argv[2], argv[3]);
    if (!strcmp(argv[1], "block0") && argc == 3)
        return dump_block(argv[2], 0, 1);
    if (!strcmp(argv[1], "anchorhex") && argc == 3)
        return dump_block(argv[2], 1, 1);
    if (!strcmp(argv[1], "extend-meta") && argc == 4)
        return cmd_extend_meta(argv[2], argv[3]);
    if (!strcmp(argv[1], "alloc-meta") && argc == 4)
        return cmd_alloc_meta(argv[2], argv[3]);
    fprintf(stderr, "FAIL: bad arguments\n");
    return 2;
}
