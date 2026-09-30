/*
 * fsck_liveness_test.c — invf-fsck -f must not destroy a key a live inode
 * still needs.
 *
 * The WP86 repair removes a QUARANTINED KEY RANGE, not a page. The range comes
 * from the parent's separators: when a base page will not validate, the walk
 * records the key interval that page owned and btree_excise drops every key in
 * it. The interval is a key interval, so it can hold any namespace in the tree
 * -- and two of them are addressed by something OTHER than a pointer a walk can
 * follow:
 *
 *   0x04 || blake3_256(serialized recipe)[32]   the content of a file, named
 *                                              only by the 32 bytes in its
 *                                              inode row
 *   0x03 || inode:u64 || name_len || name      an inode's xattrs, named by
 *                                              the row and the key
 *
 * A key that is gone leaves no trace in any page's own CRC, and nothing in
 * the tree points at a recipe blob, so every structural check the pass runs
 * still passed after the excision. That is how `invf-fsck -f` could empty a
 * volume and print OK: the torn leaf is whichever leaf sorts last, the 0x03
 * and 0x04 keys sort high, and their inode rows and dirents sat in a readable
 * page that nobody had reason to doubt.
 *
 * The contract this pins:
 *
 *   1. A quarantined range is excised only after the pass has shown that no
 *      live, READABLE object requires a key inside it (an inode row requires
 *      its recipe blob and its xattrs; a dirent requires the row it resolves
 *      to). A range it cannot clear is NOT excised, the volume is left
 *      damaged and unchanged, and the pass says CANNOT REPAIR. Nothing is
 *      destroyed, so the key is still on its page and a restore gets it back.
 *   2. A range it CAN clear is still excised, the volume still reaches CLEAN,
 *      and the tool still exits 3 on the pass that found the damage. A gate
 *      that quietly disabled the repair would be no better than the defect.
 *
 * The evidence for (1) is the round trip: this test tears one page, runs -f,
 * puts the same byte back, and reads the files back through invf-cat. On the
 * unrepaired code the keys are already excised and reclaimed, so the files
 * stay gone whatever anyone does next -- and the pass had printed OK.
 *
 * Usage: invf-fsck_liveness_test [scratch-dir]
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
#include "vol_metabuf.h"

static int checks = 0;
static int failures = 0;
static const char *g_bin = ".";
static const char *g_dir = "/tmp";
static invfs_volume *g_v;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Flip one payload byte so the page's own CRC no longer matches. The tear is
 * deterministic, which is what makes the round trip below exact. */
static int flip_page_byte(const char *img, uint64_t pba)
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

/* Every leaf in the tree, in order, with its first key. */
typedef struct {
    uint64_t pba[512];
    uint8_t  first[512][300];
    uint16_t first_n[512];
    int n;
} leaflist;

