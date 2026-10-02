/*
 * crc32c_test.c -- WP-crc32c-slice8-wrong: the portable CRC32C fallback
 * disagreed with the SSE4.2 path for every length >= 8, so every volume
 * written on a CPU without SSE4.2 (QEMU's default `qemu64` is one) lost every
 * write on reopen, and was then rejected by any host that does have SSE4.2.
 *
 * THE DEFECT, src/core/crc32c.c's table-initialisation loop:
 *
 *     static uint32_t crc32c_table8[8][256];       // static => zero-filled
 *     for (i = 0; i < 256; i++) {
 *         uint32_t c = crc32c_table[i];
 *         for (k = 1; k < 8; k++) {                // <-- k starts at 1
 *             c = crc32c_table[c & 0xFF] ^ (c >> 8);
 *             crc32c_table8[k][i] = c;
 *         }
 *     }
 *
 * T[1..7] were stored. T[0] never was. crc32c_slice8's eighth term reads
 * crc32c_table8[0][byte8] as the contribution of the EIGHTH byte of every
 * 8-byte group, so that byte contributed 0: the fallback returned the CRC32C
 * of the buffer with every 8th byte replaced by 0x00. Measured on the
 * pre-fix source, 65/65 lengths 0..64 agree with that model and disagree with
 * the real buffer; and n < 8 was always right, because the slicing loop is
 * only entered at len >= 8, so short buffers take the byte loop and never
 * touch T[0].
 *
 * WHY IT SURVIVED, and the shape of this test. crc32c_slice8 and crc32c_hw
 * were both static, and the two public entry points dispatch on CPUID -- so on
 * an SSE4.2 host NEITHER was reachable from a test, and on a non-SSE4.2 host
 * the other one could not be compared against anything. The commit that
 * introduced both (dbe9d77) records the fallback as "checked against the
 * original byte loop over 4000 random sizes up to 9 KB"; there is no third
 * entry point, so on the machine that ran, all 4000 of those sizes went to
 * crc32c_hw and compared nothing. Hence, leg by leg:
 *
 *   LEG 1  the TABLE, structurally. Needs no CPU, no reference and no volume,
 *          so it cannot be skipped for being off-path.
 *   LEG 2  known-answer vectors on the CPU's own path.
 *   LEG 3  the same vectors with the fallback FORCED. "I asked for the
 *          fallback" is not a control; "the fallback ran" is, so
 *          invfs_crc32c_using_fallback() is asserted, not assumed.
 *   LEG 4  the VOLUME: write, flush, close, REOPEN, read back, memcmp. A
 *          function-level test that passes while volumes corrupt is what got
 *          here, so the volume is the assertion.
 *   LEG 5  the CROSS-MACHINE leg: the same volume, written with the fallback
 *          pinned and reopened with it RELEASED. That is the reported
 *          symptom -- "FAIL: superblock checksum mismatch (stored 692793ab,
 *          computed 54e0f74c)", and every file gone -- in one assertion.
 *
 * A STALE-BINARY GUARD: this file asserts on crc32c.c, which is in CORE, and
 * the test binaries are prerequisites of `make test`, not of `all`, so a
 * `make -j4` that did not relink can leave a binary here that passes against
 * code it no longer measures.
 */
