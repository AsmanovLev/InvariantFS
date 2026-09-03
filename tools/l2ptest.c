/*
 * l2ptest.c — WP-L2Q harness: L2P session-index stress + read microbench
 * + heat persistence pump + pure-read journal-quiet probe.
 *
 * Modes:
 *   stress <img> <ops> <seed>
 *     Random MAP / re-MAP / UNMAP sequences over a synthetic key space
 *     (inodes 1..64, lbas 0..31) against a scratch volume, verifying
 *     vol_lookup_entry (the session index) against an independent
 *     newest-wins linear scan of vol_l2p() after every op. Interleaves
 *     flushes and close/reopen cycles (replay rebuilds the index through
 *     l2p_apply, UNMAP folds through l2p_remove_mem). Any divergence is
 *     a hard failure.
 *   pump <img> <file>...
 *     Read each file once (the real vol_read_file path -> one read-heat
 *     touch per segment this session), then vol_flush. With
 *     INVFS_JRN_FORCE_COMPACT=1 the flush compacts, so the image carries
 *     the accrued pads: this is how read heat persists under WP-L2Q
 *     (sweep-granularity, no per-read journal traffic).
 *   readflush <img> <file>...
 *     Read files, vol_flush, close. The pure-read journal-quiet probe:
 *     the caller byte-compares the image before/after.
 *   mkfiles <img> <n> <size>
 *     Create files f00000..f{n-1} (deterministic content) in one session.
 *   readall <img> <n> [seed]
 *     Read files f00000..f{n-1} in creation order (seed != 0: shuffled),
 *     one process; prints wall times. The hot-read-path microbench: pair
 *     with an INVFS_L2P_IDX=0 run (or a baseline binary) for the
 *     before/after numbers.
 *
 * Uses only the public volume.h API, so the same source builds against a
 * pre-WP-L2Q tree for the baseline numbers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "invarifs.h"
#include "volume.h"

static uint64_t rng_state;

static uint64_t rng_next(void)
{
    /* splitmix64 */
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* the independent reference: newest-wins linear scan over the public
 * table view -- exactly the pre-WP-L2Q vol_lookup_entry semantics */
static int ref_lookup(invfs_volume *v, uint64_t inode, uint64_t lba,
                      uint64_t *pba, uint64_t *len)
{
    size_t n = 0, i;
    const invfs_l2p_entry *t = vol_l2p(v, &n);
    if (!t) return -1;
    for (i = n; i-- > 0; ) {
        if (t[i].type == INVFS_JRN_MAP && t[i].inode == inode &&
            t[i].lba == lba) {
            *pba = t[i].pba;
            *len = t[i].length;
            return 0;
        }
    }
    return -1;
}

static invfs_volume *open_or_die(const char *img)
{
    int err = 0;
    invfs_volume *v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "l2ptest: cannot open %s (err %d)\n", img, err);
        exit(1);
    }
    return v;
}

/* one full parity sweep over the whole key space; returns mismatches */
static uint64_t parity_sweep(invfs_volume *v, uint64_t ninodes,
                             uint64_t nlbas)
{
    uint64_t bad = 0, checked = 0;
    uint64_t ino, lba;
    for (ino = 1; ino <= ninodes; ino++) {
        for (lba = 0; lba < nlbas; lba++) {
            uint64_t p1 = 0, l1 = 0, p2 = 0, l2 = 0;
            int r1 = vol_lookup_entry(v, ino, lba, &p1, &l1);
            int r2 = ref_lookup(v, ino, lba, &p2, &l2);
            checked++;
            if (r1 != r2 || (r1 == 0 && (p1 != p2 || l1 != l2))) {
                fprintf(stderr,
                        "l2ptest: PARITY MISMATCH (%llu,%llu): "
                        "index=%d(%llu,%llu) scan=%d(%llu,%llu)\n",
                        (unsigned long long)ino, (unsigned long long)lba,
                        r1, (unsigned long long)p1, (unsigned long long)l1,
                        r2, (unsigned long long)p2, (unsigned long long)l2);
                bad++;
            }
        }
    }
    if (!bad)
        printf("l2ptest: parity OK over %llu key probes\n",
               (unsigned long long)checked);
    return bad;
}

