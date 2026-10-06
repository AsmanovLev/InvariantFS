/* anchor_test.c — the ANC0 tail anchor driver for test-meta-anchor.sh.
 *
 * RT30 (block 0, 0x9D0) and SPT0 (0xA00) are the only copies of the two
 * descriptors that decide whether a metadata-v3 volume can be opened at all.
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
                        "corrupt-anchor-crc|fill-tail|block0|anchorhex> "
                        "<img> [args]\n", argv[0]);
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
    fprintf(stderr, "FAIL: bad arguments\n");
    return 2;
}
