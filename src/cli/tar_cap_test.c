/*
 * tar_cap_test.c — the builtin TAR lane's member cap: the DECISION, the
 * BYTES, and the REFUSAL.
 *
 * What is under test
 * ------------------
 * vol_create_tar_file()'s `n > TARX_MAX_PARTS` check, which used to read
 *
 *     #define TARX_MAX_PARTS 2048
 *     ...
 *     if (n == 0 || n > TARX_MAX_PARTS) {
 *         if (getenv("INVFS_DEBUG"))
 *             fprintf(stderr, "[vol] %s: %zu members — keep original\n", ...);
 *         return 0;
 *     }
 *
 * 2048 is below any plausible rootfs -- a pruned Debian trixie rootfs is
 * 4,733 entries, a full `debootstrap minbase` is 8,017 -- so the lane whose
 * whole job is making a rootfs cheap to ingest declined every one of them,
 * and on the normal path said nothing at all. The operator saw
 *
 *     rootfs.tar: no lane claimed it (declined or nothing to do)
 *
 * which is the same line a correctly-stored archive produces. The GZIP
 * lane's copy of the identical check printed NOTHING even under
 * INVFS_DEBUG=1.
 *
 * Why the corpus is synthesised rather than a real rootfs
 * ------------------------------------------------------
 * The archive is built in RAM from 512-byte headers with deterministic
 * payloads, so the test is exact and cheap: a 4,000-member tar is ~2.6 MB
 * and a 65,537-member tar is ~33 MB of header, neither of which is a file
 * on disk. What the bound keys on is the member COUNT, and the count is
 * exact here for the same reason it is exact on a rootfs.
 *
 * Why the assertions are byte comparisons and not `invf-verify --deep`
 * -------------------------------------------------------------------
 * `invf-verify --deep` checks readability and LENGTH only
 * (src/cli/verify.c:357-364), because `invfs_ast_block_entry`
 * (src/core/invarifs.h:972-980) carries no content hash -- only a pba. A
 * lane that paired every member with another member of the same length
 * would print "N files ok, 0 corrupt". So every leg below compares actual
 * bytes:
 *   - each of the 4,000 `name!partN` sibling inodes is read back and
 *     memcmp'd against the payload region it was cut from;
 *   - the archive itself is read back through the TARR read path (which
 *     reassembles header + parts + trailer) and memcmp'd whole.
 *
 * The legs, each with the control that flips it
 * --------------------------------------------
 *   A. THE CAP. 4,000 members -- above the old 2,048, far below any real
 *      rootfs -- DECOMPOSE. Red before the fix: returns 0.
 *   B. THE CONTROL. 2,000 members -- below the old cap -- still decompose
 *      and still round-trip. This is what proves leg A is about the BOUND
 *      and not about something else that a synthesised tar gets wrong.
 *   C. THE BYTES. Every member of leg A, plus the reassembled archive.
 *   D. THE REFUSAL. 65,537 members is above the uint16 the IVFT recipe can
 *      record, so the lane must decline -- and must SAY SO on the normal
 *      path, naming the count, the limit, and that the bound is a FORMAT
 *      one (the member count is a uint16 at offset 13, src/recipes/tarx.c),
 *      not a memory one. Red before the fix: returns 0 with EMPTY stderr.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "volume_internal.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else       printf("  OK    %s\n", what);
}

/* ---- stderr capture: the refusal's WORDING is part of the defect ----
 * A capacity refusal the operator cannot read is the same failure as the
 * wrong bound, so the text is asserted rather than assumed. */
static int  g_errfd = -1, g_saved_err = -1;
static char g_errbuf[16384];

static void err_capture_begin(void)
{
    char tmpl[] = "/tmp/invf-tarcap-err-XXXXXX";
    g_saved_err = dup(2);
    g_errfd = mkstemp(tmpl);
    if (g_errfd < 0) return;
    unlink(tmpl);
    dup2(g_errfd, 2);
}

