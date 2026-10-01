/* sweep_tree_bang_test.c -- WP147: the sweep's TTY tree and its dashboard both
 * answered "does this name have a '!' in it" where the core answers "is this
 * name one of OURS?".
 *
 * WP136 gave the core one predicate -- vol_name_is_container_sibling -- and
 * WP146 converted the last report sites in tools/invf-sweep.c. Two remained:
 *
 *   :163  sw_tree_set split the file's own name on '!' and built a nesting
 *         tree, so a user file `notes!final.txt` is DRAWN as container `notes`
 *         with member `final.txt`, and the panel under it prints `depth 2` for
 *         a file with no container anywhere on the volume. Same false statement
 *         as WP146's, drawn as a tree instead of printed as a summary line.
 *   :244  sw_dash_note_container counted '!' occurrences as "nesting depth" for
 *         the dashboard's recently-decomposed rows.
 *
 * WHY THIS TEST RUNS THE SWEEP ON A PTY. Both defects live behind
 * invfs_sweep_ui_active(), which is exactly `isatty(STDERR_FILENO)` with no
 * --log (tools/invf-sweep.c:1332). The WP146 test had to force the UI OFF and
 * could therefore not see these at all; this one forces it ON -- by allocating
 * a real pty (posix_openpt/grantpt/unlockpt) and putting the sweep's stdout AND
 * stderr on the slave end, so isatty() is true in the child for real, not by
 * an environment variable. Nothing here is env-shaped into the UI: there is no
 * knob that turns the tree on without a terminal.
 *
 * The control arms are what stop this from passing vacuously. Every leg that
 * asserts an ABSENCE first asserts the PRESENCE of the thing that absence is
 * about -- the tree really painted rows, the dashboard really recorded the
 * container -- so "no phantom row" can never be satisfied by a run where
 * nothing was drawn. That is the failure mode the previous two fixes in this
 * area had to work around.
 *
 * WHAT IS ASSERTED, and why a zone assertion would prove nothing: the lanes
 * are already correct (WP136) and the sweep exits 0 in both states. The
 * defect exists entirely in what is DRAWN, so the observable is the rendered
 * text. The exit code is asserted separately, in every leg.
 *
 * STAGING. WP135 refuses '!' at every user-name funnel and no shipped tool
 * writes one any more, so the state this is about (a pre-WP135 volume) is
 * staged with vol_v3_write_bulk -- the call vol_create_file wraps -- exactly as
 * sweep_bang_test.c and sweep_report_bang_test.c do. Exact names, never
 * vol_read_named (WP141).
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
/* _GNU_SOURCE for posix_openpt/grantpt/unlockpt/ptsname (the pty the UI
 * needs); nothing else in this file is platform-sensitive. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>

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

#define BUFSZ (48 * 1024)
static uint8_t g_buf[BUFSZ];

static void fill_buf(void)
{
    size_t i;
    for (i = 0; i < BUFSZ; i++)
        g_buf[i] = (uint8_t)("THE_QUICK_BROWN_FOX_JUMPS_OVER_THE_LAZY_DOG_"
                             "and_then_some_more_repeating_text_here\n"[i % 62]);
}

/* ---- run the REAL invf-sweep on a PTY, keep everything it drew ---------
 *
 * A pipe would be the easy way and would assert nothing: isatty() is false on
 * a pipe, the UI stays off, and both sites under test are behind it. So the
 * child gets a real terminal: posix_openpt + grantpt + unlockpt + ptsname, the
 * slave dup2'd onto fd 0/1/2, TIOCSCTTY so it is the controlling terminal too.
 * isatty(STDERR_FILENO) inside the sweep is then true because it is a tty.
 */
static char    g_cap[1 << 20];      /* the terminal's bytes, escapes and all */
static size_t  g_capn;
static int     g_rc = -1;

