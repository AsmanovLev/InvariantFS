/*
 * tz_registry_test.c -- WP143: a batch-registry lookup that did not COMPLETE
 * must not be read as "the registry is empty".
 *
 * THE DEFECT, at src/core/vol_textzone.c:327 and :422 -- ONE collapse, TWO
 * sites, and they have to move together:
 *
 *     :320  static int tz_reg_load(invfs_volume *v, tz_reg *r)
 *     :327      r->owner_id = vol_find(v, TZ_OWNER_NAME);
 *     :328      if (!r->owner_id) return 0;        <-- 0 == SUCCESS, n == 0
 *
 *     :411  static int tz_reg_store(invfs_volume *v, tz_reg *r)
 *     :422      id = vol_find(v, TZ_OWNER_NAME);
 *     :423      if (id) { ...publish over the existing inode... }
 *     :428      else    { vol_create_blob_file(...); }   <-- overwrites anyway
 *
 * vol_find returns a uint64_t, so "there is no registry" and "the lookup could
 * not be COMPLETED" are both 0. tz_reg_load therefore returns 0 -- its
 * SUCCESS answer -- with reg.n == 0. tz_flush (:601) accepts that, seals
 * this run's batches, appends them to the empty reg, and tz_reg_store
 * (:792/:422) PUBLISHES THE BLOB WITH THIS RUN'S ENTRIES ONLY.
 *
 * Every pre-existing entry is gone. And those rows are not bookkeeping: a
 * registry row is the ONLY record that a batch segment exists and who owns it
 * (the header comment at vol_textzone.c:270-291 says so). Losing them makes
 * live batch segments unreachable -- tz_gc can never see them, so it can
 * never free them, and tz_reg_owned_blocks (:385, the owner set spn_reclaim
 * consults) stops claiming their blocks too. The loss is IRREVERSIBLE: the
 * old blob is overwritten, not shadowed.
 *
 * BOTH SITES MOVE TOGETHER. :327 is where the run starts, and it is also what
 * makes the failure cheap to refuse -- refusing there aborts the flush BEFORE
 * anything is sealed, so a refused run orphans nothing. :422 is the second,
 * later lookup, after this run has already sealed and committed; leaving it
 * alone keeps the same loss reachable by the other path.
 *
 * THE ANSWER. A failed read is not entitled to assert a negative about the
 * volume -- "the registry is empty" is a claim about every batch that has ever
 * been written, and a read that did not complete has established nothing of
 * the kind. So both lookups use vol_find_rc and REFUSE on the third answer.
 * At :327 that is `return -1`, which every caller already handles as failure
 * (:385, :601, :824 all do `if (tz_reg_load(v, &reg) != 0) return -1;`), so
 * the sweep reports an error instead of rewriting the registry. At :422 it is
 * `return -1`, which tz_flush turns into rc = -1 (:792).
 *
 * SEAM DISCIPLINE (src/core/vol_fault.h). INVFS_FAULT="dirent_row_read:<n>"
 * -- the dirent row read in vol_dirent_get, which is what a quarantined base
 * page fails. The site is in vol_btree.c, so arming goes through
 * invfs_vol_btree_fault_reload(); unsetenv+setenv is NOT equivalent (the freed
 * spec string is very often handed back at the same address, the pointer
 * compare sees no change, the countdown stays SPENT, and the leg runs against
 * the healthy path -- green, proving nothing). The countdown is SEARCHED, not
 * pinned, because how many dirent row reads a flush performs is the volume's
 * business; every probed position prints its own trace, and the leg requires
 * at-least-one disturbed AND not-all disturbed, so a search that fired
 * everywhere would be recognised as measuring the healthy path.
 *
 * THE REGISTRY IS PARSED HERE, NOT TAKEN FROM THE ENGINE. The on-disk format
 * is [4B "TZV3"][4B n][n * {u32 seq, u32 algo, u64 pba, u32 phys}]
 * (TZ_REG_MAGIC / tz_reg_ent, src/core/vol_textzone.c), read here with
 * vol_find + vol_read_file -- both long-standing public entry points. A
 * regression test that only builds because the fix introduced an accessor is a
 * tautology, not a red control. A row that does not parse (a pba outside the
 * volume) is reported as MALFORMED rather than quietly testing nothing.
 *
 * Subcommands:
 *   ctl   GREEN control: a flush with a READABLE registry preserves the
 *         earlier run's entries, and adds this run's
 *   red   THE CONTROL: a flush during which the lookup fails must not lose
 *         the earlier run's entries
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
#include "vol_fault.h"
#include "codec.h"

extern void invfs_vol_btree_fault_reload(void);

#define TZ_REG_MAGIC   0x33565a54u   /* "TZV3", vol_textzone.c:291 */
#define TZ_ROW_SZ      24u           /* tz_reg_ent on LP64 */
#define TZ_OFF_PBA      8u
#define TZ_OFF_PHYS    16u
#define NFILES          6

