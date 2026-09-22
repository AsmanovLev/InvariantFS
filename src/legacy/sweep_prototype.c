/*
 * sweep.c — InvariantFS Sweep Worker (prototype)
 *
 *   invf-sweep <image> [--limit N] [--dedupe] [--dirs]
 *
 * Walks all inodes, recompresses RAW(LZ4) segments into
 * Shadow(ZSTD-19), updates L2P + AST, frees old RAW blocks.
 *
 * --dedupe: block-level dedup pass only (fast): hash every live
 * segment with BLAKE3, keep one physical copy, remap the rest.
 *
 * --dirs: also walk directory anchors (skipped by default; see
 * is_dir_anchor).
 *
 * Ctrl+C stops after the file in flight rather than killing the run --
 * see on_ctrl.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>   /* QueryPerformanceCounter for the progress clock */
#else
#include <signal.h>
#include <time.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "blake3.h"
#include "miniz.h"

/* Set from the console handler thread, read by the walk. */
#ifdef _WIN32
static volatile LONG g_stop = 0;
#else
static volatile sig_atomic_t g_stop = 0;
#endif

/* A sweep over a real tree runs for the better part of an hour, so the
   only way to stop it used to be killing it -- which drops whatever
   vol_flush had not yet written (bitmap deltas, the compacted L2P
   journal) and can leave the blocks of the file in flight allocated but
   unreferenced.
 *
 * Stopping between files is safe instead: each file is replaced whole
 * (create new inode, delete old) before the loop comes back around, and
 * a re-run skips what is already in Shadow -- the same idempotence that
 * makes a second sweep report "0 swept, N skipped". So the handler only
 * raises a flag; the walk notices it after the current file and exits
 * through the normal flush-and-summarise path.
 *
 * The wait is bounded by one file: a transcode blocks in
 * WaitForSingleObject on MAC.exe/ffmpeg, and those children are spawned
 * CREATE_NO_WINDOW so they have no console and never see the Ctrl+C
 * themselves. On a 50 MB FLAC that is tens of seconds, hence the printed
 * hint that a second Ctrl+C aborts immediately -- at the cost of the
 * guarantees above. CTRL_CLOSE_EVENT is deliberately not handled: Windows
 * gives it ~5 s before killing the process regardless, which is not
 * enough to finish a transcode, so pretending to handle it would only
 * turn a clean kill into a truncated one. */
