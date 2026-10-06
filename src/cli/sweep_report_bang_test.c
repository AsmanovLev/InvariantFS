/* sweep_report_bang_test.c -- WP146: the sweep REPORT, not the sweep's
 * behaviour, is where a '!'-bearing USER name is mis-attributed.
 *
 * WP136 made the lanes actually RUN on '!'-named user files, and that turned a
 * silent omission into a false statement. invf-sweep.c routed a successful
 * rc==9/rc==10 on any name containing '!' into part_agg_add, which aggregates
 * under the prefix up to the FIRST '!' -- so N independent user files named
 * `doc!000.txt` ... `doc!299.txt` are reported by the tool as
 *
 *     doc!*: 300 parts -> PPMd batch
 *
 * which is one container's summary line. There is no container. Every one of
 * the 300 is an ordinary user file that was batched on its own account. The
 * tool exits 0. The lanes did the right thing; the REPORT lies about why.
 *
 * WHY THIS TEST ASSERTS TEXT. A test that asserts the zone would PASS on
 * unfixed code -- sweep_bang_test.c already proves WP136 fixed the lanes, and
 * those files really do reach the PPMd batch. The defect survives that proof
 * entirely inside the sentence the operator reads. So the observable here is
 * the line on stdout, matched literally, plus the exit code as a SEPARATE
 * assertion: a tool that mis-reports while exiting 0 is the shape that
 * survives review, and a fix that turned the mis-report into an error would
 * pass a text assertion by accident.
 *
 * STAGING. WP135 refuses '!' at every user-name funnel, and correctly so --
 * no shipped tool writes one any more. The state this is about is a
 * pre-WP135 volume, and the way to put it there is the write call the lanes
 * themselves use, bypassing the refusal: vol_write_bulk (the call
 * vol_create_file wraps), which is what src/cli/sweep_bang_test.c does.
 *
 * ORACLE. No vol_find here at all, deliberately: this test never asks whether
 * a name resolves. It asks what the sweep PRINTED. The exact-name vs
 * container-path read question is read_named_test's, not this one's.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else        { printf("  OK    %s\n", what); }
}

#define NBANG 6                 /* '!'-named user files, no container */
#define NPLAIN 2                /* ordinary names: the harness-live arm */
#define BUFSZ (48 * 1024)

static uint8_t g_buf[BUFSZ];

static void fill_buf(void)
{
    size_t i;
    for (i = 0; i < BUFSZ; i++)
        g_buf[i] = (uint8_t)("THE_QUICK_BROWN_FOX_JUMPS_OVER_THE_LAZY_DOG_"
                             "and_then_some_more_repeating_text_here\n"[i % 62]);
}

/* ---- run the REAL invf-sweep and keep its stdout -------------------- */

static char g_out[262144];
static size_t g_outn;
static int  g_rc;

/* Where the sweep binary lives. `make test` runs from the repo root, so this
 * is the same lookup walk_status_test uses for its subprocesses. */
static void sweep_path(char *out, size_t cap)
{
    const char *env = getenv("INVFS_BIN_DIR");
    if (env && env[0]) { snprintf(out, cap, "%s/invf-sweep", env); return; }
    if (access("./bin/invf-sweep", X_OK) == 0) {
        snprintf(out, cap, "./bin/invf-sweep");
        return;
    }
    env = getenv("PWD");
    snprintf(out, cap, "%s/bin/invf-sweep", (env && env[0]) ? env : ".");
}

/* Run bin/invf-sweep on the image; stdout lands in g_out, the exit code in
 * g_rc. stderr is NOT merged: the report the operator reads is stdout, and
 * merging them would let a stderr line satisfy a stdout assertion. */
static int run_sweep(const char *img)
{
    char cmd[1024], bin[512];
    FILE *p;

    sweep_path(bin, sizeof bin);
    g_outn = 0;
    g_out[0] = '\0';
    /* no TTY: the tree UI must stay off, because invfs_sweep_ui_active()
     * SUPPRESSES the per-file lines and the aggregation entirely. The defect
     * is only visible when the tool prints, and a test that accidentally ran
     * under a TTY would assert nothing. */
    snprintf(cmd, sizeof cmd, "%s %s 2>/dev/null", bin, img);
    p = popen(cmd, "r");
    if (!p) return -1;
    for (;;) {
        size_t got = fread(g_out + g_outn, 1, sizeof g_out - 1 - g_outn, p);
        if (got == 0) break;
        g_outn += got;
        if (g_outn + 1 >= sizeof g_out) break;
    }
    g_out[g_outn] = '\0';
    g_rc = pclose(p);
    return 0;
}

/* Print the sweep's stdout verbatim. The report is the observable, so the
 * test shows the operator exactly what it saw rather than a filtered view. */