#define STRESS_INODES 64
#define STRESS_LBAS   32

static int do_stress(const char *img, uint64_t ops, uint64_t seed)
{
    invfs_volume *v = open_or_die(img);
    uint64_t i, bad = 0, maps = 0, unmaps = 0, probes = 0;
    uint64_t next_pba = 1000000;   /* synthetic pbas: never read through */

    rng_state = seed;
    for (i = 0; i < ops; i++) {
        uint64_t ino = (rng_next() % STRESS_INODES) + 1;
        uint64_t lba = rng_next() % STRESS_LBAS;
        uint64_t roll = rng_next() % 100;
        if (roll < 50) {
            /* MAP (fresh key or re-MAP of a live key: newest wins) */
            uint32_t len = (uint32_t)(rng_next() % 8) + 1;
            if (vol_map(v, ino, lba, next_pba, len) != 0) {
                fprintf(stderr, "l2ptest: vol_map failed at op %llu\n",
                        (unsigned long long)i);
                vol_close(v);
                return 1;
            }
            next_pba += len;
            maps++;
        } else if (roll < 75) {
            /* UNMAP */
            vol_l2p_remove(v, ino, lba);
            unmaps++;
        } else if (roll < 97) {
            /* probe parity on a random key */
            uint64_t p1 = 0, l1 = 0, p2 = 0, l2 = 0;
            int r1 = vol_lookup_entry(v, ino, lba, &p1, &l1);
            int r2 = ref_lookup(v, ino, lba, &p2, &l2);
            probes++;
            if (r1 != r2 || (r1 == 0 && (p1 != p2 || l1 != l2))) {
                fprintf(stderr,
                        "l2ptest: PARITY MISMATCH at op %llu (%llu,%llu)\n",
                        (unsigned long long)i, (unsigned long long)ino,
                        (unsigned long long)lba);
                bad++;
            }
        } else if (roll < 99) {
            /* flush: the journal append/compaction must not diverge the
             * in-memory view */
            if (vol_flush(v) != 0) {
                fprintf(stderr, "l2ptest: flush failed at op %llu\n",
                        (unsigned long long)i);
                vol_close(v);
                return 1;
            }
        } else {
            /* close + reopen: replay rebuilds the table and the index */
            vol_close(v);
            v = open_or_die(img);
        }
        if (bad > 10) { vol_close(v); return 1; }
    }
    bad += parity_sweep(v, STRESS_INODES, STRESS_LBAS);
    vol_close(v);
    /* replay after all that (incl. UNMAPs) must rebuild the same view */
    v = open_or_die(img);
    bad += parity_sweep(v, STRESS_INODES, STRESS_LBAS);
    vol_close(v);
    printf("l2ptest: stress done: %llu ops (%llu maps, %llu unmaps, "
           "%llu probes), %llu mismatches\n",
           (unsigned long long)ops, (unsigned long long)maps,
           (unsigned long long)unmaps, (unsigned long long)probes,
           (unsigned long long)bad);
    return bad ? 1 : 0;
}

static int read_one(invfs_volume *v, const char *name)
{
    uint64_t ino = vol_find(v, name);
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!ino) {
        fprintf(stderr, "l2ptest: '%s' not found\n", name);
        return -1;
    }
    if (vol_read_file(v, ino, &buf, &len) != 0) {
        fprintf(stderr, "l2ptest: read failed for '%s'\n", name);
        return -1;
    }
    free(buf);
    return 0;
}

static int do_pump(const char *img, int nfiles, char **files, int do_flush)
{
    invfs_volume *v = open_or_die(img);
    int i, rc = 0;
    for (i = 0; i < nfiles; i++)
        if (read_one(v, files[i]) != 0) rc = 1;
    if (!rc && do_flush && vol_flush(v) != 0) {
        fprintf(stderr, "l2ptest: flush failed\n");
        rc = 1;
    }
    vol_close(v);
    return rc;
}

static void fill_content(uint8_t *buf, size_t len, uint64_t file_no)
{
    /* deterministic, moderately compressible */
    uint64_t s = file_no * 2654435761u + 1;
    size_t i;
    for (i = 0; i < len; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        buf[i] = (uint8_t)((s >> 33) % 61) + ' ';
    }
}