static void cap_append(const char *b, size_t n)
{
    if (g_capn + n >= sizeof g_cap) n = sizeof g_cap - 1 - g_capn;
    memcpy(g_cap + g_capn, b, n);
    g_capn += n;
    g_cap[g_capn] = '\0';
}

/* argv0 = the sweep binary; argv[1..] = the sweep's own arguments. */
static int run_sweep_pty(char **args)
{
    int mfd, sfd, status;
    pid_t pid;

    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0) return -1;
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0) { close(mfd); return -1; }
    {
        char *slave = ptsname(mfd);
        if (!slave) { close(mfd); return -1; }
        sfd = open(slave, O_RDWR);
    }
    if (sfd < 0) { close(mfd); return -1; }

    g_capn = 0;
    g_cap[0] = '\0';
    g_rc = -1;
    pid = fork();
    if (pid < 0) { close(sfd); close(mfd); return -1; }
    if (pid == 0) {
        setsid();
        (void)ioctl(sfd, TIOCSCTTY, 0);
        dup2(sfd, STDIN_FILENO);
        dup2(sfd, STDOUT_FILENO);
        dup2(sfd, STDERR_FILENO);
        if (sfd > STDERR_FILENO) close(sfd);
        close(mfd);
        execv(args[0], args);
        _exit(127);
    }
    close(sfd);
    for (;;) {
        char b[8192];
        ssize_t n = read(mfd, b, sizeof b);
        if (n > 0) cap_append(b, (size_t)n);
        else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    close(mfd);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    g_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return 0;
}

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

/* ---- reading what the terminal showed ---------------------------------- */

/* A rendered tree row is written by sw_tree_render as
 *   "  " ["  "] "%-46s" " " "%8s" "\n"
 * i.e. exactly 57 bytes, or 59 with the deeper-level indent, the name in a
 * 46-byte field and the size in an 8-byte right-aligned one. The FIRST row of
 * a paint is glued onto the stage line (the stage line has no newline in TTY
 * mode), so a row is recognised by that fixed geometry rather than by starting
 * a line. */
typedef struct {
    char name[64];
    char size[16];
    int  depth_line;      /* the "depth N, segments ..." summary under a paint */
} trow;

#define ROW_MAX 4096
static trow g_rows[ROW_MAX];
static size_t g_nrows;

/* a size column is digits, a dot, a unit letter, or the padding between
 * them -- never a letter that starts a word, which is what keeps a stage
 * line's detail from being read as a row */
static int size_field_ok(const char *f, size_t n)
{
    size_t i;
    int sawdigit = 0;
    for (i = 0; i < n; i++) {
        char c = f[i];
        if (c >= '0' && c <= '9') { sawdigit = 1; continue; }
        if (c == '.' || c == ' ') continue;
        if ((c == 'K' || c == 'M' || c == 'G' || c == 'B') && sawdigit)
            continue;
        return 0;
    }
    return 1;
}

