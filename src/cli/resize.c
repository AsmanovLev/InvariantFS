/*
 * resize.c — InvariantFS offline volume resize (WP18)
 *
 *   invf-resize <image|device> <newsize[K|M|G]>
 *
 * Grows or shrinks a volume image to <newsize> bytes (K/M/G suffixes are
 * 1024-based: "768M", "1G"). OFFLINE ONLY: the volume must be unmounted --
 * nothing locks an image file against a mounted writer, so the guard is the
 * superblock state: the volume must be CLEAN, sealed redundancy must be off
 * (parity stripes reference absolute block numbers), and that is all.
 *
 * Layout (derived from the superblock; see mkfs.c):
 *   block 0: superblock
 *   metadata zone at block 1: [bitmap bm][L2P journal 8192][inode area]
 *   RAW zone, then shadow zone to the end of the image.
 * bm = ceil((total_blocks/8) / 4096), so whenever total_blocks crosses a
 * 32768-block (128 MB) boundary the bitmap -- and with it the journal and
 * inode area -- shifts inside the metadata zone. The RAW zone, the zone
 * boundaries and every live data block keep their physical addresses
 * ("data untouched"); the shadow zone alone absorbs the size delta at the
 * volume's tail. The inode area keeps its block count modulo the bitmap
 * delta (re-proportioning it per mkfs's INVFS_META_FRAC math would move the
 * RAW zone head, i.e. data -- out of scope for v1, like compaction).
 *
 * The metadata move can never be atomic (the new bitmap overlaps the old
 * journal head), so it runs as a staged two-phase commit:
 *
 *   1. the whole metadata payload (bitmap + used journal + used inode
 *      records) is copied into a collision-free staging area -- the grown
 *      tail on grow, a contiguous free run below the new boundary on
 *      shrink -- verified by read-back (CRC + memcmp against the sources),
 *      and flushed;
 *   2. the superblock is flipped to RECOVERY and the RSZ0 descriptor
 *      (invarifs.h, block 0 offset 0x140) is written, naming the staging
 *      and carrying the complete post-resize superblock ("armed");
 *   3. the apply then runs in vol_open (this tool's own open, or any later
 *      one after a crash): it rewrites the payload at its post-resize
 *      location FROM THE STAGING ALONE -- so re-entering after a crash is
 *      safe -- patches v2 position-kill tombstones by the area displacement
 *      (their file_size is an absolute record offset), and commits the new
 *      superblock + a cleared descriptor in one block-0 rewrite.
 *
 * A crash before the arm leaves the old volume byte-identical (the RECOVERY
 * state auto-recovers at the next open, the staging being dead weight in
 * free space); a crash after it finishes the resize at the next open.
 * Shrink truncates the image before the arm -- only after the tail-free
 * check proved every block at/above the new boundary unallocated.
 *
 * Shrink refuses (honestly, with a count) when live blocks sit at/above the
 * new boundary: there is no compaction in v1. Free the tail (delete files,
 * then invf-sweep + invf-fsck -f) and retry.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "blkio.h"

#define RSZ_MIN_BYTES (64ull * 1024ull * 1024ull)   /* mkfs's floor */

