/* meta_probe.c — isolate vol_apply_meta failures outside FUSE.
 *
 * usage: meta_probe <image> <name>           probe (MUTATES: applies a
 *                                              test setattr)
 *        meta_probe <image> --heat <name>    read-only dump: storage class
 *                                            + per-entry heat counters
 *        meta_probe <image> --xattr <name> <xattr>
 *        meta_probe <image> --zonefree
 *        meta_probe <image> --heatpump <name>...   MUTATES: reads each named
 *                                              file once through the real
 *                                              read path (one read-heat touch
 *                                              per segment), then folds the
 *                                              session's accrued read heat into
 *                                              the volume WITHOUT the sweep's
 *                                              halving decay, and flushes.
 *
 * --heatpump exists because vol_heat_persist() is a driver-facing API --
 * "persist read touches without a sweep run" -- and this is its driver. It
 * used to live in tools/l2ptest.c, whose whole subject was the retired
 * mapping journal; the three suites that need a sweep-cadence read session
 * (test-heat.sh, test-heat-mapper.sh, test-multidev.sh) call it here now.
 * It is a probe mode, not a shipped tool: it is how a test stands in for a
 * session that observed some reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "invarifs.h"
/* volume_internal.h instead of volume.h: meta_probe needs the v3 recipe and
 * AST loaders, and the two headers declare the same typedefs. */
#include "volume_internal.h"

/* WP27: dump the file's storage class, its AST block entries (zone/algo/
 * pba per segment) and the per-file heat counters from the "invfs.heat"
 * TLV (format v2: heat moved out of the journal pads into the record).
 * Read-only: nothing here touches the vol_read_* paths, so no heat
 * accrues and the volume closes clean. */
/* WP-B1: a named-xattr dump. Meta-v3's inode row has no ext blob, so
 * everything v3 keeps per file outside the row (heat, the anchor mark)
 * lives in the named-xattr tree. --heat walks the v2 record stream, which
 * on v3 cannot see any of it, so this is the only way to observe the v3
 * heat counters from a test. */
static int xattr_dump(invfs_volume *v, const char *name, const char *xname)
{
    uint8_t val[256];
    size_t vl = sizeof val;
    uint64_t id = vol_find(v, name);
    size_t i;

    printf("find(%s)=%llu\n", name, (unsigned long long)id);
    if (!id) return 1;
    if (vol_get_xattr(v, id, xname, val, &vl) != 0) {
        printf("xattr %s: absent\n", xname);
        return 1;
    }
    printf("xattr %s: %zu bytes:", xname, vl);
    for (i = 0; i < vl; i++) printf(" %02x", val[i]);
    printf("\n");
    /* the heat TLV is 4 bytes: rheat lo/hi, wheat, reserved -- print it in
     * the same shape heat_dump uses so callers can share a parser */
    if (strcmp(xname, INVFS_XATTR_HEAT) == 0 && vl >= 3)
        printf("heat rheat=%u wheat=%u\n",
               (unsigned)(val[0] | ((unsigned)val[1] << 8)),
               (unsigned)val[2]);
    return 0;
}

