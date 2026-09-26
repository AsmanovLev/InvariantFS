/*
 * btree_repair_test.c — WP86: a torn metadata-v3 base page must be
 * survivable, not fatal.
 *
 * Meta-v3 keeps the namespace in an immutable COW B+-tree anchored by the
 * RT30 double slot. Before WP86 a single unreadable base page was reported by
 * invf-fsck and repaired by nothing: `fsck -f` was a no-op, invf-rollback
 * could not help, and -- far worse -- a torn *root* page made
 * mbuf_root_read() answer "no root", so the whole namespace silently read as
 * EMPTY (every lookup returned "absent", not EIO). One lost page bricked the
 * volume or, worse, hid it.
 *
 * This is the deterministic regression gate for the repair: no fault
 * injection, no dm-flakey, no timing. Each phase tears exactly one page (a
 * payload byte, so the page CRC fails) of a freshly folded volume and asserts
 * the recovery contract:
 *
 *   A  build   a real namespace (named files + filler rows) and fold it into
 *              the base, so every key lives in exactly one tier;
 *   B  tear    one base leaf's CRC, chosen as a leaf that holds named files'
 *              inode rows and dirents (recorded with its key range, the pages
 *              of the whole tree, and the ids inside it);
 *   C  report  invf-fsck finds the damage, counts EVERY bad page (not just
 *              the first), verifies every other page, and names the
 *              quarantined key range; its exit code is 3;
 *   D  read    a quarantined key reads EIO -- never 0/absent, never garbage;
 *              every key outside the range still reads back byte-identical;
 *              a readdir of a directory holding a quarantined inode fails
 *              loudly instead of silently listing fewer names;
 *   E  repair  invf-fsck -f rebuilds a structurally valid tree that still
 *              holds every key outside the range, restores the keys the delta
 *              still covers inside it, and drops only the ones that are gone.
 *              The repair pass itself must exit nonzero (the alarm is raised
 *              by the pass that found the damage) while the volume after it
 *              is clean;
 *   F  root    a torn root page reads EIO, not "absent" (the silent-empty
 *              regression), and -f refuses with a reason instead of
 *              pretending;
 *   G  spt0    a save point whose base tree is damaged is DETECTED, not
 *              used: spt0_restore refuses to publish it.
 *
 * Usage: invf-btree_repair_test [scratch-dir]
 * exit 0 = all checks passed, 1 = a check failed, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "volume_internal.h"
#include "vol_btree.h"
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
    } else {
        printf("  ok    %s\n", what);
    }
}

#define NFILL 900u          /* filler inode rows -> a multi-level tree */
#define NFILE 40u           /* named files in the root directory       */
#define FIRST_ID 1000ull    /* first filler inode id                    */
#define RESCUE_ID (FIRST_ID + NFILL / 2)   /* a delta record inside the
                                             * quarantined key range: the
                                             * repair must bring it back */

static const char *g_bin = ".";

/* ---- key encodings (frozen WP-M5/M6 forms) ---------------------------- */

static void ino_key(uint8_t k[8], uint64_t id)
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(id >> (56 - 8 * i));
}

static uint64_t be64(const uint8_t *k)
{
    uint64_t id = 0;
    int i;
    for (i = 0; i < 8; i++)
        id = (id << 8) | k[i];
    return id;
}

static void hexkey(char *out, size_t cap, const uint8_t *k, uint16_t n,
                   int unbounded)
{
    size_t i = 0;
    int j;
    if (unbounded) {
        snprintf(out, cap, "[+inf)");
        return;
    }
    for (j = 0; j < (int)n && i + 3 < cap; j++)
        i += (size_t)snprintf(out + i, cap - i, "%02x", k[j]);
    out[i] = 0;
}

static invfs_volume *g_v;
static char g_img[512];
static uint64_t g_file_id[NFILE];

/* ---- page walk: pick a leaf and count the tree's pages ---------------- */

typedef struct {
    uint64_t victim;          /* pba of the chosen leaf                    */
    uint8_t  vlo[300];        /* its key range, low bound (inclusive)      */
    uint16_t vlo_n;
    uint8_t  vhi[300];        /* high bound (exclusive); vhi_unb if unset */
    uint16_t vhi_n;
    int      vhi_unb;
    uint64_t ids[600];        /* the inode ids the victim held             */
    int      nids;
    uint64_t pages;           /* pages reachable in the whole tree         */
} leafpick;

static uint64_t g_pages;      /* every page the walk below visits          */
static uint64_t g_leaf_rows;  /* inode rows in the current leaf            */
static uint64_t g_leaf_files; /* named-file inode rows in the current leaf */
static uint64_t g_best_rows;  /* ranking of the accepted victim            */
static int g_want_files = 1;  /* rank leaves holding a named-file row      */
static uint64_t g_best_files;
static leafpick g_lp;
static uint8_t g_cur_ids[600 * 8];
static int g_cur_n;

/* Walk every page: count them, and rank the leaves (inode rows first, named
 * files as the tie-break) so the victim is deterministic. */
static void scan_rec(invfs_blkptr ptr)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    uint8_t *p;
    int n, i;

    if (mbuf_read_ptr(g_v, &ptr, buf) != 0)
        return;                              /* unreadable: not counted */
    h = mbuf_page_chdr(buf);
    n = h->nentries;
    p = buf + sizeof(invfs_page_hdr);
    g_pages++;

    if (h->level == INVFS_PAGE_LEVEL_LEAF) {
        g_leaf_rows = 0;
        g_leaf_files = 0;
        g_cur_n = 0;
        for (i = 0; i < n; i++) {
            const uint8_t *kp;
            uint16_t kl, vl;
            memcpy(&kl, p, 2);
            p += 2;
            kp = p;
            p += kl;
            memcpy(&vl, p, 2);
            p += 2 + vl;
            if (kl != 8)
                continue;
            {
                uint64_t id = be64(kp);
                int j;
                if (id >= FIRST_ID && id < FIRST_ID + NFILL)
                    g_leaf_rows++;
                for (j = 0; j < (int)NFILE; j++)
                    if (g_file_id[j] == id)
                        g_leaf_files++;
                if (g_cur_n < 600) {
                    memcpy(g_cur_ids + (size_t)g_cur_n * 8, kp, 8);
                    g_cur_n++;
                }
            }
        }
        /* rank: prefer a leaf holding a named file, then the densest one */
        if ((g_leaf_files || !g_want_files) &&
            (g_leaf_files > g_best_files ||
             (g_leaf_files == g_best_files && g_leaf_rows > g_best_rows))) {
            g_best_files = g_leaf_files;
            g_best_rows = g_leaf_rows;
            g_lp.victim = ptr.pba;
            g_lp.nids = 0;
            g_lp.vlo_n = 0;
            g_lp.vhi_n = 0;
            g_lp.vhi_unb = 1;
            for (i = 0; i < g_cur_n; i++)
                g_lp.ids[g_lp.nids++] = be64(g_cur_ids + (size_t)i * 8);
            /* the leaf's low bound is its first key */
            p = buf + sizeof(invfs_page_hdr);
            memcpy(&g_lp.vlo_n, p, 2);
            if (g_lp.vlo_n > sizeof g_lp.vlo)
                g_lp.vlo_n = (uint16_t)sizeof g_lp.vlo;
            memcpy(g_lp.vlo, p + 2, g_lp.vlo_n);
        }
        return;
    }

    for (i = 0; i < n; i++) {
        uint16_t kl;
        invfs_blkptr child;
        memcpy(&kl, p, 2);
        p += 2;
        memcpy(&child, p + kl, sizeof child);
        p += kl + sizeof child;
        scan_rec(child);
    }
}