static uint64_t div_ceil(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

/* bitmap span in blocks for a volume of `total` blocks (mkfs/vol_open) */
static uint64_t bm_blocks(uint64_t total)
{
    return (total / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
}

static int bit_get(const uint8_t *b, uint64_t i) { return (b[i / 8] >> (i % 8)) & 1; }

#ifndef _WIN32
/* Test hook (tools/test-resize.sh): die right after the staging copy is
 * verified ("staged") or right after the descriptor arm ("armed") -- the
 * two points the crash-safety argument is about. The third stage ("moved",
 * mid-apply) lives in volume.c next to the commit. */
static int abort_at(const char *stage)
{
    const char *a = getenv("INVFS_RESIZE_ABORT_AT");
    if (a && strcmp(a, stage) == 0)
        fflush(stdout);   /* the log must show how far the kill came */
    return a && strcmp(a, stage) == 0;
}
#endif

/* "768M", "1G", "536870912", "512mb" -> bytes; 0 = unparseable */
static uint64_t parse_size(const char *s)
{
    char *endp = NULL;
    unsigned long long v, mult = 1;

    if (!s || !*s) return 0;
    v = strtoull(s, &endp, 10);
    if (endp == s) return 0;
    while (*endp == ' ' || *endp == '\t') endp++;
    switch (*endp) {
        case 'k': case 'K': mult = 1024ull; endp++; break;
        case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
        case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
        default: break;
    }
    if (*endp == 'b' || *endp == 'B') endp++;
    while (*endp == ' ' || *endp == '\t') endp++;
    if (*endp != '\0') return 0;
    if (mult && v > (unsigned long long)-1 / mult) return 0;
    return (uint64_t)(v * mult);
}

/* ------------------------------------------------------------------ *
 * metadata scans (vol_open's own walk semantics, replicated on blkio)
 * ------------------------------------------------------------------ */

/* used journal bytes. WP22d: if a valid slot header exists, the journal
 * payload worth staging is the winning slot's [header + image + chained
 * log]; *src_out moves to that slot's base. Otherwise the area is the
 * legacy flat log: entries until the first CRC failure (replay's rule)
 * from js_byte. */
static int scan_journal(blkio *io, uint64_t js_byte, uint64_t *src_out,
                        uint64_t *used_out, int *slotted_out)
{
    invfs_jrn_hdr h[2];
    int ok[2] = {0, 0};
    int pick = -1;
    uint32_t s;

    for (s = 0; s < INVFS_JRN_SLOTS; s++) {
        uint64_t off = js_byte + (uint64_t)s * INVFS_JRN_SLOT_BLOCKS *
                       INVFS_BLOCK_SIZE;
        if (blkio_pread(io, off, &h[s], sizeof h[s]) != 0)
            return -1;
        if (memcmp(h[s].magic, INVFS_JRN_MAGIC, 4) != 0 ||
            h[s].version != INVFS_JRN_VERSION ||
            h[s].image_bytes % sizeof(invfs_l2p_entry) != 0 ||
            h[s].image_bytes >
                (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1) * INVFS_BLOCK_SIZE ||
            invfs_crc32c(&h[s], offsetof(invfs_jrn_hdr, crc32c)) !=
                h[s].crc32c)
            continue;
        ok[s] = 1;
    }
    if (ok[0] && ok[1]) pick = h[0].seq >= h[1].seq ? 0 : 1;
    else if (ok[0]) pick = 0;
    else if (ok[1]) pick = 1;

    if (pick >= 0) {
        /* validate the winner's image wholesale, then walk the chained
         * log; a torn image falls back to the other slot (it holds the
         * full pre-compaction state) */
        int order[2];
        int oi;
        order[0] = pick;
        order[1] = pick ^ 1;
        for (oi = 0; oi < 2; oi++) {
            int s = order[oi];
            uint64_t base, jend, jp;
            uint32_t prev;
            if (!ok[s]) continue;
            base = js_byte + (uint64_t)s * INVFS_JRN_SLOT_BLOCKS *
                   INVFS_BLOCK_SIZE;
            jend = base + (uint64_t)INVFS_JRN_SLOT_BLOCKS *
                   INVFS_BLOCK_SIZE;
            jp = base + INVFS_BLOCK_SIZE;
            prev = invfs_crc32c(&h[s], offsetof(invfs_jrn_hdr, image_crc));
            if (h[s].image_bytes) {
                uint8_t *img = (uint8_t *)malloc((size_t)h[s].image_bytes);
                invfs_l2p_entry last;
                int good;
                if (!img) return -1;
                good = blkio_pread(io, jp, img, (size_t)h[s].image_bytes) == 0 &&
                       invfs_crc32c(img, (size_t)h[s].image_bytes) ==
                           h[s].image_crc;
                free(img);
                if (!good) continue;   /* torn compaction */
                if (blkio_pread(io, jp + h[s].image_bytes - sizeof last,
                                &last, sizeof last) != 0)
                    return -1;
                prev = last.crc;
                jp += h[s].image_bytes;
            }
            while (jp + sizeof(invfs_l2p_entry) <= jend) {
                invfs_l2p_entry e;
                if (blkio_pread(io, jp, &e, sizeof e) != 0)
                    return -1;
                if (invfs_crc32c_update(prev, &e,
                                        offsetof(invfs_l2p_entry, crc)) != e.crc)
                    break;
                prev = e.crc;
                jp += sizeof e;
            }
            *src_out = base;
            *used_out = jp - base;
            *slotted_out = 1;
            return 0;
        }
        return -1;   /* both slots torn */
    }

    {
        uint64_t jp = js_byte;
        uint64_t jend = js_byte + (uint64_t)INVFS_JOURNAL_BLOCKS *
                        INVFS_BLOCK_SIZE;
        while (jp + sizeof(invfs_l2p_entry) <= jend) {
            invfs_l2p_entry e;
            if (blkio_pread(io, jp, &e, sizeof e) != 0)
                return -1;
            if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc)
                break;
            jp += sizeof e;
        }
        *src_out = js_byte;
        *used_out = jp - js_byte;
        *slotted_out = 0;
        return 0;
    }
}

/* Live parity-owner tracking for the seal refusal: the owners are ordinary
 * records named "\x01parity*", retired by ordinary tombstones. */
typedef struct { uint64_t pos, id; } rsz_owner;

static int is_parity_name(const char *nm)
{
    return (uint8_t)nm[0] == 0x01 && strncmp(nm + 1, "parity", 6) == 0;
}

/* used inode-area bytes + seal-owner liveness; mirrors vol_open's scan:
 * stop at the first magic/length failure, skip (but carry) CRC failures. */
static int scan_inode_area(blkio *io, uint64_t area_byte, uint64_t area_end,
                           uint64_t *used_out, uint64_t *anomalies_out,
                           size_t *parity_live_out)
{
    uint64_t p = area_byte;
    rsz_owner *own = NULL;
    size_t own_n = 0, own_cap = 0;
    int rc = -1;

    while (p + sizeof(invfs_inode_rec) <= area_end) {
        invfs_inode_rec rh;
        uint8_t *rb;
        uint32_t crc_stored, crc_calc;
        if (blkio_pread(io, p, &rh, sizeof rh) != 0)
            goto out;
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC)
            break;  /* end of records */
        if (rh.rec_len < sizeof rh || rh.rec_len > INVFS_MAX_REC_LEN ||
            p + rh.rec_len + 4 > area_end) {
            (*anomalies_out)++;
            break;  /* corrupted tail -- stop (vol_open's rule) */
        }
        rb = (uint8_t *)malloc((size_t)rh.rec_len + 4);
        if (!rb) goto out;
        if (blkio_pread(io, p, rb, (size_t)rh.rec_len + 4) != 0) {
            free(rb);
            goto out;
        }
        memcpy(&crc_stored, rb + rh.rec_len, 4);
        crc_calc = invfs_crc32c(rb, rh.rec_len);
        free(rb);
        if (crc_calc != crc_stored) {
            (*anomalies_out)++;
            p += (uint64_t)rh.rec_len + 4;   /* skip, keep scanning */
            continue;
        }
        if (rh.magic == INODE_REC_MAGIC) {
            char nm[sizeof(rh.name) + 1];
            size_t nl = rh.name_len < sizeof(rh.name)
                      ? rh.name_len : sizeof(rh.name);
            memcpy(nm, rh.name, nl);
            nm[nl] = 0;
            if (is_parity_name(nm)) {
                if (own_n == own_cap) {
                    size_t nc = own_cap ? own_cap * 2 : 16;
                    void *np = realloc(own, nc * sizeof *own);
                    if (!np) goto out;
                    own = (rsz_owner *)np;
                    own_cap = nc;
                }
                own[own_n].pos = p;
                own[own_n].id = rh.inode_id;
                own_n++;
            }
        } else {   /* DELT: position kill (v2) or id kill (legacy) */
            size_t i = 0;
            while (i < own_n) {
                int hit = rh.file_size ? (own[i].pos == rh.file_size)
                                       : (own[i].id == rh.inode_id);
                if (hit)
                    own[i] = own[--own_n];
                else
                    i++;
            }
        }
        p += (uint64_t)rh.rec_len + 4;
    }
    *used_out = p - area_byte;
    *parity_live_out = own_n;
    rc = 0;
out:
    free(own);
    return rc;
}