static int checks = 0, failures = 0;
/* WP204: see write_create_path_test.c -- the Makefile passes this binary a
 * scratch root and this binary ignored it in favour of a compiled-in
 * /srv/bench/scratch, so the legs never ran off the author's bench box. */
static const char *g_dir = "/tmp";
static char g_img[512];

static void ok(int cond, const char *msg)
{
    checks++;
    printf("  %s  %s\n", cond ? "OK  " : "FAIL", msg);
    if (!cond) failures++;
}

static void mkfs_fresh(void)
{
    char cmd[700];
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

static invfs_volume *open_vol(void)
{
    int err = 0;
    invfs_volume *v = vol_open(g_img, &err);
    if (!v) { fprintf(stderr, "vol_open(%s) failed: %d\n", g_img, err); exit(2); }
    return v;
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
        "src/core/vol_textzone.c", "src/core/vol_dirs.c",
        "src/core/vol_btree.c", "src/core/vol_sweep.c", NULL
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
        printf("  linked at mtime %ld; %s was modified at mtime %ld.\n",
               bin, worst, src);
        printf("  A `make -j` does NOT relink the test binaries -- they build\n"
               "  only as prerequisites of `make test`. Rebuild:\n"
               "    rm -f %s && make %s\n", self, self);
        printf("  Everything below would be the OLD vol_textzone.c.\n");
        return 3;
    }
    return 0;
}

/* ---- the registry, parsed off the volume ----------------------------- */
typedef struct { uint32_t seq; uint64_t pba; uint32_t phys; } reg_row;

static int registry_read(invfs_volume *v, reg_row **out, size_t *n_out,
                         const char *tag)
{
    uint8_t *buf = NULL;
    size_t len = 0, i, cap = 0;
    uint32_t magic = 0, n = 0;
    uint64_t owner;
    reg_row *rows = NULL;

    *out = NULL;
    *n_out = 0;
    owner = vol_find(v, TZ_OWNER_NAME);
    if (!owner) {
        printf("  [reg %s] there is NO registry blob at all\n", tag);
        return 0;
    }
    if (vol_read_file(v, owner, &buf, &len) != 0 || !buf) {
        printf("  [reg %s] the registry blob could not be READ (inode %llu)\n",
               tag, (unsigned long long)owner);
        free(buf);
        return -1;
    }
    if (len < 8) {
        printf("  [reg %s] MALFORMED: the blob is %zu bytes\n", tag, len);
        free(buf);
        return -1;
    }
    memcpy(&magic, buf, 4);
    memcpy(&n, buf + 4, 4);
    if (magic != TZ_REG_MAGIC) {
        printf("  [reg %s] MALFORMED: magic 0x%08x\n", tag, magic);
        free(buf);
        return -1;
    }
    if ((size_t)n > (len - 8) / TZ_ROW_SZ) {
        printf("  [reg %s] MALFORMED: header says %u rows, %zu bytes of blob "
               "hold %zu\n", tag, n, len, (len - 8) / TZ_ROW_SZ);
        free(buf);
        return -1;
    }
    cap = n ? n : 1;
    rows = (reg_row *)calloc(cap, sizeof *rows);
    if (!rows) { free(buf); return -1; }
    for (i = 0; i < n; i++) {
        const uint8_t *r = buf + 8 + i * TZ_ROW_SZ;
        uint32_t seq = 0, phys = 0;
        uint64_t pba = 0;
        memcpy(&seq, r, 4);
        memcpy(&pba, r + TZ_OFF_PBA, 8);
        memcpy(&phys, r + TZ_OFF_PHYS, 4);
        if (pba == 0 || pba >= v->sb.total_blocks ||
            (uint64_t)pba + phys > v->sb.total_blocks) {
            printf("  [reg %s] MALFORMED: row %zu names pba %llu+%u on a "
                   "%llu-block volume -- this driver does not know the "
                   "format it is reading\n", tag, i,
                   (unsigned long long)pba, phys,
                   (unsigned long long)v->sb.total_blocks);
            free(rows); free(buf);
            return -1;
        }
        rows[i].seq = seq;
        rows[i].pba = pba;
        rows[i].phys = phys;
    }
    free(buf);
    *out = rows;
    *n_out = n;
    printf("  [reg %s] %zu row(s):", tag, (size_t)n);
    for (i = 0; i < n; i++)
        printf(" {seq=%u pba=%llu phys=%u}", rows[i].seq,
               (unsigned long long)rows[i].pba, rows[i].phys);
    printf("\n");
    return 0;
}

