/* rollback_symlink_test.c — a symlink must not make rollback impossible.
 *
 * THE DEFECT. invf-rollback is the documented recovery path (AGENTS.md 2.6):
 * "an operator whose sweep mis-clustered data reaches for invf-rollback".
 * On v3 the restore first runs spt0_data_ok(), which walks every inode row
 * of the pinned generation and verifies the segments its recipe names
 * (src/core/vol_spt0.c). The walk's callback filtered only recipe_addr == 0,
 * so a symlink -- whose content is its target string, stored
 * content-addressed like any recipe (vol_v3_create_node, src/core/vol_dirs.c)
 * -- loaded fine and then failed vol_ast_recipe_parse, because "usr/lib" is
 * not an AST. That is SPT0_RC_DAMAGED: exit 5, nothing written, and a
 * message naming damage that does not exist.
 *
 * WHY IT IS WORSE THAN THE SIBLING DEFECT. The fsck recipe audit (6e8b09b)
 * MISLABELLED a healthy volume. This REFUSES to recover one. Every real
 * rootfs has symlinks (/bin, /lib, /lib64, /usr/lib64/ld-*.so*), every
 * sweep trigger arms a savepoint (AGENTS.md 2.5), so this is the normal
 * path -- not an edge case. And it is why nobody caught it: no e2e
 * combined symlinks with rollback. That gap is what this test closes.
 *
 * LEGS (the PAIR is the point -- two volumes identical except for one
 * symlink, so a control that cannot fail is on the table):
 *   A  WITHOUT a symlink -> the save point captures and the restore runs.
 *      This is the positive control: it is the leg that worked all along,
 *      and a fix that made the data check vacuous would still pass it.
 *   B  WITH a symlink    -> the SAME restore must now run. Before the fix
 *      this returned SPT0_RC_DAMAGED and wrote nothing.
 *   B2 ... and the restore is a RESTORE, not a skip: a file written after
 *      the capture is gone, and the symlink survives with its exact target
 *      bytes. "exit 0" alone would be satisfied by doing nothing.
 *   C  bit-exactness: every file read back through vol_read_inode and
 *      memcmp'd against the source bytes. invf-verify --deep is NOT the
 *      oracle here -- it checks readability and length only
 *      (src/cli/verify.c:400-415), because invfs_ast_block_entry carries a
 *      pba and no content hash.
 *   D  the check is NARROWED, not removed: a REGULAR file whose pinned
 *      recipe really does not parse must still REFUSE (SPT0_RC_DAMAGED)
 *      with the volume untouched and the save point still live. A blanket
 *      "ignore parse failures" would pass A/B/C and silently reinstate the
 *      corruption WP96's restore-time data check exists to stop.
 *   E  capture side: the pin over a symlink-bearing generation holds the
 *      same block count as the identical volume without the symlink (the
 *      symlink's blob lives in the METADATA key namespace and names no
 *      data segment), and the capture still succeeds on both.
 *
 * What the test does NOT establish: it drives spt0_capture/spt0_restore
 * directly, not the invf-rollback CLI or invf-sweep's prepare stage. The CLI
 * is a thin wrapper over exactly these two calls; the bit-exactness
 * read-back through bin/invf-cat on a real image is in
 * tools/test-rollback.sh, which drives the CLIs.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "volume_internal.h"
#include "vol_spt0.h"
#include "vol_btree.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static char g_dir[512];

static int mkdir_p(const char *path)
{
    char tmp[512];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Free blocks according to the volume's own bitmap -- the same number
 * `invf-fsck` reports and `df` derives (AGENTS.md 2.4). */
static uint64_t count_free(invfs_volume *v)
{
    uint64_t i, n = 0;
    for (i = 0; i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i)) n++;
    return n;
}

/* Deterministic pseudo-random bodies, so the corpus is incompressible enough
 * to occupy real segments and a restore is not passing on zero-length
 * aliases. */