/* first free run of >= need blocks in [lo, hi); 0 = none (block 0 is
 * metadata anyway, so 0 is never a valid answer) */
static uint64_t find_free_run(const uint8_t *bitmap, uint64_t lo,
                              uint64_t hi, uint64_t need)
{
    uint64_t i, run = 0, start = 0;
    for (i = lo; i < hi; i++) {
        if (!bit_get(bitmap, i)) {
            if (!run) start = i;
            if (++run >= need) return start;
        } else {
            run = 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * WP25: two-device resize -- v1 grows the TAIL device (dev1) only.
 *
 * The volume's global block space is the concatenation dev0+dev1, so a
 * grow lands entirely on dev1's tail: dev1's image is extended, the new
 * superblock's total_blocks grows, the DEVT table follows at the commit
 * (vol_rsz0_apply), and every metadata write this tool does must reach
 * BOTH devices' block 0 (the mirror). The io router below is the
 * tool-local twin of the engine mux (which this standalone tool cannot
 * use: it works on raw blkio handles for the pre-open phases). With
 * io2 == NULL it degenerates to plain blkio on io -- the single-device
 * path is byte-identical.
 * ------------------------------------------------------------------ */
typedef struct {
    blkio   *io, *io2;         /* io2 NULL = single-device */
    uint64_t dev0_bytes;       /* global->local split */
    uint64_t meta_end;         /* mirrored span [0, meta_end) */
} rzio;

static int rz_pread(rzio *r, uint64_t off, void *buf, size_t len)
{
    if (off < r->meta_end || !r->io2)
        return blkio_pread(r->io, off, buf, len);       /* primary */
    if (off < r->dev0_bytes)
        return blkio_pread(r->io, off, buf, len);
    return blkio_pread(r->io2, off - r->dev0_bytes, buf, len);
}

static int rz_pwrite(rzio *r, uint64_t off, const void *buf, size_t len)
{
    if (off < r->meta_end) {
        /* metadata span: writethrough mirror to both devices */
        if (blkio_pwrite(r->io, off, buf, len) != 0)
            return -1;
        if (r->io2 && blkio_pwrite(r->io2, off, buf, len) != 0)
            return -1;
        return 0;
    }
    if (off < r->dev0_bytes || !r->io2)
        return blkio_pwrite(r->io, off, buf, len);
    return blkio_pwrite(r->io2, off - r->dev0_bytes, buf, len);
}

static int rz_flush(rzio *r)
{
    int rc = blkio_flush(r->io);
    if (r->io2 && blkio_flush(r->io2) != 0) rc = -1;
    return rc;
}

/* copy `len` bytes from src to dst through the bounce, chaining a CRC */
static int copy_crc2(rzio *rz, uint64_t src, uint64_t dst, uint64_t len,
                     uint8_t *buf, uint32_t *crc)
{
    while (len) {
        size_t n = len > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)len;
        if (rz_pread(rz, src, buf, n) != 0 ||
            rz_pwrite(rz, dst, buf, n) != 0)
            return -1;
        *crc = invfs_crc32c_update(*crc, buf, n);
        src += n;
        dst += n;
        len -= n;
    }
    return 0;
}

/* copy `len` bytes from src to dst through the bounce, chaining a CRC */
static int copy_crc(blkio *io, uint64_t src, uint64_t dst, uint64_t len,
                    uint8_t *buf, uint32_t *crc)
{
    while (len) {
        size_t n = len > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)len;
        if (blkio_pread(io, src, buf, n) != 0 ||
            blkio_pwrite(io, dst, buf, n) != 0)
            return -1;
        *crc = invfs_crc32c_update(*crc, buf, n);
        src += n;
        dst += n;
        len -= n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    char devbuf[64];
    blkio io;
    invfs_superblock sb;
    invfs_rsz0 rz;
    invfs_superblock nsb;
    invfs_rszs sh;
    uint8_t *bitmap = NULL, *buf = NULL, *blk = NULL;
    uint64_t want_bytes, old_total, new_total;
    uint64_t bm_old, bm_new, js_old, is_old, iend_old, meta_blocks;
    uint64_t new_js, new_is, new_iend;
    uint64_t j_used = 0, i_used = 0, anomalies = 0;
    uint64_t j_src = 0;             /* active slot base when slotted */
    int j_slotted = 0;
    uint64_t stage_start, stage_blocks, payload, free_old, free_new;
    uint64_t i, blocked, first_bad = 0;
    size_t parity_live = 0;
    int is_dev, rc, grow;
    int err = 0;
    rzio rz2;                          /* WP25: two-device io router */
    blkio io2;
    int twodev = 0;
    uint64_t dev0_blocks = 0, dev1_blocks = 0;
    char dev1_path[128];

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: invf-resize <image|device> <newsize[K|M|G]>\n"
                        "       invf-resize <image|device> --max\n"
                        "  offline volume resize (grow/shrink); the volume "
                        "must be unmounted\n"
                        "  two-device volumes (WP25): newsize is the TOTAL;\n"
                        "  growth lands on the tail device (dev1, set\n"
                        "  INVFS_DEV1); shrink is refused\n"
                        "  --max: grow to fill all available device space\n");
        return 2;
    }
    path = blkio_normalize(argv[1], devbuf, sizeof devbuf);
    if (strcmp(argv[2], "--max") == 0) {
        want_bytes = 0;  /* computed later from device capacities */
    } else {
        want_bytes = parse_size(argv[2]);
        if (!want_bytes) {
            fprintf(stderr, "invf-resize: \"%s\" is not a size\n", argv[2]);
            return 2;
        }
        want_bytes -= want_bytes % INVFS_BLOCK_SIZE;
        if (want_bytes < RSZ_MIN_BYTES) {
            fprintf(stderr, "invf-resize: volume too small: %llu bytes "
                    "(min 64MB)\n", (unsigned long long)want_bytes);
            return 1;
        }
    }

    rc = blkio_open(&io, path,
                    blkio_looks_like_device(path) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "invf-resize: cannot open %s: %s\n",
                path, blkio_strerror(rc));
        return 1;
    }
    is_dev = blkio_is_device(&io);
    memset(&rz2, 0, sizeof rz2);
    rz2.io = &io;

    if (blkio_pread(&io, 0, &sb, sizeof sb) != 0) {
        fprintf(stderr, "invf-resize: cannot read superblock\n");
        blkio_close(&io);
        return 1;
    }
    if (memcmp(sb.magic, INVFS_MAGIC, 8) != 0) {
        fprintf(stderr, "invf-resize: %s: bad magic (not an InvariantFS "
                "image?)\n", path);
        blkio_close(&io);
        return 1;
    }
    if (invfs_crc32c(&sb, offsetof(invfs_superblock, checksum)) != sb.checksum) {
        fprintf(stderr, "invf-resize: %s: superblock checksum mismatch\n",
                path);
        blkio_close(&io);
        return 1;
    }

    /* an armed resize from a killed run takes precedence: complete it (the
     * roll-forward in vol_open), report, done */
    memset(&rz, 0, sizeof rz);
    if (blkio_pread(&io, INVFS_RSZ0_OFF, &rz, sizeof rz) == 0 &&
        memcmp(rz.magic, "RSZ0", 4) == 0) {
        invfs_volume *v;
        printf("invf-resize: %s: a previous resize was interrupted; "
               "completing it\n", path);
        blkio_close(&io);
        v = vol_open(path, &err);
        if (!v) {
            fprintf(stderr, "invf-resize: roll-forward failed (err %d); "
                            "run invf-fsck -f %s and retry\n", err, path);
            return 1;
        }
        vol_close(v);
        if (blkio_open(&io, path, 0) != 0 ||
            blkio_pread(&io, 0, &sb, sizeof sb) != 0) {
            fprintf(stderr, "invf-resize: re-read after roll-forward "
                            "failed\n");
            return 1;
        }
        printf("invf-resize: completed at %llu bytes (%llu blocks)\n",
               (unsigned long long)(sb.total_blocks * INVFS_BLOCK_SIZE),
               (unsigned long long)sb.total_blocks);
        if (sb.total_blocks * INVFS_BLOCK_SIZE != want_bytes)
            printf("  (the requested target %llu differs; re-run to resize "
                   "further)\n", (unsigned long long)want_bytes);
        blkio_close(&io);
        return 0;
    }

    /* ---- refusal gate: offline, clean, unsealed ---- */
    if (sb.state != INVFS_STATE_CLEAN) {
        fprintf(stderr, "invf-resize: %s: volume is not clean "
                "(state 0x%02X) -- run recovery first: invf-fsck -f %s\n",
                path, (unsigned)sb.state, path);
        blkio_close(&io);
        return 1;
    }
    if (sb.vol_flags & VOLF_READONLY) {
        fprintf(stderr, "invf-resize: %s: volume is read-only "
                "(VOLF_READONLY)\n", path);
        blkio_close(&io);
        return 1;
    }
    {
        invfs_rdp0 rd;
        memset(&rd, 0, sizeof rd);
        if (blkio_pread(&io, INVFS_RDP0_OFF, &rd, sizeof rd) == 0 &&
            memcmp(rd.magic, "RDP0", 4) == 0) {
            invfs_rdp0 t = rd;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == rd.crc32c &&
                (rd.l1_algo || rd.l2_algo)) {
                fprintf(stderr, "invf-resize: %s: redundancy is live "
                        "(RDP0 l1=%u l2=%u) -- parity stripes reference "
                        "absolute block numbers; unseal first: "
                        "invf-sweep %s --free-redundant\n",
                        path, (unsigned)rd.l1_algo, (unsigned)rd.l2_algo,
                        path);
                blkio_close(&io);
                return 1;
            }
        }
    }
    {
        /* WP21/WP22d: a live sweep checkpoint pins the journal + inode-area
         * positions the resize is about to move (and its staging run is
         * L2P-covered scratch). Refuse like the seal refusal above: resolve
         * the checkpoint first. */
        invfs_ckp0 ck;
        memset(&ck, 0, sizeof ck);
        if (blkio_pread(&io, INVFS_CKP0_OFF, &ck, sizeof ck) == 0 &&
            memcmp(ck.magic, "CKP0", 4) == 0) {
            invfs_ckp0 t = ck;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == ck.crc32c) {
                fprintf(stderr, "invf-resize: %s: a sweep checkpoint is "
                        "live (sweep #%llu) -- resolve it first: "
                        "invf-rollback %s  or  invf-sweep %s --realize\n",
                        path, (unsigned long long)ck.sweep_seq, path, path);
                blkio_close(&io);
                return 1;
            }
        }
    }

    /* ---- current derived layout ---- */
    old_total = sb.total_blocks;
    meta_blocks = sb.metadata_zone_blocks;
    bm_old = bm_blocks(old_total);
    js_old = sb.metadata_zone_start + bm_old;
    is_old = js_old + INVFS_JOURNAL_BLOCKS;
    iend_old = sb.metadata_zone_start + meta_blocks;   /* blocks */

    /* WP25: the device table decides single- vs two-device. Two-device
     * v1 rule: GROWTH ONLY, and growth lands on the tail device (dev1);
     * dev0's size never changes. */
    {
        invfs_devt dt;
        memset(&dt, 0, sizeof dt);
        memset(dev1_path, 0, sizeof dev1_path);
        if (blkio_pread(&io, INVFS_DEVT_OFF, &dt, sizeof dt) == 0 &&
            memcmp(dt.magic, "DEVT", 4) == 0) {
            invfs_devt t = dt;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == dt.crc32c &&
                dt.version == 1 && dt.dev_count == 2 &&
                dt.dev_blocks[0] && dt.dev_blocks[1] &&
                dt.dev_blocks[0] + dt.dev_blocks[1] == sb.total_blocks &&
                memcmp(dt.vol_uuid, sb.uuid, 16) == 0) {
                twodev = 1;
                dev0_blocks = dt.dev_blocks[0];
                dev1_blocks = dt.dev_blocks[1];
                memcpy(dev1_path, dt.dev1_hint,
                       sizeof dev1_path < sizeof dt.dev1_hint ?
                       sizeof dev1_path : sizeof dt.dev1_hint);
                dev1_path[sizeof dev1_path - 1] = 0;
            } else {
                fprintf(stderr, "invf-resize: %s: DEVT device table is "
                        "torn; refusing to resize\n", path);
                goto fail;
            }
        }
    }
    if (twodev) {
        const char *d1 = getenv("INVFS_DEV1");
        if (d1 && *d1) {
            snprintf(dev1_path, sizeof dev1_path, "%s", d1);
        }
        if (!dev1_path[0]) {
            fprintf(stderr, "invf-resize: %s: two-device volume; set "
                    "INVFS_DEV1 to device 1\n", path);
            goto fail;
        }
        rc = blkio_open(&io2, dev1_path,
                        blkio_looks_like_device(dev1_path) ?
                        BLKIO_EXCLUSIVE : 0);
        if (rc != 0) {
            fprintf(stderr, "invf-resize: cannot open device 1 %s: %s\n",
                    dev1_path, blkio_strerror(rc));
            goto fail;
        }
        if (blkio_capacity(&io2) <
            dev1_blocks * (uint64_t)INVFS_BLOCK_SIZE) {
            fprintf(stderr, "invf-resize: device 1 %s is smaller than the "
                    "device table says\n", dev1_path);
            blkio_close(&io2);
            goto fail;
        }
        rz2.io = &io;
        rz2.io2 = &io2;
        rz2.dev0_bytes = dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE;
        rz2.meta_end = (sb.metadata_zone_start + meta_blocks) *
                       (uint64_t)INVFS_BLOCK_SIZE;
    }

    /* --max: compute target from device capacities */
    if (want_bytes == 0) {
        if (twodev) {
            uint64_t cap1 = blkio_capacity(&io2);
            want_bytes = dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE + cap1;
            printf("invf-resize: --max: dev0=%llu MiB + dev1=%llu MiB "
                   "= total %llu MiB\n",
                   (unsigned long long)(dev0_blocks * INVFS_BLOCK_SIZE /
                                        (1024ull * 1024)),
                   (unsigned long long)(cap1 / (1024ull * 1024)),
                   (unsigned long long)(want_bytes / (1024ull * 1024)));
        } else {
            want_bytes = blkio_capacity(&io);
            printf("invf-resize: --max: device capacity = %llu MiB\n",
                   (unsigned long long)(want_bytes / (1024ull * 1024)));
        }
        want_bytes -= want_bytes % INVFS_BLOCK_SIZE;
        if (want_bytes < RSZ_MIN_BYTES) {
            fprintf(stderr, "invf-resize: volume too small after --max "
                    "computation: %llu bytes\n",
                    (unsigned long long)want_bytes);
            goto fail;
        }
    }

    bitmap = (uint8_t *)malloc((size_t)(bm_old * INVFS_BLOCK_SIZE));
    buf = (uint8_t *)malloc(BLKIO_BOUNCE);
    blk = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!bitmap || !buf || !blk) {
        fprintf(stderr, "invf-resize: out of memory\n");
        goto fail;
    }
    if (blkio_pread(&io, sb.metadata_zone_start * (uint64_t)INVFS_BLOCK_SIZE,
                    bitmap, (size_t)(bm_old * INVFS_BLOCK_SIZE)) != 0) {
        fprintf(stderr, "invf-resize: cannot read the block bitmap\n");
        goto fail;
    }
    /* bits past the end of the volume are meaningless; the staged bitmap is
     * extended/truncated from this one, so make the tail honest first */
    for (i = old_total; i < bm_old * INVFS_BLOCK_SIZE * 8; i++)
        if (bit_get(bitmap, i)) {
            fprintf(stderr, "invf-resize: %s: bitmap marks block %llu past "
                    "the volume end -- run invf-fsck -f first\n",
                    path, (unsigned long long)i);
            goto fail;
        }

    if (scan_journal(&io, js_old * (uint64_t)INVFS_BLOCK_SIZE,
                     &j_src, &j_used, &j_slotted) != 0) {
        fprintf(stderr, "invf-resize: cannot scan the L2P journal\n");
        goto fail;
    }
    if (scan_inode_area(&io, is_old * (uint64_t)INVFS_BLOCK_SIZE,
                        iend_old * (uint64_t)INVFS_BLOCK_SIZE,
                        &i_used, &anomalies, &parity_live) != 0) {
        fprintf(stderr, "invf-resize: cannot scan the inode area\n");
        goto fail;
    }
    if (parity_live) {
        fprintf(stderr, "invf-resize: %s: %zu live parity owner record(s) "
                "-- parity stripes reference absolute block numbers; "
                "unseal first: invf-sweep %s --free-redundant\n",
                path, parity_live, path);
        goto fail;
    }
    if (anomalies)
        fprintf(stderr, "invf-resize: note: %llu damaged record(s) in the "
                "inode area are carried verbatim\n",
                (unsigned long long)anomalies);

    /* ---- target geometry ---- */
    new_total = want_bytes / INVFS_BLOCK_SIZE;
    if (new_total == old_total) {
        printf("invf-resize: %s: already at %llu bytes (%llu blocks) -- "
               "nothing to do\n", path,
               (unsigned long long)want_bytes,
               (unsigned long long)old_total);
        blkio_close(&io);
        return 0;
    }
    grow = new_total > old_total;
    if (twodev && !grow) {
        fprintf(stderr, "invf-resize: %s: two-device v1 grows only the "
                "TAIL device (dev1); shrink is not supported -- refuse\n",
                path);
        goto fail;
    }
    bm_new = bm_blocks(new_total);
    new_js = sb.metadata_zone_start + bm_new;
    new_is = new_js + INVFS_JOURNAL_BLOCKS;
    new_iend = iend_old;   /* the metadata zone's block count does not move */

    if (is_dev && !twodev) {
        uint64_t cap = blkio_capacity(&io);
        if (!grow) {
            fprintf(stderr, "invf-resize: cannot shrink a block device "
                    "(the partition is what it is)\n");
            goto fail;
        }
        if (want_bytes != cap) {
            fprintf(stderr, "invf-resize: a device resize grows into the "
                    "current partition: target must be exactly %llu bytes\n",
                    (unsigned long long)cap);
            goto fail;
        }
    }
    if (twodev) {
        /* the tail device absorbs the whole delta; dev0 never moves */
        uint64_t dev1_bytes =
            (new_total - dev0_blocks) * (uint64_t)INVFS_BLOCK_SIZE;
        if (blkio_is_device(&io2)) {
            uint64_t cap = blkio_capacity(&io2);
            if (dev1_bytes != cap) {
                fprintf(stderr, "invf-resize: device 1 is a block device: "
                        "the grown size must be exactly %llu bytes\n",
                        (unsigned long long)cap);
                goto fail;
            }
        } else if (blkio_chsize(&io2, dev1_bytes) != 0) {
            fprintf(stderr, "invf-resize: cannot grow device 1 %s to %llu "
                    "bytes\n", dev1_path, (unsigned long long)dev1_bytes);
            goto fail;
        }
        printf("  two-device: growth lands on device 1 (%s -> %llu bytes)\n",
               dev1_path, (unsigned long long)dev1_bytes);
    }

    /* the inode area absorbs the bitmap growth: it must still hold every
     * record plus one guard block */
    if (i_used + 2 * INVFS_BLOCK_SIZE >
        (new_iend - new_is) * (uint64_t)INVFS_BLOCK_SIZE) {
        fprintf(stderr, "invf-resize: %s: the inode area is too full for "
                "this target (%llu bytes of records, %llu available) -- "
                "the bitmap growth cannot be absorbed\n",
                path, (unsigned long long)i_used,
                (unsigned long long)((new_iend - new_is) *
                                     (uint64_t)INVFS_BLOCK_SIZE));
        goto fail;
    }

    /* ---- shrink: the tail must be free ---- */
    if (!grow) {
        if (new_total < 1 + meta_blocks + sb.raw_zone_blocks) {
            fprintf(stderr, "invf-resize: %s: %llu blocks is below the "
                    "metadata+RAW floor (%llu blocks)\n",
                    path, (unsigned long long)new_total,
                    (unsigned long long)(1 + meta_blocks +
                                         sb.raw_zone_blocks));
            goto fail;
        }
        blocked = 0;
        for (i = new_total; i < old_total; i++)
            if (bit_get(bitmap, i)) {
                if (!blocked) first_bad = i;
                blocked++;
            }
        if (blocked) {
            fprintf(stderr, "invf-resize: %s: cannot shrink to %llu blocks: "
                    "%llu live block(s) at/above the new boundary (first at "
                    "block %llu)\n  no compaction in v1 -- free the tail "
                    "(delete files, then invf-sweep + invf-fsck -f) and "
                    "retry\n",
                    path, (unsigned long long)new_total,
                    (unsigned long long)blocked,
                    (unsigned long long)first_bad);
            goto fail;
        }
    }

    /* ---- staging location ---- */
    payload = bm_old * (uint64_t)INVFS_BLOCK_SIZE + j_used + i_used;
    stage_blocks = 1 + div_ceil(payload, INVFS_BLOCK_SIZE);
    if (grow && stage_blocks <= new_total - old_total) {
        stage_start = old_total;   /* the grown tail: free by construction */
    } else {
        /* a contiguous free run below the region that survives */
        uint64_t limit = grow ? old_total : new_total;
        stage_start = find_free_run(bitmap, sb.raw_zone_start, limit,
                                    stage_blocks);
        if (!stage_start) {
            fprintf(stderr, "invf-resize: %s: no contiguous %llu-block free "
                    "run to stage the metadata (%llu bytes) -- free space "
                    "is too fragmented for this target\n",
                    path, (unsigned long long)stage_blocks,
                    (unsigned long long)payload);
            goto fail;
        }
    }

    free_old = 0;
    for (i = 0; i < old_total; i++)
        if (!bit_get(bitmap, i))
            free_old++;

    printf("invf-resize: %s: %s %llu -> %llu bytes (%llu -> %llu blocks)\n",
           path, grow ? "grow" : "shrink",
           (unsigned long long)(old_total * INVFS_BLOCK_SIZE),
           (unsigned long long)(new_total * INVFS_BLOCK_SIZE),
           (unsigned long long)old_total, (unsigned long long)new_total);
    printf("  metadata payload: bitmap %llu + journal %llu + inode %llu = "
           "%llu bytes\n",
           (unsigned long long)(bm_old * (uint64_t)INVFS_BLOCK_SIZE),
           (unsigned long long)j_used, (unsigned long long)i_used,
           (unsigned long long)payload);
    printf("  staging: %llu blocks at %llu%s\n",
           (unsigned long long)stage_blocks, (unsigned long long)stage_start,
           grow && stage_start == old_total ? " (grown tail)" : "");

    /* ---- grow: the file must exist at the new size before the staging
     * lands in its tail (device: validates the partition is big enough) -- */
    if (grow && !twodev && (rc = blkio_chsize(&io, want_bytes)) != 0) {
        fprintf(stderr, "invf-resize: cannot set size to %llu bytes: %s\n",
                (unsigned long long)want_bytes, blkio_strerror(rc));
        goto fail;
    }

    /* ---- phase 1: stage the payload (payload first, header second) ---- */
    {
        uint64_t off = (stage_start + 1) * (uint64_t)INVFS_BLOCK_SIZE;
        uint32_t pcrc = 0;

        /* bitmap comes from memory */
        {
            uint64_t left = bm_old * (uint64_t)INVFS_BLOCK_SIZE, put = 0;
            while (left) {
                size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
                memcpy(buf, bitmap + put, n);
                if (rz_pwrite(&rz2, off, buf, n) != 0) {
                    fprintf(stderr, "invf-resize: staging write failed\n");
                    goto fail;
                }
                pcrc = invfs_crc32c_update(pcrc, buf, n);
                off += n;
                put += n;
                left -= n;
            }
        }
        if (copy_crc2(&rz2, j_src, off, j_used,
                     buf, &pcrc) != 0) {
            fprintf(stderr, "invf-resize: staging write failed (journal)\n");
            goto fail;
        }
        off += j_used;
        if (copy_crc2(&rz2, is_old * (uint64_t)INVFS_BLOCK_SIZE, off, i_used,
                     buf, &pcrc) != 0) {
            fprintf(stderr, "invf-resize: staging write failed (inodes)\n");
            goto fail;
        }

        memset(blk, 0, INVFS_BLOCK_SIZE);
        memcpy(sh.magic, "RSZS", 4);
        sh.version = 1;
        sh.bm_bytes = bm_old * (uint64_t)INVFS_BLOCK_SIZE;
        sh.j_bytes = j_used;
        sh.i_bytes = i_used;
        sh.payload_crc = pcrc;
        sh.crc32c = 0;
        sh.crc32c = invfs_crc32c(&sh, sizeof sh);
        memcpy(blk, &sh, sizeof sh);
        if (rz_pwrite(&rz2, stage_start * (uint64_t)INVFS_BLOCK_SIZE,
                         blk, INVFS_BLOCK_SIZE) != 0) {
            fprintf(stderr, "invf-resize: staging header write failed\n");
            goto fail;
        }
        if (rz_flush(&rz2) != 0) {
            fprintf(stderr, "invf-resize: staging flush failed\n");
            goto fail;
        }
    }

    /* ---- verify the staging by read-back BEFORE anything is armed:
     * the payload CRC must hold, and the bytes must equal the sources
     * (which nothing has touched yet) ---- */
    {
        uint64_t off = (stage_start + 1) * (uint64_t)INVFS_BLOCK_SIZE;
        uint64_t left = payload;
        uint32_t pcrc = 0;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (rz_pread(&rz2, off, buf, n) != 0) {
                fprintf(stderr, "invf-resize: staging read-back failed\n");
                goto fail;
            }
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            off += n;
            left -= n;
        }
        if (pcrc != sh.payload_crc) {
            fprintf(stderr, "invf-resize: staging verification failed "
                    "(payload CRC) -- volume untouched\n");
            goto fail;
        }
    }

