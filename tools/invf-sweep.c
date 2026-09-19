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
 *
 * WP23: --extract-packs <dir> is the sweepboot helper mode (see
 * tools/sweepboot-init.sh): with NO FUSE MOUNT and no sweep, the volume
 * is opened through the engine alone and the codecpack directory stored
 * ON the volume ("/.invfs/codecpacks" when present, else
 * "/usr/lib/invfs/codecpacks") is materialized into <dir> (a tmpfs
 * scratch in the initramfs), which is then printed on stdout as the
 * mode's single payload line. The pack files are plain files -- on a
 * swept volume they are PPMd/ZSTD batch members, decoded in-process by
 * the ordinary read path -- so a maintenance boot can run the sweep with
 * INVFS_CODECPACKS=<dir> and the volume is SELF-HOSTING: it carries the
 * very tools its own sweep needs. Read-only volumes open fine (the mode
 * writes nothing to the volume; read heat accrues exactly like any
 * mount's reads).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
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

/* ---- WP23: --extract-packs (sweepboot self-hosting) -------------------
 * The sweep's codecpacks are expected to live ON the rootfs volume (that
 * is what "self-hosting" means), but the maintenance boot needs them
 * BEFORE the volume is mounted -- and a FUSE mount is exactly what the
 * sweep must be exclusive against. So the packs are read out through the
 * engine alone: plain files, decoded in-process by the ordinary read
 * path (a swept volume keeps them as PPMd/ZSTD batch members; nothing
 * here depends on their stored shape).
 */

/* mkdir -p for the extraction target; 0 = exists/created */
static int xp_mkdirs(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path), i;
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    return 0;
}

/* one volume file -> <dstdir>/<rel>; parents created; mode = the
 * recorded meta when present, 0755 otherwise (pack helpers under bin/
 * are exec'd by name, so an executable default is the safe one) */
static int xp_file(invfs_volume *v, const char *vname, const char *rel,
                   const char *dstdir)
{
    char dst[1024];
    uint8_t *buf = NULL;
    size_t len = 0;
    uint64_t id;
    FILE *f;
    invfs_meta_pub m;
    long mode = 0755;
    int rc = -1;

    if (snprintf(dst, sizeof dst, "%s/%s", dstdir, rel) >= (int)sizeof dst)
        return -1;
    id = vol_find(v, vname);
    if (!id) return -1;
    if (vol_read_file(v, id, &buf, &len) != 0) return -1;
    {
        char *sl = strrchr(dst, '/');
        if (sl) {
            *sl = '\0';
            if (xp_mkdirs(dst) != 0) { free(buf); return -1; }
            *sl = '/';
        }
    }
    f = fopen(dst, "wb");
    if (!f) { free(buf); return -1; }
    if (len && fwrite(buf, 1, len, f) != len) { fclose(f); free(buf); return -1; }
    if (fclose(f) != 0) { free(buf); return -1; }
    if (vol_get_meta(v, id, &m) == 0 && (m.mode & 0777))
        mode = m.mode & 0777;
    if (chmod(dst, (mode_t)mode) != 0) { free(buf); return -1; }
    free(buf);
    rc = 0;
    return rc;
}

static int xp_walk(invfs_volume *v, const char *vdir, const char *rel,
                   const char *dstdir, unsigned depth,
                   unsigned long *files_out)
{
    invfs_dirent *ents = NULL;
    int cap = 256, n, i, rc = 0;

    for (;;) {   /* grow the listing window until the dir fits */
        invfs_dirent *ne = realloc(ents, (size_t)cap * sizeof *ents);
        if (!ne) { free(ents); return -1; }
        ents = ne;
        n = vol_list_dir(v, vdir, ents, cap);
        if (n < 0) { free(ents); return -1; }
        if (n < cap) break;
        cap *= 2;
    }
    for (i = 0; i < n && rc == 0; i++) {
        char vchild[512], rchild[512];
        if (snprintf(vchild, sizeof vchild, "%s/%s", vdir, ents[i].name) >=
                (int)sizeof vchild ||
            snprintf(rchild, sizeof rchild, "%s%s%s", rel, rel[0] ? "/" : "",
                     ents[i].name) >= (int)sizeof rchild) {
            rc = -1; break;
        }
        if (ents[i].is_dir) {
            if (depth < 16)
                rc = xp_walk(v, vchild, rchild, dstdir, depth + 1, files_out);
            else
                rc = -1;
        } else {
            if (xp_file(v, vchild, rchild, dstdir) != 0) {
                fprintf(stderr, "extract-packs: cannot materialize %s\n",
                        vchild);
                rc = -1;
            } else {
                (*files_out)++;
            }
        }
    }
    free(ents);
    return rc;
}

