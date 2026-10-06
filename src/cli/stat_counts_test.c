/* stat_v3_counts_test — invf-stat's file counts must agree with invf-ls.
 *
 * WP stat-counts-v3: the red control.
 *
 * The defect: invf-stat populated its file table from vol_records_walk(), the
 * v2 inode-area scan, which finds nothing on a v3 volume. Every volume this
 * build opens is v3 (vol_open refuses anything without VOLF_META), so the tool
 * printed
 *
 *     files : 0 live of 0 names (0 tombstones), 0.0 B logical, largest 0.0 B
 *
 * for a volume holding files -- a confident zero, not an error. That is the
 * same family as the other "an error reads as an absence" findings today
 * (invf-import dropping root-only files, invf_readdir reporting a failed
 * listing as empty, invf-fsck calling a symlink volume DAMAGED).
 *
 * Why this control compares two tools instead of asserting a number
 * -------------------------------------------------------------------
 * The 146k-file incident recorded in stat.c's own header -- an image that
 * reported "65536 live of 65536 names", the fixed table's bound presenting as
 * a real total -- is what a hardcoded expectation looks like when the number is
 * a bound rather than a fact. So this control never pins a literal. It writes
 * a volume with a known set of files, then runs BOTH oracles on the SAME image
 * and requires them to agree:
 *
 *     invf-ls     "N file(s)"            ==  invf-stat "live" count
 *     invf-ls     sum of its byte column ==  invf-stat "logical"
 *     invf-ls     max of its byte column ==  invf-stat "largest"
 *     invf-ls     entry lines            ==  invf-stat "names"
 *
 * Two oracles cannot both be wrong in the same way by construction, and the
 * control cannot go stale when a count changes: adding or resizing a file in
 * the fixture moves both sides together. What it catches is exactly the defect
 * -- one tool's table empty, the other's not.
 *
 * The derived figures (logical bytes, largest) are asserted alongside the
 * count, not after it. They come out of the same table, so a fix that repaired
 * only the count would still print 0.0 B for a volume with content in it.
 *
 * Directories are in the table too (a v3 dirent tree yields them, and invf-ls
 * prints them), so the fixture has one: a directory is a NAME but not a FILE,
 * and each tool has to make that distinction the same way or they disagree.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "volume.h"

static int fails = 0;

static void ok(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

/* ---- invf-ls output model --------------------------------------------
 *   "         24 bytes  inode 7  srv/.../f1.txt"   (a file)
 *   "          0 bytes  inode 2  srv/"             (a directory, trailing /)
 *   "3 file(s)"                                     (non-dir entries only) */
typedef struct {
    uint64_t files;        /* the "N file(s)" tally */
    uint64_t entries;      /* every printed line: files + directories */
    uint64_t sum;          /* sum of the byte column */
    uint64_t max;          /* max of the byte column */
} ls_model;

static int parse_ls(const char *text, ls_model *m)
{
    const char *p = text;
    memset(m, 0, sizeof *m);
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        unsigned long long bytes = 0;
        char tail[512];

        if (len >= sizeof line) return -1;
        memcpy(line, p, len);
        line[len] = 0;
        p = eol ? eol + 1 : p + len;

        if (sscanf(line, "%llu bytes  inode %*u  %511[^\n]",
                   &bytes, tail) == 2) {
            size_t tl = strlen(tail);
            m->entries++;
            m->sum += bytes;
            if (bytes > m->max) m->max = bytes;
            if (tl == 0 || tail[tl - 1] != '/') m->files++;
        } else if (sscanf(line, "%llu file(s)", &bytes) == 1) {
            m->files = bytes;
        }
    }
    return 0;
}

/* ---- invf-stat output model ------------------------------------------
 *   "  files : 3 live of 8 names (5 directories), 4.9 KiB logical, largest 4.9 KiB"
 * Only the v3 form is parsed; the v2 form carries "(0 tombstones)" instead,
 * and the sizes are printed through stat's own human(), which rounds to one
 * decimal -- so the byte figures are compared through the SAME human() the tool
 * prints, not re-derived from ls's raw bytes. */