#ifndef _WIN32
    if (abort_at("staged")) {
        rz_flush(&rz2);
        kill(getpid(), SIGKILL);
    }
#endif

    /* ---- shrink: truncate now that the tail is proven free and the
     * staging sits below the boundary ---- */
    if (!grow && blkio_chsize(&io, want_bytes) != 0) {
        fprintf(stderr, "invf-resize: cannot truncate to %llu bytes\n",
                (unsigned long long)want_bytes);
        goto fail;
    }

    /* ---- phase 2: arm (RECOVERY first, then the descriptor) ---- */
    nsb = sb;
    nsb.total_blocks = new_total;
    /* zone boundaries do not move; the shadow zone absorbs the delta */
    nsb.shadow_zone_blocks = new_total - nsb.shadow_zone_start;
    /* the ENOSPC policy scales with the volume (mkfs's formulas) */
    nsb.reserved_blocks = (uint32_t)(new_total / 128 + 64);
    nsb.hard_min_blocks = (uint32_t)(new_total / 1024 + 16);
    /* WP22d: a slotted journal is staged into the new slot 0 (the apply
     * writes it at the area base); a legacy journal stays legacy */
    nsb.pad2 = j_slotted ? INVFS_JSEL_SLOT0 : INVFS_JSEL_LEGACY;
    nsb.state = INVFS_STATE_CLEAN;   /* the committed state, once applied */
    nsb.checksum = invfs_crc32c(&nsb, offsetof(invfs_superblock, checksum));

    {
        invfs_superblock rsb = sb;
        rsb.state = INVFS_STATE_RECOVERY;
        rsb.checksum = invfs_crc32c(&rsb, offsetof(invfs_superblock, checksum));
        if (rz_pwrite(&rz2, 0, &rsb, sizeof rsb) != 0) {
            fprintf(stderr, "invf-resize: cannot mark RECOVERY\n");
            goto fail;
        }
    }
    memset(&rz, 0, sizeof rz);
    memcpy(rz.magic, "RSZ0", 4);
    rz.version = 1;
    rz.stage_start = stage_start;
    rz.stage_blocks = stage_blocks;
    rz.bm_bytes = bm_old * (uint64_t)INVFS_BLOCK_SIZE;
    rz.j_bytes = j_used;
    rz.i_bytes = i_used;
    rz.old_total = old_total;
    rz.new_sb = nsb;
    rz.crc32c = 0;
    {
        invfs_rsz0 t = rz;
        t.crc32c = 0;
        rz.crc32c = invfs_crc32c(&t, sizeof t);
    }
    if (rz_pwrite(&rz2, INVFS_RSZ0_OFF, &rz, sizeof rz) != 0) {
        fprintf(stderr, "invf-resize: cannot write the RSZ0 descriptor\n");
        goto fail;
    }
    if (rz_flush(&rz2) != 0) {
        fprintf(stderr, "invf-resize: arm flush failed\n");
        goto fail;
    }
    printf("  armed: state=RECOVERY + RSZ0 (stage %llu blocks at %llu)\n",
           (unsigned long long)stage_blocks, (unsigned long long)stage_start);