/* The mode body: open volume already held. Returns 0 (dir printed on
 * stdout even when the volume carries no packs -- an empty dir is the
 * honest "nothing to self-host with") or 1. */
static int extract_packs(invfs_volume *vol, const char *dir)
{
    static const char *const roots[] = {
        ".invfs/codecpacks",          /* /.invfs/codecpacks */
        "usr/lib/invfs/codecpacks",   /* the system location */
    };
    const char *root = NULL;
    unsigned long files = 0;
    size_t i;

    for (i = 0; i < sizeof roots / sizeof roots[0]; i++)
        if (vol_is_dir(vol, roots[i])) { root = roots[i]; break; }
    if (xp_mkdirs(dir) != 0) {
        fprintf(stderr, "extract-packs: cannot create %s\n", dir);
        return 1;
    }
    if (root && xp_walk(vol, root, "", dir, 0, &files) != 0) {
        fprintf(stderr, "extract-packs: walk of /%s failed\n", root);
        return 1;
    }
    if (root)
        fprintf(stderr, "extract-packs: /%s -> %s (%lu files)\n",
                root, dir, files);
    else
        fprintf(stderr, "extract-packs: volume carries no codecpack dir\n");
    printf("%s\n", dir);   /* the payload line sweepboot-init.sh reads */
    return 0;
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

/* WP42: per-record collector state for the sweep walk. The shared
 * vol_records_walk() owns the scan (all mapper extents on v0.3.0+, the
 * legacy contiguous area otherwise) and its CRC verification; the callback
 * keeps the legacy collector policy (tombstone position-kill, newest record
 * per name, name snapshots) and accumulates the walked record bytes for the
 * compaction trigger. The arrays are reached through their addresses because
 * the callback may realloc them. */
typedef struct {
    char (**names)[256];
    uint64_t **inodes, **sizes, **poss;
    sw_bucket ***tab;
    size_t *tmask, *tcount;
    int *count, *cap;
    uint64_t rec_bytes;
    int oom;
} sweep_collect_ctx;

static int sweep_collect_cb(void *ctx_, uint64_t rec_pos,
                            const invfs_inode_rec *h, const uint8_t *rec)
{
    sweep_collect_ctx *c = (sweep_collect_ctx *)ctx_;
    char (*names)[256] = *c->names;
    uint64_t *inodes = *c->inodes;
    uint64_t *sizes = *c->sizes;
    uint64_t *poss = *c->poss;
    char name[257];
    size_t nl;

    (void)rec;
    /* same corrupt-record guards the legacy loop broke on */
    if (h->magic != INODE_REC_MAGIC && h->magic != TOMBSTONE_MAGIC)
        return 1;
    if (h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1 ||
        h->rec_len > INVFS_MAX_REC_LEN)
        return 1;
    nl = h->name_len;
    memcpy(name, h->name, nl);
    name[nl] = 0;
    c->rec_bytes += (uint64_t)h->rec_len + 4;

    if (h->magic == TOMBSTONE_MAGIC) {
        int i = sw_find(*c->tab, *c->tmask, names, name);
        if (h->file_size == 0) {   /* legacy kill-by-id */
            if (i >= 0 && inodes[i] == h->inode_id) inodes[i] = 0;
        } else if (i >= 0 && poss[i] == (uint64_t)h->file_size) {
            /* v2 position kill: retires exactly the record at that
             * position -- the name dies only if its current version IS
             * that record */
            inodes[i] = 0;
        }
        return 0;
    }

    {
        int i = sw_find(*c->tab, *c->tmask, names, name);
        if (i >= 0) {
            inodes[i] = h->inode_id; sizes[i] = h->file_size;
            poss[i] = rec_pos;
        } else {
            if (*c->count == *c->cap) {
                int ncap = *c->cap ? *c->cap * 2 : 512;
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
                *c->names = names; *c->inodes = inodes;
                *c->sizes = sizes; *c->poss = poss;
                *c->cap = ncap;
                if (!nn || !ni || !ns || !np) { c->oom = 1; return 1; }
            }
            strncpy(names[*c->count], name, 256);
            names[*c->count][255] = 0;
            inodes[*c->count] = h->inode_id;
            sizes[*c->count] = h->file_size;
            poss[*c->count] = rec_pos;
            sw_insert(c->tab, c->tmask, c->tcount, names, *c->count);
            (*c->count)++;
        }
    }
    return 0;
}

/* WP64: graceful Ctrl+C — finish the current file, then exit cleanly. */
static volatile sig_atomic_t g_stop = 0;

#ifndef _WIN32
static void on_sigint(int sig)
{
    if (g_stop) {          /* second Ctrl+C: restore default, die now */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_stop = 1;
    fprintf(stderr, "\n^C  stopping after the current file"
                    " (Ctrl+C again aborts now, losing it)\n");
}
#endif

int main(int argc, char **argv)
{
    invfs_volume *vol;
    const invfs_superblock *sb;
    int err, dry = 0, seal = 0, unseal = 0, bench = 0, realize = 0;
    int no_realize = 0, stopped = 0;
    int fast = 0, compact_only = 0;
    const char *extract_dir = NULL;    /* WP23 --extract-packs mode */
    double rb_f = -1.0, rp_f = -1.0;   /* <0: flag absent */
    int rp_algo = 0;                   /* explicit :rs-vm/:rs-cauchy suffix */
    int auto_reseal = 0;
    uint64_t rec_bytes = 0;   /* WP42: record bytes walked (compaction trigger) */
    int count = 0, cap = 0, swept = 0, skipped = 0, failed = 0;
    int reg_failed = 0;   /* WP53: retention-registry write failed */
    char (*names)[256] = NULL;
    uint64_t *inodes = NULL;
    uint64_t *sizes = NULL;
    uint64_t *poss = NULL;   /* each name's current record position
                                (v2 position-kill matching, WP22c) */
    const char *img;
    sw_bucket **tab = NULL;
    size_t tmask = 0, tcount = 0;
    int i;

    for (int j = 1; j < argc; j++) {
        if (strcmp(argv[j], "-h") == 0 || strcmp(argv[j], "--help") == 0) {
            fprintf(stderr,
                "usage: %s <image> [--dry-run] [--fast] [--compact]\n"
                "           [--seal|--unseal]\n"
                "           [--redundant-blocks <f>]\n"
                "           [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]\n"
                "           [--free-redundant] [--redundant-bench]\n"
                "           [--realize]  (accept the last sweep: free its\n"
                "                         retention registry, clear CKP0)\n"
                "           [--no-realize] (keep the previous checkpoint live;\n"
                "                         blocks stay held until the next sweep)\n"
                "  --fast      cheap pass: RAW files take the generic\n"
                "              per-segment recompress only (no classification,\n"
                "              transcodes, decomposition, batching or dedupe)\n"
                "  --compact   run only the inode-area compaction pass\n"
                "           [--extract-packs <dir>]  (WP23 sweepboot: copy the\n"
                "                         volume's codepack dir to <dir>,\n"
                "                         engine-side, no sweep, no FUSE)\n",
                argv[0]);
            return 2;
        }
        if (strcmp(argv[j], "-v") == 0 || strcmp(argv[j], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <image> [--dry-run] [--fast] [--compact]\n"
                "           [--seal|--unseal]\n"
                "           [--redundant-blocks <f>]\n"
                "           [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]\n"
                "           [--free-redundant] [--redundant-bench]\n"
                "           [--realize]  (accept the last sweep: free its\n"
                "                         retention registry, clear CKP0)\n"
                "           [--no-realize] (keep the previous checkpoint live;\n"
                "                         blocks stay held until the next sweep)\n"
                "  --fast      cheap pass: RAW files take the generic\n"
                "              per-segment recompress only (no classification,\n"
                "              transcodes, decomposition, batching or dedupe)\n"
                "  --compact   run only the inode-area compaction pass\n"
                "           [--extract-packs <dir>]  (WP23 sweepboot: copy the\n"
                "                         volume's codepack dir to <dir>,\n"
                "                         engine-side, no sweep, no FUSE)\n",
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
        } else if (strcmp(a, "--no-realize") == 0) {
            no_realize = 1;
        } else if (strcmp(a, "--fast") == 0) {
            fast = 1;
        } else if (strcmp(a, "--compact") == 0) {
            compact_only = 1;
        } else if (strcmp(a, "--extract-packs") == 0 && i + 1 < argc) {
            extract_dir = argv[++i];
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
        (seal || rb_f >= 0 || rp_f >= 0 || realize || extract_dir)) {
        fprintf(stderr, "conflicting flags\n");
        return 2;
    }
    if (extract_dir &&
        (dry || unseal || bench || seal || rb_f >= 0 || rp_f >= 0 || realize)) {
        fprintf(stderr, "--extract-packs is a standalone mode\n");
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
#ifndef _WIN32
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
#endif

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

    /* WP23 --extract-packs: a standalone, read-only, engine-side mode for
     * the sweepboot maintenance boot (tools/sweepboot-init.sh). No sweep,
     * no checkpoint, no seal -- copy the on-volume codecpack dir out and
     * leave. */
    if (extract_dir) {
        int xrc = extract_packs(vol, extract_dir);
        vol_close(vol);
        return xrc;
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
        if (vol_ckp_begin(vol, no_realize) < 0)
            fprintf(stderr, "checkpoint: arm failed; sweeping without "
                            "one\n");
    }

    /* WP42: collect live regular files through the shared mapper-aware
     * record walker. On a v0.3.0+ mapper volume the records live in dynamic
     * metadata extents; the old contiguous [area_start, inode_area_pos)
     * loop saw none and the sweep silently no-op'd. rec_bytes is the walked
     * record footprint used by the compaction trigger below and, unlike
     * pos-minus-start, cannot underflow on a mapper volume. */
    {
        sweep_collect_ctx cc;
        memset(&cc, 0, sizeof cc);
        cc.names = &names; cc.inodes = &inodes;
        cc.sizes = &sizes; cc.poss = &poss;
        cc.tab = &tab; cc.tmask = &tmask; cc.tcount = &tcount;
        cc.count = &count; cc.cap = &cap;
        vol_records_walk(vol, sweep_collect_cb, &cc);
        if (cc.oom) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        rec_bytes = cc.rec_bytes;
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
        if (g_stop) { stopped = 1; break; }
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

    /* WP25 rule 9: two-device tier migration -- canonical stays on dev1;
     * read-hot canonical segments get a dev0 acceleration copy, arena
     * pressure (<20% free) evicts the coldest copies. Runs after the
     * decay + promotion so post-decay rheat governs (the WP19 hysteresis
     * applies). No-op on a single-device volume. */
    if (!dry && !fast && vol_ndev(vol) == 2) {
        if (vol_tier_migrate(vol) < 0)
            fprintf(stderr, "tier: migration pass failed (sweep results "
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

    fprintf(stderr, "sweep done: swept=%d skipped=%d failed=%d%s\n",
            swept, skipped, failed,
            stopped ? " (stopped by Ctrl+C)" : "");
    /* WP42: the CLI-style summary line the big-volume e2e parses
     * (`sweep: N swept`); mirrors src/cli/sweep.c's report. */
    fprintf(stderr, "sweep: %d swept\n", swept);

    /* WP21: seal the retention registry (the "\x01reten" owner) holding
     * every block this run retired. From here the volume's end-state is:
     * checkpoint live + retained blocks held, until invf-rollback or the
     * next realize. A registry failure does NOT invalidate the checkpoint
     * (rollback never reads the registry); the realize of an unregistered
     * range is just deferred to the fsck after the next realize, and the
     * run exits nonzero so the failure is not silently swallowed. */
    if (!dry) {
        uint64_t rr = 0, rb = 0;
        if (vol_ckp_end(vol, &rr, &rb) != 0) {
            fprintf(stderr, "checkpoint: registry write failed (the "
                            "checkpoint itself is intact)\n");
            /* WP53: a genuine registry-write failure is a real failure --
             * surface it in the exit status. */
            reg_failed = 1;
        } else if (rb)
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
            uint64_t used = rec_bytes;   /* WP42: walked record footprint */
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
    return (failed || reg_failed) ? 1 : 0;
}