/* Identity is the WHOLE row, not just `seq`. That is not a detail: with the
 * registry read as empty, reg.next_seq is 1, so the overwriting flush mints a
 * fresh row that REUSES the earlier run's seq. A comparison on seq alone finds
 * the new row and calls the old one alive -- which is exactly how the loss
 * hides: the registry still has a row with that number, naming a different
 * block, and the batch the old number used to own has no record at all. */
static int has_row(const reg_row *rows, size_t n, const reg_row *want)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (rows[i].seq == want->seq && rows[i].pba == want->pba &&
            rows[i].phys == want->phys)
            return 1;
    return 0;
}

static int survivors(const reg_row *rows, size_t n, const reg_row *earlier,
                     size_t n_earlier, const char *tag)
{
    size_t i;
    int kept = 0;
    for (i = 0; i < n_earlier; i++) {
        int found = has_row(rows, n, &earlier[i]);
        if (found) {
            kept++;
            printf("  [%s] earlier-run row seq=%u pba=%llu phys=%u : "
                   "STILL PRESENT\n", tag, earlier[i].seq,
                   (unsigned long long)earlier[i].pba, earlier[i].phys);
        } else {
            int same_seq = 0;
            size_t j;
            for (j = 0; j < n; j++)
                if (rows[j].seq == earlier[i].seq &&
                    !(rows[j].pba == earlier[i].pba &&
                      rows[j].phys == earlier[i].phys))
                    same_seq = 1;
            printf("  [%s] earlier-run row seq=%u pba=%llu phys=%u : "
                   "*** GONE ***%s -- the batch segment it owned has no "
                   "record anywhere, so no GC can ever reclaim it\n",
                   tag, earlier[i].seq,
                   (unsigned long long)earlier[i].pba, earlier[i].phys,
                   same_seq ? " (a DIFFERENT row has since been minted with "
                               "this seq -- the identity was reused, which is "
                               "how the loss stays hidden)"
                            : "");
        }
    }
    return kept;
}

/* ---- one run of files through the text lane --------------------------- */
/* Writes NFILES text files tagged `run`, defers each into the sweep-run text
 * accumulator (exactly what vol_sweep_file does at vol_sweep.c:688-697), and
 * calls the same vol_tz_flush the sweep's stage 6 does. */