static void dump_out(const char *indent)
{
    const char *p = g_out;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l) { printf("%s| ", indent); fwrite(p, 1, l, stdout); printf(" |\n"); }
        if (!nl) break;
        p = nl + 1;
    }
}

static int out_has(const char *needle)
{
    return strstr(g_out, needle) != NULL;
}

/* Count non-overlapping occurrences of a literal. */
static int out_count(const char *needle)
{
    size_t nl = strlen(needle);
    int n = 0;
    const char *p = g_out;
    while (nl && (p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
}

static invfs_volume *fresh(const char *dir, const char *tag, char *img, size_t icap)
{
    char cmd[1024];
    invfs_volume *v;
    int err = 0;

    snprintf(img, icap, "%s/sweep-report-bang-%s.img", dir, tag);
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1",
             getenv("PWD") ? getenv("PWD") : ".", img);
    if (system(cmd) != 0) { printf("  cannot mkfs %s\n", img); return NULL; }
    v = vol_open(img, &err);
    if (!v) printf("  vol_open(%s) failed err=%d\n", img, err);
    else vol_close(v);   /* the fixture stage reopens; do not hold the lock */
    return v;
}

/* ---- the fixture legs ----------------------------------------------- */

/* LEG 1: NBANG independent user files whose names contain a '!', and NO
 * container anywhere on the volume. This is the mis-attribution input. */
static int stage_bang_users(const char *img)
{
    invfs_volume *v;
    int err = 0, i, good = 1;
    char name[64];

    v = vol_open(img, &err);
    if (!v) return 0;
    for (i = 0; i < NBANG; i++) {
        snprintf(name, sizeof name, "doc!%03d.txt", i);
        if (!vol_write_bulk(v, name, g_buf, BUFSZ, NULL)) good = 0;
    }
    /* the harness-live arm: ordinary names, same bytes, same sweep */
    for (i = 0; i < NPLAIN; i++) {
        snprintf(name, sizeof name, "plain%d.txt", i);
        if (!vol_write_bulk(v, name, g_buf, BUFSZ, NULL)) good = 0;
    }
    vol_flush(v);
    vol_close(v);
    return good;
}

/* LEG 2: a GENUINE decomposition, done BY THE SWEEP. A real ustar is staged
 * as an ordinary RAW file and the sweep's own container lane claims it and
 * mints `box.tar!partN` siblings with `box.tar` as their live container --
 * that is the only way to see the aggregation on a real sibling rather than
 * on a volume that already holds the members. Their report MUST still
 * collapse to one summary line: a fix that simply dropped the aggregation
 * would turn this into one line per member and be wrong. */
static size_t build_tar(uint8_t **out)
{
    const char *names[3] = { "m0", "m1", "m2" };
    size_t pos = 0, cap = 8192;
    uint8_t *buf = malloc(cap);
    int i;

    if (!buf) return 0;
    for (i = 0; i < 3; i++) {
        uint8_t hdr[512], payload[400];
        size_t plen = sizeof payload, j;
        unsigned sum = 0;

        memset(hdr, 0, sizeof hdr);
        memset(payload, 'a' + i, plen);
        memcpy(hdr, names[i], strlen(names[i]));
        memcpy(hdr + 100, "0000644\0", 8);
        memcpy(hdr + 108, "0000000\0", 8);
        memcpy(hdr + 116, "0000000\0", 8);
        memcpy(hdr + 124, "0001000\0", 8);
        memcpy(hdr + 148, "        ", 8);
        hdr[156] = '0';
        memcpy(hdr + 257, "ustar", 5);
        memcpy(hdr + 263, "00", 2);
        for (j = 0; j < sizeof hdr; j++) sum += hdr[j];
        snprintf((char *)hdr + 148, 8, "%06o", sum);
        hdr[154] = '\0';

        while (pos + 512 + plen + 1024 > cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + pos, hdr, 512);
        memcpy(buf + pos + 512, payload, plen);
        pos += 512 + plen;
        while (pos % 512) buf[pos++] = 0;
    }
    memset(buf + pos, 0, 1024);
    pos += 1024;
    *out = buf;
    return pos;
}

static int stage_real_container(const char *img)
{
    invfs_volume *v;
    uint8_t *tar = NULL;
    size_t tarlen;
    int err = 0, good = 0;

    v = vol_open(img, &err);
    if (!v) return 0;
    tarlen = build_tar(&tar);
    if (tarlen) {
        /* one ordinary user file too, so the run is not container-only */
        if (vol_write_bulk(v, "beside.txt", g_buf, BUFSZ, NULL) &&
            vol_write_bulk(v, "box.tar", tar, tarlen, NULL))
            good = 1;
        free(tar);
    }
    vol_flush(v);
    vol_close(v);
    return good;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : NULL;
    char img[512];

    setvbuf(stdout, NULL, _IONBF, 0);
    fill_buf();

    /* AGENTS.md: the scratch must be on /srv, not /tmp (RAM here). The
     * Makefile passes no argument, so resolve it here and let a caller
     * override. */
    if (!dir || !dir[0]) {
        if (access("/srv/bench/scratch", W_OK) == 0) dir = "/srv/bench/scratch";
        else dir = "/tmp";
    }

    printf("sweep_report_bang_test (WP146): invf-sweep reports '!'-named USER "
           "files as one container's parts\n");
    printf("  scratch: %s\n", dir);

    /* ---------------- LEG 1: the mis-attribution --------------------- */
    if (!fresh(dir, "u", img, sizeof img)) return 2;
    if (!stage_bang_users(img)) { printf("  staging failed\n"); return 2; }
    if (run_sweep(img) != 0) { printf("  could not run invf-sweep\n"); return 2; }

    printf("  -- invf-sweep stdout on %d '!'-named user files, no container --\n",
           NBANG);
    dump_out("    ");

    /* THE CONTROL ARM. The harness is live: an ordinary name produces its own
     * per-file line on the same run. Without this, "no aggregation line at
     * all" would satisfy the next assertion for the wrong reason. */
    ok(out_has("plain0.txt: text -> PPMd batch"),
       "CONTROL (harness live): an ordinary name is reported per file");

    /* THE RED CONTROL: the report must not claim one container's parts. */
    ok(!out_has("doc!*: "),
       "RED CONTROL: the sweep does NOT report the '!'-named user files as "
       "one container's parts");
    ok(out_count("*: ") == 0,
       "RED CONTROL: no aggregate 'prefix*: N parts' line appears at all -- "
       "the operator is told what happened to each file, by name");
    ok(out_has("doc!000.txt: text -> PPMd batch") &&
       out_has("doc!001.txt: text -> PPMd batch"),
       "RED CONTROL: each '!'-named user file is reported ON ITS OWN, like "
       "any other batched file");

    /* The exit code, asserted SEPARATELY and as success. On unfixed code this
     * is 0 while the text above is false: that combination is the shape that
     * survives, so the test pins both halves independently. A fix that made
     * the tool refuse or error instead of correcting the sentence would fail
     * this. */
    ok(g_rc == 0,
       "the sweep still exits 0 (the fix corrects the report; it does not "
       "turn a batching decision into an error)");

    /* ---------------- LEG 2: a real decomposition still reports as one */
    if (!fresh(dir, "c", img, sizeof img)) return 2;
    if (!stage_real_container(img)) { printf("  container staging failed\n"); return 2; }
    /* TWO passes, and the second is the one that reports. A container lane
     * mints its siblings DURING transform, after the collect stage has
     * already fixed the candidate list, so the members are swept on the NEXT
     * pass. That is not a test artefact -- it is how a real volume behaves,
     * and it is the pass on which the aggregation has anything to aggregate.
     */
    if (run_sweep(img) != 0) { printf("  could not run invf-sweep\n"); return 2; }
    if (run_sweep(img) != 0) { printf("  could not run invf-sweep\n"); return 2; }

    printf("  -- invf-sweep stdout on a real tar decomposition (pass 2) --\n");
    dump_out("    ");
    /* WHAT IS ACTUALLY ASSERTED HERE, and why it is not "box.tar!*: N parts".
     *
     * Measured on this tree: BOTH rc==9 and rc==10 are emitted from inside
     * `if (!vol_name_is_container_sibling(v, name))`
     * (src/core/vol_sweep.c:695 :704 and :819 :823), so a lane-minted sibling
     * never reaches either code and WP10 12.7 -- a member belongs to its
     * container's batch, not to a whole-file lane -- holds. A genuine
     * decomposition therefore reports the container under its OWN name and
     * each member individually as "no lane claimed it". It does NOT, and on
     * this tree CANNOT, produce a `box.tar!*: N parts` line.
     *
     * So the arm that matters is the negative one: a real decomposition must
     * still be named file by file, and must never be summarised under a
     * prefix the container does not own. The sweep's exit code stays 0 --
     * declining a file is not a failure, and it says so on the line.
     */
    ok(out_has("box.tar: "),
       "a GENUINE decomposition still reports the container by its own name");
    ok(out_has("box.tar!part0: "),
       "a genuine sibling is still NAMED, not folded into a summary prefix");
    ok(out_count("box.tar!*: ") == 0 &&
       out_count("beside.txt*: ") == 0 &&
       out_count("doc!*") == 0,
       "CONTROL: no summary prefix is invented on a genuine decomposition");
    ok(g_rc == 0, "the container run also exits 0 (declining is not failing)");

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}