static void parse_rows(void)
{
    const char *p = g_cap, *end = g_cap + g_capn;

    g_nrows = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[4096];
        size_t i, w = 0;
        const char *cand = NULL;
        size_t candlen = 0;

        if (len >= sizeof line) len = sizeof line - 1;
        /* strip CR and every CSI escape: the panel repaints with \r and
         * \033[2K / \033[1A, which say nothing about what was drawn */
        for (i = 0; i < len; i++) {
            if (p[i] == '\r') continue;
            if (p[i] == 0x1b && i + 1 < len && p[i + 1] == '[') {
                i += 2;
                while (i < len && !((p[i] >= '@' && p[i] <= '~'))) i++;
                continue;
            }
            line[w++] = p[i];
        }
        line[w] = '\0';

        /* the depth summary: "  " + 46 spaces + " " + 8 spaces + "   depth ..." */
        {
            const char *d = strstr(line, "   depth ");
            if (line[0] == ' ' && line[1] == ' ' && d) {
                if (g_nrows < ROW_MAX) {
                    memset(&g_rows[g_nrows], 0, sizeof g_rows[0]);
                    g_rows[g_nrows].depth_line = 1;
                    g_nrows++;
                }
                goto next;
            }
        }
        /* A row: the line ENDS with [>=2 spaces of indent][46-byte name
         * field][1 space][8-byte size field]. The first row of a paint is
         * glued onto the stage line (the stage line has no newline in TTY
         * mode), so rows are recognised by that trailing geometry rather than
         * by starting a line -- and the indent is 2 bytes for a top-level row
         * and 4 for a nested one, which the same rule covers. */
        if (w >= 57) {
            size_t i = w - 55;               /* start of the 46-byte name field */
            if (line[i - 1] == ' ' && line[i - 2] == ' ' &&
                size_field_ok(line + i + 47, 8)) {
                cand = line + i;
                candlen = 46;
            }
        }
        if (cand && g_nrows < ROW_MAX) {
            trow *r = &g_rows[g_nrows];
            size_t k, n = 0;
            memset(r, 0, sizeof *r);
            for (k = 0; k < candlen && cand[k] != ' '; k++)
                if (n < sizeof r->name - 1) r->name[n++] = cand[k];
            r->name[n] = '\0';
            n = 0;
            for (k = 0; k < 8; k++)
                if (cand[47 + k] != ' ' && n < sizeof r->size - 1)
                    r->size[n++] = cand[47 + k];
            r->size[n] = '\0';
            g_nrows++;
        }
next:
        if (!nl) break;
        p = nl + 1;
    }
}

static int row_seen(const char *name)
{
    size_t i;
    for (i = 0; i < g_nrows; i++)
        if (!g_rows[i].depth_line && strcmp(g_rows[i].name, name) == 0)
            return 1;
    return 0;
}

static int row_seen_with_size(const char *name)
{
    size_t i;
    for (i = 0; i < g_nrows; i++)
        if (!g_rows[i].depth_line && strcmp(g_rows[i].name, name) == 0 &&
            g_rows[i].size[0])
            return 1;
    return 0;
}

/* a container row immediately followed by one of its members -- the nesting,
 * drawn as a tree, in ONE paint */
static int row_pair_seen(const char *parent, const char *prefix)
{
    size_t i;
    for (i = 0; i + 1 < g_nrows; i++) {
        if (g_rows[i].depth_line || strcmp(g_rows[i].name, parent) != 0)
            continue;
        if (!g_rows[i + 1].depth_line &&
            !strncmp(g_rows[i + 1].name, prefix, strlen(prefix)))
            return 1;
    }
    return 0;
}

static void dump_rows(const char *indent, int limit)
{
    size_t i;
    int n = 0;
    for (i = 0; i < g_nrows; i++) {
        if (g_rows[i].depth_line) {
            if (n++ >= limit) return;
            printf("%s[depth summary]\n", indent);
        } else {
            if (n++ >= limit) return;
            printf("%srow: name=%-28s size=%s\n", indent, g_rows[i].name,
                   g_rows[i].size);
        }
    }
}

/* ---- staging ----------------------------------------------------------- */

static int fresh(const char *dir, const char *tag, char *img, size_t icap)
{
    char cmd[1024];
    const char *root = getenv("PWD");
    int err = 0;

    snprintf(img, icap, "%s/sweep-tree-bang-%s.img", dir, tag);
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1",
             root ? root : ".", img);
    if (system(cmd) != 0) { printf("  cannot mkfs %s\n", img); return 0; }
    {   /* prove the image opens before the test pretends to sweep it */
        invfs_volume *v = vol_open(img, &err);
        if (!v) { printf("  vol_open(%s) err=%d\n", img, err); return 0; }
        vol_close(v);
    }
    return 1;
}

static int stage_files(const char *img, char *const *names, int n)
{
    invfs_volume *v;
    int err = 0, i, good = 1;

    v = vol_open(img, &err);
    if (!v) { printf("  vol_open failed err=%d\n", err); return 0; }
    for (i = 0; i < n; i++)
        if (!vol_v3_write_bulk(v, names[i], g_buf, BUFSZ, NULL)) {
            printf("  staging %s failed\n", names[i]);
            good = 0;
        }
    vol_flush(v);
    vol_close(v);
    return good;
}