#include "invarifs.h"
#include "volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int checks, failures;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (!cond) failures++;
    printf("%s ", cond ? "  ok  " : "  FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void info(const char *fmt, ...)
{
    va_list ap;
    printf("  ..   ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- the reference: textbook CRC32C, derived from the definition ------- *
 * reflected polynomial 0x82F63B78, init = final xorout = 0xFFFFFFFF:
 *     c = 0xFFFFFFFF
 *     for each byte:  c = (c >> 8) ^ T[(c ^ byte) & 0xFF]
 *     return c ^ 0xFFFFFFFF
 * The table is built here from the polynomial by eight shifts. It shares a
 * PRIMITIVE with the code under test, so the result is pinned by the published
 * "123456789" vector below: a table bug common to both cannot hide behind it. */
static uint32_t ref_tab[256];

static void ref_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        ref_tab[i] = c;
    }
}

static uint32_t ref_crc32c(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = (c >> 8) ^ ref_tab[(c ^ p[i]) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

/* The length classes the defect was measured at: n < 8 agrees, n >= 8 does
 * not. 0..7 stays in the list so a pass is not only about long buffers. */
static const size_t LENS[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 23, 24, 31, 32, 33, 63, 64, 65, 127, 128, 255, 256,
    511, 512, 1023, 1024, 1025, 4095, 4096
};
#define NLENS (sizeof LENS / sizeof LENS[0])

/* Start offsets 0..15. Both paths chew bytes one at a time until the pointer
 * is 8-aligned and the defect was in the aligned body, so one offset would
 * not pin both halves. */
#define NOFF 16
#define BIGGEST_LEN 4096L                  /* must equal LENS[NLENS-1] */
#define BUFSZ (BIGGEST_LEN + NOFF)

static uint8_t buf[BUFSZ];

static void fill_buf(void)
{
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 31u + 7u);
}

/* ---- STALE-BINARY GUARD ------------------------------------------------ */
static int stale_binary(const char *src)
{
    char exe[512];
    struct stat se, ss;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    if (stat(exe, &se) || stat(src, &ss)) return 0;
    if (se.st_mtime < ss.st_mtime) {
        fprintf(stderr, "crc32c_test: STALE BINARY -- %s is older than %s\n"
                        "This binary asserts on src/core/crc32c.c, which is a "
                        "prerequisite of `make test` but not of `all`. Rebuild.\n",
                exe, src);
        return 1;
    }
    return 0;
}

/* ===================================================================== *
 * LEG 1 -- the slice-by-8 tables.
 * ===================================================================== */
static void leg1_table(void)
{
    uint32_t t0[256], want[8][256];
    int bad0 = -1, badk = -1;

    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        t0[i] = c;
    }
    /* What the slicing loop requires of the table: T[0] is the plain byte
     * table (it is the eighth byte's term), and T[k] is k more folds of it. */
    for (int i = 0; i < 256; i++) {
        uint32_t c = t0[i];
        want[0][i] = c;
        for (int k = 1; k < 8; k++) {
            c = t0[c & 0xFF] ^ (c >> 8);
            want[k][i] = c;
        }
    }

    for (int i = 0; i < 256; i++) {
        if (bad0 < 0 && invfs_crc32c_slice8_table(0, (unsigned)i) != want[0][i])
            bad0 = i;
        for (int k = 1; k < 8; k++)
            if (badk < 0 &&
                invfs_crc32c_slice8_table((unsigned)k, (unsigned)i) != want[k][i])
                badk = k;
    }

    if (bad0 < 0) {
        ok(1, "T[0] is the plain byte table (256 entries)");
    } else {
        char m[200];
        snprintf(m, sizeof m,
                 "T[0][%d] = %08x, want %08x"
                 "  <-- THE BUG: row 0 was never stored, so every eighth"
                 " byte was checksummed as 0x00",
                 bad0, invfs_crc32c_slice8_table(0, (unsigned)bad0),
                 want[0][bad0]);
        ok(0, "%s", m);
    }
    if (badk < 0)
        ok(1, "T[1..7] are 1..7 folds of it (7 x 256 entries)");
    else
        ok(0, "T[%d] does not match its folds", badk);

    ok(invfs_crc32c_slice8_table(8, 0) == 0 &&
       invfs_crc32c_slice8_table(0, 256) == 0,
       "an out-of-range (k, i) returns 0 instead of reading past the table");
}

/* ===================================================================== *
 * LEG 2 / LEG 3 -- known-answer vectors.
 * ===================================================================== */
static void leg_vectors(const char *tag, int want_fallback)
{
    int pinned = invfs_crc32c_using_fallback();
    int bad = 0, nrun = 0;
    size_t fo = 0, fn = 0;
    uint32_t fg = 0, fr = 0;

    ok(pinned == want_fallback,
       "%s: invfs_crc32c_using_fallback() == %d (got %d)",
       tag, want_fallback, pinned);

    for (size_t o = 0; o < NOFF; o++) {
        for (size_t k = 0; k < NLENS; k++) {
            size_t n = LENS[k];
            uint32_t want = ref_crc32c(buf + o, n);
            uint32_t got  = invfs_crc32c(buf + o, n);
            nrun++;
            if (got != want) {
                if (!bad) { fo = o; fn = n; fg = got; fr = want; }
                bad++;
            }
        }
    }
    if (bad)
        info("first failure: offset %zu len %zu got %08x want %08x",
             fo, fn, fg, fr);
    ok(bad == 0,
       "%s: %d lengths x %d offsets (%d vectors) all match the textbook"
       " CRC32C", tag, (int)NLENS, NOFF, nrun);

    {
        static const uint8_t nine[9] = "123456789";
        uint32_t got = invfs_crc32c(nine, 9);
        ok(got == 0xE3069283u,
           "%s: CRC32C(\"123456789\") = %08x (published answer e3069283)",
           tag, got);
    }
}

/* Chained update over ragged chunks equals one-shot, through this same path.
 * invfs_crc32c_update is a separate entry point with its own dispatch, and
 * the delta log and the resize staging both go through it rather than the
 * one-shot form. */
static void leg_chained(const char *tag)
{
    int bad = 0;
    for (size_t o = 0; o < NOFF; o++) {
        for (size_t k = 0; k < NLENS; k++) {
            size_t n = LENS[k], i = 0, step = 1;
            uint32_t one = invfs_crc32c(buf + o, n), c = 0;
            while (i < n) {
                size_t take = (n - i < step) ? (n - i) : step;
                c = invfs_crc32c_update(c, buf + o + i, take);
                i += take;
                if (step < 37) step = step * 3 + 1;
            }
            if (c != one) bad++;
        }
    }
    ok(bad == 0, "%s: invfs_crc32c_update over ragged chunks == one-shot"
                " (%d of %d split patterns disagreed)",
       tag, bad, (int)(NOFF * NLENS));
}

/* ===================================================================== *
 * LEG 4 / LEG 5 -- the volume.
 * ===================================================================== */

/* Payloads spanning the length classes above, including lengths that are not
 * multiples of 8: a defect that drops every 8th byte is length-dependent and
 * does not show at every length class equally. */
static const char *VNAMES[] = {
    "b0000", "b0001", "b0007", "b0008", "b0009", "b0015", "b0016", "b0017",
    "b0031", "b0032", "b0063", "b0064", "b0065", "b1024", "b4096"
};
#define NVNAMES (sizeof VNAMES / sizeof VNAMES[0])
static const size_t VLEN[] = {
    0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 63, 64, 65, 1024, 4096
};

static uint8_t *vbuf;

static void vfill(void)
{
    for (size_t i = 0; i < BUFSZ; i++)
        vbuf[i] = (uint8_t)((i * 1103515245u + 12345u) >> 16);
}

static void volume_legs(const char *dir)
{
    char img[512], cmd[1024];
    const char *root = getenv("PWD") ? getenv("PWD") : ".";
    invfs_volume *v;
    int err = 0;

    snprintf(img, sizeof img, "%s/invf-crc32c-test.img", dir);
    unlink(img);

    /* ---- LEG 4: written AND read through the software path ----
     * This is the volume a non-SSE4.2 host produced. Every checksum on it
     * must come from crc32c_slice8 -- including the ones invf-mkfs writes,
     * and invf-mkfs is a SUBPROCESS, so it needs the ENVIRONMENT rather than
     * the in-process hook. Hence the setenv BEFORE the system() call: set
     * afterwards, mkfs would checksum with the hardware path and this leg
     * would silently become LEG 5. */
    setenv("INVFS_CRC32C_FORCE_FALLBACK", "1", 1);
    invfs_crc32c_force_fallback(1);
    ok(invfs_crc32c_using_fallback() == 1,
       "LEG 4: the software path is pinned, in this process AND in the"
       " env invf-mkfs inherits");

    snprintf(cmd, sizeof cmd, "INVFS_CRC32C_FORCE_FALLBACK=1 %s/bin/invf-mkfs"
             " %s 96 >/dev/null 2>&1", root, img);
    if (system(cmd) != 0) {
        fprintf(stderr, "crc32c_test: cannot create volume with invf-mkfs\n");
        failures++;
        return;
    }

    v = vol_open(img, &err);
    if (!v) {
        ok(0, "LEG 4: vol_open failed: err=%d", err);
        failures++;
        return;
    }
    for (size_t i = 0; i < NVNAMES; i++)
        if (!vol_create_file(v, VNAMES[i], vbuf, VLEN[i])) {
            ok(0, "LEG 4: vol_create_file(%s, %zu) failed", VNAMES[i], VLEN[i]);
            failures++;
            vol_close(v);
            return;
        }
    vol_flush(v);
    vol_close(v);                            /* <-- CLOSE */

    v = vol_open(img, &err);                 /* <-- REOPEN, same path */
    if (!v) {
        ok(0, "LEG 4: vol_open after close failed: err=%d", err);
        failures++;
        return;
    }
    for (size_t i = 0; i < NVNAMES; i++) {
        uint64_t id = vol_find(v, VNAMES[i]);
        uint8_t *data = NULL;
        size_t len = 0;
        if (!id) {
            ok(0, "LEG 4: vol_find(%s) == 0 after reopen", VNAMES[i]);
            failures++;
            vol_close(v);
            return;
        }
        if (vol_read_file(v, id, &data, &len) != 0 || !data) {
            ok(0, "LEG 4: vol_read_file(%s) failed", VNAMES[i]);
            failures++;
            vol_close(v);
            return;
        }
        if (len != VLEN[i] || (VLEN[i] && memcmp(data, vbuf, VLEN[i]) != 0)) {
            size_t at = 0;
            while (at < VLEN[i] && at < len && data[at] == vbuf[at]) at++;
            ok(0, "LEG 4: %s reads back len=%zu want=%zu (first diff at %zu)",
               VNAMES[i], len, VLEN[i], at);
            failures++;
            free(data);
            vol_close(v);
            return;
        }
        free(data);
    }
    ok(1, "LEG 4: %d files (0..4096 B) written, closed, REOPENED and read"
          " back byte-exact through the fallback", (int)NVNAMES);
    vol_close(v);

    /* ---- LEG 5: the SAME volume, pin RELEASED ----
     * On an SSE4.2 host that is the hardware path. Pre-fix this is where a
     * volume written on a CPU without SSE4.2 is rejected: the stored
     * superblock checksum does not match the computed one and every file is
     * gone. The pin is released IN-PROCESS (the hook overrides the env), so
     * this open and this read both use the CPU's own path. */
    invfs_crc32c_force_fallback(0);
    ok(invfs_crc32c_using_fallback() == 0,
       "LEG 5: the pin released, so this open uses the CPU's own path");

    v = vol_open(img, &err);
    if (!v) {
        ok(0, "LEG 5: vol_open with the pin released failed: err=%d -- this"
              " is the cross-machine rejection", err);
        failures++;
        return;
    }
    for (size_t i = 0; i < NVNAMES; i++) {
        uint64_t id = vol_find(v, VNAMES[i]);
        uint8_t *data = NULL;
        size_t len = 0;
        if (!id) {
            ok(0, "LEG 5: vol_find(%s) == 0 -- the file is GONE", VNAMES[i]);
            failures++;
            vol_close(v);
            return;
        }
        if (vol_read_file(v, id, &data, &len) != 0 || !data) {
            ok(0, "LEG 5: vol_read_file(%s) failed", VNAMES[i]);
            failures++;
            vol_close(v);
            return;
        }
        if (len != VLEN[i] || (VLEN[i] && memcmp(data, vbuf, VLEN[i]) != 0)) {
            ok(0, "LEG 5: %s reads back len=%zu want=%zu", VNAMES[i], len, VLEN[i]);
            failures++;
            free(data);
            vol_close(v);
            return;
        }
        free(data);
    }
    ok(1, "LEG 5: the volume written by the fallback still reads back"
          " byte-exact on the CPU's own path (the cross-machine case)");
    vol_close(v);

    unsetenv("INVFS_CRC32C_FORCE_FALLBACK");
    invfs_crc32c_force_fallback(0);          /* leave the hook as found */
    unlink(img);
}

/* ===================================================================== */
int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";

    setvbuf(stdout, NULL, _IONBF, 0);
    ref_init();
    ok(LENS[NLENS - 1] == (size_t)BIGGEST_LEN,
       "the length table's largest entry is %ld", (long)BIGGEST_LEN);
    fill_buf();
    vbuf = malloc(BUFSZ);
    if (!vbuf) { fprintf(stderr, "crc32c_test: oom\n"); return 2; }
    vfill();

    printf("crc32c_test: the software fallback must agree with the hardware"
           " path and with the textbook CRC32C, on EVERY host\n");

    if (stale_binary("src/core/crc32c.c")) return 3;

    invfs_crc32c_force_fallback(0);
    info("this host has SSE4.2: %s (invfs_crc32c_using_fallback() == %d)",
         invfs_crc32c_using_fallback() ? "no" : "yes",
         invfs_crc32c_using_fallback());

    printf("\nLEG 1  the slice-by-8 tables, structurally\n");
    leg1_table();

    printf("\nLEG 2  known-answer vectors, the CPU's own path\n");
    leg_vectors("LEG 2", invfs_crc32c_using_fallback());
    leg_chained("LEG 2");

    printf("\nLEG 3  known-answer vectors, SOFTWARE PATH FORCED\n");
    invfs_crc32c_force_fallback(1);
    leg_vectors("LEG 3", 1);
    leg_chained("LEG 3");

    printf("\nLEG 4/5  volume: write, close, reopen, read back\n");
    volume_legs(dir);

    printf("\ncrc32c_test: %d checks, %d failure(s)\n", checks, failures);
    free(vbuf);
    return failures ? 1 : 0;
}