static void human(uint64_t bytes, char *buf, size_t cap)
{
    double v = (double)bytes;
    const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(buf, cap, "%.1f %s", v, u[i]);
}

/* The parenthesised word is "tombstones" on v2 and "directories" on v3, so it
 * is suppressed, not captured: this control parses BOTH forms. Bailing out on
 * the unfixed build because the line did not parse would make the failure a
 * parse error, not the disagreement between the two oracles -- which is the
 * thing under test. */
typedef struct {
    unsigned long long live, names, dirs;
    char logical[80], largest[40];
} stat_model;

static int parse_stat(const char *text, stat_model *m)
{
    const char *p = strstr(text, "  files : ");
    char lnum[32], lunit[32];
    memset(m, 0, sizeof *m);
    if (!p) return -1;
    p += strlen("  files : ");
    /* two whitespace-separated words for the size ("4.9 KiB"), "logical"
     * suppressed, then the largest size up to the line end */
    if (sscanf(p, "%llu live of %llu names (%llu %*[^)]), %31[^ ] %31[^ ]"
                  " %*[^,], largest %39[^,\r\n]",
               &m->live, &m->names, &m->dirs, lnum, lunit, m->largest) != 6)
        return -1;
    snprintf(m->logical, sizeof m->logical, "%s %s", lnum, lunit);
    return 0;
}