static void list_rec(invfs_blkptr ptr, leaflist *ll)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    uint8_t *p;
    int n, i;

    if (ll->n >= 512)
        return;
    if (mbuf_read_ptr(g_v, &ptr, buf) != 0)
        return;
    h = mbuf_page_chdr(buf);
    n = h->nentries;
    p = buf + sizeof(invfs_page_hdr);
    if (h->level == INVFS_PAGE_LEVEL_LEAF) {
        uint16_t kl;
        memcpy(&kl, p, 2);
        ll->first_n[ll->n] = kl < 300 ? kl : 300;
        memcpy(ll->first[ll->n], p + 2, ll->first_n[ll->n]);
        ll->pba[ll->n] = ptr.pba;
        ll->n++;
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

static int run_cli(const char *img, const char *flags, const char *log)
{
    char cmd[1600];
    FILE *f;
    char ln[256];
    int rc;

    snprintf(cmd, sizeof cmd, "%s/bin/invf-fsck %s %s >%s 2>&1", g_bin, img,
             flags, log);
    rc = system(cmd);
    f = fopen(log, "r");
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

/* Does the log contain `needle`? The refusal has to be SAYABLE, not just
 * performed: a repair that silently declines leaves the operator with a
 * damaged volume and no idea why. */
static int log_has(const char *log, const char *needle)
{
    char buf[8192];
    FILE *f = fopen(log, "r");
    size_t n;
    int hit;

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    hit = strstr(buf, needle) != NULL;
    return hit;
}

/* Read a named file back through invf-cat and compare it byte for byte.
 * 1 = exact, 0 = wrong bytes, -1 = the file could not be produced at all. */
static int cat_matches(const char *img, const char *name, uint8_t fill,
                       size_t len, const char *dir)
{
    char cmd[1700], out[600];
    uint8_t *got;
    FILE *f;
    size_t n;
    int rc;

    snprintf(out, sizeof out, "%s/fsck-live-cat-%ld-%s", dir, (long)getpid(),
             name);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-cat %s %s %s >/dev/null 2>&1", g_bin,
             img, name, out);
    rc = system(cmd);
    if (rc != 0) {
        unlink(out);
        return -1;
    }
    f = fopen(out, "rb");
    if (!f)
        return -1;
    got = (uint8_t *)malloc(len + 1);
    n = got ? fread(got, 1, len + 1, f) : 0;
    fclose(f);
    unlink(out);
    if (!got)
        return -1;
    {
        size_t i;
        int same = (n == len);
        for (i = 0; same && i < len; i++)
            if (got[i] != fill)
                same = 0;
        free(got);
        return same;
    }
}

static void close_v(void)
{
    if (g_v) {
        vol_close(g_v);
        g_v = NULL;
    }
}

static int mkfs(const char *img)
{
    char cmd[900];
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1", g_bin,
             img);
    return system(cmd) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    char img[600], log[600];
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    int err = 0, rc;

    g_dir = dir;
    printf("fsck excision liveness: -f must not drop a key a live inode needs\n");

    /* ================= 1: the range holds REACHABLE keys =================
     * Two files with content, three 2000-byte xattrs on the first (chunked,
     * so they span records). Folded, that is 12 keys in two leaves, and the
     * LAST leaf holds the three 0x03 xattr keys plus BOTH 0x04 recipe keys --
     * the high end of the keyspace. Their inode rows and dirents are in the
     * other leaf and stay readable, which is exactly why dropping the last
     * leaf emptied both files while the volume still listed them. */
    snprintf(img, sizeof img, "%s/invf-fsck-live-%ld.img", dir, (long)getpid());
    snprintf(log, sizeof log, "%s/invf-fsck-live-%ld.log", dir, (long)getpid());
    unlink(img);
    if (mkfs(img) != 0) {
        fprintf(stderr, "fsck_liveness_test: invf-mkfs failed\n");
        return 2;
    }
    g_v = vol_open(img, &err);
    if (!g_v) {
        fprintf(stderr, "fsck_liveness_test: vol_open failed: %d\n", err);
        return 2;
    }
    {
        uint64_t id[2];
        int j;
        for (j = 0; j < 2; j++) {
            char nm[32];
            invfs_meta_pub m;
            static uint8_t d[256];
            snprintf(nm, sizeof nm, "two%02d.bin", j);
            memset(d, (uint8_t)('a' + j), sizeof d);
            memset(&m, 0, sizeof m);
            m.type = INVFS_ITYP_REG;
            m.mode = 0644;
            m.uid = m.gid = 1000;
            m.nlink = 1;
            m.mtime = m.atime = (int64_t)time(NULL);
            m.size = sizeof d;
            id[j] = vol_v3_write_bulk(g_v, nm, d, sizeof d, &m);
            if (!id[j]) {
                fprintf(stderr, "fsck_liveness_test: write failed\n");
                return 2;
            }
        }
        {
            static uint8_t big[2000];
            memset(big, 'x', sizeof big);
            for (j = 0; j < 3; j++) {
                char nm[32];
                snprintf(nm, sizeof nm, "chunky%d", j);
                if (vol_v3_xattr_set(g_v, id[0], nm, big, sizeof big) != 0) {
                    fprintf(stderr, "fsck_liveness_test: xattr_set failed\n");
                    return 2;
                }
            }
        }
    }
    if (vol_v3_fold(g_v) != 0) {
        fprintf(stderr, "fsck_liveness_test: fold failed\n");
        return 2;
    }
    {
        invfs_blkptr root;
        leaflist ll;
        uint64_t victim;
        int nleaves;

        memset(&ll, 0, sizeof ll);
        if (vol_v3_base_root(g_v, &root) != 0)
            return 2;
        list_rec(root, &ll);
        nleaves = ll.n;
        printf("  two-child tree: %d leaves\n", nleaves);
        ok(nleaves == 2, "the volume is the two-leaf shape this needs");
        if (nleaves < 2)
            return 2;
        victim = ll.pba[ll.n - 1];
        close_v();
        if (flip_page_byte(img, victim) != 0)
            return 2;

        rc = run_cli(img, "-f", log);
        printf("  invf-fsck -f on the torn last leaf: exit %d\n", rc);
        ok(rc == 3, "-f exits 3: the pass that found the damage raises the "
                    "alarm");
        ok(log_has(log, "CANNOT REPAIR"),
           "the refusal is SAYABLE (CANNOT REPAIR), not a silent no-op");
        ok(log_has(log, "REFUSED"),
           "the report says the excision was REFUSED");
        ok(log_has(log, "two00.bin") && log_has(log, "two01.bin"),
           "both doomed files are NAMED, so the operator knows what is at stake");

        /* Nothing was destroyed: the key is still on its page, so the byte goes
         * back and the content comes back with it. On the unrepaired code the
         * range was already excised and the page reclaimed, and this is where
         * the files stayed gone. */
        close_v();
        {
            int c0 = cat_matches(img, "two00.bin", 'a', 256, dir);
            int c1 = cat_matches(img, "two01.bin", 'b', 256, dir);
            printf("  invf-cat while the page is still torn: two00=%d two01=%d\n",
                   c0, c1);
            ok(c0 == -1 && c1 == -1,
               "the files read EIO, NOT absent: the keys are still referenced, "
               "so they are still recoverable");
        }
        /* The tear is one deterministic byte, so putting it back is exact. */
        if (flip_page_byte(img, victim) != 0)
            return 2;
        {
            int c0 = cat_matches(img, "two00.bin", 'a', 256, dir);
            int c1 = cat_matches(img, "two01.bin", 'b', 256, dir);
            printf("  invf-cat after the page is restored: two00=%d two01=%d\n",
                   c0, c1);
            ok(c0 == 1 && c1 == 1,
               "BOTH files read back byte-identical once the page is restored: "
               "the refused repair destroyed nothing");
        }
        rc = run_cli(img, "", log);
        printf("  invf-fsck on the restored volume: exit %d\n", rc);
        ok(rc == 0, "and the volume is CLEAN again once the damage is gone");
    }
    close_v();
    /* INVFS_FSCK_LIVE_KEEP=1 leaves the image behind so a red control can be
     * walked by hand afterwards (invf-verify --deep, invf-cat). Off by
     * default: `make test` must not leave images behind in the scratch dir. */
    if (!getenv("INVFS_FSCK_LIVE_KEEP")) {
        unlink(img);
    } else {
        printf("  (kept %s for inspection)\n", img);
    }

    /* ================= 2: the range holds DEAD keys =======================
     * A gate that refused everything would be as useless as the defect, so the
     * other half is here: a quarantined range whose keys nothing reachable
     * requires -- inode rows no directory entry names, carrying no content --
     * is still excised, the volume still reaches CLEAN, and -f still exits 3
     * on the pass that found the damage.
     *
     * The torn leaf is a MIDDLE one on purpose. A rightmost leaf is
     * quarantined as [its first key, +inf), which swallows the whole 0x03 and
     * 0x04 namespaces: nothing above it can be enumerated, because every
     * lookup's descent ends at the damaged page. That case is undecidable by
     * construction, which is why phase 1 -- exactly that shape -- refuses. A
     * bounded range is decidable, and this is the case the gate must not
     * block. */
    snprintf(img, sizeof img, "%s/invf-fsck-live-dead-%ld.img", dir,
             (long)getpid());
    unlink(img);
    if (mkfs(img) != 0)
        return 2;
    g_v = vol_open(img, &err);
    if (!g_v)
        return 2;
    {
        /* 192 rows no name points at, no content, no xattrs: the key space
         * [id, id+1) for ids far above the root, which no dirent reaches and
         * which shares no key with the 0x03/0x04 namespaces. */
        uint64_t base = 100000;
        int j;
        for (j = 0; j < 192; j++) {
            invfs_v3_inode in;
            memset(&in, 0, sizeof in);
            in.type = INVFS_ITYP_REG;
            in.mode = 0644;
            in.nlink = 1;
            in.mtime = in.atime = (int64_t)time(NULL);
            in.size = 0;
            if (vol_v3_inode_put(g_v, base + (uint64_t)j, &in) != 0) {
                fprintf(stderr, "fsck_liveness_test: inode_put failed\n");
                return 2;
            }
        }
        if (vol_v3_fold(g_v) != 0)
            return 2;
        {
            invfs_blkptr root;
            leaflist ll;
            uint64_t victim;

            memset(&ll, 0, sizeof ll);
            if (vol_v3_base_root(g_v, &root) != 0)
                return 2;
            list_rec(root, &ll);
            printf("  orphan-row tree: %d leaves\n", ll.n);
            if (ll.n < 3) {
                fprintf(stderr, "fsck_liveness_test: want >=3 leaves\n");
                return 2;
            }
            victim = ll.pba[1];            /* a MIDDLE leaf: [lo, hi) is finite */
            close_v();
            if (flip_page_byte(img, victim) != 0)
                return 2;
        }
    }
    rc = run_cli(img, "-f", log);
    printf("  invf-fsck -f on a provably dead range: exit %d\n", rc);
    ok(rc == 3, "-f exits 3 even when it goes on to repair");
    ok(!log_has(log, "CANNOT REPAIR"),
       "a range nothing reachable requires is NOT refused");
    close_v();
    rc = run_cli(img, "", log);
    printf("  invf-fsck after that repair: exit %d\n", rc);
    ok(rc == 0, "the volume is CLEAN again: the gate did not disable the "
                "repair, it stopped it destroying live content");
    close_v();
    unlink(img);
    unlink(log);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