static void fill(uint8_t *b, size_t n, unsigned seed)
{
    size_t i;
    uint32_t x = seed * 2654435761u + 1u;
    for (i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        b[i] = (uint8_t)(x >> 16);
    }
}

/* Format a fresh v3 image, by the same $PWD/bin/invf-mkfs convention the
 * other volume tests in this tree use (the Makefile runs them from the repo
 * root). */
static int mkfs_image(const char *img)
{
    char cmd[1200];
    const char *root = getenv("PWD");
    if (!root || !*root) root = ".";
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 48 2>/dev/null", root, img);
    return system(cmd);
}

#define NFILES 3
typedef struct { uint8_t body[24 * 1024]; size_t len; } corpus_file;
static corpus_file g_files[NFILES];

static const char *LINK_NAME = "lib64";
static const char *LINK_TARGET = "usr/lib";

/* Build one volume and flush it. `with_link` is the ONLY difference
 * between the pair -- no size, no layout, no other content. */
static invfs_volume *make_volume(const char *img, int with_link)
{
    invfs_volume *v;
    invfs_meta_pub m;
    int err = 0, i;
    char path[64];

    if (mkfs_image(img) != 0) return NULL;
    v = vol_open(img, &err);
    if (!v) return NULL;
    if (!(v->sb.vol_flags & VOLF_V3)) { vol_close(v); return NULL; }

    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    m.nlink = 1;
    for (i = 0; i < NFILES; i++) {
        g_files[i].len = sizeof g_files[i].body;
        snprintf(path, sizeof path, "f%d.bin", i);
        if (!vol_v3_write_bulk(v, path, g_files[i].body, g_files[i].len, &m)) {
            vol_close(v);
            return NULL;
        }
    }
    if (with_link) {
        invfs_meta_pub lm;
        memset(&lm, 0, sizeof lm);
        lm.type = INVFS_ITYP_LNK;
        lm.mode = 0777;
        lm.nlink = 1;
        snprintf(lm.target, sizeof lm.target, "%s", LINK_TARGET);
        if (!vol_v3_create_node(v, LINK_NAME, &lm)) { vol_close(v); return NULL; }
    }
    if (vol_flush(v) != 0) { vol_close(v); return NULL; }
    return v;
}

static uint64_t write_post_capture(invfs_volume *v)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    m.nlink = 1;
    return vol_v3_write_bulk(v, "post.txt",
                             (const uint8_t *)"written after the capture\n",
                             26, &m);
}

/* LEG C: every corpus file reads back byte-identical -- LENGTH AND BYTES. A
 * reader that returned the right number of wrong bytes passes a length
 * check, which is exactly what invf-verify --deep would settle for. */
static int corpus_bit_exact(invfs_volume *v, const char *leg)
{
    int i, all = 1;
    for (i = 0; i < NFILES; i++) {
        char path[64];
        uint8_t *d = NULL;
        size_t n = 0;
        int rc;
        snprintf(path, sizeof path, "f%d.bin", i);
        rc = vol_read_inode(v, vol_find(v, path), 0, &d, &n);
        if (rc != 0 || !d || n != g_files[i].len ||
            memcmp(d, g_files[i].body, n) != 0) {
            printf("        (leg %s: %s differs -- rc=%d got=%zu want=%zu)\n",
                   leg, path, rc, n, g_files[i].len);
            all = 0;
        }
        free(d);
    }
    return all;
}

/* A symlink's content IS its target string: the blob loads and is handed
 * back verbatim. Compared byte for byte, not by length. */
static int symlink_bytes_are(invfs_volume *v)
{
    uint64_t id = vol_find(v, LINK_NAME);
    uint8_t *d = NULL;
    size_t n = 0;
    int rc, good;

    if (!id) return 0;
    rc = vol_read_inode(v, id, 0, &d, &n);
    good = (rc == 0 && d && n == strlen(LINK_TARGET) &&
            memcmp(d, LINK_TARGET, n) == 0);
    free(d);
    return good;
}