/* Every leaf in the tree, with the first key of each (phase H tears several
 * pages at once, so it needs the leaves rather than one chosen victim). */
typedef struct {
    uint64_t pba[512];
    uint8_t  first[512][300];   /* a dirent key can be 210 bytes */
    uint16_t first_n[512];
    int      nent[512];         /* records in that leaf */
    int      n;
} leaflist;

static void list_rec(invfs_blkptr ptr, leaflist *ll)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    uint8_t *p;
    int n, i;

    if (mbuf_read_ptr(g_v, &ptr, buf) != 0)
        return;
    h = mbuf_page_chdr(buf);
    n = h->nentries;
    p = buf + sizeof(invfs_page_hdr);
    if (h->level == INVFS_PAGE_LEVEL_LEAF) {
        if (ll->n < 512) {
            uint16_t kl;
            memcpy(&kl, p, 2);
            ll->first_n[ll->n] = kl < 300 ? kl : 300;
            memcpy(ll->first[ll->n], p + 2, ll->first_n[ll->n]);
            ll->pba[ll->n] = ptr.pba;
            ll->nent[ll->n] = n;
            ll->n++;
        }
        return;
    }
    for (i = 0; i < n; i++) {
        uint16_t kl;
        invfs_blkptr child;
        memcpy(&kl, p, 2);
        p += 2;
        memcpy(&child, p + kl, sizeof child);
        p += kl + sizeof child;
        list_rec(child, ll);
    }
}

/* The victim's upper bound: the first key at or after the victim's low bound
 * that lives in a LATER leaf. 0 = found (*hi set), 1 = keep searching. */
static int fill_vhi(invfs_blkptr ptr, const uint8_t *lo, uint16_t lon)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    uint8_t *p;
    int n, i;

    if (mbuf_read_ptr(g_v, &ptr, buf) != 0)
        return 1;
    h = mbuf_page_chdr(buf);
    n = h->nentries;
    p = buf + sizeof(invfs_page_hdr);

    if (h->level == INVFS_PAGE_LEVEL_LEAF) {
        if (ptr.pba == g_lp.victim)
            return 1;                        /* the victim: keep searching */
        for (i = 0; i < n; i++) {
            const uint8_t *kp;
            uint16_t kl, vl, cn;
            int c;
            memcpy(&kl, p, 2);
            p += 2;
            kp = p;
            p += kl;
            memcpy(&vl, p, 2);
            p += 2 + vl;
            cn = lon < kl ? lon : kl;
            c = cn ? memcmp(kp, lo, cn) : 0;
            if (c > 0) {
                g_lp.vhi_n = cn;
                memcpy(g_lp.vhi, kp, cn);
                g_lp.vhi_unb = 0;
                return 0;
            }
        }
        return 1;
    }
    for (i = 0; i < n; i++) {
        uint16_t kl;
        invfs_blkptr child;
        memcpy(&kl, p, 2);
        p += 2;
        memcpy(&child, p + kl, sizeof child);
        p += kl + sizeof child;
        if (fill_vhi(child, lo, lon) == 0)
            return 0;
    }
    return 1;
}

/* Byte-lexicographic compare of two keys; `hi_unb` makes `hi` +infinity.
 * Returns <0 when key < bound, 0 when equal, >0 when key > bound. */