static int do_mkfiles(const char *img, uint64_t n, size_t size)
{
    invfs_volume *v = open_or_die(img);
    uint8_t *buf = (uint8_t *)malloc(size);
    uint64_t i;
    char name[64];
    double t0 = now_ms();
    if (!buf) { vol_close(v); return 1; }
    for (i = 0; i < n; i++) {
        snprintf(name, sizeof name, "f%05llu", (unsigned long long)i);
        fill_content(buf, size, i);
        if (!vol_create_file(v, name, buf, size)) {
            fprintf(stderr, "l2ptest: create %s failed\n", name);
            free(buf);
            vol_close(v);
            return 1;
        }
        if ((i + 1) % 5000 == 0)
            fprintf(stderr, "  ..%llu files\n", (unsigned long long)(i + 1));
    }
    free(buf);
    vol_close(v);
    printf("l2ptest: mkfiles n=%llu size=%zu wall_ms=%.1f\n",
           (unsigned long long)n, size, now_ms() - t0);
    return 0;
}

static int do_readall(const char *img, uint64_t n, uint64_t seed)
{
    invfs_volume *v = open_or_die(img);
    uint64_t i;
    uint64_t *order = (uint64_t *)malloc(n * sizeof *order);
    double t0, t1;
    size_t total = 0;
    char name[64];
    if (!order) { vol_close(v); return 1; }
    for (i = 0; i < n; i++) order[i] = i;
    if (seed) {
        rng_state = seed;
        for (i = n; i-- > 1; ) {   /* Fisher-Yates */
            uint64_t j = rng_next() % (i + 1);
            uint64_t t = order[i]; order[i] = order[j]; order[j] = t;
        }
    }
    t0 = now_ms();
    for (i = 0; i < n; i++) {
        uint64_t ino;
        uint8_t *buf = NULL;
        size_t len = 0;
        snprintf(name, sizeof name, "f%05llu", (unsigned long long)order[i]);
        ino = vol_find(v, name);
        if (!ino || vol_read_file(v, ino, &buf, &len) != 0) {
            fprintf(stderr, "l2ptest: readall failed at %s\n", name);
            free(order);
            vol_close(v);
            return 1;
        }
        total += len;
        free(buf);
    }
    t1 = now_ms();
    free(order);
    printf("l2ptest: readall n=%llu bytes=%zu loop_ms=%.1f (%.1f us/file)\n",
           (unsigned long long)n, total, t1 - t0,
           (t1 - t0) * 1000.0 / (double)n);
    t0 = now_ms();
    vol_close(v);
    printf("l2ptest: readall close_ms=%.1f\n", now_ms() - t0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: l2ptest stress <img> <ops> <seed>\n"
                "       l2ptest pump <img> <file>...\n"
                "       l2ptest readflush <img> <file>...\n"
                "       l2ptest mkfiles <img> <n> <size>\n"
                "       l2ptest readall <img> <n> [seed]\n");
        return 2;
    }
    if (strcmp(argv[1], "stress") == 0 && argc == 5)
        return do_stress(argv[2], strtoull(argv[3], NULL, 10),
                         strtoull(argv[4], NULL, 10));
    if (strcmp(argv[1], "pump") == 0 && argc >= 4)
        return do_pump(argv[2], argc - 3, argv + 3, 1);
    if (strcmp(argv[1], "readflush") == 0 && argc >= 3)
        return do_pump(argv[2], argc - 3, argv + 3, 1);
    if (strcmp(argv[1], "mkfiles") == 0 && argc == 5)
        return do_mkfiles(argv[2], strtoull(argv[3], NULL, 10),
                          (size_t)strtoul(argv[4], NULL, 10));
    if (strcmp(argv[1], "readall") == 0 && argc >= 4)
        return do_readall(argv[2], strtoull(argv[3], NULL, 10),
                          argc >= 5 ? strtoull(argv[4], NULL, 10) : 0);
    fprintf(stderr, "l2ptest: bad args\n");
    return 2;
}