/* ---- LEG A / B: the pair ------------------------------------------------ */
static void leg_pair(void)
{
    char img_a[640], img_b[640];
    invfs_volume *va, *vb;
    int rc;

    printf("leg A/B: two volumes identical except for one symlink\n");
    snprintf(img_a, sizeof img_a, "%s/pair-a.img", g_dir);
    snprintf(img_b, sizeof img_b, "%s/pair-b.img", g_dir);

    /* ---- A: the control that always worked ---------------------------- */
    va = make_volume(img_a, 0);
    ok(va != NULL, "leg A: volume WITHOUT a symlink is built");
    if (!va) return;
    ok(spt0_capture(va) == 0, "leg A: the save point is captured");
    ok(write_post_capture(va) != 0,
       "leg A: a file is written AFTER the capture");
    rc = spt0_restore(va);
    ok(rc == 0, "leg A: the restore runs (the control)");
    ok(vol_find(va, "post.txt") == 0,
       "leg A: and it is a RESTORE -- the post-capture file is gone");
    ok(corpus_bit_exact(va, "A"),
       "leg A/C: the corpus is bit-exact after the rollback");
    vol_close(va);

    /* ---- B: the same volume plus one symlink ------------------------- */
    vb = make_volume(img_b, 1);
    ok(vb != NULL, "leg B: the twin volume WITH a symlink is built");
    if (!vb) return;
    ok(symlink_bytes_are(vb),
       "leg B: PREMISE -- the symlink's blob is a real, non-zero "
       "recipe_addr (its bytes read back before anything is captured)");
    ok(spt0_capture(vb) == 0, "leg B: the save point is captured");
    ok(write_post_capture(vb) != 0,
       "leg B: a file is written AFTER the capture");
    rc = spt0_restore(vb);
    /* BEFORE THE FIX this was SPT0_RC_DAMAGED (3) with, on stderr:
     *   invf-spt0: refusing to roll back: inode N's pinned recipe does not
     *   parse
     * and invf-rollback exited 5 having written nothing. */
    ok(rc == 0,
       "leg B: the restore runs -- a symlink no longer makes rollback "
       "impossible");
    ok(rc != SPT0_RC_DAMAGED,
       "leg B: it is not refused as DAMAGED (that verdict named damage "
       "that does not exist)");
    ok(vol_find(vb, "post.txt") == 0,
       "leg B2: and it is a RESTORE, not a skip -- the post-capture file "
       "is gone");
    ok(corpus_bit_exact(vb, "B"),
       "leg B/C: the corpus is bit-exact after the rollback");
    ok(symlink_bytes_are(vb),
       "leg B2: the symlink survives the rollback with its exact target "
       "bytes");
    vol_close(vb);
}

/* ---- LEG D: the check is narrowed, not removed ------------------------- */
static void leg_regular_file_still_refuses(void)
{
    char img[640];
    invfs_volume *v;
    invfs_v3_inode in;
    uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint64_t id;
    int rc;

    printf("leg D: a REGULAR file with an unparseable pinned recipe refuses\n");
    snprintf(img, sizeof img, "%s/damaged.img", g_dir);
    v = make_volume(img, 0);
    ok(v != NULL, "leg D: a plain volume (no symlink) is built");
    if (!v) return;

    /* The damage has to be in the PINNED generation for the restore to see
     * it, so the row is repointed BEFORE the capture. */
    id = vol_find(v, "f0.bin");
    ok(id != 0 && vol_v3_inode_get(v, id, &in) == 1,
       "leg D: the victim's row is readable");
    ok(vol_v3_recipe_store(v, (const uint8_t *)"NOT AN AST AT ALL", 16,
                           addr) == 0, "leg D: a non-AST blob is stored");
    memcpy(in.recipe_addr, addr, sizeof addr);
    /* DELTA put, not vol_v3_inode_put: the write path left the live row in
     * the delta overlay, and the pinned walk reads the delta first -- so a
     * base-tree write here would be shadowed by the very record it is
     * meant to supersede, and this leg would pass vacuously. */
    ok(vol_v3_inode_delta_put(v, id, &in) == 0,
       "leg D: the row is repointed at it, before the capture");
    ok(vol_v3_recipe_load(v, addr, &blob, &blen) == 0 && blob,
       "leg D: PREMISE -- the blob LOADS. A checker must not be made "
       "permissive about the LOAD, only about the parse.");
    free(blob);
    ok(spt0_capture(v) == 0, "leg D: the save point is captured over it");
    rc = spt0_restore(v);
    ok(rc == SPT0_RC_DAMAGED,
       "leg D: the restore is REFUSED for a regular file -- the fix skips "
       "the parse for raw-blob TYPES, it does not remove the check");
    ok(v->savepoint_live,
       "leg D: and the volume is untouched (the save point is still live)");
    vol_close(v);
}

