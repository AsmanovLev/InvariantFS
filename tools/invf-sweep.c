/*
 * invf-sweep.c — offline sweep driver for Linux
 *
 *   invf-sweep <image> [--dry-run]
 *                      [--seal|--unseal]
 *                      [--redundant-blocks <f>]
 *                      [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]
 *                      [--free-redundant]
 *                      [--redundant-bench]
 *
 * Walks live records (same CRC-validated scan as invf-ls), feeds every
 * regular file with segments to vol_sweep_one() — the unified per-inode
 * dispatch (containers/transcodes/text-batching/generic ZSTD-19). Then the
 * per-segment dedupe pass (vol_sweep_dedupe, WP12(h)) merges identical
 * stored segments. Text candidates defer into the volume's accumulator and
 * are sealed into shared PPMd batches by vol_tz_flush() at the end of the
 * run, after the dead-batch GC (vol_tz_gc). The author's sweep.c CLI is
 * Windows-only.
 *
 * WP20: --seal re-seals the shadow-zone XOR parity AFTER the sweep is fully
 * flushed (idempotent check-and-update, see vol_seal); --unseal frees every
 * parity block and removes the owners, without sweeping.
 *
 * WP20b: --redundant-blocks <f> configures layer-1 XOR with stripe
 * k = clamp(round(1/f), 8..128) (f = overhead fraction);
 * --redundant-paranoic <f>[:algo] adds layer-2 RS(32+m2, 32) with
 * m2 = clamp(round(f*32/(1-f)), 2..8) (algo picked by --redundant-bench on
 * first use, persisted in the RDP0 descriptor); --free-redundant removes
 * both layers and the descriptor (same as --unseal). A bare run (no
 * redundancy flags) on a volume with a live descriptor auto-reseals after
 * the sweep. --redundant-bench prints rs-vm vs rs-cauchy MB/s and exits.
 *
 * WP21: every non-dry run arms a sweep checkpoint (CKP0) BEFORE the walk
 * and holds the blocks it retires in the "\x01reten" retention registry
 * (see vol_ckp_begin); invf-rollback undoes the last sweep from it.
 * --realize is the point of no return: the previous run's retained blocks
 * are freed and CKP0 is cleared, then a normal (freshly checkpointed)
 * sweep proceeds. The next bare sweep auto-realizes the same way --
 * K=1 means one checkpoint, and only --realize or invf-rollback resolve
 * it by hand. Checkpointing is declined (the sweep runs without one) on
 * read-only/recovering volumes, under a live redundancy seal (rollback
 * would invalidate the parity stripes), and with INVFS_CHECKPOINT=0.
 *
 * WP22e: --fast narrows the per-file decision to "generic or nothing"
 * (RAW files take the per-segment profile recompress; classification,
 * container decomposition, codec transcodes, batching, dedupe and the
 * promotion pass never run; the walk, the checkpoint and the reports are
 * the usual ones).
 *
 * WP22e: the run ends with online inode-area compaction when the dead
 * share of the area (superseded versions + tombstones) exceeds ~30% of the
 * used bytes ("inode area compacted: X -> Y bytes"). Never while a CKP0
 * checkpoint is live (rollback truncates to absolute checkpoint positions)
 * or on a read-only volume; INVFS_NO_COMPACT=1 disables the automatic
 * pass. --compact forces the pass alone (no walk, no checkpoint).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "codec.h"
#include "rs.h"

typedef struct sw_bucket { struct sw_bucket *next; int slot; } sw_bucket;

/* WP14b: container-part deferrals ("name!partN") are aggregated per
 * container and printed as one summary line at the end of the walk --
 * a Silesia mozilla/samba/xml run would otherwise log 1573 near-identical
 * per-part lines. */
typedef struct {
    char prefix[256];   /* container name including the '!' */
    int  n_text;        /* parts deferred to PPMd batches */
    int  n_bin;         /* parts deferred to ZSTD batches */
} part_agg;

static part_agg *g_parts;
static size_t   g_parts_n, g_parts_cap;