#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    if (InterlockedExchange(&g_stop, 1) != 0)
        return FALSE;   /* second one: let the default handler kill us */
    printf("\n^C  stopping after the current file"
           " (Ctrl+C again aborts now, losing it)\n");
    return TRUE;
}
#else
static void on_sigint(int sig)
{
    if (g_stop) {          /* second one: restore default, die for real */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_stop = 1;
    printf("\n^C  stopping after the current file"
           " (Ctrl+C again aborts now, losing it)\n");
}
#endif

/* Directory anchors are zero-length records named "dir/". vol_sweep_one
   reads one, finds nothing a codec wants, and reports "skipped (no
   codec)" -- which on a music tree is hundreds of lines of noise burying
   the files that did something, and pads the denominator the percentage
   is measured against. --dirs brings them back. */
static int is_dir_anchor(const char *n, uint32_t len)
{
    return len > 0 && n[len - 1] == '/';
}

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

/* Fixed-width so columns line up down the run; four buffers because a
   single line prints several of these in one printf. */
static const char *human(uint64_t b)
{
    static char buf[4][32];
    static int slot = 0;
    char *s = buf[slot = (slot + 1) & 3];
    if (b >= 1ull << 30) sprintf(s, "%7.2f GB", (double)b / (1ull << 30));
    else if (b >= 1ull << 20) sprintf(s, "%7.2f MB", (double)b / (1ull << 20));
    else if (b >= 1ull << 10) sprintf(s, "%7.2f KB", (double)b / (1ull << 10));
    else sprintf(s, "%7llu B ", (unsigned long long)b);
    return s;
}

static const char *hms(double ms)
{
    static char buf[2][16];
    static int slot = 0;
    char *s = buf[slot = (slot + 1) & 1];
    unsigned t = (unsigned)(ms / 1000.0 + 0.5);
    if (t >= 3600) sprintf(s, "%u:%02u:%02u", t / 3600, (t / 60) % 60, t % 60);
    else sprintf(s, "%u:%02u", t / 60, t % 60);
    return s;
}

/* Paths here run to 200+ chars of album directory, and the basename is the
   part that identifies the file. Keep the tail. */
static const char *shortname(const char *n, char *out, size_t cap)
{
    size_t len = strlen(n);
    if (len < cap) { strcpy(out, n); return out; }
    out[0] = out[1] = out[2] = '.';
    memcpy(out + 3, n + len - (cap - 4), cap - 4);
    out[cap - 1] = 0;
    return out;
}

/* one live segment to hash */
typedef struct {
    uint8_t  hash[32];
    uint64_t inode, lba, pba;
    uint32_t phys;      /* physical blocks of this segment */
    uint8_t  zone;
} dedup_seg;

static int dedup_cmp(const void *a, const void *b)
{
    const dedup_seg *x = (const dedup_seg *)a, *y = (const dedup_seg *)b;
    int c = memcmp(x->hash, y->hash, 32);
    if (c) return c;
    if (x->inode != y->inode) return x->inode < y->inode ? -1 : 1;
    if (x->lba  != y->lba)  return x->lba  < y->lba  ? -1 : 1;
    return 0;
}

/* block-level dedup: BLAKE3 over each live segment, merge duplicates */
static int sweep_dedupe(invfs_volume *vol, uint64_t p, uint64_t inode_area_end)
{
    const invfs_superblock *sb = vol_sb(vol);
    size_t n = 0, cap = 1 << 16;
    dedup_seg *segs = (dedup_seg *)malloc(cap * sizeof(dedup_seg));
    if (!segs) return -1;
    blake3_hasher hx;
    /* These traced a bug during bring-up and print one line per record and
       per segment. Now that dedupe runs as part of every sweep, that is
       thousands of lines of stderr on a real tree -- behind the flag. */
    const int dbg = getenv("INVFS_DEBUG") != NULL;

    /* pass 1: hash every live segment */
    if (dbg) fprintf(stderr, "dedupe: pass1 start p=%llu\n", (unsigned long long)p);
    while (p + sizeof(invfs_inode_rec) <= inode_area_end) {
        invfs_inode_rec h;
        uint8_t *rec = NULL;
        uint32_t num_blocks = 0, rec_size, ast_hdr_len = 0;
        size_t base;
        uint64_t i;
        if (vol_read_raw(vol, p, &h, sizeof(h)) != 0) break;
        if (dbg)
            fprintf(stderr, "dedupe: rec at %llu magic=%08x inode=%llu\n",
                    (unsigned long long)p, h.magic, (unsigned long long)h.inode_id);
        if (h.magic != 0x444F4E49u) {
            if (h.magic == 0x544C4544u) { p += (uint64_t)h.rec_len + 4; continue; }
            break;
        }
        if (h.name_len > INVFS_MAX_NAME) { p += (uint64_t)h.rec_len + 4; continue; }
        if (h.rec_len < INVFS_REC_HDR_LEN + h.name_len + 1 ||
            h.rec_len > INVFS_MAX_REC_LEN) { p += (uint64_t)h.rec_len + 4; continue; }

        rec = (uint8_t *)malloc(h.rec_len);
        if (!rec) { free(segs); return -1; }
        if (vol_read_raw(vol, p, rec, h.rec_len) != 0) { free(rec); free(segs); return -1; }
        base = (size_t)(invfs_rec_body((invfs_inode_rec *)rec) - rec);
        if (vol_find(vol, ((invfs_inode_rec *)rec)->name) != h.inode_id) {
            free(rec);
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        /* WP22a: the recipe header is v1 (16 B) or v2 (24 B) -- parse it,
         * never pun fixed offsets */
        {
            invfs_ast_hdr ah;
            if (invfs_ast_hdr_parse(rec + base, h.rec_len - base, &ah) != 0) {
                free(rec);
                p += (uint64_t)h.rec_len + 4;
                continue;
            }
            num_blocks = ah.num_blocks;
            ast_hdr_len = ah.hdr_len;
        }
        rec_size = (uint32_t)base + ast_hdr_len +
                   (uint32_t)num_blocks * 24;
        if (rec_size > h.rec_len) {   /* truncated recipe: not ours */
            free(rec);
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        for (i = 0; i < num_blocks; i++) {
            uint32_t *e = (uint32_t *)((uint8_t *)rec +
                          base + ast_hdr_len + i * 24);
            uint64_t pba, len;
            uint8_t *blob;
            uint32_t hdr4;
            uint32_t bits = e[4];                 /* zone:2 algo:6 block_id:24 */
            uint32_t zone = (bits >> 0) & 3;
            uint32_t algo = (bits >> 2) & 0x3F;
            uint32_t block_id = bits >> 8;
            if (algo == INVFS_ALGO_JXL || algo == INVFS_ALGO_APE)
                continue;  /* whole-file blobs: unique by construction */
            if (zone == INVFS_ZONE_TEXT)
                continue;  /* WP10 §11: shared PPMd batches belong to the
                              owner inode; never dedup candidates */
            if (vol_lookup_entry(vol, h.inode_id, block_id, &pba, &len) != 0)
                continue;
            if (vol_read_raw(vol, pba * INVFS_BLOCK_SIZE, &hdr4, 4) != 0) continue;
            blob = (uint8_t *)malloc(hdr4);
            if (!blob) continue;
            if (vol_read_raw(vol, pba * INVFS_BLOCK_SIZE + 4, blob, hdr4) != 0) {
                free(blob); continue;
            }
            blake3_hasher_init(&hx);
            blake3_hasher_update(&hx, blob, hdr4);
            blake3_hasher_finalize(&hx, segs[n].hash, 32);
            segs[n].inode = h.inode_id;
            segs[n].lba = block_id;
            segs[n].pba = pba;
            segs[n].phys = (uint32_t)len;
            segs[n].zone = zone;
            free(blob);
            if (++n >= cap) {
                cap *= 2;
                dedup_seg *ns = (dedup_seg *)realloc(segs, cap * sizeof(dedup_seg));
                if (!ns) { free(segs); return -1; }
                segs = ns;
            }
        }
        free(rec);
        p += (uint64_t)h.rec_len + 4;
        /* Hashing every live segment is minutes on a full volume. Stopping
           here is free: pass 1 only reads, so nothing is half-done. */
        if (g_stop) { free(segs); return 0; }
    }
    printf("dedupe: hashed %zu live segments\n", n);

    /* pass 2: sort by hash, find duplicates */
    qsort(segs, n, sizeof(dedup_seg), dedup_cmp);
    {
        size_t i = 0, merged = 0;
        uint64_t freed_blocks = 0;
        uint8_t *freed = (uint8_t *)calloc((size_t)(sb->total_blocks / 8 + 1), 1);
        if (!freed) { free(segs); return -1; }
        while (i < n) {
            size_t j = i + 1;
            while (j < n && memcmp(segs[i].hash, segs[j].hash, 32) == 0) j++;
            if (j - i > 1) {
                /* canonical = first physical copy */
                uint64_t canon_pba = segs[i].pba;
                for (size_t k = i + 1; k < j; k++) {
                    dedup_seg *s = &segs[k];
                    if (s->pba == canon_pba) continue;  /* already shared */
                /* remap to canonical copy */
                {
                    uint64_t old_len = 0;
                    if (vol_lookup_entry(vol, s->inode, s->lba, &s->pba, &old_len) == 0) {
                        vol_l2p_remove(vol, s->inode, s->lba);
                        vol_map(vol, s->inode, s->lba, canon_pba, (uint32_t)(old_len ? old_len : s->phys));
                    }
                }
                    /* free old copy once */
                    if (s->pba < sb->total_blocks && !(freed[s->pba >> 3] & (1 << (s->pba & 7)))) {
                        freed[s->pba >> 3] |= (uint8_t)(1 << (s->pba & 7));
                        vol_free_blocks(vol, s->pba, s->phys);
                        freed_blocks += s->phys;
                    }
                    merged++;
                }
            }
            i = j;
        }
        printf("dedupe: merged %zu segments, freed %llu blocks\n",
               merged, (unsigned long long)freed_blocks);
        free(freed);
    }
    free(segs);
    return 0;
}

/* Sequential reader over the inode area.
   blkio aligns every read to a 4 KB sector through a bounce buffer, so
   fetching one 296-byte record header costs a whole sector off the device.
   Records run a few hundred bytes, so the record-at-a-time walk re-read the
   same sector a dozen times over: on the 266 MB inode area of a real volume
   that is gigabytes of I/O and hundreds of thousands of syscalls, which is
   why the survey sat silent for minutes before the first progress line.
   One aligned megabyte at a time reads each sector once instead.

   Safe only because the survey runs before the main loop touches anything:
   the area is quiescent, so a buffered copy cannot go stale. The main loop
   still reads through vol_read_raw for exactly that reason -- it appends
   records and writes tombstones as it goes. */
#define SURVEY_BUF (1024u * 1024u)

typedef struct {
    invfs_volume *vol;
    uint8_t *buf;      /* NULL => fall back to unbuffered reads */
    uint64_t base;     /* volume offset of buf[0] */
    size_t   len;      /* valid bytes */
    uint64_t end;      /* never read past this */
} scan_reader;

static int scan_read(scan_reader *r, uint64_t pos, void *out, size_t len)
{
    if (!r->buf)
        return vol_read_raw(r->vol, pos, out, len);
    if (pos < r->base || pos + len > r->base + r->len) {
        /* Refill aligned so blkio's bounce path does one whole-chunk read
           rather than splitting on a partial head sector. */
        uint64_t base = pos & ~(uint64_t)(INVFS_BLOCK_SIZE - 1);
        size_t want = SURVEY_BUF;
        if (base >= r->end) return -1;
        if (base + want > r->end) want = (size_t)(r->end - base);
        if (base + want < pos + len) return -1;   /* record crosses the end */
        if (vol_read_raw(r->vol, base, r->buf, want) != 0) return -1;
        r->base = base;
        r->len  = want;
    }
    memcpy(out, r->buf + (size_t)(pos - r->base), len);
    return 0;
}

/* Walk the inode area counting only what the main loop will actually
   process, so the progress line has a denominator. This reads record
   headers, not file content, but the area is hundreds of megabytes on a
   real volume -- not the "few hundred KB" an earlier comment here claimed
   -- so it is buffered and reports progress of its own. Skipping it would
   leave "processing file 47" with no idea whether 47 is nearly done or
   barely started. */
static void sweep_survey(invfs_volume *vol, uint64_t p, uint64_t end,
                         int limit, int want_dirs,
                         int *files_out, uint64_t *bytes_out)
{
    int files = 0;
    uint64_t bytes = 0;
    uint64_t span = end > p ? end - p : 0;
    uint64_t start = p, next_tick = p;
    scan_reader r;

    r.vol = vol; r.base = 0; r.len = 0; r.end = end;
    r.buf = (uint8_t *)malloc(SURVEY_BUF);   /* NULL is handled, not fatal */

    while (p + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        char name[INVFS_MAX_NAME + 1];
        if (scan_read(&r, p, &h, sizeof(h)) != 0) break;
        if (p >= next_tick && span) {
            printf("\r       scanned %3llu%%",
                   (unsigned long long)((p - start) * 100 / span));
            fflush(stdout);
            next_tick = p + span / 20 + 1;
        }
        if (h.magic != INODE_REC_MAGIC) {
            if (h.magic == TOMBSTONE_MAGIC) { p += (uint64_t)h.rec_len + 4; continue; }
            break;
        }
        if (h.name_len > INVFS_MAX_NAME) { p += (uint64_t)h.rec_len + 4; continue; }
        if (h.rec_len < INVFS_REC_HDR_LEN + h.name_len + 1) {
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        if (scan_read(&r, p + INVFS_REC_HDR_LEN, name, h.name_len) != 0) break;
        name[h.name_len] = 0;
        if (!want_dirs && is_dir_anchor(name, h.name_len)) {
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        if (vol_find(vol, name) == h.inode_id) {
            files++;
            bytes += h.file_size;
            /* Stop where the main loop will stop, or --limit 2 counts two
               files against every byte on the volume and the run ends at
               "40%". The survey has to model the same walk, not a longer
               one. */
            if (limit && files >= limit) break;
        }
        p += (uint64_t)h.rec_len + 4;
    }
    if (span) { printf("\r                    \r"); fflush(stdout); }
    free(r.buf);
    *files_out = files;
    *bytes_out = bytes;
}

int main(int argc, char **argv)
{
    const char *img;
    invfs_volume *vol;
    const invfs_superblock *sb;
    int err;
    uint64_t p, p_start, inode_area_end, bm;
    int limit = 0, swept_files = 0, dedupe = 0, want_dirs = 0, stopped = 0;
    int no_dedupe = 0;
    const char *arg = argv[1];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-sweep <image> [--limit N] [--dirs] [--dedupe] [--no-dedupe]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 2) {
        fprintf(stderr, "usage: invf-sweep <image> [--limit N] [--dirs]"
                        " [--dedupe] [--no-dedupe]\n");
        return 2;
    }
    img = arg;
    if (argc >= 4 && strcmp(argv[2], "--limit") == 0)
        limit = atoi(argv[3]);
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--dedupe") == 0) dedupe = 1;
        if (strcmp(argv[a], "--no-dedupe") == 0) no_dedupe = 1;
        if (strcmp(argv[a], "--dirs") == 0) want_dirs = 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
    signal(SIGINT, on_sigint);
#endif
    vol = vol_open(img, &err);
    if (!vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }
    /* WP10 memory policy: same size grammar as INVFS_ARC_BYTES in volume.c;
     * unset keeps the volume default. */
    {
        const char *dl = getenv("INVFS_DEC_MEM_LIMIT");
        if (dl) {
            char *endp = NULL;
            unsigned long long want = strtoull(dl, &endp, 10);
            unsigned long long mult = 1;
            int ok = (endp != dl);
            if (ok) {
                while (*endp == ' ' || *endp == '\t') endp++;
                switch (*endp) {
                    case 'k': case 'K': mult = 1024ull; endp++; break;
                    case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
                    case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
                    default: break;
                }
                if (*endp == 'b' || *endp == 'B') endp++;
                while (*endp == ' ' || *endp == '\t') endp++;
                if (*endp != '\0') ok = 0;
                if (want > (unsigned long long)SIZE_MAX / mult) ok = 0;
            }
            if (ok)
                vol_set_dec_mem_limit(vol, (uint64_t)(want * mult));
            else
                fprintf(stderr, "[sweep] INVFS_DEC_MEM_LIMIT=\"%s\" is not a "
                                "size; ignored\n", dl);
        }
    }
    sb = vol_sb(vol);
    bm = (sb->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    p = (sb->metadata_zone_start + bm + INVFS_JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
    p_start = p;   /* the walk advances p; dedupe needs the start again */
    inode_area_end = vol_inode_area_pos(vol);  /* CRC-validated extent */

    if (dedupe) {
        int r = sweep_dedupe(vol, p, inode_area_end);
        if (vol_flush(vol) != 0) r = -1;
        vol_close(vol);
        return r;
    }

    {
    int total_files = 0, seen = 0, skipped = 0, failed = 0;
    uint64_t total_bytes = 0, done_bytes = 0;
    uint64_t free0 = vol_free_blocks_cached(vol);
    double t_start;
    char nb[64];

    printf("sweep: surveying inode area...\n");
    {
        /* Printed because this walk is the part that regressed: reading it
           record-at-a-time cost a 4 KB sector per 296-byte header and the
           survey sat silent for minutes. A number here makes the next
           regression visible instead of merely slow. */
        double t_sv = now_ms();
        sweep_survey(vol, p, inode_area_end, limit, want_dirs,
                     &total_files, &total_bytes);
        printf("sweep: %d file(s), %s to process  (survey %.2fs)\n",
               total_files, human(total_bytes), (now_ms() - t_sv) / 1000.0);
    }
    printf("       free before: %s\n\n", human(free0 * INVFS_BLOCK_SIZE));
    t_start = now_ms();

    while (p + sizeof(invfs_inode_rec) <= inode_area_end) {
        invfs_inode_rec h;
        char name[INVFS_MAX_NAME + 1];
        int rc;
        double t0, dt;
        uint64_t before, after;
        int64_t delta;
        const char *what;
        if (vol_read_raw(vol, p, &h, sizeof(h)) != 0) break;
        if (h.magic != 0x444F4E49u) {
            if (h.magic == 0x544C4544u) {  /* tombstone: skip */
                p += (uint64_t)h.rec_len + 4;
                continue;
            }
            break;
        }
        /* ensure name is NUL-terminated and safe for strlen/strcmp */
        if (h.name_len > INVFS_MAX_NAME) {
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        if (h.rec_len < INVFS_REC_HDR_LEN + h.name_len + 1) {
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        if (vol_read_raw(vol, p + INVFS_REC_HDR_LEN, name, h.name_len) != 0) break;
        name[h.name_len] = 0;
        /* Directory anchors carry no data; the survey already left them out
           of the denominator, so processing them here would print "[412/410]". */
        if (!want_dirs && is_dir_anchor(name, h.name_len)) {
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        /* only process the latest version of a name (idempotent sweep) */
        if (vol_find(vol, name) != h.inode_id) {
            p += (uint64_t)h.rec_len + 4;
            continue;
        }
        if (g_stop) { stopped = 1; break; }
        seen++;

        /* Name and size first, on their own line and before the work starts:
           a 50 MB FLAC takes tens of seconds, and a progress display that
           only prints on completion looks identical to a hang. */
        printf("[%3d/%3d] %s  %s\n", seen, total_files,
               human(h.file_size), shortname(name, nb, sizeof nb));

        before = vol_free_blocks_cached(vol);
        t0 = now_ms();
        /* unified per-inode core: ZIP explode / FLAC / TAR / GZ / PNG /
           generic shadow move (vol_sweep_one) */
        rc = vol_sweep_one(vol, h.inode_id, name);
        dt = now_ms() - t0;
        after = vol_free_blocks_cached(vol);
        delta = (int64_t)after - (int64_t)before;   /* + means space reclaimed */
        done_bytes += h.file_size;

        if (rc > 0) {
            swept_files++;
            switch (rc) {
            case 1:  what = "exploded -> members";       break;
            case 2:  what = "FLAC -> APE + recipe";      break;
            case 3:  what = "tar -> parts + recipe";     break;
            case 4:  what = "gz -> parts + recipe";      break;
            case 5:  what = "PNG -> JXL + recipe";       break;
            case 7:  what = "JPEG -> JXL (lossless)";    break;
            case 8:  what = "MP3 -> PMP (packMP3)";      break;
            case 9:  what = "text -> PPMd batch";        break;
            case 10: what = "binary -> ZSTD batch";      break;   /* WP14a */
            default: what = "RAW -> ZSTD-19";            break;
            }
        } else if (rc < 0) {
            /* A failure used to abort the run, so one bad file out of 323
               cost the other 322 and printed no summary. Counting it and
               carrying on is the better trade at this scale -- but not a
               free one: vol_sweep_file has error returns after alloc_blocks
               and vol_map (volume.c ~2908, ~2922), so a failure here can
               leave blocks allocated but unreferenced. That is a space leak
               fsck reclaims as orphans, not lost data -- the summary says so
               when the count is non-zero. */
            failed++;
            what = "FAILED";
        } else {
            skipped++;
            what = "skipped (no codec)";
        }

        {
            /* Reclaimed bytes are the point of the whole operation, so show
               them per file and running. A negative delta is real and worth
               seeing: APE can come out larger than the FLAC it replaced. */
            double frac = total_bytes ? (double)done_bytes / (double)total_bytes : 0;
            double el = now_ms() - t_start;
            int64_t dbytes = delta * (int64_t)INVFS_BLOCK_SIZE;
            printf("          %-22s %6.1fs  %s%s  |  %4.1f%%  elapsed %s",
                   what, dt / 1000.0,
                   dbytes >= 0 ? "+" : "-", human(dbytes >= 0 ? (uint64_t)dbytes
                                                              : (uint64_t)-dbytes),
                   frac * 100.0, hms(el));
            /* ETA only once there is enough of a sample to mean anything;
               the first file is usually unrepresentative. */
            if (frac > 0.02 && seen > 1)
                printf("  eta %s", hms(el / frac - el));
            printf("\n");
        }

        /* Count files looked at, not files successfully swept. The survey
           computes the denominator by walking records, and it cannot know in
           advance which ones a codec will decline -- so counting sweeps here
           made "[3/2]" possible as soon as one file was skipped. This also
           makes --limit mean what it reads like: examine at most N files. */
        if (limit && seen >= limit) break;
        p += (uint64_t)h.rec_len + 4;
    }

    /* Dedupe in the same run. It was a separate --dedupe mode, which meant
       nobody ever got the space back without knowing to make a second pass:
       the transcode above is what creates the duplicates worth finding
       (every album's cover art lands in Shadow as identical segments), so
       running it right after is when there is most to merge. --no-dedupe
       opts out; --dedupe still means "only this, skip the transcode". */
    if (!no_dedupe && !stopped) {
        printf("\ndedupe: scanning live segments...\n");
        if (sweep_dedupe(vol, p_start, inode_area_end) != 0)
            fprintf(stderr, "dedupe: pass failed (sweep results are intact)\n");
    }

    /* WP10 §7: text-batch GC runs after the dedupe pass (so dedupe never
     * sees a zone==TEXT entry) and before the flush seals new batches. */
    {
        int gcrc = vol_tz_gc(vol);
        if (gcrc > 0)
            printf("text gc: %u dead batches reclaimed\n", (unsigned)gcrc);
        else if (gcrc < 0)
            fprintf(stderr, "text gc failed (rc=%d)\n", gcrc);
    }

    /* WP10: seal the partial text batch the walk accumulated. rc==1 means
     * nothing pending -- not an error, not worth a line. */
    {
        int tzrc = vol_tz_flush(vol);
        if (tzrc == 0)
            printf("text batches flushed: rc=%d\n", tzrc);
        else if (tzrc < 0)
            fprintf(stderr, "text batch flush failed (rc=%d)\n", tzrc);
    }

    if (vol_flush(vol) != 0) {
        fprintf(stderr, "flush failed\n");
        vol_close(vol);
        return 1;
    }
    {
        uint64_t free1 = vol_free_blocks_cached(vol);
        int64_t saved = ((int64_t)free1 - (int64_t)free0) * (int64_t)INVFS_BLOCK_SIZE;
        double el = now_ms() - t_start;
        printf("\nsweep: %d swept", swept_files);
        if (skipped) printf(", %d skipped", skipped);
        if (failed)  printf(", %d FAILED", failed);
        if (stopped) printf(", stopped at %d/%d", seen, total_files);
        printf(" in %s\n", hms(el));
        printf("       free after: %s  (%s%s reclaimed)\n",
               human(free1 * INVFS_BLOCK_SIZE),
               saved >= 0 ? "+" : "-",
               human(saved >= 0 ? (uint64_t)saved : (uint64_t)-saved));
        if (failed)
            printf("       run invf-fsck: a failed transcode can leak blocks\n");
        if (stopped)
            printf("       run again to finish: swept files are skipped on the"
                   " next pass\n");
    }
    vol_close(vol);
    /* Ctrl+C is not a failure -- the volume is consistent and the summary
       says where it stopped. Only a real transcode error earns a nonzero
       exit, or a script wrapping this cannot tell the two apart. */
    return failed ? 1 : 0;
    }
}