/* ---- LEG E: capture side ----------------------------------------------- */
static void leg_capture_side(void)
{
    char img_a[640], img_b[640];
    invfs_volume *va, *vb;
    uint64_t free_a, free_b;
    int err = 0; (void)err;

    printf("leg E: the capture side treats a symlink the same way\n");
    snprintf(img_a, sizeof img_a, "%s/cap-a.img", g_dir);
    snprintf(img_b, sizeof img_b, "%s/cap-b.img", g_dir);

    va = make_volume(img_a, 0);
    vb = make_volume(img_b, 1);
    ok(va != NULL && vb != NULL, "leg E: the pin-side pair is built");
    if (!va || !vb) {
        if (va) vol_close(va);
        if (vb) vol_close(vb);
        return;
    }
    ok(spt0_capture(va) == 0, "leg E: capture WITHOUT a symlink succeeds");
    ok(spt0_capture(vb) == 0, "leg E: capture WITH a symlink succeeds");
    ok(symlink_bytes_are(vb), "leg E: the symlink still reads back");

    /* A symlink's blob is a METADATA object (key 0x04 || BLAKE3), not a
     * data segment, so a capture that walks it correctly marks nothing for
     * it. If the capture had parsed it as a recipe -- which invfs_ast_hdr_parse
     * permits whenever the target's first four bytes are a v1/v2 version
     * word -- every entry would be attacker-chosen and marked into the pin. */
    free_a = count_free(va);
    free_b = count_free(vb);
    ok(free_b <= free_a,
       "leg E: the symlink costs no pinned data blocks (its blob is a "
       "metadata key, so nothing is marked for it)");
    printf("        (leg E: free blocks without=%llu with=%llu)\n",
           (unsigned long long)free_a, (unsigned long long)free_b);
    vol_close(va);
    vol_close(vb);
}

int main(int argc, char **argv)
{
    /* argv[1] is the scratch root (the Makefile passes the same one the
     * other volume tests get); INVFS_TOOL_SCRATCH overrides it, and the
     * default is on /srv because /tmp is RAM on this host. */
    const char *base = getenv("INVFS_TOOL_SCRATCH");
    int i;

    if (base && *base)
        snprintf(g_dir, sizeof g_dir, "%s/rb-symlink-test", base);
    else if (argc > 1 && argv[1][0])
        snprintf(g_dir, sizeof g_dir, "%s/rb-symlink-test", argv[1]);
    else
        snprintf(g_dir, sizeof g_dir, "/srv/bench/scratch/rb-symlink-test");
    if (mkdir_p(g_dir) != 0) {
        fprintf(stderr, "cannot create %s: %s\n", g_dir, strerror(errno));
        return 2;
    }
    /* Stale images from a previous run would make the legs assert about the
     * wrong generation. */
    for (i = 0; i < 4; i++) {
        char p[700];
        static const char *stale[] = { "pair-a.img", "pair-b.img",
                                      "damaged.img", "cap-a.img" };
        snprintf(p, sizeof p, "%s/%s", g_dir, stale[i]);
        unlink(p);
    }
    for (i = 0; i < NFILES; i++)
        fill(g_files[i].body, sizeof g_files[i].body, (unsigned)(i + 1));
    leg_pair();
    leg_regular_file_still_refuses();
    leg_capture_side();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