static int heat_dump(invfs_volume *v, const char *name)
{
    uint64_t id = vol_find(v, name);
    uint8_t cls = 0, algo = 0;
    uint16_t gen = 0;
    const invfs_superblock *sb = vol_sb(v);

    printf("find(%s)=%llu\n", name, (unsigned long long)id);
    if (!id) return 1;
    if (vol_get_class(v, id, &cls, &algo, &gen) == 0)
        printf("class=%u algo=%u gen=%u\n", cls, algo, gen);
    else
        printf("class=absent\n");

    /* the heat TLV (0/0 when absent) */
    {
        uint8_t hv[4] = {0, 0, 0, 0};
        size_t hl = sizeof hv;
        if (vol_get_xattr(v, id, INVFS_XATTR_HEAT, hv, &hl) == 0 && hl >= 3)
            printf("heat rheat=%u wheat=%u\n",
                   (unsigned)(hv[0] | ((unsigned)hv[1] << 8)),
                   (unsigned)hv[2]);
        else
            printf("heat=absent\n");
    }

    /* AST entries: the segment table is the inode's RMC1 recipe, read
     * through the same loader the engine uses. */
    {
        invfs_v3_inode in;
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t n_ents = 0, k;

        if (vol_v3_inode_get(v, id, &in) != 1) { printf("v3 inode read FAIL\n"); return 1; }
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
            printf("v3 recipe load FAIL\n");
            return 1;
        }
        /* A symlink's blob is its target string, not an AST, so there is
         * nothing to iterate and nothing to report. Say so, rather than
         * printing a false "v3 recipe parse FAIL" -- which reads as a
         * corrupt blob on a perfectly healthy volume and sends whoever
         * is debugging exactly the wrong thing. Same predicate the read
         * path dispatches on (src/core/volume.h); the LOAD above is kept
         * because this probe is the thing that reports an unreadable
         * blob, and skipping it here would hide a real failure. */
        if (invfs_inode_content_is_raw_blob(in.type)) {
            printf("symlink target: %.*s\n", (int)blen, (const char *)blob);
            free(blob);
            return 0;
        }
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 || !ents) {
            printf("v3 recipe parse FAIL\n");
            free(blob);
            return 1;
        }
        for (k = 0; k < n_ents; k++) {
            /* the physical extent comes from the segment's framed header
             * (the entry itself stores none) */
            uint64_t phys = 0;
            uint8_t hb[8];
            uint32_t cs;
            if (ents[k].pba && ents[k].pba < sb->total_blocks &&
                vol_read_raw(v, ents[k].pba * INVFS_BLOCK_SIZE, hb, 8) == 0) {
                memcpy(&cs, hb, 4);
                if (cs)
                    phys = ((uint64_t)cs + 8 + INVFS_BLOCK_SIZE - 1) /
                           INVFS_BLOCK_SIZE;
            }
            printf("ast i=%zu off=%llu len=%llu zone=%u algo=%u "
                   "bid=%u boff=%u pba=%llu phys=%llu\n", k,
                   (unsigned long long)ents[k].file_offset,
                   (unsigned long long)ents[k].length,
                   ents[k].zone, ents[k].algo,
                   ents[k].block_id, ents[k].block_offset,
                   (unsigned long long)ents[k].pba,
                   (unsigned long long)phys);
        }
        printf("entries=%zu\n", n_ents);
        free(blob);
        return 0;
    }
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0, rc;
    uint64_t id, r2;
    invfs_meta_pub m;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: %s <img> <name>|--heat <name>|--xattr <name> <xattr>|--zonefree|--heatpump <name>...\n", argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 3) { fprintf(stderr, "usage: %s <img> <name>|--heat <name>|--xattr <name> <xattr>|--zonefree|--heatpump <name>...\n",
                            argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    v = vol_open(argv[1], &err);
    if (!v) { printf("open FAIL err=%d\n", err); return 1; }
    if (strcmp(argv[2], "--heatpump") == 0) {
        int i, bad = 0;
        if (argc < 4) { vol_close(v); return 2; }
        for (i = 3; i < argc; i++) {
            uint64_t ino = vol_find(v, argv[i]);
            uint8_t *buf = NULL;
            size_t len = 0;
            if (!ino) {
                fprintf(stderr, "heatpump: '%s' not found\n", argv[i]);
                bad = 1;
                continue;
            }
            /* the REAL read path: this is what accrues the touches. A probe
             * that short-circuited it would accrue nothing and the suites
             * would go green on a volume whose heat never moved. */
            if (vol_read_file(v, ino, &buf, &len) != 0) {
                fprintf(stderr, "heatpump: read failed for '%s'\n", argv[i]);
                bad = 1;
                continue;
            }
            free(buf);
        }
        /* fold the session's read heat into the volume; the sweep's decay
         * pass does this too, plus the halving, and is not what a test
         * asking for "16 read sessions, rheat >= 16" wants. */
        if (!bad) vol_heat_persist(v);
        if (!bad && vol_flush(v) != 0) {
            fprintf(stderr, "heatpump: flush failed\n");
            bad = 1;
        }
        vol_close(v);
        return bad;
    }
    if (strcmp(argv[2], "--heat") == 0) {
        if (argc < 4) { vol_close(v); return 2; }
        rc = heat_dump(v, argv[3]);
        vol_close(v);
        return rc;
    }
    if (strcmp(argv[2], "--xattr") == 0) {
        if (argc < 5) { vol_close(v); return 2; }
        rc = xattr_dump(v, argv[3], argv[4]);
        vol_close(v);
        return rc;
    }
    if (strcmp(argv[2], "--zonefree") == 0) {
        uint64_t rf = 0, rt = 0, sf = 0, st = 0;
        rc = vol_zone_free(v, &rf, &rt, &sf, &st);
        printf("raw %llu/%llu shadow %llu/%llu\n",
               (unsigned long long)(rt - rf), (unsigned long long)rt,
               (unsigned long long)(st - sf), (unsigned long long)st);
        vol_close(v);
        return rc;
    }
    id = vol_find(v, argv[2]);
    printf("find(%s)=%llu\n", argv[2], (unsigned long long)id);
    if (!id) { vol_close(v); return 1; }
    rc = vol_get_meta(v, id, &m);
    printf("get_meta#1 rc=%d", rc);
    if (rc == 0) printf(" type=%d mode=%o uid=%u gid=%u target='%s'",
                        m.type, m.mode, m.uid, m.gid, m.target);
    printf("\n");

    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0755;
    m.uid = 1000;
    m.gid = 1000;
    m.mtime = m.atime = (int64_t)time(NULL);
    m.nlink = 1;
    r2 = vol_apply_meta(v, argv[2], &m);
    printf("apply_meta -> %llu (0=fail)\n", (unsigned long long)r2);

    rc = vol_get_meta(v, id, &m);
    printf("get_meta#2 rc=%d", rc);
    if (rc == 0) printf(" type=%d mode=%o uid=%u gid=%u", m.type, m.mode, m.uid, m.gid);
    printf("\n");
    vol_close(v);
    return 0;
}