/* ---- run a tool, slurp its stdout ------------------------------------- */
static char *run_tool(const char *tool, const char *img, const char *extra)
{
    char cmd[1400];
    char *buf = NULL;
    size_t n = 0;
    FILE *f;
    const char *repo = getenv("PWD") ? getenv("PWD") : ".";

    snprintf(cmd, sizeof cmd, "%s/bin/%s %s%s%s 2>/dev/null",
             repo, tool, img, extra ? " " : "", extra ? extra : "");
    f = popen(cmd, "r");
    if (!f) return NULL;
    for (;;) {
        char chunk[4096];
        size_t got = fread(chunk, 1, sizeof chunk, f);
        if (!got) break;
        char *nb = (char *)realloc(buf, n + got + 1);
        if (!nb) { free(buf); pclose(f); return NULL; }
        buf = nb;
        memcpy(buf + n, chunk, got);
        n += got;
    }
    pclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v;
    int err = 0;

    /* Five files at five different sizes -- including 0 and a size that is not
     * a block multiple, so "largest" is not accidentally "the only big one".
     * The numbers are chosen here, not asserted: what the control checks is
     * that the two tools read the same volume the same way. */
    static const uint64_t sizes[] = { 0, 1, 100, 4096, 5000 };
    const size_t nf = sizeof sizes / sizeof sizes[0];

    char *ls_out = NULL, *stat_out = NULL;
    ls_model ls;
    stat_model st;
    uint64_t want_sum = 0, want_max = 0;

    printf("stat_v3_counts_test: invf-stat and invf-ls must agree about the "
           "same volume\n");

    snprintf(img, sizeof img, "%s/invf-stat-v3-counts.img", dir);
    unlink(img);

    /* mkfs's own chatter is not worth parsing; vol_open below is the real check
     * that it produced an image. */
    free(run_tool("invf-mkfs", img, "32"));

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "  vol_open(%s) failed: err=%d\n", img, err);
        return 2;
    }

    /* a directory, so "names" and "files" are different numbers and each tool
     * has to draw that line in the same place */
    if (vol_mkdir(v, "d") == 0) {
        fprintf(stderr, "  cannot mkdir d\n");
        vol_close(v);
        return 2;
    }
    for (size_t i = 0; i < nf; i++) {
        char name[64];
        uint8_t *b;
        snprintf(name, sizeof name, "f%zu", i);
        b = (uint8_t *)malloc(sizes[i] ? sizes[i] : 1);
        if (!b) { vol_close(v); return 2; }
        for (uint64_t j = 0; j < sizes[i]; j++)
            b[j] = (uint8_t)(j * 7 + i);
        if (vol_write_bulk(v, name, b, sizes[i], NULL) == 0) {
            fprintf(stderr, "  cannot write %s\n", name);
            free(b); vol_close(v); return 2;
        }
        free(b);
        want_sum += sizes[i];
        if (sizes[i] > want_max) want_max = sizes[i];
    }
    {   /* one file inside the directory */
        uint8_t b[321];
        for (size_t j = 0; j < sizeof b; j++) b[j] = (uint8_t)j;
        if (vol_write_bulk(v, "d/g", b, sizeof b, NULL) == 0) {
            fprintf(stderr, "  cannot write d/g\n");
            vol_close(v); return 2;
        }
        want_sum += sizeof b;
        if (sizeof b > want_max) want_max = sizeof b;
    }
    if (vol_flush(v) != 0) { fprintf(stderr, "  flush failed\n"); vol_close(v); return 2; }
    vol_close(v);

    ls_out = run_tool("invf-ls", img, NULL);
    stat_out = run_tool("invf-stat", img, NULL);
    if (!ls_out || !stat_out) {
        fprintf(stderr, "  cannot run invf-ls / invf-stat\n");
        free(ls_out); free(stat_out);
        return 2;
    }
    if (parse_ls(ls_out, &ls) != 0 || parse_stat(stat_out, &st) != 0) {
        fprintf(stderr, "  cannot parse the tool output\n--- invf-ls ---\n%s"
                        "--- invf-stat ---\n%s", ls_out, stat_out);
        free(ls_out); free(stat_out);
        return 2;
    }

    printf("  invf-ls   : %llu file(s) over %llu entries, %llu B total, "
           "largest %llu B\n",
           (unsigned long long)ls.files, (unsigned long long)ls.entries,
           (unsigned long long)ls.sum, (unsigned long long)ls.max);
    printf("  invf-stat : %llu live of %llu names (%llu directories), "
           "%s logical, largest %s\n",
           st.live, st.names, st.dirs, st.logical, st.largest);

    /* the fixture itself has to be what we think it is, or "both tools agree"
     * could mean "both tools see an empty volume" */
    ok(ls.files == nf + 1,
       "invf-ls sees the files the fixture wrote (control is not vacuous)");
    ok(ls.sum == want_sum, "invf-ls's byte total is the fixture's total");

    /* ---- the actual control: the two oracles must not disagree --------- */
    if (st.live != ls.files) {
        printf("        ^ invf-stat reports %llu live file(s), invf-ls "
               "counts %llu on the same image\n",
               (unsigned long long)st.live, (unsigned long long)ls.files);
    }
    ok(st.live == ls.files, "invf-stat's live file count == invf-ls's file(s)");

    ok(st.names == ls.entries,
       "invf-stat's name count == invf-ls's entry lines");

    {
        char want[32];
        human(ls.sum, want, sizeof want);
        if (strcmp(want, st.logical) != 0)
            printf("        ^ invf-stat says %s logical, invf-ls's byte "
                   "column sums to %llu B (%s)\n",
                   st.logical, (unsigned long long)ls.sum, want);
        ok(strcmp(want, st.logical) == 0,
           "invf-stat's logical bytes == invf-ls's summed bytes");

        human(ls.max, want, sizeof want);
        if (strcmp(want, st.largest) != 0)
            printf("        ^ invf-stat says largest %s, invf-ls's largest "
                   "entry is %llu B (%s)\n",
                   st.largest, (unsigned long long)ls.max, want);
        ok(strcmp(want, st.largest) == 0,
           "invf-stat's largest == invf-ls's largest entry");
    }

    /* The derived figures are the ones that made this a shipped lie: a count
     * of zero with "0.0 B logical" reads as "this volume is empty", which is a
     * conclusion a reader acts on. Guard the degenerate agreement explicitly. */
    ok(st.live != 0 && strcmp(st.logical, "0.0 B") != 0,
       "invf-stat does not report an empty volume as 0 files / 0.0 B logical");

    free(ls_out);
    free(stat_out);
    printf("%s: %d failure(s)\n", fails ? "RED" : "GREEN", fails);
    return fails ? 1 : 0;
}