static const char *err_capture_end(void)
{
    ssize_t n;
    if (g_saved_err < 0) return "";
    fflush(stderr);
    dup2(g_saved_err, 2);
    close(g_saved_err);
    g_saved_err = -1;
    lseek(g_errfd, 0, SEEK_SET);
    n = read(g_errfd, g_errbuf, sizeof g_errbuf - 1);
    close(g_errfd);
    g_errfd = -1;
    if (n < 0) n = 0;
    g_errbuf[n] = 0;
    return g_errbuf;
}

/* ---- a ustar header with a correct checksum ---- */
static void put_octal(uint8_t *p, size_t n, uint64_t v)
{
    size_t i;
    for (i = n - 1; i > 0; i--) { p[i - 1] = (uint8_t)('0' + (v & 7)); v >>= 3; }
    p[0] = (uint8_t)('0' + (v & 7));
}

static void tar_header(uint8_t *h, const char *nm, size_t nm_len,
                       uint64_t size)
{
    memset(h, 0, 512);
    memset(h, ' ', 100);
    memcpy(h, nm, nm_len < 100 ? nm_len : 100);
    put_octal(h + 100, 8, 0644);          /* mode */
    put_octal(h + 108, 8, 0);             /* uid */
    put_octal(h + 116, 8, 0);             /* gid */
    put_octal(h + 124, 12, size);         /* size */
    put_octal(h + 136, 12, 0);            /* mtime */
    memset(h + 148, ' ', 8);              /* chksum field: spaces while summing */
    h[156] = '0';                         /* typeflag: regular file */
    memcpy(h + 257, "ustar", 5);
    memcpy(h + 263, "00", 2);
    unsigned s = 0;
    for (int i = 0; i < 512; i++) s += h[i];
    put_octal(h + 148, 7, s);
    h[154] = 0; h[155] = ' ';
}

/* Build a tar with `n` members. Member i carries `plen(i)` deterministic
 * bytes at offset `offs(i)` of the returned buffer; both are derivable by
 * the caller so it can compare a part against its source without keeping a
 * second copy. */
static uint64_t payload_len(size_t i) { return 1 + (i * 37) % 251; } /* 1..251 */

static uint8_t *build_tar(size_t n, size_t *out_len, size_t *offsets)
{
    size_t total = 0, cap = 0;
    for (size_t i = 0; i < n; i++) {
        if (offsets) offsets[i] = total;
        total += 512 + payload_len(i);
        total += (512 - (total % 512)) % 512;
    }
    total += 1024;                      /* two zero blocks: the trailer */
    cap = total;
    uint8_t *t = (uint8_t *)calloc(cap ? cap : 1, 1);
    if (!t) return NULL;
    char nm[64];
    for (size_t i = 0; i < n; i++) {
        uint64_t pl = payload_len(i);
        size_t off = offsets ? offsets[i] : 0;
        snprintf(nm, sizeof nm, "member%06zu", i);
        tar_header(t + off, nm, strlen(nm), pl);
        for (size_t j = 0; j < pl; j++)
            t[off + 512 + j] = (uint8_t)((i * 131 + j * 17 + 7) & 0xFF);
    }
    *out_len = cap;
    return t;
}