static int one_run(invfs_volume *v, const char *run, int armed, int n)
{
    uint8_t *body;
    size_t blen, i;
    char name[64], spec[64];
    int rc;

    blen = 24 * 1024;
    body = (uint8_t *)malloc(blen);
    if (!body) { fprintf(stderr, "oom\n"); exit(2); }
    /* Compressible prose, varied per file, so the PPMd seal's size guard
     * accepts the batch rather than refusing it -- a refused batch would
     * seal nothing and the registry would never be written, which would make
     * the whole leg vacuous. */
    for (i = 0; i < blen; i++)
        body[i] = (uint8_t)(' ' + ((i + i / 37 + (size_t)(run[3] * 7)) % 26));

    for (i = 0; i < NFILES; i++) {
        uint64_t id;
        snprintf(name, sizeof name, "%s%u.txt", run, (unsigned)i);
        id = vol_write_bulk(v, name, body, blen, NULL);
        if (!id) {
            printf("  [%s] could not write %s\n", run, name);
            free(body);
            return -1;
        }
    }
    for (i = 0; i < NFILES; i++) {
        uint64_t id = 0;
        snprintf(name, sizeof name, "%s%u.txt", run, (unsigned)i);
        if (vol_path_lookup(v, name, &id) != 1) {
            printf("  [%s] %s does not resolve\n", run, name);
            free(body);
            return -1;
        }
        if (tz_defer(v, id, name, blen,
                     (uint32_t)INVFS_TEXT_FAMILY_PROSE) != 0) {
            printf("  [%s] could not defer %s\n", run, name);
            free(body);
            return -1;
        }
    }
    free(body);

    if (armed) {
        snprintf(spec, sizeof spec, "dirent_row_read:%d", n);
        setenv("INVFS_FAULT", spec, 1);
        invfs_vol_btree_fault_reload();
    }
    rc = vol_tz_flush(v);
    if (armed) {
        unsetenv("INVFS_FAULT");
        invfs_vol_btree_fault_reload();
    }
    printf("  [%s%s] vol_tz_flush returned %d\n", run, armed ? " ARMED" : "",
           rc);
    return rc;
}

/* ---- the GREEN control ------------------------------------------------ */
/* Same two runs, no fault. The invariant under test is not "the flush did
 * something", it is "an EARLIER run's registry rows are still there after a
 * LATER run has added its own" -- which is what the fix has to preserve and
 * what a fix that simply stopped writing the registry would also pass, hence
 * the explicit check that the second run's rows were ADDED. */
static void leg_ctl(void)
{
    invfs_volume *v;
    reg_row *r1 = NULL, *r2 = NULL;
    size_t n1 = 0, n2 = 0, i;
    int kept;

    printf("== leg `ctl`: a flush with a READABLE registry preserves the "
           "earlier run's entries ==\n");
    mkfs_fresh();
    v = open_vol();

    one_run(v, "a", 0, 0);
    if (registry_read(v, &r1, &n1, "ctl/run1") != 0 || n1 == 0) {
        printf("  the first run produced no registry rows; the leg would be "
               "vacuous\n");
        failures++;
        ok(0, "ctl: the first run wrote a registry");
        free(r1); vol_close(v);
        return;
    }
    ok(n1 > 0, "ctl: the first run's registry has rows");

    one_run(v, "b", 0, 0);
    if (registry_read(v, &r2, &n2, "ctl/run2") != 0) {
        ok(0, "ctl: the second run's registry is readable");
        free(r1); free(r2); vol_close(v);
        return;
    }
    kept = survivors(r2, n2, r1, n1, "ctl");
    ok(kept == (int)n1,
       "ctl: every entry the EARLIER run wrote is still in the registry");
    printf("  [ctl] %d of %zu earlier row(s) survived\n", kept, n1);

    /* The rows this run ADDED must be there too: a "fix" that stopped
     * writing the registry would pass the line above. */
    {
        int added = 0;
        for (i = 0; i < n2; i++)
            if (!has_row(r1, n1, &r2[i])) added++;
        printf("  [ctl] %zu row(s) after two runs, %d of them new\n", n2, added);
        ok(n2 > n1,
           "ctl: the second run ADDED rows (the registry is not frozen)");
    }
    free(r1);
    free(r2);
    vol_close(v);
}

/* ---- the position search --------------------------------------------- */
typedef struct {
    int disturbed;        /* the flush was visibly disturbed */
    int lost;             /* and an earlier row was lost anyway */
    int rc;
    int n1, n2;
} probe_t;

/* One scenario from a fresh image: an earlier run writes a registry, then a
 * second run flushes with a dirent row read armed to fail at position `n`. */