static void part_agg_add(const char *name, int binary)
{
    const char *bang = strchr(name, '!');
    size_t plen = bang ? (size_t)(bang - name) + 1 : 0;
    size_t i;

    if (!plen || plen >= 256) return;
    for (i = 0; i < g_parts_n; i++)
        if (strncmp(g_parts[i].prefix, name, plen) == 0 &&
            g_parts[i].prefix[plen] == 0)
            break;
    if (i == g_parts_n) {
        if (g_parts_n == g_parts_cap) {
            size_t nc = g_parts_cap ? g_parts_cap * 2 : 16;
            part_agg *na = realloc(g_parts, nc * sizeof *na);
            if (!na) return;
            g_parts = na;
            g_parts_cap = nc;
        }
        memset(&g_parts[i], 0, sizeof g_parts[i]);
        memcpy(g_parts[i].prefix, name, plen);
        g_parts_n++;
    }
    if (binary) g_parts[i].n_bin++;
    else        g_parts[i].n_text++;
}

static void part_agg_print(void)
{
    size_t i;
    for (i = 0; i < g_parts_n; i++) {
        if (g_parts[i].n_bin)
            printf("  %s*: %d parts -> ZSTD batch\n", g_parts[i].prefix,
                   g_parts[i].n_bin);
        if (g_parts[i].n_text)
            printf("  %s*: %d parts -> PPMd batch\n", g_parts[i].prefix,
                   g_parts[i].n_text);
    }
    free(g_parts);
    g_parts = NULL;
    g_parts_n = g_parts_cap = 0;
}

static uint64_t sw_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static int sw_find(sw_bucket **tab, size_t mask, char (*names)[256],
                   const char *name)
{
    const sw_bucket *b;
    if (!tab) return -1;
    for (b = tab[sw_hash(name) & mask]; b; b = b->next)
        if (strcmp(names[b->slot], name) == 0) return b->slot;
    return -1;
}

static void sw_insert(sw_bucket ***tabp, size_t *maskp, size_t *countp,
                      char (*names)[256], int slot)
{
    sw_bucket *b;
    size_t h;
    if (!*tabp) {
        *tabp = (sw_bucket **)calloc(1024, sizeof **tabp);
        *maskp = 1023;
    } else if (*countp > *maskp) {
        size_t ncap = (*maskp + 1) * 2, i;
        sw_bucket **nt = (sw_bucket **)calloc(ncap, sizeof *nt);
        if (nt) {
            for (i = 0; i <= *maskp; i++) {
                sw_bucket *e = (*tabp)[i];
                while (e) {
                    sw_bucket *nx = e->next;
                    size_t nb = sw_hash(names[e->slot]) & (ncap - 1);
                    e->next = nt[nb]; nt[nb] = e;
                    e = nx;
                }
            }
            free(*tabp);
            *tabp = nt;
            *maskp = ncap - 1;
        }
    }
    b = (sw_bucket *)malloc(sizeof *b);
    if (!b) return;
    b->slot = slot;
    h = sw_hash(names[slot]) & *maskp;
    b->next = (*tabp)[h];
    (*tabp)[h] = b;
    (*countp)++;
}