#ifndef _WIN32
    if (abort_at("armed")) {
        rz_flush(&rz2);
        kill(getpid(), SIGKILL);
    }
#endif

    /* ---- phase 3: the apply runs in vol_open (the same code a crash
     * recovery would take) ---- */
    blkio_close(&io);
    {
        invfs_volume *v = vol_open(path, &err);
        if (!v) {
            fprintf(stderr, "invf-resize: roll-forward failed (err %d); "
                            "re-run invf-resize or invf-fsck -f %s\n",
                    err, path);
            return 1;
        }
        vol_close(v);
    }

    /* ---- summary ---- */
    rc = blkio_open(&io, path, 0);
    if (rc != 0 || blkio_pread(&io, 0, &sb, sizeof sb) != 0) {
        fprintf(stderr, "invf-resize: cannot re-read the superblock\n");
        return 1;
    }
    bm_new = bm_blocks(sb.total_blocks);
    free_new = 0;
    {
        uint8_t *nbm = (uint8_t *)malloc((size_t)(bm_new * INVFS_BLOCK_SIZE));
        if (nbm) {
            if (blkio_pread(&io, sb.metadata_zone_start *
                                 (uint64_t)INVFS_BLOCK_SIZE, nbm,
                            (size_t)(bm_new * INVFS_BLOCK_SIZE)) == 0) {
                for (i = 0; i < sb.total_blocks; i++)
                    if (!bit_get(nbm, i))
                        free_new++;
            }
            free(nbm);
        }
    }
    printf("  committed: %llu blocks x %u = %llu bytes\n",
           (unsigned long long)sb.total_blocks, INVFS_BLOCK_SIZE,
           (unsigned long long)(sb.total_blocks * INVFS_BLOCK_SIZE));
    printf("  metadata zone: %llu blocks (bitmap %llu -> %llu, journal %u, "
           "inode area %llu -> %llu blocks)\n",
           (unsigned long long)sb.metadata_zone_blocks,
           (unsigned long long)bm_old, (unsigned long long)bm_new,
           (unsigned)INVFS_JOURNAL_BLOCKS,
           (unsigned long long)(meta_blocks - bm_old - INVFS_JOURNAL_BLOCKS),
           (unsigned long long)(meta_blocks - bm_new - INVFS_JOURNAL_BLOCKS));
    printf("  raw zone:      %llu blocks (unchanged)\n",
           (unsigned long long)sb.raw_zone_blocks);
    printf("  shadow zone:   %llu -> %llu blocks\n",
           (unsigned long long)(old_total - sb.shadow_zone_start),
           (unsigned long long)sb.shadow_zone_blocks);
    printf("  free blocks:   %llu -> %llu\n",
           (unsigned long long)free_old, (unsigned long long)free_new);
    printf("invf-resize: OK\n");
    blkio_close(&io);
    rz2.io2 = NULL;
    free(bitmap);
    free(buf);
    free(blk);
    return 0;

fail:
    if (rz2.io2) blkio_close(rz2.io2);
    blkio_close(&io);
    free(bitmap);
    free(buf);
    free(blk);
    return 1;
}