/* a real ustar, so the sweep's own container lane claims it and mints
 * `box.tar!partN` with `box.tar` as their live container -- the only way to see
 * a REAL decomposition's tree rather than one this test staged by hand */
static size_t build_tar(uint8_t **out)
{
    const char *names[3] = { "m0", "m1", "m2" };
    size_t pos = 0, cap = 8192;
    uint8_t *buf = (uint8_t *)malloc(cap);
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

static int stage_tar(const char *img)
{
    invfs_volume *v;
    uint8_t *tar = NULL;
    size_t tarlen;
    int err = 0, good = 0;

    v = vol_open(img, &err);
    if (!v) return 0;
    tarlen = build_tar(&tar);
    if (tarlen) {
        if (vol_v3_write_bulk(v, "box.tar", tar, tarlen, NULL)) good = 1;
        free(tar);
    }
    vol_flush(v);
    vol_close(v);
    return good;
}

/* ---- the dashboard fixture -------------------------------------------- */

/* sw_dash_note_container is reached only on `rc >= 100`, i.e. a codecpack
 * transcode. There is no shipped fixture for that in the unit suites, so this
 * writes one: a gzip-based pack that claims the staged content by magic and
 * round-trips it (vol_pack_sweep decodes back and memcmps before it replaces
 * anything, so the transcode is a real one). Its encoder sleeps two seconds --
 * not for flavour: the dashboard rewrites its HTML at most once a second
 * (:269), so a run that finished inside that window could write the page
 * BEFORE the transcode and the leg would assert nothing. The sleep makes the
 * one direction deterministic instead of probable, and the CONTROL below fails
 * loudly if the page still has no row rather than letting the absence pass. */
static int write_dash_pack(const char *dir)
{
    char path[1024], cmd[1024];
    FILE *f;

    snprintf(path, sizeof path, "%s/packs", dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(path, sizeof path, "%s/packs/tst.codecpack", dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(path, sizeof path, "%s/packs/tst.codecpack/bin", dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return 0;

    snprintf(path, sizeof path, "%s/packs/tst.codecpack/manifest", dir);
    f = fopen(path, "w");
    if (!f) return 0;
    /* "THE_QUIC" -- the first 8 bytes of g_buf, which every staged file has */
    fprintf(f,
            "name = tst\nalgo = 45\ncodec_id = 45\npack_version = 1\n"
            "version = 1\nmin_read = 1\ngeneration = 1\nfamily = tst\n"
            "category = primary\nprovides = tst\nreplaces =\nconflicts =\n"
            "priority = 100\ncaps = wholefile|external\ndec_mem = 0\n"
            "sniff.magic = 5448455f51554943\n"
            "encode = %s/packs/tst.codecpack/bin/enc {in} {out}\n"
            "decode = %s/packs/tst.codecpack/bin/dec {in} {out}\n",
            dir, dir);
    fclose(f);

    snprintf(path, sizeof path, "%s/packs/tst.codecpack/bin/enc", dir);
    f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "#!/bin/sh\nsleep 2\nexec gzip -c \"$1\" > \"$2\"\n");
    fclose(f);
    snprintf(path, sizeof path, "%s/packs/tst.codecpack/bin/dec", dir);
    f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "#!/bin/sh\nexec gzip -dc \"$1\" > \"$2\"\n");
    fclose(f);
    snprintf(cmd, sizeof cmd, "chmod +x '%s/packs/tst.codecpack/bin/enc' "
                              "'%s/packs/tst.codecpack/bin/dec'", dir, dir);
    return system(cmd) == 0;
}

/* Pull one `class=e` row's padding and label out of the dashboard page. */
static int dash_row(const char *html, size_t n, const char *label,
                    char *pad, size_t padcap)
{
    const char *p = html, *end = html + n;
    const char *q;

    pad[0] = '\0';
    while (p < end) {
        q = strstr(p, "<div class=e>");
        if (!q || q >= end) return 0;
        {
            const char *rowend = strstr(q, "</div>");
            const char *lab = strstr(q, label);
            const char *pp;
            if (lab && lab < rowend) {
                pp = strstr(q, "padding-left:");
                if (pp && pp < lab) {
                    size_t k = pp + 13 - q;
                    size_t w = 0;
                    while (k < (size_t)(lab - q) && w + 1 < padcap &&
                           q[k] != '"')
                        pad[w++] = q[k++];
                    pad[w] = '\0';
                    return 1;
                }
            }
        }
        p = q + 1;
    }
    return 0;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long sz;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    *len = fread(b, 1, (size_t)sz, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : NULL;
    char bin[512], img[512], html[600];
    char *args[8];

    setvbuf(stdout, NULL, _IONBF, 0);
    fill_buf();
    sweep_path(bin, sizeof bin);

    if (!dir || !dir[0]) {
        if (access("/srv/bench/scratch", W_OK) == 0) dir = "/srv/bench/scratch";
        else dir = "/tmp";
    }

    printf("sweep_tree_bang_test (WP147): the TTY tree and the dashboard depth "
           "answer about a BYTE, not about a name we minted\n");
    printf("  sweep:    %s\n", bin);
    printf("  scratch:  %s\n", dir);
    if (access(bin, X_OK) != 0) {
        printf("  no sweep binary at %s\n", bin);
        return 2;
    }

    /* ================= LEG 1: a USER file on the TTY panel ============ */
    if (!fresh(dir, "u", img, sizeof img)) return 2;
    {
        char *names[1];
        names[0] = (char *)"notes!final.txt";
        /* ONE file, so it is necessarily the last transform candidate and
         * therefore the chain every later forced paint redraws -- the
         * assertion does not depend on the walk's file order. */
        if (!stage_files(img, names, 1)) return 2;
    }
    args[0] = bin; args[1] = img; args[2] = NULL;
    if (run_sweep_pty(args) != 0) { printf("  could not run invf-sweep\n"); return 2; }
    parse_rows();
    printf("  -- rows the sweep drew for `notes!final.txt` (one user file, "
           "no container) --\n");
    dump_rows("    ", 12);

    /* CONTROL: the panel really drew rows. Without this, "no phantom row"
     * would be satisfied by a sweep that drew nothing at all -- and by a
     * sweep that never took the TTY path, which is the exact vacuity the
     * WP146 test had to avoid in the other direction. */
    ok(g_nrows > 0,
       "CONTROL: the TTY panel drew tree rows (the sweep ran with a real tty "
       "on fd 1 and 2, so the UI was live)");
    ok(row_seen_with_size("notes!final.txt"),
       "CONTROL: the '!'-named user file is drawn under its OWN name, with "
       "its own size looked up on the volume");
    /* THE RED CONTROL. `notes` does not exist on this volume: it is the
     * prefix of a name the user typed. Drawing it as a row is drawing a
     * container that has never existed. */
    ok(!row_seen("notes"),
       "RED CONTROL: no tree row invents the container `notes` out of a user "
       "file named notes!final.txt");
    ok(g_rc == 0,
       "the sweep still exits 0 (the panel is corrected; it does not turn a "
       "swept file into an error)");

    /* ================= LEG 2: a REAL decomposition still nests ======== */
    if (!fresh(dir, "d", img, sizeof img)) return 2;
    if (!stage_tar(img)) { printf("  tar staging failed\n"); return 2; }
    /* TWO passes: a container lane mints its siblings during transform,
     * after the collect stage fixed the candidate list, so the members are
     * swept on the NEXT pass. That is how a real volume behaves, not a test
     * artefact. */
    args[0] = bin; args[1] = img; args[2] = NULL;
    if (run_sweep_pty(args) != 0) return 2;
    if (run_sweep_pty(args) != 0) return 2;
    parse_rows();
    printf("  -- rows the sweep drew for a genuine tar decomposition --\n");
    dump_rows("    ", 12);
    ok(row_seen_with_size("box.tar"),
       "CONTROL: the container is still drawn as its own level");
    ok(row_pair_seen("box.tar", "box.tar!part"),
       "CONTROL: a real member still NESTS under its container -- the "
       "container row is immediately followed by a box.tar!partN row, in one "
       "paint. A fix that made every '!' name flat would fail this.");
    ok(g_rc == 0, "the container run also exits 0");

    /* ================= LEG 3: the dashboard depth ===================== */
    if (!write_dash_pack(dir)) { printf("  dash pack fixture failed\n"); return 2; }
    if (!fresh(dir, "x", img, sizeof img)) return 2;
    {
        /* `photos.img!backup` is a name the USER typed and the pack claims,
         * so rc >= 100 and sw_dash_note_container runs; the staged
         * `photos.img!backup!mbr0001-x` beside it is what makes the function
         * get past its `if (!members) return`. There is no `photos.img` on
         * this volume and no container by that name anywhere. */
        char *names[2];
        names[0] = (char *)"photos.img!backup";
        names[1] = (char *)"photos.img!backup!mbr0001-x";
        if (!stage_files(img, names, 2)) return 2;
    }
    snprintf(html, sizeof html, "%s/sweep-tree-bang-dash.html", dir);
    unlink(html);
    {
        /* the registry scans <dirs>/<name>.codecpack/manifest for each
         * colon-separated dir in INVFS_CODECPACKS, so the value is the
         * PARENT of the pack, not the pack (src/codecs/codec.c:676) */
        char packs[1024];
        snprintf(packs, sizeof packs, "%s/packs", dir);
        setenv("INVFS_CODECPACKS", packs, 1);
    }
    setenv("INVFS_CODECPACKS_SYS", "0", 1);
    args[0] = bin; args[1] = img;
    args[2] = "--dash"; args[3] = html; args[4] = NULL;
    if (run_sweep_pty(args) != 0) return 2;
    {
        size_t hlen = 0;
        char *page = slurp(html, &hlen);
        char pad[32];

        if (!page) { printf("  no dashboard page written\n"); return 2; }
        printf("  -- dashboard rows --\n");
        {
            char *q = strstr(page, "decomposed");
            if (q) printf("    %.*s\n", (int)(hlen - (size_t)(q - page) > 220 ?
                          220 : hlen - (size_t)(q - page)), q);
        }
        ok(dash_row(page, hlen, "photos.img!backup", pad, sizeof pad),
           "CONTROL: the dashboard recorded the transcoded container at all "
           "(so the depth assertion below is about its indentation, not "
           "about a missing row)");
        ok(pad[0] == '0',
           "RED CONTROL: a user file with a '!' in its name is listed at depth "
           "0 -- not indented under a container named after its own prefix");
        /* the same defect in the dashboard's ACTIVE-CHAIN panel, which draws
         * the tree: `photos.img!backup!mbr0001-x` must be one row, not a
         * three-level chain */
        {
            char act[4096];
            const char *a = strstr(page, "class=act");
            int onelevel;
            act[0] = '\0';
            if (a) {
                const char *e = strstr(a, "</div></div>");
                size_t l = e ? (size_t)(e - a) : 0;
                if (l >= sizeof act) l = sizeof act - 1;
                memcpy(act, a, l);
                act[l] = '\0';
            }
            printf("    active chain: %s\n", act);
            onelevel = strstr(act, ">photos.img!backup!mbr0001-x<") != NULL;
            ok(onelevel,
               "RED CONTROL: the dashboard's active chain shows the whole name "
               "as ONE level, not photos.img / backup / mbr0001-x");
        }
        ok(g_rc == 0, "the codecpack run also exits 0 (a transcode is not an "
                      "error)");
        free(page);
    }

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}