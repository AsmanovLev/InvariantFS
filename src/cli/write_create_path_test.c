/*
 * write_create_path_test.c -- WP142: a bulk write whose NAME LOOKUP DID NOT
 * COMPLETE must not destroy the name's prior content.
 *
 * THE DEFECT, at src/core/vol_write.c:993:
 *
 *     nid = vol_find(v, name);
 *     if (!nid) {
 *         nid = vol_v3_create_node(v, name, meta);   // :995  <-- DESTRUCTIVE
 *         if (!nid) return 0;
 *     }
 *     if (vol_write_begin(v, name, 1, &ws) == 0 || !ws)
 *         return 0;                                  // :998
 *     rc = len ? vol_write_range(ws, 0, data, len) : 0;  // :1000
 *     ...
 *     return vol_find(v, name);                      // :1014
 *
 * vol_find returns a uint64_t, so "there is no such name" and "the lookup
 * could not be COMPLETED" are both the value 0. On the second one this takes
 * the CREATE branch, and create-on-v3 is not "add a new name" -- it is
 * "install an EMPTY node over whatever was there" (src/core/vol_dirs.c:
 *
 *     :294  rc = vol_v3_dirent_get(v, pino, leaf, &existing);
 *     :299  if (rc == 1) { id = existing; ... }        <- the SAME inode id
 *     :340-344   in.size = 0; memset(in.recipe_addr, 0, ...)   <- recipe address
 *     :345  vol_v3_inode_delta_put(v, id, &in)          <- durable, now
 *     :365-367 vol_v3_free_recipe_blocks(v, old_addr, 0) <- blocks gone
 *
 * ). So the old inode is emptied and its blocks freed BEFORE vol_write_begin
 * is ever reached. If the write session then cannot be opened (:998), or the
 * range write fails (:1000), or len == 0 (:1000 writes nothing), the prior
 * content is gone and nothing was written back. Data loss, on the path
 * invf-import and invf-cp take for every file they write.
 *
 * :80/:99 (WP "7791a87", vol_write_begin's vol_find_rc) DOES NOT COVER THIS.
 * It is a different call in a different function, and it runs at :998 --
 * strictly AFTER vol_v3_create_node has already emptied the row and freed the
 * blocks. Even a fully-correct vol_write_begin cannot undo that; by the time it
 * is asked, the content it was supposed to supersede is already released.
 *
 * THE ANSWER. A lookup that could not be completed is not entitled to assert
 * that the name is absent -- that is the same asymmetry 42ea0a6 fixed in
 * table_sync_one_locked, and it cuts the same way. vol_find_rc already keeps
 * the three answers apart, so :993 uses it and REFUSES on the third: return 0,
 * touch nothing. :1014 needs the same treatment for a different reason --
 * there the content is already committed, so a failed re-lookup does not make
 * the write a failure, it only makes the ANSWER unknowable, and reporting 0
 * turns a committed write into a caller-visible "write failed".
 *
 * THE ORACLE IS BYTES. invf-verify --deep is not usable here: it checks
 * readability and length only (src/cli/verify.c:357-364). This test reads the
 * file back through vol_read_inode and memcmps against the bytes that were
 * written, and prints the first differing offset. The question is not "did the
 * call return an error" -- it is "are the bytes still there".
 *
 * SEAM DISCIPLINE (src/core/vol_fault.h). The failure is armed with
 * INVFS_FAULT="v3_dirent_row_read:<n>" -- the dirent row read inside
 * vol_v3_dirent_get, which is what a quarantined base page fails.
 *   - The site lives in src/core/vol_btree.c, so arming needs
 *     invfs_vol_btree_fault_reload(). unsetenv+setenv is NOT equivalent (the
 *     freed spec string is very often handed back at the same address, the
 *     pointer compare sees no change, the countdown stays SPENT and the leg
 *     runs against the healthy path -- green, proving nothing).
 *   - The ORDINAL is searched, not pinned, and every probed position prints
 *     its own trace line. The leg requires at-least-one match AND not-all
 *     matches: a search that matched everywhere would be measuring nothing,
 *     and a search that matched nowhere did not arm anything.
 *   - The MATCH SIGNAL is one-sided on purpose: "the prior content is still
 *     byte-exact after the armed write". Before the fix that is false at the
 *     position where the fault landed (the bytes are gone); after the fix it
 *     is true at that position (the write was refused). Positions where the
 *     fault landed elsewhere or nowhere show the healthy replace and do not
 *     match.
 *
 * THE TWO GREEN CONTROLS. "Refuse on a failed lookup" is only distinguishable
 * from "refuse always" if the two normal paths still work, so:
 *   create  a write to an ABSENT name still creates it, byte-exact.
 *   replace a write over a PRESENT name still replaces it, byte-exact.
 *
 * A STALE-BINARY GUARD: this binary links src/core/vol_write.c, and a
 * `make -j` does NOT relink it -- it is built only as a prerequisite of
 * `make test`. A stale binary would run the OLD :993 and report the fix as
 * broken (or, worse, run the NEW :993 and report the defect as fixed). So the
 * test compares its own mtime against the mtimes of the core sources it
 * depends on and exits 3, loudly, if it is older. It never greps for a
 * literal from the fix: a red control that requires the fix's own message to
 * be present cannot demonstrate the defect.
 *
 * Subcommands:
 *   create    GREEN control: write to an absent name -> created, byte-exact
 *   replace   GREEN control: write over a present name -> replaced, byte-exact
 *   red       THE CONTROL: prior content, one failed lookup, prior content
 *   stale     print the binary/source mtimes (diagnostic)
 *
 * exit 0 = pass, 1 = failure, 2 = setup error, 3 = stale binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "invarifs.h"
#include "volume_internal.h"

/* The cross-TU fault door: the site is vol_v3_dirent_get, inside
 * vol_btree.c, so its arming state is that file's TU statics. */