/* ---- leg C helper: read one sibling part and compare it to its source ---- */
static int part_matches(invfs_volume *v, const char *base, size_t i,
                        const uint8_t *tar, size_t off)
{
    char pn[320];
    snprintf(pn, sizeof pn, "%s!part%zu", base, i);
    uint64_t ino = vol_find(v, pn);
    if (!ino) return 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (vol_read_inode(v, ino, 0, &buf, &len) != 0) { free(buf); return 0; }
    uint64_t pl = payload_len(i);
    int good = (len == pl) && (memcmp(buf, tar + off + 512, (size_t)pl) == 0);
    free(buf);
    return good;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char img[512];
    int err = 0;
    invfs_volume *v;

    printf("tar_cap_test: the TAR lane's member cap, its bytes, and its "
           "refusal\n");

    snprintf(img, sizeof img, "%s/invf-tar-cap-test.img", dir);
    unlink(img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 48 2>/dev/null", root, img);
        if (system(cmd) != 0) { printf("  cannot create volume with invf-mkfs\n"); return 2; }
    }
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "tar_cap_test: vol_open failed: err=%d\n", err); return 2; }

    /* ---- LEG A + C: 4,000 members, above the old 2,048 cap ---- */
    {
        const size_t N = 4000;
        size_t len = 0;
        size_t *offs = (size_t *)malloc(N * sizeof *offs);
        uint8_t *tar = build_tar(N, &len, offs);
        uint64_t ino;
        ok(tar != NULL, "leg A: built a 4,000-member tar in RAM");

        ino = vol_create_tar_file(v, "rootfs.tar", tar, len);
        ok(ino != 0,
           "leg A: a 4,000-member tar DECOMPOSES (the old cap was 2,048, and a "
           "pruned trixie rootfs is 4,733)");

        if (ino) {
            /* every member, byte for byte */
            size_t bad = 0;
            for (size_t i = 0; i < N; i++)
                if (!part_matches(v, "rootfs.tar", i, tar, offs[i])) bad++;
            ok(bad == 0, "leg C: all 4,000 members read back byte-identical "
                          "(invf-verify --deep checks length only)");

            /* and the whole archive, reassembled through the TARR read path */
            uint8_t *back = NULL; size_t blen = 0;
            int rc = vol_read_inode(v, ino, 0, &back, &blen);
            ok(rc == 0 && blen == len && back && memcmp(back, tar, len) == 0,
               "leg C: the reassembled 4,000-member archive is byte-identical "
               "to the source");
            free(back);
        }
        free(tar);
        free(offs);
        /* leave the volume clean for the next leg */
        for (size_t i = 0; i < N; i++) {
            char pn[320];
            snprintf(pn, sizeof pn, "rootfs.tar!part%zu", i);
            vol_unlink(v, pn);
        }
        vol_unlink(v, "rootfs.tar");
    }

    /* ---- LEG B: the control below the old cap ---- */
    {
        const size_t N = 2000;
        size_t len = 0;
        size_t *offs = (size_t *)malloc(N * sizeof *offs);
        uint8_t *tar = build_tar(N, &len, offs);
        uint64_t ino = tar ? vol_create_tar_file(v, "small.tar", tar, len) : 0;
        ok(ino != 0,
           "leg B: the CONTROL -- 2,000 members, below the old cap, still "
           "decomposes (so leg A is about the bound)");
        if (ino) {
            size_t bad = 0;
            for (size_t i = 0; i < N; i++)
                if (!part_matches(v, "small.tar", i, tar, offs[i])) bad++;
            ok(bad == 0, "leg B: all 2,000 members read back byte-identical");
        }
        free(tar);
        free(offs);
    }

    /* ---- LEG D: above the format's uint16, and the refusal is legible ---- */
    {
        const size_t N = 65537;   /* TARX_MAX_PARTS + 2 */
        size_t len = 0;
        size_t *offs = (size_t *)malloc(N * sizeof *offs);
        uint8_t *tar = build_tar(N, &len, offs);
        const char *log;
        uint64_t ino;
        ok(tar != NULL, "leg D: built a 65,537-member tar in RAM (~33 MB)");

        err_capture_begin();
        ino = tar ? vol_create_tar_file(v, "huge.tar", tar, len) : 0;
        log = err_capture_end();

        ok(ino == 0,
           "leg D: 65,537 members is refused (the IVFT nparts is a uint16)");
        ok(strstr(log, "65537") != NULL,
           "leg D: the refusal NAMES THE COUNT on the normal path");
        ok(strstr(log, "65535") != NULL,
           "leg D: the refusal NAMES THE LIMIT on the normal path");
        ok(strstr(log, "FORMAT bound") != NULL &&
           strstr(log, "not a memory one") != NULL,
           "leg D: the refusal says the bound is a FORMAT one, not a memory "
           "one");
        if (!strstr(log, "65537")) printf("       (stderr was: %.400s)\n", log);
        free(tar);
        free(offs);
    }

    vol_close(v);
    unlink(img);
    printf("tar_cap_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