int main(int argc, char **argv)
{
    invfs_volume *vol;
    const invfs_superblock *sb;
    int err, dry = 0, seal = 0, unseal = 0, bench = 0, realize = 0;
    int fast = 0, compact_only = 0;
    double rb_f = -1.0, rp_f = -1.0;   /* <0: flag absent */
    int rp_algo = 0;                   /* explicit :rs-vm/:rs-cauchy suffix */
    int auto_reseal = 0;
    uint64_t bm, area_start, area_end, p;
    int count = 0, cap = 0, swept = 0, skipped = 0, failed = 0;
    char (*names)[256] = NULL;
    uint64_t *inodes = NULL;
    uint64_t *sizes = NULL;
    uint64_t *poss = NULL;   /* each name's current record position
                                (v2 position-kill matching, WP22c) */
    const char *img;
    sw_bucket **tab = NULL;
    size_t tmask = 0, tcount = 0;
    int i;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <image> [--dry-run] [--fast] [--compact]\n"
                "           [--seal|--unseal]\n"
                "           [--redundant-blocks <f>]\n"
                "           [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]\n"
                "           [--free-redundant] [--redundant-bench]\n"
                "           [--realize]  (accept the last sweep: free its\n"
                "                         retention registry, clear CKP0)\n"
                "  --fast      cheap pass: RAW files take the generic\n"
                "              per-segment recompress only (no classification,\n"
                "              transcodes, decomposition, batching or dedupe)\n"
                "  --compact   run only the inode-area compaction pass\n",
                argv[0]);
        return 2;
    }
    img = argv[1];
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--dry-run") == 0) {
            dry = 1;
        } else if (strcmp(a, "--realize") == 0) {
            realize = 1;
        } else if (strcmp(a, "--fast") == 0) {
            fast = 1;
        } else if (strcmp(a, "--compact") == 0) {
            compact_only = 1;
        } else if (strcmp(a, "--seal") == 0) {
            seal = 1;
        } else if (strcmp(a, "--unseal") == 0 ||
                   strcmp(a, "--free-redundant") == 0) {
            unseal = 1;
        } else if (strcmp(a, "--redundant-bench") == 0) {
            bench = 1;
        } else if (strcmp(a, "--redundant-blocks") == 0 && i + 1 < argc) {
            char *endp = NULL;
            rb_f = strtod(argv[++i], &endp);
            if (endp == argv[i] || *endp != '\0' || !(rb_f > 0.0)) {
                fprintf(stderr, "--redundant-blocks: bad fraction '%s'\n",
                        argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--redundant-paranoic") == 0 && i + 1 < argc) {
            char *endp = NULL;
            const char *colon;
            rp_f = strtod(argv[++i], &endp);
            if (endp == argv[i] || !(rp_f > 0.0 && rp_f < 1.0) ||
                (*endp != '\0' && *endp != ':')) {
                fprintf(stderr, "--redundant-paranoic: bad fraction '%s'\n",
                        argv[i]);
                return 2;
            }
            colon = strchr(argv[i], ':');
            if (colon) {
                if (strcmp(colon + 1, "rs-vm") == 0)
                    rp_algo = RS_ALGO_VM;
                else if (strcmp(colon + 1, "rs-cauchy") == 0)
                    rp_algo = RS_ALGO_CAUCHY;
                else {
                    fprintf(stderr, "--redundant-paranoic: unknown algo "
                                    "'%s'\n", colon + 1);
                    return 2;
                }
            }
        } else {
            fprintf(stderr, "unknown flag '%s'\n", a);
            return 2;
        }
    }
    if (dry + unseal + bench > 0 &&
        (seal || rb_f >= 0 || rp_f >= 0 || realize)) {
        fprintf(stderr, "conflicting flags\n");
        return 2;
    }
    if (seal && (rb_f >= 0 || rp_f >= 0)) {
        fprintf(stderr, "--seal conflicts with --redundant-*\n");
        return 2;
    }
    /* --compact is the pass alone: no walk, no checkpoint, no seal */
    if (compact_only &&
        (dry || fast || seal || unseal || bench || realize ||
         rb_f >= 0 || rp_f >= 0)) {
        fprintf(stderr, "--compact conflicts with the sweep/seal flags\n");
        return 2;
    }

    /* per-file lines go to stdout, the summary to stderr: unbuffered, or a
     * redirected log tears a line at every 4 KB flush boundary */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* WP20b --redundant-bench: synthetic head-to-head, no volume needed
     * (k=32, m=4, 64 MiB of data in RAM) */
    if (bench) {
        double vm, ca;
        if (rs_bench(32, 4, INVFS_BLOCK_SIZE, 512, &vm, &ca) != 0) {
            fprintf(stderr, "--redundant-bench: benchmark failed\n");
            return 1;
        }
        printf("[bench] rs-vm: %.1f MB/s, rs-cauchy: %.1f MB/s "
               "(k=32, m=4, 64 MiB data); winner: %s\n",
               vm, ca, vm >= ca ? "rs-vm" : "rs-cauchy");
        return 0;
    }

    /* WP16b: the codec profile rides the environment (INVFS_PROFILE).
     * Capture the setting BEFORE vol_open publishes the default into the
     * env: a default run must produce byte-identical LOGS too. */
    {
        int prof_from_env = getenv("INVFS_PROFILE") != NULL;
        vol = vol_open(img, &err);
        if (!vol) {
            fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
            return 1;
        }
        if (prof_from_env) {
            int ga = invfs_profile_generic_algo((int)vol_get_profile(vol));
            if (ga == INVFS_ALGO_ZSTD)
                fprintf(stderr, "profile: %s (generic zstd level %d)\n",
                        invfs_profile_name((int)vol_get_profile(vol)),
                        invfs_profile_zstd_level((int)vol_get_profile(vol)));
            else
                fprintf(stderr, "profile: %s (generic %s)\n",
                        invfs_profile_name((int)vol_get_profile(vol)),
                        ga == INVFS_ALGO_LZ4 ? "lz4" : "verbatim");
        }
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

    /* WP22e: an interrupted inode-area compaction left CMP0 armed and the
     * volume latched read-only -- sweeping on it would append onto a
     * possibly torn area. invf-fsck -f rolls the staged stream in
     * (idempotent) and clears the latch. */
    if (vol_compact_pending(vol)) {
        fprintf(stderr, "invf-sweep: %s: an interrupted inode-area "
                "compaction is pending; run invf-fsck -f %s to finish it "
                "first\n", img, img);
        vol_close(vol);
        return 1;
    }

    /* WP22e --compact: the compaction pass alone (no realize, no
     * checkpoint, no walk, no seal). The engine prints the outcome or the
     * decline reason (a live CKP0 checkpoint bars compaction: rollback
     * truncates to its absolute positions). */
    if (compact_only) {
        int crc;
        uint64_t before = 0, after = 0;
        crc = vol_inode_compact(vol, &before, &after);
        if (crc > 0) {
            printf("inode area compacted: %llu -> %llu bytes\n",
                   (unsigned long long)before, (unsigned long long)after);
            if (vol_flush(vol) != 0)
                fprintf(stderr, "warning: final flush failed\n");
        }
        vol_close(vol);
        return crc < 0 ? 1 : 0;
    }

    /* WP20b: apply the requested redundancy configuration (persisted into
     * the RDP0 descriptor by vol_seal at the end of the run) */
    if (rb_f >= 0 || rp_f >= 0) {
        uint32_t k1 = 0, m2 = 0;
        int l2 = -1;
        if (rb_f >= 0) {
            long lk = (long)(1.0 / rb_f + 0.5);
            if (lk < 8) lk = 8;
            if (lk > 128) lk = 128;
            k1 = (uint32_t)lk;
        }
        if (rp_f >= 0) {
            long lm = (long)(rp_f * 32.0 / (1.0 - rp_f) + 0.5);
            if (lm < 2) lm = 2;
            if (lm > 8) lm = 8;
            m2 = (uint32_t)lm;
            l2 = rp_algo;
            if (!l2) {
                /* no explicit suffix: keep the persisted algo; on the
                 * first paranoic configure the bench picks the winner */
                uint32_t ok1, om;
                int oa;
                vol_redun_state(vol, &ok1, &oa, &om);
                if (oa) {
                    l2 = oa;
                } else {
                    double vm, ca;
                    if (rs_bench(32, 4, INVFS_BLOCK_SIZE, 512,
                                 &vm, &ca) != 0) {
                        fprintf(stderr, "redundant-paranoic: internal "
                                        "bench failed\n");
                        vol_close(vol);
                        return 1;
                    }
                    l2 = vm >= ca ? RS_ALGO_VM : RS_ALGO_CAUCHY;
                    fprintf(stderr, "redundant-paranoic: bench picked %s "
                            "(rs-vm %.1f vs rs-cauchy %.1f MB/s)\n",
                            rs_algo_name(l2), vm, ca);
                }
            }
        }
        vol_redun_config(vol, k1, l2, m2);
    }
    /* auto-reseal: no redundancy flags but a live descriptor -> continue
     * the persisted configuration after the sweep */
    if (!seal && !unseal && !dry && rb_f < 0 && rp_f < 0) {
        uint32_t k1c, m2c;
        int l2c;
        if (vol_redun_state(vol, &k1c, &l2c, &m2c)) {
            auto_reseal = 1;
            fprintf(stderr, "redundancy: live RDP0 descriptor (k1=%u, "
                    "l2=%s m2=%u) -- auto-reseal after sweep\n",
                    (unsigned)k1c, rs_algo_name(l2c), (unsigned)m2c);
        }
    }

    /* WP20 --unseal: free all parity blocks and remove the owners; no sweep
     * walk runs (there is nothing to recompress, only seal state to drop). */
    if (unseal) {
        invfs_seal_report rep;
        if (vol_seal(vol, 1, &rep) != 0) {
            fprintf(stderr, "unseal failed\n");
            vol_close(vol);
            return 1;
        }
        printf("[unseal] %llu parity blocks freed, seal removed\n",
               (unsigned long long)rep.freed);
        if (vol_flush(vol) != 0)
            fprintf(stderr, "warning: final flush failed\n");
        vol_close(vol);
        return 0;
    }

    /* WP21+WP22d: resolve the previous sweep's checkpoint. --realize is
     * the standalone point of no return (free the retention registry,
     * clear CKP0), then a normal sweep proceeds. A bare sweep instead
     * goes straight to vol_ckp_begin, which arms the NEW checkpoint FIRST
     * and realizes the old registry only with the new net already live
     * (realize-after-arm): a torn sweep never leaves the volume with
     * neither a checkpoint nor intact data. A dry run touches nothing. A
     * decline (sealed / read-only / no room for the staging) never stops
     * the sweep -- the run just goes uncheckpointed. */
    if (!dry) {
        if (realize) {
            uint64_t rfree = 0;
            int rrc = vol_ckp_realize(vol, &rfree);
            /* on a read-only/recovering volume the realize refusal is not
             * fatal here -- the sweep's own machinery refuses the same way
             * (and --seal needs to print its own read-only diagnostic) */
            if (rrc < 0 && vol_write_enabled(vol)) {
                fprintf(stderr, "checkpoint: realizing the previous run "
                                "failed\n");
                vol_close(vol);
                return 1;
            }
            if (rrc > 0)
                fprintf(stderr, "checkpoint: previous run realized "
                        "(%llu retained blocks freed)\n",
                        (unsigned long long)rfree);
            else
                fprintf(stderr, "checkpoint: nothing to realize\n");
        }
        if (vol_ckp_begin(vol) < 0)
            fprintf(stderr, "checkpoint: arm failed; sweeping without "
                            "one\n");
    }

    bm = (sb->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    area_start = (sb->metadata_zone_start + bm + INVFS_JOURNAL_BLOCKS)
                 * INVFS_BLOCK_SIZE;
    area_end = vol_inode_area_pos(vol);
    p = area_start;

    /* collect live regular files */
    while (p + sizeof(invfs_inode_rec) <= area_end) {
        invfs_inode_rec h;
        char name[257];
        uint32_t crc_stored, crc_calc;
        uint8_t *rb;

        if (vol_read_raw(vol, p, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.name_len > 256 || h.rec_len < sizeof(invfs_inode_rec) ||
            h.rec_len > INVFS_MAX_REC_LEN) break;
        if (vol_read_raw(vol, p + offsetof(invfs_inode_rec, name),
                         name, h.name_len) != 0) break;
        name[h.name_len] = 0;
        rb = (uint8_t *)malloc((size_t)h.rec_len + 4);
        if (!rb) break;
        if (vol_read_raw(vol, p, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
        memcpy(&crc_stored, rb + h.rec_len, 4);
        crc_calc = invfs_crc32c(rb, h.rec_len);
        free(rb);
        p += (uint64_t)h.rec_len + 4;
        if (crc_calc != crc_stored) continue;

        if (h.magic == TOMBSTONE_MAGIC) {
            int i = sw_find(tab, tmask, names, name);
            if (h.file_size == 0) {   /* legacy kill-by-id */
                if (i >= 0 && inodes[i] == h.inode_id) inodes[i] = 0;
            } else if (i >= 0 && poss[i] == (uint64_t)h.file_size) {
                /* v2 position kill: retires exactly the record at that
                 * position -- the name dies only if its current version IS
                 * that record (an unlink/rename kill names the last one;
                 * a meta_rewrite's names the already-superseded one) */
                inodes[i] = 0;
            }
            continue;
        }

        {
            int i = sw_find(tab, tmask, names, name);
            if (i >= 0) { inodes[i] = h.inode_id; sizes[i] = h.file_size;
                          poss[i] = p - h.rec_len - 4; }   /* p advanced already */
            else {
                if (count == cap) {
                    int ncap = cap ? cap * 2 : 512;
                    char (*nn)[256] =
                        (char (*)[256])realloc(names, (size_t)ncap * 256);
                    uint64_t *ni =
                        (uint64_t *)realloc(inodes, (size_t)ncap * sizeof *ni);
                    uint64_t *ns =
                        (uint64_t *)realloc(sizes, (size_t)ncap * sizeof *ns);
                    uint64_t *np =
                        (uint64_t *)realloc(poss, (size_t)ncap * sizeof *np);
                    if (nn) names = nn;
                    if (ni) inodes = ni;
                    if (ns) sizes = ns;
                    if (np) poss = np;
                    if (!nn || !ni || !ns || !np) {
                        fprintf(stderr, "out of memory\n");
                        return 1;
                    }
                    cap = ncap;
                }
                strncpy(names[count], name, 256);
                names[count][255] = 0;
                inodes[count] = h.inode_id;
                sizes[count] = h.file_size;
                poss[count] = p - h.rec_len - 4;   /* p advanced already */
                sw_insert(&tab, &tmask, &tcount, names, count);
                count++;
            }
        }
    }

    /* WP22d: the walk above collects the newest record per name, but the
     * live answer is the name index's consistent cut (a torn newest
     * version is hidden and the name resolves to an older id, or is
     * absent). Sweep exactly the live ids -- sweeping a hidden record
     * would fail its reads and could resurrect dead ids' blocks. */
    {
        int j, kept = 0;
        for (j = 0; j < count; j++) {
            uint64_t live;
            if (inodes[j] == 0 || sizes[j] == 0) continue;
            live = vol_find(vol, names[j]);
            if (live == 0) { inodes[j] = 0; continue; }
            inodes[j] = live;   /* may be the fallback version's id */
            kept++;
        }
        fprintf(stderr, "live entries: %d (of %d walked)\n", kept, count);
    }

    /* WP19: the once-per-RUN heat decay (rheat >>= 1, wheat -= 1), before
     * the walk so the walk's write-hot skip and the promotion pass below
     * both see post-decay values. */
    if (!dry)
        vol_heat_sweep_begin(vol);

    /* sweep candidates: regular files with actual payload */
    for (int i = 0; i < count; i++) {
#ifndef _WIN32
        /* WP21 test hook (tools/test-rollback.sh): die mid-walk, after N
         * candidates, with the checkpoint armed and retention half-filled
         * -- the crash-mid-sweep rollback leg. (Keyed on the walk index:
         * deferred batching candidates move neither swept nor skipped.) */
        {
            const char *ab = getenv("INVFS_SWEEP_ABORT_AFTER");
            if (ab && !dry && i + 1 == atoi(ab) && atoi(ab) > 0)
                kill(getpid(), SIGKILL);
        }
#endif
        if (inodes[i] == 0 || sizes[i] == 0) { skipped++; continue; }
        if (dry) { printf("would sweep %s (%llu bytes)\n",
                          names[i], (unsigned long long)sizes[i]); continue; }
        {
            /* vol_sweep_one: 0 = nothing to do, >0 = transcoded/swept,
             * 7 = JPEG->JXL, 9 = text deferred into the batch accumulator,
             * 10 = binary deferred into the WP14a binary accumulator (both
             * sealed by vol_tz_flush below), 11 = exe-as-container carve
             * (WP14b M2), >=100 = codecpack transcode (100+algo, WP13),
             * <0 = hard error */
            int rc;
            if (fast) {
                /* WP22e --fast: the decision narrows to "generic or
                 * nothing" (vol_sweep_file_generic: 0 = swept to Shadow,
                 * 1 = nothing to do, <0 = hard error). No per-file line:
                 * the generic floor prints none in the full pass either. */
                rc = vol_sweep_file_generic(vol, inodes[i]);
                if (rc == 0) swept++;
                else if (rc > 0) skipped++;
                else failed++;
                goto progress;
            }
            rc = vol_sweep_one(vol, inodes[i], names[i]);
            if (rc == 9) {
                if (strchr(names[i], '!'))
                    part_agg_add(names[i], 0);
                else
                    printf("  %s: text -> PPMd batch\n", names[i]);
            }
            else if (rc == 10) {
                if (strchr(names[i], '!'))
                    part_agg_add(names[i], 1);
                else
                    printf("  %s: binary -> ZSTD batch\n", names[i]);
            }
            else if (rc == 11) {
                swept++;
                printf("  %s: exe media -> JXL (%u parts)\n", names[i],
                       vol_exer_last_parts(vol));
            }
            else if (rc == 7) {
                swept++;
                printf("  %s: JPEG -> JXL (lossless)\n", names[i]);
            }
            else if (rc >= 100) {
                const invfs_codec *pc = invfs_codec_by_algo((uint32_t)(rc - 100));
                swept++;
                printf("  %s: %s (codecpack)\n", names[i],
                       pc ? pc->name : "unknown-pack");
            }
            else if (rc > 0) swept++;
            else if (rc == 0) skipped++;
            else failed++;
        }
progress:
        if ((swept + skipped) % 5000 == 0)
            fprintf(stderr, "  ..%d done (swept=%d)\n", swept + skipped, swept);
    }

    /* WP14b: print the aggregated container-part deferral lines collected
     * during the walk (one line per container instead of one per part) */
    part_agg_print();

    /* WP19: extract read-hot PPMd batch members to standalone per-segment
     * ZSTD (class GENERIC) -- between the walk and the dedupe pass, so the
     * promoted segments can merge and the GC below reclaims any batch the
     * promotions killed. The pass prints its own counts.
     * WP22e: --fast skips this (a transcode), along with dedupe/batching. */
    if (!dry && !fast) {
        if (vol_heat_promote(vol) < 0)
            fprintf(stderr, "heat: promotion pass failed (sweep results "
                            "are intact)\n");
    }

    /* WP12(h): per-segment dedupe between the walk and the text-batch GC
     * (order: walk -> dedupe -> GC -> flush). The walk's transcodes are
     * what create the duplicates worth finding -- identical content lands
     * in Shadow as identical segments -- and dedupe runs before the GC so
     * it never sees a zone==TEXT entry (WP10 §11). The pass prints its
     * own merged/freed counts. */
    if (!dry && !fast) {
        if (vol_sweep_dedupe(vol) < 0)
            fprintf(stderr, "dedupe: pass failed (sweep results are intact)\n");
    }

    /* WP10 §7 + WP14a: reclaim owner batches no live member references,
     * then seal the accumulated text AND binary candidates into shared
     * batches (one vol_tz_flush drains both accumulators). The deferred
     * counts come from the accumulators themselves: parts deferred at
     * container-explode time (WP14b) never produced a walk line.
     * --fast deferred nothing, so the GC/flush are skipped with it. */
    if (!dry && !fast) {
        int gcrc = vol_tz_gc(vol);
        size_t tzp = vol_acc_pending(vol, 0);
        size_t bzp = vol_acc_pending(vol, 1);
        int tzrc;
        if (gcrc > 0)
            printf("text gc: %u dead batches reclaimed\n", (unsigned)gcrc);
        else if (gcrc < 0)
            fprintf(stderr, "text gc failed (rc=%d)\n", gcrc);
        tzrc = vol_tz_flush(vol);
        if (tzrc == 0) {
            if (tzp)
                printf("text batches flushed (%zu deferred)\n", tzp);
            if (bzp)
                printf("binary batches flushed (%zu deferred)\n", bzp);
        }
        else if (tzrc < 0) {
            fprintf(stderr, "batch flush failed (rc=%d)\n", tzrc);
            failed++;
        }
    }

    fprintf(stderr, "sweep done: swept=%d skipped=%d failed=%d\n",
            swept, skipped, failed);

    /* WP21: seal the retention registry (the "\x01reten" owner) holding
     * every block this run retired. From here the volume's end-state is:
     * checkpoint live + retained blocks held, until invf-rollback or the
     * next realize. A registry failure does NOT invalidate the checkpoint
     * (rollback never reads the registry); the realize of an unregistered
     * range is just deferred to the fsck after the next realize. */
    if (!dry) {
        uint64_t rr = 0, rb = 0;
        if (vol_ckp_end(vol, &rr, &rb) != 0)
            fprintf(stderr, "checkpoint: registry write failed (the "
                            "checkpoint itself is intact)\n");
        else if (rb)
            fprintf(stderr, "checkpoint: %llu retained blocks held for "
                    "rollback (%llu ranges)\n",
                    (unsigned long long)rb, (unsigned long long)rr);
    }

    if (!dry) {
        if (vol_flush(vol) != 0)
            fprintf(stderr, "warning: final flush failed\n");
    }

    /* WP22e: hot tail pruning. The sweep appends a fresh record version +
     * tombstone per rewritten/stamped file, so the inode area's dead share
     * climbs; past ~30% dead bytes, compact the area online (live records
     * verbatim, id order, tombstones dropped; the CMP0 crash protocol makes
     * a mid-pass kill recoverable). NEVER while a CKP0 checkpoint is live
     * -- rollback truncates the area to the checkpoint's absolute
     * positions -- and never on a read-only volume; the engine prints the
     * skip reason. INVFS_NO_COMPACT=1 opts out (the --compact form is the
     * manual override). A failure here never invalidates the sweep. */
    if (!dry) {
        const char *nc = getenv("INVFS_NO_COMPACT");
        int compact_off = nc && strcmp(nc, "0") != 0;   /* =1 (or any
                        non-"0" value) disables the automatic pass */
        if (!compact_off) {
            uint64_t used = vol_inode_area_pos(vol) - vol_inode_area_start(vol);
            uint64_t live = vol_inode_live_bytes(vol);
            if (live && used > live && (used - live) * 10 > used * 3) {
                uint64_t before = 0, after = 0;
                int crc = vol_inode_compact(vol, &before, &after);
                if (crc > 0)
                    printf("inode area compacted: %llu -> %llu bytes\n",
                           (unsigned long long)before,
                           (unsigned long long)after);
                else if (crc < 0)
                    fprintf(stderr, "inode compact: pass failed (sweep "
                                    "results are intact)\n");
            }
        }
    }

    /* WP20 --seal / WP20b: (re)seal the shadow-zone parity AFTER the sweep
     * is fully flushed -- the parity covers the post-sweep state.
     * Idempotent check-and-update: an unchanged volume reports 0 stripes
     * updated. Runs for --seal, the --redundant-* configures, and the
     * auto-reseal (live descriptor, no flags). */
    if (seal || rb_f >= 0 || rp_f >= 0 || auto_reseal) {
        invfs_seal_report rep;
        uint32_t k1c, m2c;
        int l2c;
        vol_redun_state(vol, &k1c, &l2c, &m2c);
        if (vol_seal(vol, 0, &rep) != 0) {
            fprintf(stderr, "seal failed\n");
            vol_close(vol);
            return 1;
        }
        printf("[seal] %llu stripes, %llu parity blocks, overhead %.2f%% of "
               "occupied shadow; %llu stripes updated, %llu unchanged, "
               "%llu dirty-skipped (k1=%u)",
               (unsigned long long)rep.stripes,
               (unsigned long long)rep.parity_blocks, rep.overhead_pct,
               (unsigned long long)rep.updated,
               (unsigned long long)rep.unchanged,
               (unsigned long long)rep.dirty_skipped,
               (unsigned)k1c);
        if (rep.added || rep.freed)
            printf(" (%llu added, %llu stale freed)",
                   (unsigned long long)rep.added,
                   (unsigned long long)rep.freed);
        if (rep.unprotected)
            printf(", %llu unprotected (ENOSPC)",
                   (unsigned long long)rep.unprotected);
        printf("\n");
        if (l2c) {
            printf("[seal2] %llu stripes, %llu parity blocks, overhead "
                   "%.2f%% of occupied shadow; %llu stripes updated, "
                   "%llu unchanged, %llu dirty-skipped (%s, k=32, m=%u)",
                   (unsigned long long)rep.l2_stripes,
                   (unsigned long long)rep.l2_parity_blocks,
                   rep.l2_overhead_pct,
                   (unsigned long long)rep.l2_updated,
                   (unsigned long long)rep.l2_unchanged,
                   (unsigned long long)rep.l2_dirty_skipped,
                   rs_algo_name(l2c), (unsigned)m2c);
            if (rep.l2_added || rep.l2_freed)
                printf(" (%llu added, %llu stale freed)",
                       (unsigned long long)rep.l2_added,
                       (unsigned long long)rep.l2_freed);
            if (rep.l2_unprotected)
                printf(", %llu unprotected (ENOSPC)",
                       (unsigned long long)rep.l2_unprotected);
            printf("\n");
        }
        if (vol_flush(vol) != 0)
            fprintf(stderr, "warning: final flush failed\n");
    }

    vol_close(vol);
    return failed ? 1 : 0;
}