static int keycmp(const uint8_t *k, uint16_t kn,
                  const uint8_t *b, uint16_t bn, int b_unb)
{
    uint16_t m;
    int c;
    if (b_unb)
        return -1;
    m = kn < bn ? kn : bn;
    c = m ? memcmp(k, b, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (kn < bn)
        return -1;
    if (kn > bn)
        return 1;
    return 0;
}

static int in_quarantine(uint64_t id)
{
    uint8_t k[8];
    ino_key(k, id);
    return keycmp(k, 8, g_lp.vlo, g_lp.vlo_n, 0) >= 0 &&
           keycmp(k, 8, g_lp.vhi, g_lp.vhi_n, g_lp.vhi_unb) < 0;
}

/* ---- raw page corruption (the deterministic tear) --------------------- */

/* Flip one payload byte so the page's own CRC no longer matches: exactly what
 * a drop_writes window that swallows a base page leaves behind -- the page is
 * named by a valid pointer but its bytes are not what was sealed. */
static int tear_page(const char *img, uint64_t pba)
{
    int fd = open(img, O_RDWR);
    uint8_t b[1];
    off_t off = (off_t)pba * INVFS_BLOCK_SIZE + 64;

    if (fd < 0)
        return -1;
    if (lseek(fd, off, SEEK_SET) != off || read(fd, b, 1) != 1) {
        close(fd);
        return -1;
    }
    b[0] ^= 0x5A;
    if (lseek(fd, off, SEEK_SET) != off || write(fd, b, 1) != 1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* ---- the invf-fsck CLI: the exit code IS the contract ----------------- */

static char g_cli_log[512];

static int fsck_cli(const char *img, int fix)
{
    char cmd[1400];
    FILE *f;
    char ln[256];
    int rc;

    if (!g_cli_log[0])
        return -3;
    snprintf(cmd, sizeof cmd, "%s/bin/invf-fsck %s%s >%s 2>&1",
             g_bin, img, fix ? " -f" : "", g_cli_log);
    rc = system(cmd);
    f = fopen(g_cli_log, "r");
    if (f) {
        while (fgets(ln, sizeof ln, f)) {
            ln[strcspn(ln, "\n")] = 0;
            printf("    | %s\n", ln);
        }
        fclose(f);
    }
    if (rc == -1)
        return -1;
    return (rc & 0x7f) ? -2 : ((rc >> 8) & 0xff);
}

/* The volume takes an exclusive image lock, so an in-process handle and the
 * CLI cannot share it: every CLI probe closes the handle first. */
static void close_v(void)
{
    if (g_v) {
        vol_close(g_v);
        g_v = NULL;
    }
}

static int open_v(const char *img)
{
    int err = 0;
    g_v = vol_open(img, &err);
    if (!g_v)
        fprintf(stderr, "btree_repair_test: vol_open(%s) failed: err=%d\n",
                img, err);
    return g_v ? 0 : -1;
}

/* Run the CLI on `img` with the in-process handle released. */
static int fsck_cli_offline(const char *img, int fix)
{
    int rc;
    close_v();
    rc = fsck_cli(img, fix);
    (void)open_v(img);
    return rc;
}

/* ---- phase A: build a namespace and fold it into the base ------------ */

static int build_and_fold(void)
{
    int err = 0, i;
    uint8_t k[8], val[INVFS_V3_INODE_ROW_FIXED];
    invfs_v3_inode_row r;

    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "btree_repair_test: vol_open(%s) failed: err=%d\n",
                g_img, err);
        return -1;
    }
    if (!(vol_sb(g_v)->vol_flags & VOLF_V3)) {
        fprintf(stderr, "btree_repair_test: %s is not a v3 volume\n", g_img);
        return -1;
    }

    /* named files: dirent + inode row + recipe, through the delta tier */
    for (i = 0; i < (int)NFILE; i++) {
        char name[64];
        invfs_meta_pub m;
        static uint8_t data[512];
        uint64_t id;
        snprintf(name, sizeof name, "f%03u.bin", i);
        memset(data, (uint8_t)('A' + (i % 26)), sizeof data);
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_REG;
        m.mode = 0644;
        m.uid = m.gid = 1000;
        m.nlink = 1;
        m.mtime = m.atime = (int64_t)time(NULL);
        m.size = sizeof data;
        id = vol_v3_write_bulk(g_v, name, data, sizeof data, &m);
        if (!id) {
            fprintf(stderr, "btree_repair_test: write_bulk(%s) failed\n", name);
            return -1;
        }
        g_file_id[i] = id;
    }

    /* filler rows: enough keys to force a multi-level tree */
    memset(&r, 0, sizeof r);
    r.row_version = INVFS_V3_INODE_ROW_VERSION;
    r.type = INVFS_ITYP_REG;
    r.mode = 0600;
    r.uid = r.gid = 1000;
    r.nlink = 1;
    r.mtime = r.atime = (int64_t)time(NULL);
    for (i = 0; i < (int)NFILL; i++) {
        uint64_t id = FIRST_ID + (uint64_t)i;
        ino_key(k, id);
        r.size = (uint64_t)i;
        if (vol_delta_append(g_v, k, sizeof k, (const uint8_t *)&r,
                             (uint16_t)sizeof r, 0) != 0) {
            fprintf(stderr, "btree_repair_test: delta_append(%llu) failed\n",
                    (unsigned long long)id);
            return -1;
        }
    }

    if (vol_v3_fold(g_v) != 0) {
        fprintf(stderr, "btree_repair_test: fold failed\n");
        return -1;
    }
    {
        invfs_blkptr root;
        bt_stat st;
        char e[128];
        e[0] = 0;
        if (vol_v3_base_root(g_v, &root) != 0) {
            fprintf(stderr, "btree_repair_test: base root unreadable\n");
            return -1;
        }
        if (btree_check(g_v, root, &st, e, sizeof e) != 0) {
            fprintf(stderr, "btree_repair_test: folded tree invalid: %s\n", e);
            return -1;
        }
        printf("  base: %llu pages, %llu keys, height %u\n",
               (unsigned long long)st.n_pages, (unsigned long long)st.nkeys,
               st.height);
        if (st.height < 2) {
            fprintf(stderr, "btree_repair_test: tree is too shallow to test "
                            "subtree containment\n");
            return -1;
        }
    }

    /* the rescue record: an inode row that lives ONLY in the delta and whose
     * key falls inside the key range the tear will quarantine. The repair must
     * bring it back (it is not lost -- the delta still has it). */
    {
        invfs_v3_inode_row rr;
        uint64_t id = RESCUE_ID;
        memset(&rr, 0, sizeof rr);
        rr.row_version = INVFS_V3_INODE_ROW_VERSION;
        rr.type = INVFS_ITYP_REG;
        rr.mode = 0640;
        rr.uid = rr.gid = 1000;
        rr.nlink = 1;
        rr.size = 0x5AFE5AFEull;
        rr.mtime = rr.atime = (int64_t)time(NULL);
        ino_key(k, id);
        memcpy(val, &rr, sizeof rr);
        if (vol_delta_append(g_v, k, sizeof k, val,
                             (uint16_t)sizeof rr, 0) != 0) {
            fprintf(stderr, "btree_repair_test: rescue delta append failed\n");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    invfs_blkptr root;
    bt_stat st;
    char err[128];
    invfs_fsck_report rep;
    int err_open = 0, i, eio = 0, abs = 0, invented = 0, kept = 0, outside = 0;
    int lost_names = 0, rc, cli;
    char cmd[1200];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: %s [scratch-dir]\n", argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("%s version %s (build %s)\n", argv[0],
                   INVFS_VERSION_STRING, INVFS_BUILD_DATE);
            return 0;
        }
    }
    if (getenv("PWD"))
        g_bin = getenv("PWD");
    snprintf(g_img, sizeof g_img, "%s/invf-btree-repair-test.img", dir);
    snprintf(g_cli_log, sizeof g_cli_log, "%s/invf-btree-repair-fsck.log", dir);
    unlink(g_img);

    printf("WP86: a torn v3 base page must be survivable (btree_repair_test)\n");

    /* ---- A: build + fold ---- */
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null", g_bin, g_img);
    if (system(cmd) != 0) {
        printf("  cannot create volume with invf-mkfs\n");
        return 2;
    }
    if (build_and_fold() != 0)
        return 2;

    /* ---- B: pick a leaf, tear it ---- */
    memset(&g_lp, 0, sizeof g_lp);
    if (vol_v3_base_root(g_v, &root) != 0)
        return 2;
    scan_rec(root);
    g_lp.pages = g_pages;
    if (!g_lp.victim || g_lp.nids == 0) {
        fprintf(stderr, "btree_repair_test: no leaf with named-file rows\n");
        return 2;
    }
    if (fill_vhi(root, g_lp.vlo, g_lp.vlo_n) != 0) {
        fprintf(stderr, "btree_repair_test: victim has no upper bound\n");
        return 2;
    }
    {
        char a[160], b[160];
        int nfiles = 0;
        for (i = 0; i < (int)NFILE; i++)
            if (in_quarantine(g_file_id[i]))
                nfiles++;
        hexkey(a, sizeof a, g_lp.vlo, g_lp.vlo_n, 0);
        hexkey(b, sizeof b, g_lp.vhi, g_lp.vhi_n, g_lp.vhi_unb);
        printf("  tree: %llu pages; tearing leaf pba %llu: key range "
               "[%s, %s) with %d inode rows (%d of them named files)\n",
               (unsigned long long)g_lp.pages, (unsigned long long)g_lp.victim,
               a, b, g_lp.nids, nfiles);
        if (nfiles == 0) {
            fprintf(stderr, "btree_repair_test: victim holds no named file\n");
            return 2;
        }
        if (g_lp.vhi_unb) {
            fprintf(stderr, "btree_repair_test: victim has an unbounded top\n");
            return 2;
        }
    }
    vol_close(g_v);
    g_v = NULL;
    if (tear_page(g_img, g_lp.victim) != 0) {
        fprintf(stderr, "btree_repair_test: could not tear pba %llu\n",
                (unsigned long long)g_lp.victim);
        return 2;
    }

    /* ---- reopen on the damaged volume ---- */
    g_v = vol_open(g_img, &err_open);
    ok(g_v != NULL, "vol_open still succeeds on a volume with one torn page");
    if (!g_v)
        return 1;

    /* ---- C: report ---- */
    memset(&rep, 0, sizeof rep);
    err[0] = 0;
    vol_fsck_scan(g_v, &rep, 0);
    printf("  fsck report: damaged=%d bad_pages=%llu keys=%llu pages=%llu "
           "(of %llu)\n", rep.v3_damaged, (unsigned long long)rep.v3_bad_pages,
           (unsigned long long)rep.v3_keys, (unsigned long long)rep.v3_pages_walked,
           (unsigned long long)g_lp.pages);
    ok(rep.v3_damaged == 1, "fsck reports the volume DAMAGED");
    ok(rep.v3_bad_pages >= 1, "fsck counts the unreadable page");
    ok(rep.v3_pages_walked == g_lp.pages,
       "fsck walks the WHOLE tree past the bad page (containment, not abort)");
    ok(rep.v3_quarantined >= 1,
       "fsck names the quarantined key range(s)");

    /* the CLI is the contract: nonzero on damage, always */
    cli = fsck_cli_offline(g_img, 0);
    printf("  invf-fsck exit code on the damaged volume: %d\n", cli);
    ok(cli == 3, "invf-fsck exits 3 (damage) on a damaged volume");

    /* ---- D: reads ---- */
    for (i = 0; i < (int)NFILE; i++) {
        invfs_v3_inode in;
        int r = vol_v3_inode_get(g_v, g_file_id[i], &in);
        if (!in_quarantine(g_file_id[i]))
            continue;
        lost_names++;
        if (r < 0)
            eio++;
        else if (r == 0)
            abs++;
        else
            invented++;
    }
    for (i = 0; i < (int)NFILL; i++) {
        uint64_t id = FIRST_ID + (uint64_t)i;
        invfs_v3_inode in;
        int r;
        if (id == RESCUE_ID)
            continue;          /* delta-only: asserted separately below */
        r = vol_v3_inode_get(g_v, id, &in);
        if (in_quarantine(id)) {
            if (r < 0)
                eio++;
            else if (r == 0)
                abs++;
            else
                invented++;
        } else {
            outside++;
            if (r == 1 && in.size == (uint64_t)i)
                kept++;
        }
    }
    {
        invfs_v3_inode in;
        int r = vol_v3_inode_get(g_v, RESCUE_ID, &in);
        ok(r == 1 && in.size == 0x5AFE5AFEull,
           "a delta-only key inside the quarantined range still reads back");
    }
    printf("  quarantined: %d named names + filler ids -> %d EIO, %d absent, "
           "%d invented | outside: %d/%d read back\n",
           lost_names, eio, abs, invented, kept, outside);
    ok(abs == 0, "a quarantined key does NOT read as absent (no silent loss)");
    ok(invented == 0, "a quarantined key does NOT read as data (no invention)");
    ok(eio == lost_names, "a quarantined key reads EIO");
    ok(kept == outside, "every key outside the range still reads back");

    /* a named file whose dirent + inode row are in the torn leaf: the listing
     * must not silently come back shorter (WP86: never invent, never hide). */
    {
        invfs_dirent ents[256];
        int n = vol_v3_path_list_dir(g_v, "", ents, 256);
        printf("  readdir of the root: %d entries\n", n);
        ok(n < 0, "readdir fails loudly instead of silently listing fewer names");
    }

    /* ---- E: repair ---- */
    cli = fsck_cli_offline(g_img, 1);
    printf("  invf-fsck -f exit code on the damaged volume: %d\n", cli);
    ok(cli == 3, "invf-fsck -f exits 3 too: the pass that found the damage "
                 "raises the alarm even as it repairs");
    cli = fsck_cli_offline(g_img, 0);
    printf("  invf-fsck exit code after the repair: %d\n", cli);
    ok(cli == 0, "invf-fsck is clean (exit 0) once the volume is repaired");

    err[0] = 0;
    if (vol_v3_base_root(g_v, &root) == 0 &&
        btree_check(g_v, root, &st, err, sizeof err) == 0)
        ok(1, "the repaired base tree is structurally valid");
    else
        ok(0, "the repaired base tree is structurally valid");

    {
        int kept2 = 0, outside2 = 0, rescued = 0, gone = 0;
        invfs_v3_inode in;
        for (i = 0; i < (int)NFILL; i++) {
            uint64_t id = FIRST_ID + (uint64_t)i;
            int r;
            if (id == RESCUE_ID)
                continue;      /* delta-only: counted as `rescued` below */
            r = vol_v3_inode_get(g_v, id, &in);
            if (in_quarantine(id)) {
                if (r == 1)
                    rescued++;
                else
                    gone++;
            } else {
                outside2++;
                if (r == 1 && in.size == (uint64_t)i)
                    kept2++;
            }
        }
        rescued += vol_v3_inode_get(g_v, RESCUE_ID, &in) == 1 &&
                   in.size == 0x5AFE5AFEull ? 1 : 0;
        printf("  after repair: %d/%d keys outside the range intact, "
               "%d quarantined keys recovered from the delta, %d gone\n",
               kept2, outside2, rescued, gone);
        ok(kept2 == outside2, "the repair kept every readable key");
        ok(rescued == 1, "the repair recovered the delta's copy of a "
                         "quarantined key");
    }
    {
        int still = 0;
        for (i = 0; i < (int)NFILE; i++) {
            char nm[64];
            uint64_t id = 0, sz = 0, ct = 0;
            snprintf(nm, sizeof nm, "f%03u.bin", i);
            if (vol_v3_path_stat(g_v, nm, &id, &sz, &ct) == 0 &&
                !in_quarantine(g_file_id[i]))
                still++;
        }
        printf("  named files still resolvable by path after the repair: %d\n",
               still);
        ok(still > 0, "the surviving names still resolve by path");
    }

    /* the repair must survive a remount */
    vol_close(g_v);
    g_v = NULL;
    g_v = vol_open(g_img, &err_open);
    ok(g_v != NULL, "the repaired volume reopens");
    if (g_v) {
        err[0] = 0;
        if (vol_v3_base_root(g_v, &root) == 0 &&
            btree_check(g_v, root, &st, err, sizeof err) == 0)
            ok(1, "the repaired tree is still valid after a remount");
        else
            ok(0, "the repaired tree is still valid after a remount");
        vol_close(g_v);
        g_v = NULL;
    }
    unlink(g_img);

    /* ---- H: several damaged pages at once ----
     * One torn page is the common case; the walk must contain N of them, name
     * N ranges and excise N subtrees in one pass. This tears three leaves
     * spread across the tree and asserts the same contract for all of them. */
    {
        char img3[512];
        uint64_t victims[3];
        uint8_t vlos[3][300];
        uint16_t vlos_n[3];
        uint8_t vhis[3][300];
        uint16_t vhis_n[3];
        int vhi_unb[3];
        int nv = 0, k, l;
        int eio2 = 0, abs2 = 0, inv2 = 0, kept2 = 0, out2 = 0;

        snprintf(img3, sizeof img3, "%s/invf-btree-repair-multi.img", dir);
        unlink(img3);
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 g_bin, img3);
        if (system(cmd) != 0)
            return 2;
        g_v = vol_open(img3, &err_open);
        if (!g_v)
            return 2;
        {
            uint8_t kk[8], vv[INVFS_V3_INODE_ROW_FIXED];
            invfs_v3_inode_row rr;
            memset(&rr, 0, sizeof rr);
            rr.row_version = INVFS_V3_INODE_ROW_VERSION;
            rr.type = INVFS_ITYP_REG;
            rr.mode = 0600;
            rr.uid = rr.gid = 1000;
            rr.nlink = 1;
            rr.mtime = rr.atime = (int64_t)time(NULL);
            for (k = 0; k < (int)NFILL; k++) {
                ino_key(kk, FIRST_ID + (uint64_t)k);
                rr.size = (uint64_t)k;
                memcpy(vv, &rr, sizeof rr);
                if (vol_delta_append(g_v, kk, sizeof kk, vv,
                                     (uint16_t)sizeof rr, 0) != 0)
                    return 2;
            }
            if (vol_v3_fold(g_v) != 0)
                return 2;
        }
        /* collect every leaf, then take three spread across the tree */
        {
            leaflist ll;
            memset(&ll, 0, sizeof ll);
            if (vol_v3_base_root(g_v, &root) != 0)
                return 2;
            list_rec(root, &ll);
            if (ll.n < 6) {
                fprintf(stderr, "btree_repair_test: not enough leaves (%d)\n",
                        ll.n);
                return 2;
            }
            for (k = 0; k < 3; k++) {
                int idx = (int)((long)ll.n * k / 3) + 1;
                if (idx >= ll.n)
                    idx = ll.n - 1;
                victims[nv] = ll.pba[idx];
                memcpy(vlos[nv], ll.first[idx], ll.first_n[idx]);
                vlos_n[nv] = ll.first_n[idx];
                vhi_unb[nv] = 0;
                vhis_n[nv] = 0;
                nv++;
            }
            /* exact upper bound of each victim: the first key of the next leaf */
            for (k = 0; k < nv; k++) {
                memset(&g_lp, 0, sizeof g_lp);
                g_lp.victim = victims[k];
                memcpy(g_lp.vlo, vlos[k], vlos_n[k]);
                g_lp.vlo_n = vlos_n[k];
                if (fill_vhi(root, g_lp.vlo, g_lp.vlo_n) != 0) {
                    fprintf(stderr, "btree_repair_test: victim %d unbounded\n", k);
                    return 2;
                }
                memcpy(vhis[k], g_lp.vhi, g_lp.vhi_n);
                vhis_n[k] = g_lp.vhi_n;
            }
        }
        printf("  tearing 3 base leaves: ");
        for (k = 0; k < nv; k++)
            printf("pba %llu ", (unsigned long long)victims[k]);
        printf("\n");
        vol_close(g_v);
        g_v = NULL;
        for (k = 0; k < nv; k++)
            if (tear_page(img3, victims[k]) != 0)
                return 2;

        g_v = vol_open(img3, &err_open);
        ok(g_v != NULL, "vol_open succeeds with three torn pages");
        if (!g_v)
            return 1;
        memset(&rep, 0, sizeof rep);
        vol_fsck_scan(g_v, &rep, 0);
        printf("  fsck report: bad_pages=%llu quarantined=%llu pages=%llu\n",
               (unsigned long long)rep.v3_bad_pages,
               (unsigned long long)rep.v3_quarantined,
               (unsigned long long)rep.v3_pages_walked);
        ok(rep.v3_bad_pages == 3, "fsck counts ALL THREE unreadable pages "
                                  "(the pre-fix walk stopped at the first)");
        ok(rep.v3_quarantined == 3, "fsck reports three quarantined ranges");

        /* every key in a quarantined range reads EIO; every other key reads */
        for (k = 0; k < (int)NFILL; k++) {
            uint64_t id = FIRST_ID + (uint64_t)k;
            uint8_t kk[8];
            invfs_v3_inode in;
            int r, inq = 0;
            ino_key(kk, id);
            for (l = 0; l < nv; l++)
                if (keycmp(kk, 8, vlos[l], vlos_n[l], 0) >= 0 &&
                    keycmp(kk, 8, vhis[l], vhis_n[l], vhi_unb[l]) < 0)
                    inq = 1;
            r = vol_v3_inode_get(g_v, id, &in);
            if (inq) {
                if (r < 0)
                    eio2++;
                else if (r == 0)
                    abs2++;
                else
                    inv2++;
            } else {
                out2++;
                if (r == 1 && in.size == (uint64_t)k)
                    kept2++;
            }
        }
        printf("  quarantined across 3 ranges: %d EIO, %d absent, %d invented | "
               "outside: %d/%d read back\n", eio2, abs2, inv2, kept2, out2);
        ok(eio2 > 0, "the three torn leaves hold keys and they read EIO");
        ok(abs2 == 0 && inv2 == 0,
           "no key in any quarantined range reads as absent or as data");
        ok(kept2 == out2, "every key outside the three ranges still reads back");

        cli = fsck_cli_offline(img3, 1);
        printf("  invf-fsck -f with three torn pages: exit %d\n", cli);
        ok(cli == 3, "invf-fsck -f exits 3 after repairing three pages");
        err[0] = 0;
        if (vol_v3_base_root(g_v, &root) == 0 &&
            btree_check(g_v, root, &st, err, sizeof err) == 0)
            ok(1, "the tree with three excised subtrees is structurally valid");
        else
            ok(0, "the tree with three excised subtrees is structurally valid");
        {
            int kept3 = 0, out3 = 0;
            for (k = 0; k < (int)NFILL; k++) {
                uint64_t id = FIRST_ID + (uint64_t)k;
                uint8_t kk[8];
                invfs_v3_inode in;
                int r, inq = 0;
                ino_key(kk, id);
                for (l = 0; l < nv; l++)
                    if (keycmp(kk, 8, vlos[l], vlos_n[l], 0) >= 0 &&
                        keycmp(kk, 8, vhis[l], vhis_n[l], vhi_unb[l]) < 0)
                        inq = 1;
                r = vol_v3_inode_get(g_v, id, &in);
                if (inq)
                    continue;
                out3++;
                if (r == 1 && in.size == (uint64_t)k)
                    kept3++;
            }
            printf("  after the repair: %d/%d keys outside the three ranges "
                   "intact\n", kept3, out3);
            ok(kept3 == out3, "the repair kept every readable key (3 ranges)");
        }
        vol_close(g_v);
        g_v = NULL;
        unlink(img3);
    }

    /* ---- I: long keys around the damage ----
     * A dirent key is parent:u64 + name_len:u16 + name, so a 200-character
     * file name makes a 210-byte key. The quarantine range of the page next
     * to it is therefore longer than a naive 64-byte bound can hold, and a
     * half-recorded range would be a WILDCARD: the excision would drop the
     * whole tree instead of one subtree. This phase proves it does not --
     * the long-named files outside the damaged page survive the repair. */
    {
        char img4[512], longname[256];
        uint64_t victim;
        uint8_t vlo[300], vhi[300];
        uint16_t vlo_n = 0, vhi_n = 0;
        int vhi_unb = 0, j, survivors = 0;

        memset(longname, 'n', 200);
        longname[200] = 0;
        snprintf(img4, sizeof img4, "%s/invf-btree-repair-longname.img", dir);
        unlink(img4);
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 g_bin, img4);
        if (system(cmd) != 0)
            return 2;
        g_v = vol_open(img4, &err_open);
        if (!g_v)
            return 2;
        for (j = 0; j < 24; j++) {
            char nm[300];
            invfs_meta_pub m;
            static uint8_t d[256];
            uint64_t id;
            snprintf(nm, sizeof nm, "%03d-%s", j, longname);
            memset(d, (uint8_t)('a' + j % 26), sizeof d);
            memset(&m, 0, sizeof m);
            m.type = INVFS_ITYP_REG;
            m.mode = 0644;
            m.uid = m.gid = 1000;
            m.nlink = 1;
            m.mtime = m.atime = (int64_t)time(NULL);
            m.size = sizeof d;
            id = vol_v3_write_bulk(g_v, nm, d, sizeof d, &m);
            if (!id) {
                fprintf(stderr, "btree_repair_test: long-name write failed\n");
                return 2;
            }
        }
        if (vol_v3_fold(g_v) != 0)
            return 2;
        {
            leaflist ll;
            memset(&ll, 0, sizeof ll);
            if (vol_v3_base_root(g_v, &root) != 0)
                return 2;
            list_rec(root, &ll);
            if (ll.n < 4) {
                fprintf(stderr, "btree_repair_test: long-name tree has %d "
                                "leaves\n", ll.n);
                return 2;
            }
            /* the SECOND leaf, so the first keeps a >200-byte separator */
            victim = ll.pba[1];
            memset(&g_lp, 0, sizeof g_lp);
            g_lp.victim = victim;
            g_lp.vlo_n = ll.first_n[1] < sizeof g_lp.vlo
                       ? ll.first_n[1] : (uint16_t)sizeof g_lp.vlo;
            memcpy(g_lp.vlo, ll.first[1], g_lp.vlo_n);
            if (fill_vhi(root, g_lp.vlo, g_lp.vlo_n) != 0) {
                fprintf(stderr, "btree_repair_test: long-name victim unbounded\n");
                return 2;
            }
            vlo_n = g_lp.vlo_n;
            memcpy(vlo, g_lp.vlo, vlo_n);
            vhi_n = g_lp.vhi_n;
            memcpy(vhi, g_lp.vhi, vhi_n);
            vhi_unb = g_lp.vhi_unb;
            printf("  long-name tree: %d leaves; tearing pba %llu "
                   "(bounds %u/%u bytes)\n", ll.n, (unsigned long long)victim,
                   (unsigned)vlo_n, (unsigned)vhi_n);
            ok(vlo_n > 64 || vhi_n > 64,
               "the quarantined key range is longer than 64 bytes (the case "
               "a short bound would turn into a wildcard)");
        }
        vol_close(g_v);
        g_v = NULL;
        if (tear_page(img4, victim) != 0)
            return 2;
        g_v = vol_open(img4, &err_open);
        ok(g_v != NULL, "vol_open succeeds with a torn page under long keys");
        if (!g_v)
            return 1;
        cli = fsck_cli_offline(img4, 1);
        printf("  invf-fsck -f with long keys: exit %d\n", cli);
        ok(cli == 3, "invf-fsck -f exits 3 (damage found) on the long-key tree");
        for (j = 0; j < 24; j++) {
            char nm[300];
            uint64_t id = 0, sz = 0, ct = 0;
            snprintf(nm, sizeof nm, "%03d-%s", j, longname);
            if (vol_v3_path_stat(g_v, nm, &id, &sz, &ct) == 0)
                survivors++;
        }
        printf("  long-named files still resolvable after the repair: %d of 24\n",
               survivors);
        ok(survivors > 0 && survivors < 24,
           "the repair dropped exactly the damaged subtree, not the whole "
           "namespace");
        cli = fsck_cli_offline(img4, 0);
        printf("  invf-fsck after the long-key repair: exit %d\n", cli);
        ok(cli == 0, "the long-key volume is clean after the repair");
        vol_close(g_v);
        g_v = NULL;
        unlink(img4);
    }

    /* ---- J: a root that collapses to its only child ----
     * A root with exactly two children -- one unreadable leaf -- is left with
     * a single child, which btree_check rejects, so the excision hands the
     * survivor up and the new root IS that leaf: an old page with an old gen.
     * RT30 keeps whichever slot holds the higher gen, so publishing it as it
     * stands would let the next open pick the pre-repair root back and the
     * damage with it -- a repair that silently does not stick. The reopen
     * below is what proves it stuck.
     *
     * Three 2000-byte xattrs on one inode (chunked to 1024 B per record)
     * make exactly two leaves, which is the shape this needs. */
    {
        char img5[512];
        uint64_t id0 = 0, id1 = 0, keys_before = 0;
        int torn_leaf_keys = 0;
        int j;

        snprintf(img5, sizeof img5, "%s/invf-btree-repair-collapse.img", dir);
        unlink(img5);
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 g_bin, img5);
        if (system(cmd) != 0)
            return 2;
        g_v = vol_open(img5, &err_open);
        if (!g_v)
            return 2;
        for (j = 0; j < 2; j++) {
            char nm[32];
            invfs_meta_pub m;
            static uint8_t d[256];
            uint64_t id;
            snprintf(nm, sizeof nm, "two%02d.bin", j);
            memset(d, (uint8_t)('a' + j), sizeof d);
            memset(&m, 0, sizeof m);
            m.type = INVFS_ITYP_REG;
            m.mode = 0644;
            m.uid = m.gid = 1000;
            m.nlink = 1;
            m.mtime = m.atime = (int64_t)time(NULL);
            m.size = sizeof d;
            id = vol_v3_write_bulk(g_v, nm, d, sizeof d, &m);
            if (!id)
                return 2;
            if (j == 0)
                id0 = id;
            else
                id1 = id;
        }
        {
            static uint8_t big[2000];
            memset(big, 'x', sizeof big);
            for (j = 0; j < 3; j++) {
                char nm[32];
                snprintf(nm, sizeof nm, "chunky%d", j);
                if (vol_v3_xattr_set(g_v, id0, nm, big, sizeof big) != 0) {
                    fprintf(stderr, "btree_repair_test: xattr_set failed\n");
                    return 2;
                }
            }
        }
        if (vol_v3_fold(g_v) != 0)
            return 2;
        {
            leaflist ll;
            char e0[128];
            memset(&ll, 0, sizeof ll);
            if (vol_v3_base_root(g_v, &root) != 0)
                return 2;
            e0[0] = 0;
            if (btree_check(g_v, root, &st, e0, sizeof e0) != 0)
                return 2;
            keys_before = st.nkeys;
            list_rec(root, &ll);
            torn_leaf_keys = ll.nent[ll.n ? ll.n - 1 : 0];
            printf("  two-child tree: %d leaves, %llu keys "
                   "(the torn leaf holds %d)\n", ll.n,
                   (unsigned long long)keys_before, torn_leaf_keys);
            if (ll.n != 2) {
                fprintf(stderr, "btree_repair_test: expected 2 leaves, got %d\n",
                        ll.n);
                return 2;
            }
            vol_close(g_v);
            g_v = NULL;
            /* the xattr leaf is the last one; the dirents and inode rows
             * survive, so the collapsed root must still resolve the files */
            if (tear_page(img5, ll.pba[ll.n - 1]) != 0)
                return 2;
        }
        g_v = vol_open(img5, &err_open);
        ok(g_v != NULL, "vol_open succeeds on the two-child volume with a torn "
                        "leaf");
        if (!g_v)
            return 1;
        {
            invfs_v3_inode in;
            ok(vol_v3_inode_get(g_v, id0, &in) == 1,
               "both files resolve while the tree still has two children");
        }
        cli = fsck_cli_offline(img5, 1);
        printf("  invf-fsck -f on the collapsing root: exit %d\n", cli);
        ok(cli == 3, "invf-fsck -f exits 3 (one leaf lost)");
        {
            invfs_v3_inode in;
            ok(vol_v3_inode_get(g_v, id1, &in) == 1,
               "the file outside the torn leaf is readable after the repair");
        }
        vol_close(g_v);
        g_v = NULL;
        g_v = vol_open(img5, &err_open);
        ok(g_v != NULL, "the collapsed-root volume reopens");
        if (g_v) {
            invfs_v3_inode in;
            ok(vol_v3_inode_get(g_v, id1, &in) == 1,
               "the file is STILL readable after a remount (the recopied root "
               "won the RT30 slot)");
            err[0] = 0;
            if (vol_v3_base_root(g_v, &root) == 0 &&
                btree_check(g_v, root, &st, err, sizeof err) == 0) {
                ok(1, "the collapsed root is a valid single-leaf tree");
                printf("  keys after the repair: %llu (was %llu, torn leaf "
                       "held %d)\n", (unsigned long long)st.nkeys,
                       (unsigned long long)keys_before, torn_leaf_keys);
                ok(st.nkeys + (uint64_t)torn_leaf_keys == keys_before,
                   "exactly the torn leaf's keys were dropped -- no more");
            } else {
                ok(0, "the collapsed root is a valid single-leaf tree");
            }
            vol_close(g_v);
            g_v = NULL;
        }
        cli = fsck_cli_offline(img5, 0);
        printf("  invf-fsck after the collapse repair: exit %d\n", cli);
        ok(cli == 0, "the collapsed-root volume is clean after the repair");
        unlink(img5);
    }

    /* ---- F: a torn root page ---- */
    {
        char img2[512];
        invfs_v3_inode in;
        int r;

        snprintf(img2, sizeof img2, "%s/invf-btree-repair-root.img", dir);
        unlink(img2);
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 g_bin, img2);
        if (system(cmd) != 0)
            return 2;
        g_v = vol_open(img2, &err_open);
        if (!g_v)
            return 2;
        {
            uint8_t k[8], val[INVFS_V3_INODE_ROW_FIXED];
            invfs_v3_inode_row row;
            memset(&row, 0, sizeof row);
            row.row_version = INVFS_V3_INODE_ROW_VERSION;
            row.type = INVFS_ITYP_REG;
            row.mode = 0644;
            row.nlink = 1;
            row.size = 4242;
            row.mtime = row.atime = (int64_t)time(NULL);
            ino_key(k, FIRST_ID);
            memcpy(val, &row, sizeof row);
            if (vol_delta_append(g_v, k, sizeof k, val,
                                 (uint16_t)sizeof row, 0) != 0 ||
                vol_v3_fold(g_v) != 0) {
                fprintf(stderr, "btree_repair_test: root-image seed failed\n");
                return 2;
            }
        }
        if (vol_v3_base_root(g_v, &root) != 0)
            return 2;
        vol_close(g_v);
        g_v = NULL;
        if (tear_page(img2, root.pba) != 0) {
            fprintf(stderr, "btree_repair_test: could not tear the root\n");
            return 2;
        }
        g_v = vol_open(img2, &err_open);
        ok(g_v != NULL, "vol_open still succeeds with a torn base root");
        if (!g_v)
            return 1;
        r = vol_v3_inode_get(g_v, FIRST_ID, &in);
        printf("  torn root: inode_get(%llu) = %d\n",
               (unsigned long long)FIRST_ID, r);
        ok(r < 0, "a torn base root reads EIO, not 'absent' "
                  "(the silent-empty-volume regression)");
        ok(!(r == 1 && in.size == 4242), "a torn base root never invents a row");
        cli = fsck_cli_offline(img2, 0);
        printf("  invf-fsck on a torn root: exit %d\n", cli);
        ok(cli == 3, "invf-fsck exits nonzero for a torn root");
        cli = fsck_cli_offline(img2, 1);
        printf("  invf-fsck -f on a torn root: exit %d\n", cli);
        ok(cli == 3, "invf-fsck -f refuses a torn root instead of pretending");
        r = vol_v3_inode_get(g_v, FIRST_ID, &in);
        ok(r < 0, "the refused repair left the torn root unreadable "
                  "(no tree was fabricated)");
        vol_close(g_v);
        g_v = NULL;
        unlink(img2);
    }

    /* ---- G: a save point on a damaged base tree ---- */
    {
        invfs_volume *v3;
        invfs_spt0 sp;
        int sp_rc, restore_rc;

        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 g_bin, g_img);
        if (system(cmd) != 0)
            return 2;
        v3 = vol_open(g_img, &err_open);
        if (!v3)
            return 2;
        g_v = v3;
        {
            uint8_t k[8], val[INVFS_V3_INODE_ROW_FIXED];
            invfs_v3_inode_row row;
            int j;
            memset(&row, 0, sizeof row);
            row.row_version = INVFS_V3_INODE_ROW_VERSION;
            row.type = INVFS_ITYP_REG;
            row.mode = 0644;
            row.nlink = 1;
            row.mtime = row.atime = (int64_t)time(NULL);
            for (j = 0; j < (int)NFILL; j++) {
                ino_key(k, FIRST_ID + (uint64_t)j);
                row.size = (uint64_t)j;
                memcpy(val, &row, sizeof row);
                if (vol_delta_append(v3, k, sizeof k, val,
                                     (uint16_t)sizeof row, 0) != 0) {
                    fprintf(stderr, "btree_repair_test: spt0 seed failed\n");
                    return 2;
                }
            }
            if (vol_v3_fold(v3) != 0)
                return 2;
        }
        sp_rc = spt0_capture(v3);
        ok(sp_rc == 0 && spt0_info(v3, &sp) == 1, "save point captured");
        if (sp_rc != 0)
            return 1;
        memset(&g_lp, 0, sizeof g_lp);
        g_best_files = 0;
        g_best_rows = 0;
        g_want_files = 0;      /* this image has no named files */
        g_pages = 0;
        if (vol_v3_base_root(v3, &root) != 0)
            return 2;
        scan_rec(root);
        if (!g_lp.victim) {
            fprintf(stderr, "btree_repair_test: no leaf for the spt0 case\n");
            return 2;
        }
        vol_close(v3);
        g_v = NULL;
        if (tear_page(g_img, g_lp.victim) != 0)
            return 2;
        v3 = vol_open(g_img, &err_open);
        ok(v3 != NULL, "vol_open succeeds with a live save point on a "
                       "damaged base");
        if (!v3)
            return 1;
        g_v = v3;
        ok(spt0_info(v3, &sp) == 1, "the save point is still live");
        if (vol_v3_base_root(v3, &root) == 0)
            ok(1, "base root readable before the rollback attempt");
        else
            return 2;
        restore_rc = spt0_restore(v3);
        printf("  spt0_restore on a damaged save point: rc=%d\n", restore_rc);
        ok(restore_rc != 0, "spt0_restore refuses a damaged save point base");
        {
            invfs_blkptr after;
            int same = (vol_v3_base_root(v3, &after) == 0) &&
                       after.pba == root.pba && after.gen == root.gen;
            ok(same, "the refused rollback published nothing (the volume is "
                     "exactly as it was)");
        }
        ok(spt0_info(v3, &sp) == 1, "the refused rollback kept the save point "
                                    "for a later, explicit decision");
        vol_close(v3);
        g_v = NULL;
    }

    unlink(g_img);
    printf("%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("BTREE REPAIR TEST FAIL\n");
        return 1;
    }
    printf("BTREE REPAIR TEST PASS\n");
    return 0;
}