extern void invfs_vol_btree_fault_reload(void);

static int checks = 0, failures = 0;
static const char *g_dir = "/srv/bench/scratch";
static char g_img[512];

static void ok(int cond, const char *msg)
{
    checks++;
    printf("  %s  %s\n", cond ? "OK  " : "FAIL", msg);
    if (!cond) failures++;
}

static void open_vol_or_die(invfs_volume **out)
{
    int err = 0;
    invfs_volume *v = vol_open(g_img, &err);
    if (!v) { fprintf(stderr, "vol_open(%s) failed: %d\n", g_img, err); exit(2); }
    *out = v;
}

static void mkfs_fresh(void)
{
    char cmd[700];
    unlink(g_img);
    snprintf(cmd, sizeof cmd,
             "./bin/invf-mkfs %s 64 >/dev/null 2>&1", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

/* INCOMPRESSIBLE and multi-segment, so the prior content is a real recipe
 * with several blocks to lose rather than one inline block. */
static uint8_t *mk_body(size_t n, uint32_t seed)
{
    uint8_t *d = (uint8_t *)malloc(n);
    uint32_t r = seed ? seed : 1;
    size_t i;
    if (!d) { fprintf(stderr, "oom\n"); exit(2); }
    for (i = 0; i < n; i++) {
        r = r * 1103515245u + 12345u;
        d[i] = (uint8_t)(r >> 16);
    }
    return d;
}

/* THE BIT-EXACTNESS ORACLE: read back through the normal read path and
 * compare BYTES. Prints the first differing offset when it differs. */
static int oracle(invfs_volume *v, const char *name, const uint8_t *want,
                  size_t want_len, const char *tag)
{
    uint64_t id = 0;
    uint8_t *buf = NULL;
    size_t len = 0, i;
    int rc, same;

    if (vol_v3_path_lookup(v, name, &id) != 1) {
        printf("  [oracle %s] %s: THE NAME IS GONE\n", tag, name);
        return 0;
    }
    rc = vol_read_inode(v, id, 0, &buf, &len);
    if (rc != 0) {
        printf("  [oracle %s] %s: READ FAILED (rc=%d)\n", tag, name, rc);
        return 0;
    }
    same = (len == want_len) && (want_len == 0 || memcmp(buf, want, len) == 0);
    if (same) {
        printf("  [oracle %s] %s: %zu bytes BYTE-EXACT vs source\n",
               tag, name, len);
    } else {
        size_t firstbad = 0;
        for (i = 0; i < len && i < want_len; i++)
            if (buf[i] != want[i]) { firstbad = i; break; }
        printf("  [oracle %s] *** PRIOR CONTENT NOT INTACT *** %s: %zu bytes "
               "on the volume, %zu in the source; first differing byte %zu "
               "(volume 0x%02x, source 0x%02x)\n",
               tag, name, len, want_len, firstbad,
               firstbad < len ? buf[firstbad] : 0,
               firstbad < want_len ? want[firstbad] : 0);
    }
    free(buf);
    return same;
}

/* ---- the stale-binary guard ------------------------------------------ */
static long mtime_of(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long)st.st_mtime;
}

static int stale_guard(void)
{
    static const char *core[] = {
        "src/core/vol_write.c", "src/core/vol_dirs.c", "src/core/vol_ast.c",
        "src/core/vol_btree.c", NULL
    };
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    long bin, src = 0;
    const char *worst = NULL;
    int i;

    if (n <= 0) return 0;
    self[n] = 0;
    bin = mtime_of(self);
    for (i = 0; core[i]; i++) {
        long m = mtime_of(core[i]);
        if (m > src) { src = m; worst = core[i]; }
    }
    if (bin < 0 || src < 0) return 0;
    if (bin < src) {
        printf("STALE BINARY: %s\n", self);
        printf("  this binary was linked at mtime %ld; %s was modified at "
               "mtime %ld.\n", bin, worst, src);
        printf("  A `make -j` does NOT relink the test binaries -- they build\n"
               "  only as prerequisites of `make test`. Rebuild it:\n"
               "    rm -f %s && make %s\n", self, self);
        printf("  Every result below would be the OLD vol_write.c, so they\n"
               "  would mean nothing either way.\n");
        return 3;
    }
    return 0;
}

/* ---- GREEN control: create over an ABSENT name ------------------------ */
static void leg_create(void)
{
    invfs_volume *v;
    size_t n = 8 * INVFS_SEGMENT_SIZE;
    uint8_t *d = mk_body(n, 0xC0FFEEu);

    printf("== leg `create`: a write to an ABSENT name still creates it ==\n");
    mkfs_fresh();
    open_vol_or_die(&v);
    ok(vol_find(v, "fresh.bin") == 0, "the name is absent before the write");
    ok(vol_v3_write_bulk(v, "fresh.bin", d, n, NULL) != 0,
       "vol_v3_write_bulk returned a live inode id");
    ok(oracle(v, "fresh.bin", d, n, "create"),
       "create: the new file is byte-exact");
    free(d);
    vol_close(v);
}

/* ---- GREEN control: replace over a PRESENT name ----------------------- */
static void leg_replace(void)
{
    invfs_volume *v;
    size_t n = 8 * INVFS_SEGMENT_SIZE;
    uint8_t *a = mk_body(n, 0x11111111u);
    uint8_t *b = mk_body(n, 0x22222222u);

    printf("== leg `replace`: a write over a PRESENT name still replaces it "
           "==\n");
    mkfs_fresh();
    open_vol_or_die(&v);
    ok(vol_v3_write_bulk(v, "hot.bin", a, n, NULL) != 0, "wrote the first body");
    ok(oracle(v, "hot.bin", a, n, "replace/pre"),
       "replace/pre: the first body is byte-exact");
    ok(vol_v3_write_bulk(v, "hot.bin", b, n, NULL) != 0, "wrote the second body");
    ok(oracle(v, "hot.bin", b, n, "replace/post"),
       "replace/post: the SECOND body is byte-exact (the replace happened)");
    free(a);
    free(b);
    vol_close(v);
}

/* ---- THE CONTROL: prior content, one failed lookup -------------------- */
/* One scenario from a fresh image, with a dirent row read made to fail at
 * countdown position `n`. Sets *refused when vol_v3_write_bulk reported the
 * write as not done. Returns 1 if the prior content SURVIVED -- which is what
 * the fix makes true at the position where the fault landed on the write's own
 * decision -- and 0 if it did not. */
static int red_attempt(int n, int *refused, int zero)
{
    invfs_volume *v;
    size_t nbytes = 8 * INVFS_SEGMENT_SIZE;
    size_t rbytes = zero ? 0 : 3 * INVFS_SEGMENT_SIZE;
    uint8_t *prior = mk_body(nbytes, 0xABCDEF01u);
    uint8_t *repl = zero ? NULL : mk_body(rbytes, 0x0BADF00Du);
    char spec[64];
    uint64_t id;
    int intact;

    *refused = 0;
    mkfs_fresh();
    open_vol_or_die(&v);

    /* The PRIOR CONTENT. This is what must still be there afterwards. */
    id = vol_v3_write_bulk(v, "victim.bin", prior, nbytes, NULL);
    if (!id) { printf("  [n=%d] could not lay down the prior content\n", n);
               free(prior); free(repl); vol_close(v); return 0; }
    if (!oracle(v, "victim.bin", prior, nbytes, "pre")) {
        printf("  [n=%d] SETUP FAILED: the prior content is not readable "
               "before anything is armed\n", n);
        free(prior); free(repl); vol_close(v); return 0;
    }
    {
        invfs_v3_inode in;
        vol_v3_inode_get(v, id, &in);
        printf("  [n=%d] victim.bin: inode %llu, size %llu, recipe addr "
               "%02x%02x%02x%02x...\n", n, (unsigned long long)id,
               (unsigned long long)in.size, in.recipe_addr[0], in.recipe_addr[1],
               in.recipe_addr[2], in.recipe_addr[3]);
    }

    /* ARM. One dirent-row read fails. The fault is ONE-SHOT, so by the time a
     * later lookup asks again the volume answers normally: whatever the fault
     * made happen at the decision point has already happened.
     *
     * The write carries a DIFFERENT body of a DIFFERENT length -- or, when
     * `zero`, no body at all, which is the other half of the defect: an empty
     * payload means vol_write_range is skipped entirely (:1000), so if the
     * create branch was taken there is nothing to write back at all and the
     * file's content is simply gone. Either way the possible outcomes are
     * told apart by their BYTES and not only by their length: "still the
     * prior body" is the fix, "the replacement body" is a write that ran
     * normally because the fault landed elsewhere, and "neither" is the data
     * loss. A diagnostic that cannot tell those last two apart names one
     * cause when there are two, and ends the search. */
    snprintf(spec, sizeof spec, "v3_dirent_row_read:%d", n);
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_btree_fault_reload();
    id = vol_v3_write_bulk(v, "victim.bin", repl, rbytes, NULL);
    unsetenv("INVFS_FAULT");
    invfs_vol_btree_fault_reload();

    *refused = (id == 0);
    printf("  [n=%d] the armed write (%s) returned %llu (%s)\n", n,
           zero ? "len == 0" : "a replacement body",
           (unsigned long long)id, *refused ? "REFUSED" : "reported success");
    intact = oracle(v, "victim.bin", prior, nbytes, "post");
    if (intact) {
        printf("  [n=%d] PRIOR CONTENT INTACT -- the write was refused\n", n);
    } else if (zero) {
        printf("  [n=%d] *** DATA LOSS: 8 segments of a live file were "
               "released and NOTHING was written back -- the name now reads "
               "as %s ***\n", n, "an empty file");
    } else if (oracle(v, "victim.bin", repl, rbytes, "post/repl")) {
        printf("  [n=%d] prior content replaced by the new body -- the fault "
               "did not land on the decision point, so the write ran "
               "normally\n", n);
    } else {
        printf("  [n=%d] *** DATA LOSS: 8 segments of a live file were "
               "released, and what is on the volume is NEITHER the prior body "
               "NOR the replacement ***\n", n);
    }
    free(prior);
    free(repl);
    vol_close(v);
    return intact;
}

static void leg_red(void)
{
    int matched = 0, probed = 0, n, refused = 0, r1, r2;

    printf("== leg `red`: one failed lookup must not destroy prior content "
           "==\n");

    /* PART A -- THE DETERMINISTIC ASSERTION, at countdown position 1.
     *
     * Position 1 is not a guess. Between `setenv` and the create-versus-
     * replace decision at vol_write.c:993 the call does only three things
     * that touch no dirent row -- the argument checks, the MAX_FILE_SIZE
     * check and vol_write_enabled -- and then vol_find goes straight to
     * vol_v3_path_lookup, whose first act for a bare name is ONE
     * vol_v3_dirent_get. So the first dirent row read after arming IS the
     * lookup that decides what this write does to the name, and the
     * scenario below is exactly "that lookup did not complete". */
    printf("  -- position n=1: the write's OWN lookup, deterministically\n");
    r1 = red_attempt(1, &refused, 0);
    r2 = refused;
    ok(r1, "n=1: the prior content is still byte-exact after a lookup that "
           "did not complete");
    ok(r2, "n=1: vol_v3_write_bulk reported the write as not done");
    if (failures)
        printf("  -- position n=1: *** DATA LOSS REPRODUCED -- a live "
               "file's content was released by a lookup that never "
               "completed ***\n");

    /* PART A2 -- the same decision point, with len == 0. That is the half of
     * the defect where nothing is even ATTEMPTED: vol_write_range is skipped
     * outright (:1000), so the create branch released the blocks and there
     * was never going to be anything to write back. */
    printf("  -- position n=1, len == 0: nothing would be written back at "
           "all\n");
    {
        int f0 = failures;
        red_attempt(1, &refused, 1);
        ok(failures == f0 && refused,
           "n=1/len==0: the prior content is still byte-exact after a lookup "
           "that did not complete");
    }

    /* PART B -- THE POSITION SEARCH. This is the SEAM's self-check, not the
     * defect's assertion: how many dirent row reads a write performs after
     * the one that decides is the volume's business, so the countdown is
     * searched and every probed position prints its own trace. The two
     * assertions are what make the search mean anything -- at-least-one says
     * the arming really did fire somewhere, not-all says it did not fire
     * everywhere (which would be the healthy path under another name). */
    printf("  -- searching the countdown (the ordinal is not a constant)\n");
    for (n = 1; n <= 8; n++) {
        int m, r;
        printf("  -- probe position n=%d\n", n);
        fflush(stdout);
        m = red_attempt(n, &r, 0);
        probed++;
        if (m) matched++;
        printf("  -- probe position n=%d: %s\n", n,
               m ? "MATCH (prior content survived)"
                 : "no match (prior content did not survive)");
    }
    printf("  -- searched %d positions, %d matched\n", probed, matched);
    ok(matched >= 1,
       "the fault was landed on a write-path lookup at least once "
       "(a search that matched nowhere armed nothing)");
    ok(matched < probed,
       "not every probed position matched (a search that matched everywhere "
       "is measuring the healthy path)");
}

static void leg_stale(void)
{
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    static const char *core[] = {
        "src/core/vol_write.c", "src/core/vol_dirs.c", "src/core/vol_ast.c",
        "src/core/vol_btree.c", NULL
    };
    int i;
    if (n > 0) { self[n] = 0; printf("binary  %s  mtime %ld\n", self,
                                    mtime_of(self)); }
    for (i = 0; core[i]; i++)
        printf("source  %s  mtime %ld\n", core[i], mtime_of(core[i]));
}

int main(int argc, char **argv)
{
    const char *leg = (argc > 1) ? argv[1] : "red";
    int sg = stale_guard();

    if (sg == 3 && strcmp(leg, "stale") != 0) return 3;
    snprintf(g_img, sizeof g_img, "%s/wf_write_create.img", g_dir);

    if (!strcmp(leg, "create"))       leg_create();
    else if (!strcmp(leg, "replace")) leg_replace();
    else if (!strcmp(leg, "red"))     leg_red();
    else if (!strcmp(leg, "stale"))   leg_stale();
    else {
        fprintf(stderr, "usage: %s {create|replace|red|stale}\n", argv[0]);
        return 2;
    }

    printf("%s: %d checks, %d failures\n", leg, checks, failures);
    return failures ? 1 : 0;
}