static void red_attempt(int n, probe_t *out)
{
    invfs_volume *v;
    reg_row *r1 = NULL, *r2 = NULL;
    size_t n1 = 0, n2 = 0;
    int rc, kept;

    memset(out, 0, sizeof *out);
    mkfs_fresh();
    v = open_vol();

    one_run(v, "a", 0, 0);
    if (registry_read(v, &r1, &n1, "red/run1") != 0 || n1 == 0) {
        printf("  [n=%d] SETUP FAILED: the first run wrote no registry\n", n);
        free(r1); vol_close(v);
        return;
    }
    printf("  [n=%d] the earlier run left %zu registry row(s)\n", n, n1);

    rc = one_run(v, "b", 1, n);

    if (registry_read(v, &r2, &n2, "red/run2") != 0) {
        printf("  [n=%d] the registry is UNREADABLE after the flush -- that "
               "is the loss in its strongest form\n", n);
        out->disturbed = 1;
        out->lost = 1;
        out->rc = rc;
        free(r1); free(r2); vol_close(v);
        return;
    }
    kept = survivors(r2, n2, r1, n1, "red");
    out->rc = rc;
    out->n1 = (int)n1;
    out->n2 = (int)n2;
    out->lost = (kept != (int)n1);
    /* The flush was disturbed at this position if EITHER observable says so:
     * the fix refuses (rc < 0), the defect overwrites (rows gone). A position
     * where the fault landed nowhere shows a healthy append -- rc >= 0 AND
     * every earlier row still present -- and must NOT count, or the search
     * would pass without having armed anything. */
    out->disturbed = out->lost || (rc < 0);
    printf("  [n=%d] flush rc=%d, %zu row(s) now, %d of %zu earlier rows "
           "survived -> %s\n", n, rc, n2, kept, n1,
           out->disturbed ? "DISTURBED" : "not disturbed");
    free(r1);
    free(r2);
    vol_close(v);
}

static void leg_red(void)
{
    int disturbed = 0, probed = 0, lost_any = 0, n;
    probe_t p;

    printf("== leg `red`: a flush whose registry lookup did not COMPLETE must "
           "not lose the earlier run's entries ==\n");

    /* PART A -- the deterministic assertion, at countdown position 1.
     * Position 1 is not a guess: vol_tz_flush calls tz_flush_one_v3(v, 0)
     * first, and its FIRST act is tz_reg_load (vol_textzone.c:601), whose
     * first act is vol_find(TZ_OWNER_NAME) (:327) -> vol_path_lookup ->
     * one vol_dirent_get. Nothing else touches a dirent row before it. */
    printf("  -- position n=1: tz_reg_load's OWN lookup, "
           "deterministically\n");
    fflush(stdout);
    red_attempt(1, &p);
    probed++;
    if (p.disturbed) disturbed++;
    if (p.lost) lost_any++;
    ok(p.disturbed,
       "n=1: the failed registry lookup was VISIBLE to the flush -- either it "
       "refused, or it overwrote; this position really armed the fault");
    ok(!p.lost,
       "n=1: every entry the earlier run wrote is still in the registry");

    /* PART B -- the seam's self-check: the ordinal is not a constant, so the
     * countdown is searched and every probed position prints its trace.
     * at-least-one and not-all are both asserted. */
    printf("  -- searching the countdown (the ordinal is not a constant)\n");
    for (n = 1; n <= 4; n++) {
        printf("  -- probe position n=%d\n", n);
        fflush(stdout);
        red_attempt(n, &p);
        probed++;
        if (p.disturbed) disturbed++;
        if (p.lost) lost_any++;
        printf("  -- probe position n=%d: %s\n", n,
               p.disturbed ? "DISTURBED" : "not disturbed");
    }
    printf("  -- searched %d positions, %d disturbed, %d lost rows\n",
           probed, disturbed, lost_any);
    ok(disturbed >= 1,
       "the fault was landed on a registry lookup at least once "
       "(a search that matched nowhere armed nothing)");
    ok(disturbed < probed,
       "not every probed position was disturbed (a search that fired "
       "everywhere is measuring the healthy path)");
}

int main(int argc, char **argv)
{
    const char *leg = (argc > 1) ? argv[1] : "red";

    if (argc > 2 && argv[2][0])
        g_dir = argv[2];

    if (stale_guard() == 3) return 3;
    snprintf(g_img, sizeof g_img, "%s/wf_tz_registry.img", g_dir);

    if (!strcmp(leg, "ctl"))      leg_ctl();
    else if (!strcmp(leg, "red")) leg_red();
    else {
        fprintf(stderr, "usage: %s {ctl|red}\n", argv[0]);
        return 2;
    }
    printf("%s: %d checks, %d failures\n", leg, checks, failures);
    return failures ? 1 : 0;
}